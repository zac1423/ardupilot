/*
  driver for Beken_2425 radio
 */
#include <AP_HAL/AP_HAL.h>

#pragma GCC optimize("O0")

#if defined(HAL_RCINPUT_WITH_AP_RADIO) && CONFIG_HAL_BOARD_SUBTYPE == HAL_BOARD_SUBTYPE_CHIBIOS_SKYVIPER_F412

#include <AP_Math/AP_Math.h>
#include "AP_Radio_bk2425.h"
#include <utility>
#include <stdio.h>
#include <StorageManager/StorageManager.h>
#include <AP_Notify/AP_Notify.h>
#include <GCS_MAVLink/GCS_MAVLink.h>

#define TIMEOUT_PRIORITY 250	//Right above timer thread
#define EVT_TIMEOUT EVENT_MASK(0) // Event in the irq handler thread triggered by a timeout interrupt
#define EVT_IRQ EVENT_MASK(1) // Event in the irq handler thread triggered by a radio IRQ (Tx finished, Rx finished, MaxRetries limit)
#define EVT_BIND EVENT_MASK(2) // (not used yet) The user has clicked on the "start bind" button in the web interface (or equivalent).

extern const AP_HAL::HAL& hal;

// Output debug information on the UART, wrapped in MavLink packets
#define Debug(level, fmt, args...)   do { if ((level) <= get_debug_level()) { hal.console->printf(fmt, ##args); }} while (0)
// Output fast debug information on the UART, in raw format. MavLink should be disabled if you want to understand these messages.
// This is for debugging issues with frequency hopping and synchronisation.
#define DebugPrintf(level, fmt, args...)   do { if (radio_instance && ((level) <= radio_instance->get_debug_level())) { printf(fmt, ##args); }} while (0)


// object instance for trampoline
AP_Radio_beken *AP_Radio_beken::radio_instance;
thread_t *AP_Radio_beken::_irq_handler_ctx;
virtual_timer_t AP_Radio_beken::timeout_vt;
// See variable definitions in AP_Radio_bk2425.h for comments
uint32_t AP_Radio_beken::irq_time_us;
uint32_t AP_Radio_beken::irq_when_us;
uint32_t AP_Radio_beken::last_timeout_us;
uint32_t AP_Radio_beken::next_timeout_us;
uint32_t AP_Radio_beken::delta_timeout_us = 1000;
uint32_t AP_Radio_beken::next_switch_us;
uint32_t AP_Radio_beken::bind_time_ms;

// -----------------------------------------------------------------------------
// We have received a packet
// Sort out our timing relative to the tx to avoid clock drift
void SyncTiming::Rx(uint32_t when)
{
	uint32_t ld = delta_rx_time_us;
	uint32_t d = when - rx_time_us;
	if ((d > ld - DIFF_DELTA_RX) && (d < ld + DIFF_DELTA_RX)) // Two deltas are similar to each other
	{
		if ((d > TARGET_DELTA_RX-SLOP_DELTA_RX) && (d < TARGET_DELTA_RX+SLOP_DELTA_RX)) // delta is within range of single packet distance
		{
			// Use filter to change the estimate of the time in microseconds between the transmitters packet (according to OUR clock)
			sync_time_us = ((sync_time_us * (256-16)) + (d * 16)) / 256;
		}
	}
	rx_time_us = when;
	delta_rx_time_us = d;
	last_delta_rx_time_us = ld;
}

// -----------------------------------------------------------------------------
// Implement queuing (a 92 byte packet) in the circular buffer
void FwUpload::queue(const uint8_t *pSrc, uint8_t len)
{
	if (len == 0 || len > free_length())
		return; // Safety check for out of space error
	if (pending_head + len > SZ_BUFFER)
	{
		uint8_t n = SZ_BUFFER-pending_head;
		memcpy(&pending_data[pending_head], pSrc, n);
		memcpy(&pending_data[0], pSrc+n, len-n);
	}
	else
	{
		memcpy(&pending_data[pending_head], pSrc, len);
	}
	pending_head = (pending_head + len) & (SZ_BUFFER-1);
	added += len;
}

// -----------------------------------------------------------------------------
// Implement dequeing (a 16 byte packet)
void FwUpload::dequeue(uint8_t *pDst, uint8_t len)
{
	if (len == 0 || len > pending_length())
		return; // Safety check for underflow error
	if (pending_tail + len > SZ_BUFFER)
	{
		uint8_t n = SZ_BUFFER-pending_tail;
		memcpy(pDst, &pending_data[pending_tail], n);
		memcpy(pDst+n, &pending_data[0], len-n);
	}
	else
	{
		memcpy(pDst, &pending_data[pending_tail], len);
	}
	pending_tail = (pending_tail + len) & (SZ_BUFFER-1);
	sent += len;
}


// -----------------------------------------------------------------------------

/*
  constructor
 */
AP_Radio_beken::AP_Radio_beken(AP_Radio &_radio) :
    AP_Radio_backend(_radio),
    beken(hal.spi->get_device("beken")) // trace this later - its on libraries/AP_HAL_ChibiOS/SPIDevice.cpp:92
{
    // link to instance for irq_trampoline
    
    // (temporary) go into test mode
    radio_instance = this;
    beken.fcc.fcc_mode = 0;
    beken.fcc.channel = 23;
    beken.fcc.power = 7;
}

