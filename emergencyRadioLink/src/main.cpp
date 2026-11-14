#include "main.h"

#include <stdio.h>
#include <string.h>
#include <pico/stdlib.h>
#include <pico/multicore.h>
#include <pico/util/queue.h>
#include <pico/rand.h>
#include <RadioLib.h>

#include "network.h"
#include "flash.h"
#include "compass_handler.h"
#include "gps_handler.h"
#include "uart.h"
#include "hal/RPiPico/PicoHal.h"
#include "protocol/erl_handler.h"
#include "protocol/merl_handler.h"
#include "watchdog.h"
#include "ws2812.h"

#define LED_BRIGHTNESS 64   // 0-255

typedef struct {
	uint16_t length;
	uint8_t data[RADIO_FRAME_DATA_SIZE];
	int8_t rssi;
	int8_t snr_q4;              // received frames: SNR in 0.25 dB steps
	int32_t freq_error_hz;      // received frames: carrier offset against our crystal
	uint32_t rx_ms;             // received frames: ms since boot
} radio_frame_t;

static int8_t clamp_snr_q4(const float snr_db) {
	const float q = snr_db * 4.0f;
	if (q < -128.0f)
		return -128;
	if (q > 127.0f)
		return 127;
	return static_cast<int8_t>(q < 0 ? q - 0.5f : q + 0.5f);
}

static int8_t clamp_rssi(const float rssi) {
	if (rssi < -128.0f)
		return -128;
	if (rssi > 127.0f)
		return 127;
	return static_cast<int8_t>(rssi);
}

extern "C" uint32_t ublox_millis() {
	return to_ms_since_boot(get_absolute_time());
}

static PicoHal *hal = new PicoHal(CONFIG_LORA_SPI_HW, CONFIG_LORA_MISO, CONFIG_LORA_MOSI, CONFIG_LORA_SCK);
static SX1278 radio = new Module(hal, CONFIG_LORA_NSS, CONFIG_LORA_DIO0, CONFIG_LORA_RESET, CONFIG_LORA_DIO1);
static network_t network{.address = CONFIG_ERL_NETWORK_ADDRESS, .subnet_mask = CONFIG_ERL_NETWORK_SUBNET};

static queue_t g_tx_queue;
static queue_t g_rx_queue;

/* -- Addressing helpers -- */
inline uint16_t local_broadcast_address() {
	return static_cast<uint16_t>((g_device_network_info.address & g_device_network_info.subnet_mask) |
	                             ~g_device_network_info.subnet_mask);
}

inline bool in_local_subnet(uint16_t address) {
	return (address & g_device_network_info.subnet_mask) ==
	       (g_device_network_info.address & g_device_network_info.subnet_mask);
}

inline bool packet_is_for_us(const erl_packet_t *packet) {
	return packet->receiver == g_device_network_info.address ||
	       (packet->receiver == local_broadcast_address() && in_local_subnet(packet->address));
}

inline bool frame_crc_valid(const radio_frame_t &frame) {
	if (frame.length < sizeof(erl_packet_t) + 1)
		return false;
	return crc8_calc(frame.data, frame.length - 1, ERL_PROTOCOL_CRC8_POLYNOMIAL) == frame.data[frame.length - 1];
}

/* -- Radio Setup -- */
static bool radio_check(int state, const char *what) {
	if (state == RADIOLIB_ERR_NONE)
		return true;
	printf("%s failed, code %d\r\n", what, state);
	return false;
}

