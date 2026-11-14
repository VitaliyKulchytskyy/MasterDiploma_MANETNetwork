# Emergency Radio Link

---

## Архітектура проєкту

Пристрій складається з двох частин: головний блок (Luckfox Lyra + WiFi USB adapter) та   
блок екстреного зв'язку (RP2040 + Ra-01 (LoRa TX/RX) + GPS). Ці блоки спілкуються через CONFIG_UART_MAIN_HW.

Задача головного блоку:  
- Об'єднувати пристрої в одній WiFi мережі
- Інформувати користувача про інших клієнтів мережі
- Реалізовувати інтерфейс спілкування з блоком, який реалізовує екстрений зв'язок

Задача блоку екстреного зв'язку:
- Реалізовувати передачу даних через протокол ERL
- Передавати телеметрію у вказаному інтервалі, поки є живлення
- Реалізовувати інтерфейс спілкування з головним блоком

---

## UART

Драйвер UART (`include/uart.h`, `src/uart.c`) абстрагує два логічні порти:
- `UART_MAIN` — зв'язок з головним блоком за протоколом MERL;
- `UART_GPS` — приймач GPS-модуля.

Апаратура, піни та швидкість кожного порту задаються через `CONFIG_UART_<NAME>_*` у
`CMakePresets.json`. Для кожного порту окремо вмикається DMA на прийом (`rx_use_dma`)
і/або передачу (`tx_use_dma`): `UART_MAIN` використовує DMA в обидва боки, `UART_GPS` —
лише на прийом (передача NMEA одностороння, TX не потрібен). Без DMA прийом реалізовано
через переривання; з DMA — через кільцевий буфер, який DMA-канал заповнює самостійно, без
участі CPU (переривання RX для цього порту при цьому не використовується).

---

## GPS та позиціювання

Позиція пристрою — з фікса GPS-модуля через `UART_GPS`, або встановлена вручну через
`MERL_CMD_POSITION_SET` — зберігається в `gps_handler` (`include/gps_handler.h`) у форматі:
- `longitude`/`latitude` — градуси * 1e6 (мікроградуси); захід і південь — від'ємні значення;
- `time` — мілісекунди з півночі UTC за останнім фіксом, або 0 для позиції, встановленої вручну.

NMEA-парсер (`lib/ublox_gps`) читає готові речення (`$..GGA`, `$..RMC` тощо) з `UART_GPS`.
Час (`millis()`), потрібний бібліотеці, надається через слабкий (`weak`) символ
`ublox_millis()`: бібліотека має власну типову реалізацію, а прошивка (`main.cpp`)
перевизначає її своєю сильною версією.

Позиція розповсюджується мережею двома шляхами (див. нижче про `PACKET_TYPE_POSITION` та
`PACKET_TYPE_NETWORK`):
1. окремим пакетом `PACKET_TYPE_POSITION` кожні `POSITION_INTERVAL_MS` (15 с) — лише якщо
   позиція вже відома;
2. разом із heartbeat (`PACKET_TYPE_NETWORK`) кожні `HEARTBEAT_INTERVAL_MS` (5 с) — до нього
   додається остання відома позиція, якщо вона є.

Debug-вивід сирого NMEA-потоку та розпізнаних фіксів вмикається прапорцем
`CONFIG_DEBUG_GPS` (`CMakePresets.json`), незалежно від `DEBUG_MODE`.

---

## Таблиця сусідів (Peer table) та оцінка відстані

Кожен пристрій веде таблицю сусідів `g_peers_table[NETWORK_MAX_PEERS]` (32 записи — за
кількістю хостових адрес дефолтної підмережі з маскою 0xFFE0). Запис про сусіда не шукається
лінійним перебором: його індекс — це хостова частина адреси в межах власної підмережі
(`address & ~subnet_mask`), тому кожен сусід завжди потрапляє в один і той самий слот.

Формат запису (`peer_t`, `include/protocol/network.h`):

```c
typedef struct __attribute__((__packed__)) {
    uint32_t address;
    int8_t rssi;          // dBm останнього прийнятого пакета від сусіда — для оцінки відстані
    uint32_t uptime;       // секунд з моменту завантаження сусіда
    int32_t longitude;     // градуси * 1e6, захід — від'ємне
    int32_t latitude;      // градуси * 1e6, південь — від'ємне
    uint32_t config: 24;   // біти 0-15: номер останнього heartbeat сусіда; 16-23 зарезервовано
    int16_t compass_x;     // сирі відліки QMC5883 (не каліброване, не в градусах/гаусах)
    int16_t compass_y;
    int16_t compass_z;
} peer_t;
```