/*
  initialise radio
 */
bool AP_Radio_beken::init(void)
{
    if(_irq_handler_ctx != nullptr) {
        AP_HAL::panic("AP_Radio_beken: double instantiation of irq_handler\n");
    }
    chVTObjectInit(&timeout_vt);
    _irq_handler_ctx = chThdCreateFromHeap(NULL,
                                           THD_WORKING_AREA_SIZE(2048),
                                           "radio_bk2425",
                                           TIMEOUT_PRIORITY,          /* Initial priority.    */
                                           irq_handler_thd,           /* Thread function.     */
                                           NULL);                     /* Thread parameter.    */
    sem = hal.util->new_semaphore();    
    
    return reset();
}

/*
  reset radio
 */
bool AP_Radio_beken::reset(void)
{
    if (!beken.lock_bus()) {
        return false;
    }

    radio_init();
    beken.unlock_bus();

    return true;
}

/*
  return statistics structure from radio
 */
const AP_Radio::stats &AP_Radio_beken::get_stats(void)
{
    return stats;
}

/*
  read one pwm channel from radio
 */
uint16_t AP_Radio_beken::read(uint8_t chan)
{
    if (chan >= BEKEN_MAX_CHANNELS) {
        return 0;
    }
    return pwm_channels[chan];
}

/*
  update status - called from main thread
 */
void AP_Radio_beken::update(void)
{
	check_fw_ack();
}
    

/*
  return number of active channels, and updates the data
 */
uint8_t AP_Radio_beken::num_channels(void)
{
    uint32_t now = AP_HAL::millis();
    uint8_t chan = get_rssi_chan();
    if ((chan > 0) && ((chan-1) < BEKEN_MAX_CHANNELS)) {
		uint8_t value = BK_RSSI_DEFAULT; // Fixed value that will not update (halfway in the RSSI range for Cypress chips, 0..31)
		if (beken.fcc.enable_cd)
		{
			if (beken.fcc.last_cd)
				value += 4;
			else
				value -= 4;
		}
		if (t_status.pps == 0)
			value = BK_RSSI_MIN; // No packets = no RSSI
        pwm_channels[chan-1] = value;
        chan_count = MAX(chan_count, chan);
    }

    chan = get_pps_chan();
    if ((chan > 0) && ((chan-1) < BEKEN_MAX_CHANNELS)) {
        pwm_channels[chan-1] = t_status.pps; // How many packets received per second
        chan_count = MAX(chan_count, chan);
    }

    chan = get_tx_rssi_chan();
    if ((chan > 0) && ((chan-1) < BEKEN_MAX_CHANNELS)) {
        pwm_channels[chan-1] = BK_RSSI_DEFAULT; // Fixed value that will not update (halfway in the RSSI range for Cypress chips, 0..31)
        chan_count = MAX(chan_count, chan);
    }

    chan = get_tx_pps_chan();
    if ((chan > 0) && ((chan-1) < BEKEN_MAX_CHANNELS)) {
        pwm_channels[chan-1] = tx_pps;
        chan_count = MAX(chan_count, chan);
    }
    
    // Every second, update the statistics
    if (now - last_pps_ms > 1000) {
        last_pps_ms = now;
        t_status.pps = stats.recv_packets - last_stats.recv_packets;
        last_stats = stats;
        if (stats.lost_packets != 0 || stats.timeouts != 0) {
            Debug(3,"lost=%lu timeouts=%lu\n", stats.lost_packets, stats.timeouts);
        }
        stats.lost_packets=0;
        stats.timeouts=0;
    }
    return chan_count;
}

/*
  return time of last receive in microseconds
 */
uint32_t AP_Radio_beken::last_recv_us(void)
{
    return synctm.packet_timer;
}

/*
  send len bytes as a single packet
 */
bool AP_Radio_beken::send(const uint8_t *pkt, uint16_t len)
{
    // disabled for now
    return false;
}

/*
  initialise the radio
 */
void AP_Radio_beken::radio_init(void)
{
	DebugPrintf(1, "radio_init\r\n");
    beken.SetRBank(1);
    uint8_t id = beken.ReadReg(BK2425_R1_WHOAMI); // id is now 99
    beken.SetRBank(0); // Reset to default register bank.

    if (id != BK_CHIP_ID_BK2425) {
        
        Debug(1, "bk2425: radio not found\n"); // We have to keep trying  because it takes time to initialise
        return; // Failure
    }

    Debug(1, "beken: radio_init starting\n");

    beken.bkReady = 0;
    spd = beken.gTxSpeed;
	beken.SwitchToIdleMode();
    hal.scheduler->delay(100); // delay more than 50ms.

    // Initialise Beken registers
    beken.SetRBank(0);
    beken.InitBank0Registers(beken.gTxSpeed);
    beken.SetRBank(1);
    beken.InitBank1Registers(beken.gTxSpeed);
    hal.scheduler->delay(100); // delay more than 50ms.
    beken.SetRBank(0);
    
    beken.SwitchToRxMode(); // switch to RX mode
    beken.bkReady = 1;
    hal.scheduler->delay_microseconds(10*1000); // 10ms seconds delay
    
    // setup handler for rising edge of IRQ pin
    hal.gpio->attach_interrupt(HAL_GPIO_RADIO_IRQ, trigger_irq_radio_event, HAL_GPIO_INTERRUPT_FALLING);

    if (load_bind_info()) { // See if we already have bound to the address of a tx
        Debug(3,"Loaded bind info\n");
        nextChannel(1);
    }

	beken.EnableCarrierDetect(true); // For autobinding

    chVTSet(&timeout_vt, MS2ST(10), trigger_timeout_event, nullptr); // Initial timeout?
    if (3 <= get_debug_level())
		beken.DumpRegisters();
}