static bool radio_init_all() {
	printf("[SX1276] Initializing ... ");
	if (!radio_check(radio.begin(), "begin"))
		return false;
	printf("success!\r\n");

	printf("[SX1276] Configuring ... ");
	return radio_check(radio.setFrequency(RADIO_FREQUENCY_MHZ), "setFrequency") &&
	       radio_check(radio.setBandwidth(RADIO_BANDWIDTH_KHZ), "setBandwidth") &&
	       radio_check(radio.setSpreadingFactor(RADIO_SPREADING_FACTOR), "setSpreadingFactor") &&
	       radio_check(radio.setCodingRate(RADIO_CODING_RATE), "setCodingRate") &&
	       radio_check(radio.setOutputPower(RADIO_OUTPUT_POWER_DBM), "setOutputPower") &&
	       radio_check(radio.setPreambleLength(RADIO_PREAMBLE_LENGTH), "setPreambleLength") &&
	       radio_check(radio.setCRC(true), "setCRC");
}

/* -- User messages -- */
static bool queue_user_message(const uint16_t receiver, const uint8_t *message, const uint8_t length) {
	if (length > ERL_PROTOCOL_PAYLOAD_MAX_SIZE)
		return false;

	uint8_t packet_buf[sizeof(erl_packet_t) + ERL_PROTOCOL_PAYLOAD_MAX_SIZE];
	auto *packet = reinterpret_cast<erl_packet_t *>(packet_buf);
	packet->receiver = receiver;
	packet->type = PACKET_TYPE_USER_MESSAGE;
	packet->length = length;
	memcpy(packet->payload, message, length);

	radio_frame_t frame;
	frame.length = erl_handler_build_packet(packet, frame.data, sizeof(frame.data));
	if (frame.length == 0)
		return false;
	if (!queue_try_add(&g_tx_queue, &frame)) {
		++g_radio_stats.tx_queue_drop;
		return false;
	}
	return true;
}

static bool g_alert_active;
static absolute_time_t g_alert_until;

static void emergency_alert_callback() {
	D_printf("[ALERT] Alert pin triggered\r\n");
	ws2812_set(255, 0, 0);   // red
	g_alert_active = true;
	g_alert_until = make_timeout_time_ms(CONFIG_ALERT_DURATION_MS);
}

static void update_alert_led() {
	if (g_alert_active && time_reached(g_alert_until)) {
		ws2812_set(0, 255, 0);
		g_alert_active = false;
	}
}

// Level-triggered: as long as the pin reads HIGH, the alert stays (re-)armed every poll.
static void check_alert_pin() {
	if (gpio_get(CONFIG_INT_ALERT_PIN))
		emergency_alert_callback();
}

static void queue_heartbeat() {
	uint8_t packet_buf[sizeof(erl_packet_t) + sizeof(erl_network_info_t) + sizeof(erl_position_t)];
	auto *packet = reinterpret_cast<erl_packet_t *>(packet_buf);
	packet->receiver = local_broadcast_address();
	packet->type = PACKET_TYPE_NETWORK;
	packet->length = sizeof(erl_network_info_t);

	static uint16_t heartbeat_seq;
	auto *info = reinterpret_cast<erl_network_info_t *>(packet->payload);
	info->uptime = to_ms_since_boot(get_absolute_time()) / 1000;
	info->config = heartbeat_seq++;   // bits 0-15: sequence number, receivers count lost heartbeats
	erl_handler_set_peer_info(g_device_network_info.address, 0, info->uptime, info->config);

	gps_position_t position;
	if (gps_handler_get_position(&position)) {
		auto *body = reinterpret_cast<erl_position_t *>(packet->payload + sizeof(erl_network_info_t));
		body->time = position.time;
		body->longitude = position.longitude;
		body->latitude = position.latitude;
		packet->length += sizeof(erl_position_t);
		erl_handler_set_peer_position(g_device_network_info.address, 0, position.longitude, position.latitude);
	}

	radio_frame_t frame;
	frame.length = erl_handler_build_packet(packet, frame.data, sizeof(frame.data));

	if (frame.length > 0) {
		queue_add_blocking(&g_tx_queue, &frame);
	}
}