`rssi` оновлюється при прийомі будь-якого пакета від сусіда (`radio.getRSSI()` одразу
після прийому в `radio_service_rx()`) — за ним можна прикидати відстань до сусіда (сильніший
сигнал зазвичай означає меншу відстань, з поправкою на перешкоди, орієнтацію антени тощо;
точної формули переведення в метри наразі немає). Пристрій підтримує й власний запис у
таблиці, з `rssi = 0` (RSSI власної передачі не має сенсу) — тому одним запитом
`MERL_CMD_PEERS_GET` можна отримати повну картину мережі, включно з собою.

`compass_x/y/z` не мають окремого пакета: вони їдуть разом з позицією (`erl_compass_t`,
додається до `PACKET_TYPE_POSITION` одразу після координат, коли показання вже відоме) — тому
й оновлюються так само, як позиція, з отриманого пакета для сусіда, або з власного
`compass_handler_get()` для запису пристрою про самого себе (`rssi = 0`). Нульові значення
означають "показання ще не відоме", а не буквально нульове поле (0 — теж не рідкісне справжнє
значення для сирих відліків магнітометра).

---

## Збереження параметрів у Flash

Мережеві параметри пристрою (адреса та маска підмережі) зберігаються в останньому секторі
flash-пам'яті (`include/flash.h`, `src/flash.c`) як журнал із 16 записів по 256 байт, кожен
із CRC32 та номером послідовності. Це зменшує знос флеша (сектор стирається раз на 16
збережень) і захищає від втрати даних при збої живлення саме під час запису.

При отриманні `MERL_CMD_NETWORK_SET` пристрій спершу зберігає нові параметри у flash і лише
потім застосовує їх: якщо запис не вдався — повертається помилка, а робочі параметри
залишаються незмінними. Під час завантаження пристрій читає збережені параметри й використовує
їх замість дефолтних зі збірки (`CONFIG_ERL_NETWORK_ADDRESS` / `CONFIG_ERL_NETWORK_SUBNET`).

---

## Протокол: Emergency Radio Link (ERL)

```c++
// [* - sync] ['a' version - 1 byte] [sender addr - 2 bytes] [receiver addr - 2 bytes] [type - 1 byte] [len - 1 byte] [PAYLOAD - len bytes] [checksum - crc8]
typedef struct __attribute__((__packed__)) {
    uint8_t sync;
    uint8_t version;
    uint16_t address;
    uint16_t receiver;
    packet_type_t type;
    uint8_t length;
    uint8_t payload[];
    // the last byte - checksum crc8
} erl_packet_t;
```

```c++
typedef enum: uint8_t {
    PACKET_TYPE_RESPONSE = 0,
    PACKET_TYPE_COMMAND = 1,
    PACKET_TYPE_NETWORK = 2,
    PACKET_TYPE_POSITION = 3,
    PACKET_TYPE_USER_MESSAGE = 5,
} packet_type_t;
```

Приклади використання:

```
Send heartbeat to all in the subnetwork (at lest half in a minute)
subnet mask: 0xFFE0
[*][a][0xC701][0xC71F][0x00][0x00][checksum]

Send emergency to all in the subnetwork
subnet mask: 0xFFE0
[*][a][0xC701][0xC71F][0x00][0x01][0xFF][checksum]

Send coordinates to all in the subnetwork
subnet mask: 0xFFE0
[*][a][0xC70A][0xC71F][0x01][0x0B][time, longitude, latitude][checksum]

Send custom msg to all in the subnetwork
subnet mask: 0xFFE0
[*][a][0xC701][0xC71F][0x05][0x05]['H', 'i', '!', '\r', '\n'][checksum]
```

> **Примітка:** приклад heartbeat вище показує застарілий, порожній payload. Наразі
> `PACKET_TYPE_NETWORK` завжди несе `uptime` пристрою та `config` (біти 0-15 — порядковий номер heartbeat,
за пропусками в ньому отримувач рахує втрачені пакети; біти 16-23 зарезервовано), а якщо
> позиція вже відома — ще й `time`/`longitude`/`latitude` (`erl_network_info_t` та
> `erl_position_t` у `include/protocol/erl.h`). Позиція також розсилається окремим пакетом
> `PACKET_TYPE_POSITION` — деталі в розділі "GPS та позиціювання" вище. Показання компаса
> (`erl_compass_t`: `x`/`y`/`z`, сирі відліки QMC5883) окремого пакета не має — воно додається
> одразу після координат у той самий пакет `PACKET_TYPE_POSITION`, коли вже відоме, тож ходить
> по мережі з тією ж періодичністю, що й позиція (`POSITION_INTERVAL_MS`), і лише разом з нею.