void AP_Radio_beken::trigger_irq_radio_event()
{
    //we are called from ISR context
    chSysLockFromISR();
    irq_time_us = AP_HAL::micros();
    chEvtSignalI(_irq_handler_ctx, EVT_IRQ);
    chSysUnlockFromISR();
}

void AP_Radio_beken::trigger_timeout_event(void *arg)
{
    (void)arg;
    //we are called from ISR context
	next_timeout_us += delta_timeout_us;
	last_timeout_us = AP_HAL::micros();
	if (int32_t(next_timeout_us - last_timeout_us) < 500) // Too late for this one
		next_timeout_us = last_timeout_us + delta_timeout_us;
	uint32_t delta = US2ST(next_timeout_us - last_timeout_us);

    chSysLockFromISR();
    chVTSetI(&timeout_vt, delta, trigger_timeout_event, nullptr); // Timeout every 1 ms
    chEvtSignalI(_irq_handler_ctx, EVT_TIMEOUT);
    chSysUnlockFromISR();
}

// ----------------------------------------------------------------------------
// The user has clicked on the "Start Bind" button on the web interface
void AP_Radio_beken::start_recv_bind(void)
{
    chan_count = 0;
    synctm.packet_timer = AP_HAL::micros();
	radio_instance->bind_time_ms = AP_HAL::millis();
    chEvtSignal(_irq_handler_ctx, EVT_BIND);
    Debug(1,"Starting bind\n");
}

// ----------------------------------------------------------------------------
// handle a data96 mavlink packet for fw upload
void AP_Radio_beken::handle_data_packet(mavlink_channel_t chan, const mavlink_data96_t &m)
{
    uint32_t ofs=0;
    memcpy(&ofs, &m.data[0], 4); // Assumes the endianness of the data!
	Debug(4, "got data96 of len %u from chan %u at offset %u\n", m.len, chan, unsigned(ofs));
    if (sem->take_nonblocking()) {
        fwupload.chan = chan;
        fwupload.need_ack = false;
        if (ofs == 0)
        {
			fwupload.reset();
			fwupload.file_length = ((uint16_t(m.data[4]) << 8) | (m.data[5])) + 6; // Add the header to the length
		}
		if (ofs != fwupload.added)
		{
			fwupload.need_ack = true; // We want more data
		}
		else
		{
			if (m.type == 43) {
				// sending a tune to play - for development testing
				fwupload.fw_type = TELEM_PLAY;
				fwupload.queue(&m.data[0], MIN(m.len, 90));
			} else {
				// sending a chunk of firmware OTA upload
				fwupload.fw_type = TELEM_FW;
				fwupload.queue(&m.data[4], MIN(m.len-4, 92)); // This might fail if mavlink sends it too fast to me, in which case it will retry later
			}
		}
        sem->give();
    } 
}


// ----------------------------------------------------------------------------
// Update the telemetry status variable; can be called in irq thread
// since the functions it calls are lightweight
void AP_Radio_beken::update_SRT_telemetry(void)
{
    t_status.flags = 0;
    t_status.flags |= AP_Notify::flags.gps_status >= 3?TELEM_FLAG_GPS_OK:0;
    t_status.flags |= AP_Notify::flags.pre_arm_check?TELEM_FLAG_ARM_OK:0;
    t_status.flags |= AP_Notify::flags.failsafe_battery?0:TELEM_FLAG_BATT_OK;
    t_status.flags |= hal.util->get_soft_armed()?TELEM_FLAG_ARMED:0;
    t_status.flags |= AP_Notify::flags.have_pos_abs?TELEM_FLAG_POS_OK:0;
    t_status.flags |= AP_Notify::flags.video_recording?TELEM_FLAG_VIDEO:0;
    t_status.flight_mode = AP_Notify::flags.flight_mode;
    t_status.tx_max = get_tx_max_power();
    t_status.note_adjust = get_tx_buzzer_adjust();
}