static void queue_position_broadcast() {
	gps_position_t position;
	if (!gps_handler_get_position(&position))
		return;

	erl_handler_set_peer_position(g_device_network_info.address, 0, position.longitude, position.latitude);

	uint8_t packet_buf[sizeof(erl_packet_t) + sizeof(erl_position_t) + sizeof(erl_compass_t)];
	auto *packet = reinterpret_cast<erl_packet_t *>(packet_buf);
	packet->receiver = local_broadcast_address();
	packet->type = PACKET_TYPE_POSITION;
	packet->length = sizeof(erl_position_t);

	auto *body = reinterpret_cast<erl_position_t *>(packet->payload);
	body->time = position.time;
	body->longitude = position.longitude;
	body->latitude = position.latitude;

	compass_reading_t reading;
	if (compass_handler_get(&reading)) {
		auto *compass = reinterpret_cast<erl_compass_t *>(packet->payload + sizeof(erl_position_t));
		compass->x = reading.x;
		compass->y = reading.y;
		compass->z = reading.z;
		packet->length += sizeof(erl_compass_t);
		erl_handler_set_peer_compass(g_device_network_info.address, 0, reading.x, reading.y, reading.z);
	}

	radio_frame_t frame;
	frame.length = erl_handler_build_packet(packet, frame.data, sizeof(frame.data));

	if (frame.length > 0 && !queue_try_add(&g_tx_queue, &frame)) {
		++g_radio_stats.tx_queue_drop;
	}
}

static bool is_for_this_unit(const uint16_t receiver) {
	const auto own = static_cast<uint16_t>(g_device_network_info.address);
	const auto mask = static_cast<uint16_t>(g_device_network_info.subnet_mask);
	const auto host_bits = static_cast<uint16_t>(~mask);
	return receiver == own ||
	       (host_bits != 0 && (receiver & mask) == (own & mask) && (receiver & host_bits) == host_bits);
}

static void process_rx_queue() {
	radio_frame_t rx_frame;
	if (!queue_try_remove(&g_rx_queue, &rx_frame))
		return;

	auto *packet = reinterpret_cast<erl_packet_t *>(&rx_frame.data);
	D_packet_print(packet);

	const bool length_valid = sizeof(erl_packet_t) + packet->length + sizeof(uint8_t) == rx_frame.length;
	if (!length_valid || packet->address == g_device_network_info.address)
		return;
	erl_handler_link_update(packet->address, rx_frame.rssi, rx_frame.snr_q4, rx_frame.freq_error_hz, rx_frame.rx_ms);

	constexpr uint8_t network_info_with_position_length = sizeof(erl_network_info_t) + sizeof(erl_position_t);
	constexpr uint8_t position_with_compass_length = sizeof(erl_position_t) + sizeof(erl_compass_t);

	if (packet->type == PACKET_TYPE_NETWORK &&
	    (packet->length == sizeof(erl_network_info_t) || packet->length == network_info_with_position_length)) {
		auto *info = reinterpret_cast<const erl_network_info_t *>(packet->payload);
		erl_handler_set_peer_info(packet->address, rx_frame.rssi, info->uptime, info->config);
		erl_handler_link_heartbeat(packet->address, static_cast<uint16_t>(info->config & 0xFFFF), info->uptime);

		if (packet->length == network_info_with_position_length) {
			auto *body = reinterpret_cast<const erl_position_t *>(packet->payload + sizeof(erl_network_info_t));
			erl_handler_set_peer_position(packet->address, rx_frame.rssi, body->longitude, body->latitude);
		}
	} else if (packet->type == PACKET_TYPE_POSITION &&
	          (packet->length == sizeof(erl_position_t) || packet->length == position_with_compass_length)) {
		auto *body = reinterpret_cast<const erl_position_t *>(packet->payload);
		erl_handler_set_peer_position(packet->address, rx_frame.rssi, body->longitude, body->latitude);

		if (packet->length == position_with_compass_length) {
			auto *compass = reinterpret_cast<const erl_compass_t *>(packet->payload + sizeof(erl_position_t));
			erl_handler_set_peer_compass(packet->address, rx_frame.rssi, compass->x, compass->y, compass->z);
		}
	} else {
		erl_handler_touch_peer(packet->address, rx_frame.rssi);
	}

	if (packet->type == PACKET_TYPE_USER_MESSAGE && is_for_this_unit(packet->receiver)) {
		merl_handler_forward_user_message(packet->address, packet->receiver, packet->payload, packet->length);
	}
}