---

## Протокол: MasterERL (MERL)

```c++
// [* - sync] ['a' version - 1 byte] ['<'/'>' - 1 byte] [type - 1 byte] [len - 1 byte] [PAYLOAD - len bytes] [checksum - crc8]
typedef struct __attribute__((__packed__)) {
    uint8_t sync;
    uint8_t version;
    uint8_t direction; // '<' (answer) or '>' (request)
    uint16_t command;
    uint8_t length;
    uint8_t payload[];
    // the last byte - checksum crc8
} merl_packet_t;
```

Головною відмінністю від ERL є обов'язкова відповідь від Slave пристрою (блок екстреного зв'язку).


Commands table
```
Responses:
0x0000 - ok to the response
payload: 2bytes of the command from the request

0x0001 - ok, with a response body 
payload: N-bytes (based on the request command)

0x0100 - error
payload: 2bytes of the command from the request

0x0101 - unknown request
payload: 2bytes of the command from the request

0x0102 - unimplemented request
payload: 2bytes of the command from the request

0x0110 - could not response due to overload
payload: 2bytes of the command from the request
------------------------------------------------
Network commands:
0x1000 - get network params from the device
payload: 0 bytes
response: 2 bytes addres, 2 bytes subnetwork mask

0x1001 - set network params from the device
payload: 2 bytes addres, 2 bytes subnetwork mask
response: 0x0000
------------------------------------------------
Peer commands:
0x2000 - download peer table from the device
payload: 0 bytes
response: peers table

0x2001 - clear the table (and the link statistics below)
payload: 0 bytes
response: 0x0000

0x2002 - radio statistics since boot (feature LINK_STATS)
payload: 0 bytes
response: 47 bytes, big-endian: uptime_ms, rx_ok, rx_crc_error, rx_bad_frame, rx_not_for_us,
          rx_queue_drop, tx_ok, tx_error, tx_airtime_ms, lbt_busy, tx_queue_drop (u32 each),
          noise_dbm, noise_min_dbm, noise_avg_dbm (i8 each; channel RSSI sampled every 250 ms
          while nothing is on the air, 0 = no sample yet). Counters only grow: take differences.

0x2003 - per-peer link statistics (feature LINK_STATS)
payload: 0 bytes
response: N * 32 bytes, the peers heard most recently first, big-endian:
          address (u16), rx_packets, hb_received, hb_missed (u32), rssi_dbm (i8), snr_q4 (i8, SNR in
          0.25 dB), freq_error_hz (i32), age_ms (u32), rssi_sum (i32), snr_q4_sum (i32) - last packet's
          values plus sums over rx_packets for exact means between two reads. hb_missed counts gaps in
          the peers' heartbeat sequence numbers (config bits 0-15 of PACKET_TYPE_NETWORK).
------------------------------------------------
Position commands:
0x3000 - get position from internal gps
payload: 0 bytes
response: time (4 bytes), longitude (4 bytes), latitude (4 bytes)

0x3001 - set position
payload: longitude (4 bytes), latitude (4 bytes)
response: 0x0000

- - - - - - - - - - - - - - - - - - - - - - - -
implemented compas modules: QMC5883
- - - - - - - - - - - - - - - - - - - - - - - -
0x3002 - get compas data
payload: 0 bytes
response: X (2 bytes), Y (2 bytes), Z (2 bytes)

0x3003 - set compas data
payload: X (2 bytes), Y (2 bytes), Z (2 bytes)
response: 0x0000

0x3004 - get gps receiver status (fix or not)
payload: 0 bytes
response: 5 bytes total -
  fix (1 byte, GGA fix quality: 0 - none, 1 - GPS, 2 - differential),
  satellites used in the fix (1 byte, GGA),
  satellites in view (1 byte, GSV),
  HDOP * 100 (2 bytes, big-endian, 0 - unknown)
  all zero until the receiver sends its first sentence;
  0x0102 (UNIMPLEMENTED) for a firmware built without FEATURE_GPS
------------------------------------------------
Device commands:
0x4000 - get firmware version and build info
payload: 0 bytes
response: 25 bytes total -
  major version (1 byte), minor version (1 byte, both from CONFIG_DEVICE_*_VERSION),
  build number (4 bytes, big-endian, auto-incremented once per build, see .build_number),
  build date (11 bytes, compiler __DATE__, e.g. "Sep 27 2026", not NUL-terminated),
  build time (8 bytes, compiler __TIME__, e.g. "14:23:01", not NUL-terminated)

0x4001 - get compiled-in features
payload: 0 bytes
response: N bytes, one per feature actually compiled into this firmware build (see
          CONFIG_DEVICE_FEATURES in CMakePresets.json), not a fixed bitmask - a feature
          absent from CONFIG_DEVICE_FEATURES is simply absent from this list
feature ids: 0x00 - GPS, 0x01 - COMPASS, 0x02 - LINK_STATS (0x2002 / 0x2003, always compiled in)
------------------------------------------------
User message (chat):
0x5000 - user message, both directions
payload: 4 bytes of address, N-bytes of the message (must ends up with \r\n, at most 128 bytes)
  main -> device: send the message over ERL; address = receiver (a peer or the subnet broadcast)
  device -> main: a message received over ERL; address = receiver << 16 | sender
                  (2 bytes receiver, 2 bytes sender: shows who wrote it and whether it was a broadcast)
response: 0x0000 (from the device, resp. from the main device)

0x5001 - passthrough from the ERL over MERL back (reserved, not used by the chat)
payload: N-bytes
response: 0x0000 (from the main device)
```
Examples:
```
Get peer table
[*][a][>][0x20, 0x00][0x00][checksum]
[*][a][<][0x00, 0x01][len][peer table][checksum]

Set network params (address and subnet mask)
[*][a][>][0x10, 0x01][0x04][0xCF, 0x01, 0xFF, 0xE0][checksum]
[*][a][<][0x00, 0x00][0x02][0x10, 0x01][checksum]

Get position (time, longitude, latitude; longitude/latitude signed, degrees * 1e6)
[*][a][>][0x30, 0x00][0x00][checksum]
[*][a][<][0x00, 0x01][0x0C][time, longitude, latitude][checksum]

Get firmware version and build info (major=1, minor=0, build #42, built Sep 27 2026 14:23:01)
[*][a][>][0x40, 0x00][0x00][checksum]
[*][a][<][0x00, 0x01][0x19][0x01, 0x00, 0x00, 0x00, 0x00, 0x2A, 'S','e','p',' ','2','7',' ','2','0','2','6', '1','4',':','2','3',':','0','1'][checksum]

Get compiled-in features (GPS is the only one compiled in)
[*][a][>][0x40, 0x01][0x00][checksum]
[*][a][<][0x00, 0x01][0x01][0x00][checksum]
```