// ----------------------------------------------------------------------------
// Update a radio control packet
// Called from IRQ context.
// Returns true for DFU, false for telemetry
bool AP_Radio_beken::UpdateTxData(void)
{
    // send firmware update packet for 7/8 of packets if any data pending
    if ((fwupload.added >= (fwupload.acked + SZ_DFU)) && // Do we have a new packet to upload?
        ((fwupload.counter++ & 0x07) != 0) && // Avoid starvation of telemetry
        sem->take_nonblocking()) // Is the other threads busy with fwupload data?
	{
		// Send DFU packet
		packetFormatDfu* tx = &beken.pktDataDfu;
		if (fwupload.sent > fwupload.acked)
		{
			// Resend the last tx packet until it is acknowledged
			DebugPrintf(4, "resend %u %u %u\r\n", fwupload.added, fwupload.sent, fwupload.acked);
		}
		else
		{
			// Send firmware update packet
			tx->packetType = BK_PKT_TYPE_DFU;
		//	tx->channel;
			uint16_t addr = fwupload.sent;
			tx->address_lo = addr & 0xff;
			tx->address_hi = (addr >> 8);
			fwupload.dequeue(&tx->data[0], SZ_DFU);
			DebugPrintf(4, "send %u %u %u\r\n", fwupload.added, fwupload.sent, fwupload.acked);
			if (fwupload.free_length() >= 96)
			{
				fwupload.need_ack = true; // Request a new mavlink packet
			}
		}
        sem->give();
        return true;
    } else {
		// Send telemetry packet
		packetFormatTx* tx = &beken.pktDataTx;
		update_SRT_telemetry();
		tx->packetType = BK_PKT_TYPE_TELEMETRY; ///< The packet type
	//	tx->channel;
		tx->pps = t_status.pps;
		tx->flags = t_status.flags;
		tx->droneid[0] = myDroneId[0];
		tx->droneid[1] = myDroneId[1];
		tx->droneid[2] = myDroneId[2];
		tx->droneid[3] = myDroneId[3];
		tx->flight_mode = t_status.flight_mode;
		tx->wifi = t_status.wifi_chan + (24 * t_status.tx_max);
		tx->note_adjust = t_status.note_adjust;
		return false;
    }
	
}

// ----------------------------------------------------------------------------
// When (most of) a 92 byte packet has been sent to the Tx, ask for another one
// called from main thread
void AP_Radio_beken::check_fw_ack(void)
{
    if (fwupload.need_ack && sem->take_nonblocking()) {
        // ack the send of a DATA96 fw packet to TX
        if (fwupload.added < fwupload.file_length)
        {
			fwupload.need_ack = false;
			uint8_t data16[16] {};
			uint32_t ack_to = fwupload.added;
			memcpy(&data16[0], &ack_to, 4); // Assume endianness matches
			mavlink_msg_data16_send(fwupload.chan, 42, 4, data16);
		}
		else if (fwupload.added & 0x7f) // Are we on a boundary
		{
			// Pad out some bytes at the end
			uint8_t data16[16];
			memset(&data16[0], 0, sizeof(data16));
			if (fwupload.free_length() > 16)
			{
				fwupload.queue(&data16[0], 16-(fwupload.added & 15));
			}
			DebugPrintf(4, "Pad to %d\r\n", fwupload.added);
		}
		else if (fwupload.acked < fwupload.added)
		{
			// Keep sending to the tx until it is acked
			DebugPrintf(4, "PadResend %u %u %u\r\n", fwupload.added, fwupload.sent, fwupload.acked);
		}
		else
		{
			fwupload.need_ack = false; // All done
			DebugPrintf(3, "StopUpload\r\n");
			uint8_t data16[16] {};
			uint32_t ack_to = fwupload.file_length; // Finished
			memcpy(&data16[0], &ack_to, 4); // Assume endianness matches
			mavlink_msg_data16_send(fwupload.chan, 42, 4, data16);
		}
        sem->give();
    }
}

// ----------------------------------------------------------------------------
/* support all 4 rc input modes by swapping channels. */
void AP_Radio_beken::map_stick_mode(void)
{
    switch (get_stick_mode()) {
    case 1: {
        // mode1 = swap throttle and pitch
        uint16_t tmp = pwm_channels[1];
        pwm_channels[1] = pwm_channels[2];
        pwm_channels[2] = tmp;
        break;
    }

    case 3: {
        // mode3 = swap throttle and pitch, swap roll and yaw
        uint16_t tmp = pwm_channels[1];
        pwm_channels[1] = pwm_channels[2];
        pwm_channels[2] = tmp;
        tmp = pwm_channels[0];
        pwm_channels[0] = pwm_channels[3];
        pwm_channels[3] = tmp;
        break;
    }

    case 4: {
        // mode4 = swap roll and yaw
        uint16_t tmp = pwm_channels[0];
        pwm_channels[0] = pwm_channels[3];
        pwm_channels[3] = tmp;
        break;
    }
        
    case 2:
    default:
        // nothing to do, transmitter is natively mode2
        break;
    }

    // reverse pitch input to match ArduPilot default
    pwm_channels[1] = 3000 - pwm_channels[1];
}

// ----------------------------------------------------------------------------
// This is a valid manual/auto binding packet.
// The type of binding is valid now, and it came with the right address.
void AP_Radio_beken::ProcessBindPacket(const packetFormatRx * rx)
{
	// Set the address on which we are receiving the control data
	syncch.SetChannel(rx->channel);
	if (get_factory_test() == 0) // Final check that we are not in factory mode
	{
		beken.SetAddresses(&rx->u.bind.bind_address[0]);
		Debug(3, " Bound to %x %x %x %x %x\r\n", rx->u.bind.bind_address[0],
			rx->u.bind.bind_address[1], rx->u.bind.bind_address[2],
			rx->u.bind.bind_address[3], rx->u.bind.bind_address[4]);
		save_bind_info(); // May take some time
	}
}