// A period +- PERIOD_JITTER_MS at random: units that boot together, or whose transmissions
// once lined up, do not keep sending (and colliding) at the same moments
static uint32_t jittered_ms(const uint32_t period_ms) {
	return period_ms - PERIOD_JITTER_MS + get_rand_32() % (2 * PERIOD_JITTER_MS + 1);
}

void core1_entry() {
	flash_core_init();
	gps_handler_init();
	compass_handler_init();

	gpio_init(CONFIG_INT_ALERT_PIN);
	gpio_set_dir(CONFIG_INT_ALERT_PIN, GPIO_IN);
	gpio_pull_down(CONFIG_INT_ALERT_PIN);

	absolute_time_t next_heartbeat = get_absolute_time();
	absolute_time_t next_position_broadcast = get_absolute_time();
	absolute_time_t next_compass_poll = get_absolute_time();

	while (1) {
		if (time_reached(next_heartbeat)) {
			queue_heartbeat();
			next_heartbeat = make_timeout_time_ms(jittered_ms(HEARTBEAT_INTERVAL_MS));
		}

		if (time_reached(next_position_broadcast)) {
			queue_position_broadcast();
			next_position_broadcast = make_timeout_time_ms(jittered_ms(POSITION_INTERVAL_MS));
		}

		if (time_reached(next_compass_poll)) {
			compass_handler_service();
			next_compass_poll = make_timeout_time_ms(COMPASS_POLL_INTERVAL_MS);
		}

		process_rx_queue();
		merl_handler_service();
		gps_handler_service();
		check_alert_pin();
		update_alert_led();
		watchdog_mark_alive();
		sleep_ms(LOOP_DELAY_MS);
	}
}

/* -- Core 0 -- */
static absolute_time_t g_tx_not_before;

static void radio_listen() {
	const int state = radio.startReceive();
	if (state != RADIOLIB_ERR_NONE) {
		D_printf("[RX] startReceive failed, code %d\r\n", state);
	}
}

static bool radio_channel_busy() {
	return (radio.getModemStatus() & 0x0B) != 0;
}

static void radio_service_tx() {
	radio_frame_t tx_frame;
	if (!time_reached(g_tx_not_before) || !queue_try_peek(&g_tx_queue, &tx_frame))
		return;

	if (radio_channel_busy()) {
		++g_radio_stats.lbt_busy;
		g_tx_not_before = make_timeout_time_ms(TX_BACKOFF_MIN_MS + get_rand_32() % TX_BACKOFF_RANGE_MS);
		return;
	}
	queue_try_remove(&g_tx_queue, &tx_frame);

	D_packet_print(reinterpret_cast<erl_packet_t *>(&tx_frame.data));
	const int state = radio.transmit(tx_frame.data, tx_frame.length);

	if (state == RADIOLIB_ERR_NONE) {
		D_printf("success!\r\n");
		++g_radio_stats.tx_ok;
		g_radio_stats.tx_airtime_ms += static_cast<uint32_t>(radio.getTimeOnAir(tx_frame.length) / 1000);
	} else {
		D_printf("failed, code %d\r\n", state);
		++g_radio_stats.tx_error;
	}

	radio_listen();
	g_tx_not_before = make_timeout_time_ms(get_rand_32() % TX_GAP_RANGE_MS);
}