---

## Збірка проєкту

Проєкт використовує C23 (`enum : uint8_t` з явним базовим типом) — потрібен GCC 13 або
новіше. Системний `arm-none-eabi-gcc` часто старіший (наприклад, 10.x): перед конфігурацією
CMake переконайтесь, що `PICO_TOOLCHAIN_PATH` вказує на сумісний тулчейн.

Команди (`justfile`):
- `just build` — конфігурація + збірка (Debug-пресет за замовчуванням);
- `just flash` — прошивка через `picotool` (плата має бути в режимі BOOTSEL, або передати
  `-f` для примусової прошивки без BOOTSEL);
- `just monitor` — серійний монітор (`tio`) для debug-виводу по USB CDC;
- `just bfm` — build + flash + monitor одним викликом.

---

## Інструменти для роботи з MERL

У `test/` є Python-утиліти для роботи з протоколом MERL по UART (потрібен пакет `pyserial`),
підключаються до пінів `UART_MAIN`, а не до debug-порту `/dev/ttyACM0`:
- `merl_link.py` — спільний рівень (формат кадру, CRC8, серійне з'єднання) для обох скриптів;
- `merl_test.py` — автоматизований набір тестів протоколу;
- `merl_cli.py` — інтерактивна CLI / one-shot утиліта (`network get/set`, `peers get/clear`,
  `position get/set`, `message send`, `listen`, `raw <command>`).

Приклади:
```
./test/merl_cli.py --port /dev/ttyUSB0 position set 30.523456 50.450100
./test/merl_cli.py --port /dev/ttyUSB0 peers get
./test/merl_test.py --port /dev/ttyUSB0
```