// ----------------------------------------------------------------------------
// Handle receiving a packet (we are still in an interrupt!)
void AP_Radio_beken::ProcessPacket(const uint8_t* packet, uint8_t rxaddr)
{
	const packetFormatRx * rx = (const packetFormatRx *) packet; // Interpret the packet data
	switch (rx->packetType) {
	case BK_PKT_TYPE_CTRL_FOUND:
	case BK_PKT_TYPE_CTRL_LOST:
		// We haz data
		if (rxaddr == 0)
		{
			syncch.SetChannel(rx->channel);
		    synctm.packet_timer = AP_HAL::micros(); // This is essential for letting the channels update
		    if (!already_bound)
		    {
				already_bound = true; // Do not autobind to a different tx unless we power off
// test rssi	beken.EnableCarrierDetect(false); // Save 1ma of power
			}
			// Put the data into the control values (assuming mode2)
			pwm_channels[0] = 1000 + rx->u.ctrl.roll     + (uint16_t(rx->u.ctrl.msb & 0xC0) << 2); // Roll
			pwm_channels[1] = 1000 + rx->u.ctrl.pitch    + (uint16_t(rx->u.ctrl.msb & 0x30) << 4); // Pitch
			pwm_channels[2] = 1000 + rx->u.ctrl.throttle + (uint16_t(rx->u.ctrl.msb & 0x0C) << 6); // Throttle
			pwm_channels[3] = 1000 + rx->u.ctrl.yaw      + (uint16_t(rx->u.ctrl.msb & 0x03) << 8); // Yaw
			pwm_channels[4] = 1000 + ((rx->u.ctrl.buttons_held & 0x07) >> 0) * 100; // SW1, SW2, SW3
			pwm_channels[5] = 1000 + ((rx->u.ctrl.buttons_held & 0x38) >> 3) * 100; // SW4, SW5, SW6
			// cope with mode1/mode2/mode3/mode4
			map_stick_mode();
			chan_count = MAX(chan_count, 7);
			switch (rx->u.ctrl.data_type) {
			case BK_INFO_FW_VER: break;
			case BK_INFO_DFU_RX:
				{
					uint16_t ofs = rx->u.ctrl.data_value_hi;
					ofs <<= 8;
					ofs |= rx->u.ctrl.data_value_lo;
					if (ofs == fwupload.acked + SZ_DFU)
						fwupload.acked = ofs;
				}
				break;
			case BK_INFO_FW_CRC_LO: break;
			case BK_INFO_FW_CRC_HI: break;
			case BK_INFO_FW_YM: break;
			case BK_INFO_FW_DAY: break;
			case BK_INFO_MODEL: break;
			case BK_INFO_PPS:
				tx_pps = rx->u.ctrl.data_value_lo; // Remember pps from tx
				break;
			case BK_INFO_BATTERY:
				// "voltage from TX is in 0.025 volt units". Convert to 0.01 volt units for easier display
				// The CC2500 code (and this) actually assumes it is in 0.04 volt units, hence the tx scaling by 23/156 (38/256) instead of 60/256)
				// Which means a maximum value is 152 units representing 6.0v rather than 240 units representing 6.0v
		        pwm_channels[6] = rx->u.ctrl.data_value_lo * 4;
				break;
			case BK_INFO_COUNTDOWN:
				if (get_factory_test() == 0)
				{
					if (rx->u.ctrl.data_value_lo)
					{
						syncch.SetCountdown(rx->u.ctrl.data_value_lo+1, rx->u.ctrl.data_value_hi);
						DebugPrintf(2, "(%d) ", rx->u.ctrl.data_value_lo);
					}
				}
				break;
			default:
				break;
			};
		}
		break;
		
	case BK_PKT_TYPE_BIND_AUTO:
		if (rxaddr == 1)
		{
			if (get_autobind_rssi() > BK_RSSI_DEFAULT) // Have we disabled autobind using fake RSSI parameter?
				break;
			if (get_autobind_time() == 0) // Have we disabled autobind using zero time parameter?
				break;
			if (already_bound) // Do not auto-bind (i.e. to another tx) until we reboot.
				break;
			uint32_t now = AP_HAL::millis();
			if (now < get_autobind_time() * 1000) // Is this too soon from rebooting/powering up to autobind?
				break;
			// Check the carrier detect to see if the drone is too far away to auto-bind
			if (!beken.CarrierDetect())
				break;
			ProcessBindPacket(rx);
		}
		break;
		
	case BK_PKT_TYPE_BIND_MANUAL: // Sent by the tx for a few seconds after power-up when
		if (rxaddr == 1)
		{
			if (bind_time_ms == 0) // We have never receiving a binding click
				break; // Do not bind
			if (already_bound) // Do not manually-bind (i.e. to another tx) until we reboot.
				break;
//			if (uint32_t(AP_HAL::millis() - bind_time_ms) > 1000ul * 60u) // Have we pressed the button to bind recently? One minute timeout
//				break; // Do not bind
			ProcessBindPacket(rx);
		}
		break;
		
	case BK_PKT_TYPE_TELEMETRY:
	case BK_PKT_TYPE_DFU:
	default:
		// This is one of our packets! Ignore it.
		break;
	}
}

// ----------------------------------------------------------------------------
// Prepare to send a FCC packet
void AP_Radio_beken::UpdateFccScan(void)
{
	// Support scan mode
    if (beken.fcc.scan_mode) {
        beken.fcc.scan_count++;
        if (beken.fcc.scan_count >= 200) {
            beken.fcc.scan_count = 0;
            beken.fcc.channel += 2; // Go up by 2Mhz
            if (beken.fcc.channel >= CHANNEL_FCC_HIGH) {
                beken.fcc.channel = CHANNEL_FCC_LOW;
            }
        }
    }
}