static void radio_service_rx() {
	if (!gpio_get(CONFIG_LORA_DIO0))
		return;

	radio_frame_t rx_frame;
	const size_t length = radio.getPacketLength();
	const int state = radio.readData(rx_frame.data, sizeof(rx_frame.data));
	// the last packet's link quality, before receiving again
	rx_frame.rssi = clamp_rssi(radio.getRSSI());
	rx_frame.snr_q4 = clamp_snr_q4(radio.getSNR());
	rx_frame.freq_error_hz = static_cast<int32_t>(radio.getFrequencyError());
	rx_frame.rx_ms = to_ms_since_boot(get_absolute_time());
	radio_listen();

	if (state != RADIOLIB_ERR_NONE) {
		D_printf("[RX] Failed, code %d\r\n", state);
		if (state == RADIOLIB_ERR_CRC_MISMATCH)
			++g_radio_stats.rx_crc_error;
		return;
	}
	if (length > sizeof(rx_frame.data)) {
		D_printf("[RX] Dropped packet, %u bytes is longer than an ERL frame\r\n", static_cast<unsigned>(length));
		++g_radio_stats.rx_bad_frame;
		return;
	}
	rx_frame.length = static_cast<uint16_t>(length);

	if (!frame_crc_valid(rx_frame)) {
		D_printf("[RX] Dropped packet, bad CRC8\r\n");
		++g_radio_stats.rx_bad_frame;
		return;
	}
	++g_radio_stats.rx_ok;

	if (!packet_is_for_us(reinterpret_cast<const erl_packet_t *>(rx_frame.data))) {
		++g_radio_stats.rx_not_for_us;
	} else if (!queue_try_add(&g_rx_queue, &rx_frame)) {
		++g_radio_stats.rx_queue_drop;
	}
}

// The noise floor: the channel's RSSI now, only while no packet is being received
static void radio_sample_noise() {
	static absolute_time_t next;
	static float average;
	if (!time_reached(next))
		return;
	next = make_timeout_time_ms(NOISE_SAMPLE_MS);
	if (gpio_get(CONFIG_LORA_DIO0) || radio_channel_busy())
		return;

	const int8_t now = clamp_rssi(radio.getRSSI(false, true));
	average = g_radio_stats.noise_avg_dbm == 0 ? now : average + (now - average) * NOISE_AVG_WEIGHT;
	g_radio_stats.noise_dbm = now;
	if (g_radio_stats.noise_min_dbm == 0 || now < g_radio_stats.noise_min_dbm)
		g_radio_stats.noise_min_dbm = now;
	g_radio_stats.noise_avg_dbm = clamp_rssi(average);
}

int main() {
	stdio_init_all();
	sleep_ms(100);

	if (watchdog_caused_last_reboot()) {
		D_printf("Previous boot ended in a watchdog reset\r\n");
	}

	const ws2812_config_t led_config = {
		.pin = CONFIG_WS2812_PIN,
		.frequency_hz = 0,
		.order = WS2812_ORDER_GRB,
		.brightness = LED_BRIGHTNESS,
	};
	if (ws2812_init(&led_config)) {
		ws2812_set(0, 255, 0);
		ws2812_set_brightness(1);
	} else {
		D_printf("[LED] ws2812_init failed\r\n");
	}

	if (!uart_init_all())
		return 1;

	if (!radio_init_all())
		return 1;
	D_printf("Configuration complete!\r\n\r\n");

	queue_init(&g_tx_queue, sizeof(radio_frame_t), TX_QUEUE_DEPTH);
	queue_init(&g_rx_queue, sizeof(radio_frame_t), RX_QUEUE_DEPTH);

	flash_core_init();

	// saved network params (set over MERL) win over the build-time defaults
	if (flash_network_load(&network)) {
		D_printf("Network params loaded from flash\r\n");
	}
	erl_handler_network_init(&network);
	merl_handler_init(queue_user_message);
	watchdog_init(WATCHDOG_TIMEOUT_MS);
	multicore_launch_core1(core1_entry);

	radio_listen();

	while (1) {
		radio_service_rx();
		radio_service_tx();
		radio_sample_noise();
		watchdog_mark_alive();
		watchdog_service();
		hal->delay(LOOP_DELAY_MS);
	}

	return 0;
}