// ----------------------------------------------------------------------------
// main IRQ handler
void AP_Radio_beken::irq_handler(uint32_t when)
{
    if (beken.fcc.fcc_mode) {
        // don't process interrupts in FCCTEST mode
		beken.WriteReg(BK_WRITE_REG | BK_STATUS,
			(BK_STATUS_RX_DR | BK_STATUS_TX_DS | BK_STATUS_MAX_RT)); // clear RX_DR or TX_DS or MAX_RT interrupt flag
        return;
    }
    
	// Determine which state fired the interrupt
	uint8_t bk_sta = beken.ReadStatus();
	if (bk_sta & BK_STATUS_TX_DS)
	{
		// Packet was sent towards the Tx board
		synctm.tx_time_us = when;
//		stats.sentPacketCount++;
		if (beken.fcc.disable_crc_mode && !beken.fcc.disable_crc)
		{
			beken.SwitchToIdleMode();
			beken.SetCrcMode(true);
		}
		beken.SwitchToRxMode(); // Prepare to receive next packet (on the next channel)
		nextChannel(1);
//		DebugPrintf(2, "T");
	}
	if (bk_sta & BK_STATUS_MAX_RT)
	{
		// We have had a "max retries" error
	}
	bool bReply = false;
	if (bk_sta & BK_STATUS_RX_DR)
	{
		// We have received a packet
		uint8_t rxstd = 0;
//		DebugPrintf(2, "R%ld,%ld\r\n", when, synctm.sync_time_us);
//		DebugPrintf(2, "R");
		// Which pipe (address) have we received this packet on?
		if ((bk_sta & BK_STATUS_RX_MASK) == BK_STATUS_RX_P_0)
		{
			rxstd = 0;
		}
		else if ((bk_sta & BK_STATUS_RX_MASK) == BK_STATUS_RX_P_1)
		{
			rxstd = 1;
		}
		else
		{
			stats.recv_errors++;
		}
		
		uint8_t len, fifo_sta;
		uint8_t packet[32];
		do
		{
            stats.recv_packets++;
			len = beken.ReadReg(BK_R_RX_PL_WID_CMD);	// read received packet length in bytes

			if (len <= PACKET_LENGTH_RX_MAX)
			{
				bReply = true;
				synctm.Rx(when);
//				printf("%d ", when + synctm.sync_time_us + 3000 - next_switch_us);
				next_switch_us = when + synctm.sync_time_us + 3000; // Switch channels if we miss the next packet
				// This includes short packets (e.g. where no telemetry was sent)
				beken.ReadRegisterMulti(BK_RD_RX_PLOAD, packet, len); // read receive payload from RX_FIFO buffer
//				DebugPrintf(3, "Packet %d(%d) %d %d %d %d %d %d %d %d ...\r\n", rxstd, len,
//					packet[0], packet[1], packet[2], packet[3], packet[4], packet[5], packet[6], packet[7]);
			}
			else // Packet was too long
			{
				beken.ReadRegisterMulti(BK_RD_RX_PLOAD, packet, 32); // read receive payload from RX_FIFO buffer
				beken.Strobe(BK_FLUSH_RX); // flush Rx
			}
			fifo_sta = beken.ReadReg(BK_FIFO_STATUS);	// read register FIFO_STATUS's value
		} while (!(fifo_sta & BK_FIFO_STATUS_RX_EMPTY)); // while not empty
		beken.WriteReg(BK_WRITE_REG | BK_STATUS,
			(BK_STATUS_RX_DR | BK_STATUS_TX_DS | BK_STATUS_MAX_RT)); // clear RX_DR or TX_DS or MAX_RT interrupt flag
		ProcessPacket(packet, rxstd);
		if (beken.fcc.enable_cd)
		{
			beken.fcc.last_cd = beken.CarrierDetect(); // Detect if close or not
		}
		else
		{
			beken.fcc.last_cd = true; // Assumed to be close
		}
	}

	// Clear the bits
	beken.WriteReg((BK_WRITE_REG|BK_STATUS), (BK_STATUS_MAX_RT | BK_STATUS_TX_DS | BK_STATUS_RX_DR));
	if (bReply)
	{
		if (get_telem_enable()) // Note that the user can disable telemetry, but the transmitter will be less functional in this case.
		{
			// Send the telemetry reply to the controller
			beken.Strobe(BK_FLUSH_TX); // flush Tx
			beken.ClearAckOverflow();
			bool txDfu = UpdateTxData();
			if (txDfu)
				beken.pktDataDfu.channel = syncch.channel;
			else
				beken.pktDataTx.channel = syncch.channel;
			if (beken.fcc.disable_crc_mode)
			{
				// Only disable the CRC on reception, not transmission, so the connection remains.
				beken.SwitchToIdleMode();
				beken.SetCrcMode(false);
			}
			beken.SwitchToTxMode();
			hal.scheduler->delay_microseconds(100); // delay to give the (remote) tx a chance to switch to receive mode
			if (txDfu)
				beken.SendPacket(BK_W_TX_PAYLOAD_NOACK_CMD, (uint8_t *)&beken.pktDataDfu, PACKET_LENGTH_TX_DFU);
			else
				beken.SendPacket(BK_W_TX_PAYLOAD_NOACK_CMD, (uint8_t *)&beken.pktDataTx, PACKET_LENGTH_TX_TELEMETRY);
		}
		else // Try to still work when telemetry is disabled
		{
			nextChannel(1);
		}
	}
}	

// ----------------------------------------------------------------------------
// handle timeout IRQ (called every ms)
void AP_Radio_beken::irq_timeout(void)
{
	if (!beken.bkReady) // We are reinitialising the chip in the main thread
	{
		return;
	}

	static uint8_t check_params_timer = 0;
	if (++check_params_timer >= 10) // We don't need to test the parameter logic every ms.
	{
		check_params_timer = 0;
		// Set the transmission power
		uint8_t pwr = get_transmit_power();
		if (pwr != beken.fcc.power + 1)
		{
			if ((pwr > 0) && (pwr <= 8))
			{
				beken.SwitchToIdleMode();
				beken.SetPower(pwr-1);
			}
		}
		
		// Set CRC mode
		uint8_t crc = get_disable_crc();
		if (crc != beken.fcc.disable_crc_mode)
		{
			beken.SwitchToIdleMode();
			beken.SetCrcMode(crc);
			beken.fcc.disable_crc_mode = crc;
		}

		// Do we need to change our factory test mode?
		uint8_t factory = get_factory_test();
		if (factory != beken.fcc.factory_mode)
		{
			beken.SwitchToIdleMode();
			// Set frequency
			syncch.channel = factory ? (factory-1) + CHANNEL_COUNT_LOGICAL*CHANNEL_NUM_TABLES : 0;
			// Set address
			beken.SetFactoryMode(factory);
		}
		
		// Do we need to change our fcc test mode status?
		uint8_t fcc = get_fcc_test();
		if (fcc != beken.fcc.fcc_mode)
		{
			beken.Strobe(BK_FLUSH_TX);
			if (fcc == 0) // Turn off fcc test mode
			{
				if (beken.fcc.CW_mode)
				{
					beken.SwitchToIdleMode();
					beken.SetCwMode(false);
				}
			}
			else
			{
				if (fcc > 3)
				{
					if (!beken.fcc.CW_mode)
					{
						beken.SwitchToIdleMode();
						beken.SetCwMode(true);
						beken.DumpRegisters();
					}
				}
				else
				{
					if (beken.fcc.CW_mode)
					{
						beken.SwitchToIdleMode();
						beken.SetCwMode(false);
					}
				}
				switch (fcc) {
				case 1: case 4:
				default:
					beken.fcc.channel = CHANNEL_FCC_LOW;
					break;
				case 2: case 5:
					beken.fcc.channel = CHANNEL_FCC_MID;
					break;
				case 3: case 6:
					beken.fcc.channel = CHANNEL_FCC_HIGH;
					break;
				};
			}
			beken.fcc.fcc_mode = fcc;
			DebugPrintf(1, "\r\nFCC mode %d\r\n", fcc);
		}
	}

	// For fcc mode, just send packets on timeouts
	if (beken.fcc.fcc_mode)
	{
		static uint8_t tt = 0;
		if (++tt >= 5) // Space out to every 5 ms
		{
			tt = 0;
		}
		else
		{
			return;
		}

		beken.SwitchToTxMode();
		beken.ClearAckOverflow();
		UpdateFccScan();
		beken.SetChannel(beken.fcc.channel);
		UpdateTxData();
		beken.pktDataTx.channel = 0;
		if (!beken.fcc.CW_mode)
		{
			beken.SendPacket(BK_WR_TX_PLOAD, (uint8_t *)&beken.pktDataTx, PACKET_LENGTH_TX_TELEMETRY);
//			DebugPrintf(3, "*");
		}
		return;
	}

	// Normal modes - we have timed out for channel hopping
	if (last_timeout_us >= next_switch_us) // We can swap channels now
	{
		int32_t d = synctm.sync_time_us; // Time between packets, e.g. 5100 us
		uint32_t dt = last_timeout_us - synctm.rx_time_us;
		if (dt > 50*d) // We have lost sync (missed 50 packets) so slow down the channel hopping until we resync
		{
			d *= 4;
			DebugPrintf(2, "C");
		}
		else
		{
			DebugPrintf(2, "c");
		}
		{
			uint8_t fifo_sta = radio_instance->beken.ReadReg(BK_FIFO_STATUS);	// read register FIFO_STATUS's value
			if (!(fifo_sta & BK_FIFO_STATUS_RX_EMPTY)) // while not empty
			{
				DebugPrintf(2, "#");
				radio_instance->irq_handler(AP_HAL::micros());
			}
			else
			{
				next_switch_us += d; // Switch channels if we miss the next packet
			}
		}
		int32_t ss = int32_t(next_switch_us - last_timeout_us);
		if (ss < 1000) // Not enough time
		{
			next_switch_us = last_timeout_us + d; // Switch channels if we miss the next packet
			DebugPrintf(2, "j");
		}
		if (!beken.WasRxMode())
		{
			beken.SwitchToRxMode();
		}
		nextChannel(1);
		beken.ClearAckOverflow();
	}
}

// ----------------------------------------------------------------------------
// Thread that supports Beken Radio work triggered by interrupts
// This is the only thread that should access the Beken radio chip via SPI.
void AP_Radio_beken::irq_handler_thd(void *arg)
{
    (void) arg;
    while(true) {
        eventmask_t evt = chEvtWaitAny(ALL_EVENTS);
        if (_irq_handler_ctx != nullptr) // Sanity check
			_irq_handler_ctx->name = "RadioBeken"; // Only useful to be done once but here is done often

        radio_instance->beken.lock_bus();
        irq_when_us = irq_time_us; // Get the time of the event
        switch(evt) {
        case EVT_IRQ:
            if (radio_instance->beken.fcc.fcc_mode != 0) {
                DebugPrintf(3, "IRQ FCC\n");
            }
            radio_instance->irq_handler(irq_when_us);
            break;
        case EVT_TIMEOUT:
			radio_instance->irq_timeout();
            break;
        case EVT_BIND: // The user has clicked on the "Start Bind" button on the web interface
			DebugPrintf(2, "\r\nBtnStartBind\r\n");
            break;
        default:
            break;
        }
        radio_instance->beken.unlock_bus();
    }
}

void AP_Radio_beken::setChannel(uint8_t channel)
{
	beken.SetChannel(channel);
}

const uint8_t bindHopData[CHANNEL_NUM_TABLES*CHANNEL_COUNT_LOGICAL+CHANNEL_COUNT_TEST] = {
#if 0 // Single frequency mode
	23,23,23,23,23,23,23,23,23,23,23,23,23,23,23,23, // Normal
	23,23,23,23,23,23,23,23,23,23,23,23,23,23,23,23, // Wifi channel 1,2,3,4,5
	23,23,23,23,23,23,23,23,23,23,23,23,23,23,23,23, // Wifi channel 6
	23,23,23,23,23,23,23,23,23,23,23,23,23,23,23,23, // Wifi channel 7
	23,23,23,23,23,23,23,23,23,23,23,23,23,23,23,23, // Wifi channel 8
	23,23,23,23,23,23,23,23,23,23,23,23,23,23,23,23, // Wifi channel 9,10,11
	23,23,23,23,23,23,23,23,23,23,23,23,23,23,23,23, // Test mode channels
#else // Normal mode
	46,41,31,52,36,13,72,69, 21,56,16,26,61,66,10,45, // Normal
	57,62,67,72,58,63,68,59, 64,69,60,65,70,61,66,71, // Wifi channel 1,2,3,4,5
	62,10,67,72,63,68,11,64, 69,60,65,70,12,61,66,71, // Wifi channel 6
	10,67,11,72,12,68,13,69, 14,65,15,70,16,66,17,71, // Wifi channel 7
	10,70,15,20,11,71,16,21, 12,17,22,72,13,18,14,19, // Wifi channel 8
	10,15,20,25,11,16,21,12, 17,22,13,18,23,14,19,24, // Wifi channel 9,10,11
	46,41,31,52,36,13,72,69, 21,56,16,26,61,66,10,43, // Test mode channels
#endif
};

void AP_Radio_beken::nextChannel(uint8_t skip)
{
    if (skip)
		syncch.NextChannel();
    setChannel(bindHopData[syncch.channel]);
}

/*
  save bind info
 */
void AP_Radio_beken::save_bind_info(void)
{
    // access to storage for bind information
    StorageAccess bind_storage(StorageManager::StorageBindInfo);
    struct bind_info info;
    
    info.magic = bind_magic;
    info.bindTxId[0] = beken.TX_Address[0];
    info.bindTxId[1] = beken.TX_Address[1];
    info.bindTxId[2] = beken.TX_Address[2];
    info.bindTxId[3] = beken.TX_Address[3];
    info.bindTxId[4] = beken.TX_Address[4];
    bind_storage.write_block(0, &info, sizeof(info));
}

/*
  load bind info
 */
bool AP_Radio_beken::load_bind_info(void)
{
    // access to storage for bind information
    StorageAccess bind_storage(StorageManager::StorageBindInfo);
    struct bind_info info;

    if (!bind_storage.read_block(&info, 0, sizeof(info)) || info.magic != bind_magic) {
        return false;
    }

	beken.SetAddresses(&info.bindTxId[0]);

    return true;
}

// Step through the channels
void SyncChannel::NextChannel(void)
{
	if (channel >= CHANNEL_COUNT_LOGICAL*CHANNEL_NUM_TABLES)
	{
		// We are in the factory test modes. Keep the channel as is.
	}
	else
	{
		if (countdown != countdown_invalid)
		{
			if (--countdown == 0)
			{
				channel = countdown_chan;
				countdown = countdown_invalid;
				return;
			}
		}
		uint8_t table = channel / CHANNEL_COUNT_LOGICAL;
		channel = (channel + 1) % CHANNEL_COUNT_LOGICAL;
		channel += table * CHANNEL_COUNT_LOGICAL;
	}
}

// If we have not received any packets for ages, try a WiFi table that covers all frequencies
void SyncChannel::SafeTable(void)
{
	if (channel >= CHANNEL_COUNT_LOGICAL*CHANNEL_NUM_TABLES)
	{
		// We are in the factory test modes. Keep the channel as is.
	}
	else
	{
		uint8_t table = channel / CHANNEL_COUNT_LOGICAL;
		if ((table != CHANNEL_BASE_TABLE) && (table != CHANNEL_SAFE_TABLE)) // Are we using a table that is high end or low end only?
		{
			channel %= CHANNEL_COUNT_LOGICAL;
			channel += CHANNEL_SAFE_TABLE * CHANNEL_COUNT_LOGICAL;
		}
	}
}

#endif // HAL_RCINPUT_WITH_AP_RADIO
