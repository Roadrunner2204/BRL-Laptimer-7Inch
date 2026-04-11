/**
 * gps.cpp — Tau1201 GNSS driver (ESP-IDF / ESP32-P4)
 *
 * Reads NMEA data over UART1, parses $GPRMC and $GPGGA sentences with
 * a self-contained character-by-character parser, and writes results
 * into g_state.gps.
 *
 * Replaces the Arduino HardwareSerial + TinyGPSPlus implementation.
 */

#include "gps.h"
#include "../compat.h"
#include "../data/lap_data.h"

#include "driver/uart.h"
#include "driver/gpio.h"

static const char *TAG = "gps";

// ---------------------------------------------------------------------------
// NMEA parser state
// ---------------------------------------------------------------------------
#define NMEA_MAX_LEN     120
#define NMEA_MAX_FIELDS  20

static char    s_nmea_buf[NMEA_MAX_LEN + 1];
static uint8_t s_nmea_idx      = 0;
static bool    s_nmea_in_sentence = false;

// Module-static date/time from the last valid RMC sentence
static GpsDateTime s_datetime = {};

// Update rate measurement (count GGA sentences per second)
static uint32_t s_fix_count = 0;       // fixes in current window
static uint32_t s_last_rate_ms = 0;    // millis at last rate calculation
static uint8_t  s_update_rate = 0;     // measured Hz

// ---------------------------------------------------------------------------
// UART read buffer
// ---------------------------------------------------------------------------
#define GPS_RX_BUF_SIZE  1024
static uint8_t s_uart_buf[GPS_RX_BUF_SIZE];

// ---------------------------------------------------------------------------
// Helper: split an NMEA sentence by commas into field pointers
//   Returns number of fields.  Fields point into `sentence` (modified in-place).
// ---------------------------------------------------------------------------
static int nmea_split_fields(char *sentence, char *fields[], int max_fields) {
    int count = 0;
    fields[count++] = sentence;
    for (char *p = sentence; *p && count < max_fields; p++) {
        if (*p == ',') {
            *p = '\0';
            fields[count++] = p + 1;
        }
    }
    return count;
}

// ---------------------------------------------------------------------------
// Helper: NMEA checksum verification
//   sentence points to the full "$....*XX\r\n" string (null-terminated)
//   Returns true if checksum matches or if no checksum is present.
// ---------------------------------------------------------------------------
static bool nmea_verify_checksum(const char *sentence) {
    if (sentence[0] != '$') return false;

    const char *star = nullptr;
    uint8_t calc_cksum = 0;

    for (const char *p = sentence + 1; *p; p++) {
        if (*p == '*') {
            star = p;
            break;
        }
        calc_cksum ^= (uint8_t)*p;
    }

    if (!star) return true;  // no checksum field — accept anyway

    // Parse the two hex digits after '*'
    unsigned int recv_cksum = 0;
    if (sscanf(star + 1, "%02x", &recv_cksum) != 1 &&
        sscanf(star + 1, "%02X", &recv_cksum) != 1) {
        return false;
    }

    return (uint8_t)recv_cksum == calc_cksum;
}

// ---------------------------------------------------------------------------
// Helper: parse NMEA latitude  (ddmm.mmmmm,N/S)
// ---------------------------------------------------------------------------
static double nmea_parse_lat(const char *field, const char *ns) {
    if (!field[0] || !ns[0]) return 0.0;
    // Format: ddmm.mmmmm
    double raw = strtod(field, nullptr);
    int    deg = (int)(raw / 100.0);
    double min = raw - deg * 100.0;
    double lat = deg + min / 60.0;
    if (ns[0] == 'S' || ns[0] == 's') lat = -lat;
    return lat;
}

// ---------------------------------------------------------------------------
// Helper: parse NMEA longitude (dddmm.mmmmm,E/W)
// ---------------------------------------------------------------------------
static double nmea_parse_lon(const char *field, const char *ew) {
    if (!field[0] || !ew[0]) return 0.0;
    double raw = strtod(field, nullptr);
    int    deg = (int)(raw / 100.0);
    double min = raw - deg * 100.0;
    double lon = deg + min / 60.0;
    if (ew[0] == 'W' || ew[0] == 'w') lon = -lon;
    return lon;
}

// ---------------------------------------------------------------------------
// Parse $GPGGA (or $GNGGA)
//   $GPGGA,hhmmss.ss,ddmm.mmmmm,N,dddmm.mmmmm,E,q,nn,h.h,a.a,M,g.g,M,,*cs
//   fields: 0=id, 1=time, 2=lat, 3=N/S, 4=lon, 5=E/W, 6=quality,
//           7=sats, 8=hdop, 9=alt, 10=alt_unit, ...
// ---------------------------------------------------------------------------
static void parse_gga(char *fields[], int nfields) {
    if (nfields < 10) return;

    GpsData &g = g_state.gps;

    // Fix quality (0 = no fix)
    int quality = atoi(fields[6]);
    if (quality == 0) {
        g.valid = false;
        return;
    }

    g.lat        = nmea_parse_lat(fields[2], fields[3]);
    g.lon        = nmea_parse_lon(fields[4], fields[5]);
    g.satellites = (uint8_t)atoi(fields[7]);
    g.hdop       = (float)strtod(fields[8], nullptr);
    g.altitude_m = (float)strtod(fields[9], nullptr);

    // GGA alone marks a valid fix
    g.valid = true;

    // Update rate measurement
    s_fix_count++;
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    uint32_t elapsed = now - s_last_rate_ms;
    if (elapsed >= 1000) {
        s_update_rate = (uint8_t)((s_fix_count * 1000) / elapsed);
        s_fix_count = 0;
        s_last_rate_ms = now;
    }
}

// ---------------------------------------------------------------------------
// Parse $GPRMC (or $GNRMC)
//   $GPRMC,hhmmss.ss,A,ddmm.mmmmm,N,dddmm.mmmmm,E,s.s,h.h,ddmmyy,d.d,E,m*cs
//   fields: 0=id, 1=time, 2=status(A/V), 3=lat, 4=N/S, 5=lon, 6=E/W,
//           7=speed_knots, 8=heading, 9=date, ...
// ---------------------------------------------------------------------------
static void parse_rmc(char *fields[], int nfields) {
    if (nfields < 10) return;

    GpsData &g = g_state.gps;

    bool status_valid = (fields[2][0] == 'A');
    g.valid = status_valid;

    if (status_valid) {
        g.lat         = nmea_parse_lat(fields[3], fields[4]);
        g.lon         = nmea_parse_lon(fields[5], fields[6]);

        // Speed: knots -> km/h  (1 knot = 1.852 km/h)
        if (fields[7][0]) {
            g.speed_kmh = (float)(strtod(fields[7], nullptr) * 1.852);
        }

        // Heading / course over ground
        if (fields[8][0]) {
            g.heading_deg = (float)strtod(fields[8], nullptr);
        }
    }

    // Parse time: hhmmss.ss
    if (fields[1][0]) {
        int raw_time = atoi(fields[1]);  // truncates fractional part
        s_datetime.hour   = (uint8_t)(raw_time / 10000);
        s_datetime.minute = (uint8_t)((raw_time / 100) % 100);
        s_datetime.second = (uint8_t)(raw_time % 100);
    }

    // Parse date: ddmmyy
    if (fields[9][0]) {
        int raw_date = atoi(fields[9]);
        s_datetime.day   = (uint8_t)(raw_date / 10000);
        s_datetime.month = (uint8_t)((raw_date / 100) % 100);
        uint16_t yy      = (uint16_t)(raw_date % 100);
        s_datetime.year  = 2000 + yy;
    }

    s_datetime.valid = status_valid;
}

// ---------------------------------------------------------------------------
// Process one complete NMEA sentence
// ---------------------------------------------------------------------------
static void nmea_process_sentence(char *sentence) {
    if (!nmea_verify_checksum(sentence)) {
        return;
    }

    // Strip checksum part so it doesn't pollute the last field
    char *star = strchr(sentence, '*');
    if (star) *star = '\0';

    char *fields[NMEA_MAX_FIELDS];
    int nfields = nmea_split_fields(sentence + 1, fields, NMEA_MAX_FIELDS);
    // fields[0] now points to "GPGGA", "GNRMC", etc.

    if (nfields < 2) return;

    const char *id = fields[0];

    // Match sentence types (support GP, GN, GL, GA prefixes)
    if (strlen(id) >= 5) {
        const char *type = id + 2;  // skip "GP", "GN", etc.
        if (strcmp(type, "GGA") == 0) {
            parse_gga(fields, nfields);
        } else if (strcmp(type, "RMC") == 0) {
            parse_rmc(fields, nfields);
        }
    }
}

// ---------------------------------------------------------------------------
// Feed one byte to the NMEA parser
// ---------------------------------------------------------------------------
static void nmea_feed_char(char c) {
    if (c == '$') {
        // Start of a new sentence
        s_nmea_idx = 0;
        s_nmea_buf[s_nmea_idx++] = c;
        s_nmea_in_sentence = true;
        return;
    }

    if (!s_nmea_in_sentence) return;

    if (c == '\r' || c == '\n') {
        // End of sentence
        s_nmea_buf[s_nmea_idx] = '\0';
        if (s_nmea_idx > 6) {
            nmea_process_sentence(s_nmea_buf);
        }
        s_nmea_in_sentence = false;
        s_nmea_idx = 0;
        return;
    }

    if (s_nmea_idx < NMEA_MAX_LEN) {
        s_nmea_buf[s_nmea_idx++] = c;
    } else {
        // Sentence too long — discard
        s_nmea_in_sentence = false;
        s_nmea_idx = 0;
    }
}

// ---------------------------------------------------------------------------
// UBX protocol helpers for TAU1201 configuration
// ---------------------------------------------------------------------------

// Compute UBX checksum (CK_A, CK_B) over payload starting after sync bytes
static void ubx_checksum(const uint8_t *msg, size_t len, uint8_t *ck_a, uint8_t *ck_b)
{
    *ck_a = 0;
    *ck_b = 0;
    // Checksum covers class, id, length, and payload (skip sync bytes 0xB5,0x62)
    for (size_t i = 2; i < len; i++) {
        *ck_a += msg[i];
        *ck_b += *ck_a;
    }
}

// Send a UBX command and wait briefly for it to be transmitted
static void ubx_send(const uint8_t *msg, size_t len)
{
    uart_write_bytes(GPS_UART_PORT, msg, len);
    uart_wait_tx_done(GPS_UART_PORT, pdMS_TO_TICKS(100));
}

// Build and send a UBX-CFG-VALSET command (RAM layer) for a single U1/U2/U4 key
static void ubx_cfg_valset_u1(uint32_t key, uint8_t value)
{
    // UBX-CFG-VALSET: class=0x06, id=0x8A
    // Payload: version(1) + layers(1) + reserved(2) + cfgData(key:4 + val:1)
    uint8_t msg[9 + 6 + 2]; // header(6) + payload(9) + checksum(2) = 17
    msg[0] = 0xB5; msg[1] = 0x62;  // sync
    msg[2] = 0x06; msg[3] = 0x8A;  // class, id
    msg[4] = 9;    msg[5] = 0;     // length = 9 (little-endian)
    msg[6] = 0x00;                 // version
    msg[7] = 0x01;                 // layers: RAM only
    msg[8] = 0x00; msg[9] = 0x00;  // reserved
    // Key (little-endian)
    msg[10] = (key >>  0) & 0xFF;
    msg[11] = (key >>  8) & 0xFF;
    msg[12] = (key >> 16) & 0xFF;
    msg[13] = (key >> 24) & 0xFF;
    // Value
    msg[14] = value;
    // Checksum
    uint8_t ck_a, ck_b;
    ubx_checksum(msg, 15, &ck_a, &ck_b);
    msg[15] = ck_a;
    msg[16] = ck_b;
    ubx_send(msg, 17);
}

static void ubx_cfg_valset_u2(uint32_t key, uint16_t value)
{
    uint8_t msg[10 + 6 + 2]; // 18 bytes
    msg[0] = 0xB5; msg[1] = 0x62;
    msg[2] = 0x06; msg[3] = 0x8A;
    msg[4] = 10;   msg[5] = 0;     // length = 10
    msg[6] = 0x00; msg[7] = 0x01;
    msg[8] = 0x00; msg[9] = 0x00;
    msg[10] = (key >>  0) & 0xFF;
    msg[11] = (key >>  8) & 0xFF;
    msg[12] = (key >> 16) & 0xFF;
    msg[13] = (key >> 24) & 0xFF;
    msg[14] = (value >> 0) & 0xFF;
    msg[15] = (value >> 8) & 0xFF;
    uint8_t ck_a, ck_b;
    ubx_checksum(msg, 16, &ck_a, &ck_b);
    msg[16] = ck_a;
    msg[17] = ck_b;
    ubx_send(msg, 18);
}

// Configure TAU1201 for motorsport use:
//   - 10 Hz navigation rate
//   - GPS + Galileo + BeiDou + GLONASS enabled
//   - Automotive dynamic model
static void gps_configure_tau1201(void)
{
    // Wait for module to be ready after power-on
    vTaskDelay(pdMS_TO_TICKS(500));

    // ── Navigation rate: 10 Hz (100ms measurement period) ──
    // CFG-RATE-MEAS = 0x30210001 (U2, ms)
    ubx_cfg_valset_u2(0x30210001, 100);
    vTaskDelay(pdMS_TO_TICKS(50));

    // CFG-RATE-NAV = 0x30210002 (U2, cycles per nav solution)
    ubx_cfg_valset_u2(0x30210002, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    // ── Dynamic platform model: Automotive ──
    // CFG-NAVSPG-DYNMODEL = 0x20110021 (U1, 4=automotive)
    ubx_cfg_valset_u1(0x20110021, 4);
    vTaskDelay(pdMS_TO_TICKS(50));

    // ── Enable GNSS constellations ──
    // CFG-SIGNAL-GPS_ENA = 0x1031001F (L, GPS L1C/A)
    ubx_cfg_valset_u1(0x1031001F, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    // CFG-SIGNAL-GPS_L5_ENA = 0x10310004 (L, GPS L5)  -- only on supported modules
    ubx_cfg_valset_u1(0x10310004, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    // CFG-SIGNAL-GAL_ENA = 0x10310021 (L, Galileo)
    ubx_cfg_valset_u1(0x10310021, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    // CFG-SIGNAL-GAL_E5A_ENA = 0x10310025 (L, Galileo E5a)
    ubx_cfg_valset_u1(0x10310025, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    // CFG-SIGNAL-BDS_ENA = 0x10310022 (L, BeiDou)
    ubx_cfg_valset_u1(0x10310022, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    // CFG-SIGNAL-BDS_B2A_ENA = 0x10310028 (L, BeiDou B2a)
    ubx_cfg_valset_u1(0x10310028, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    // CFG-SIGNAL-GLO_ENA = 0x10310025 -- GLONASS (same key collision with GAL_E5A on some FW)
    // TAU1201 supports GPS+Galileo+BeiDou on L1+L5 natively
    // GLONASS only on L1 — enable if available
    // CFG-SIGNAL-GLO_ENA = 0x10310020
    ubx_cfg_valset_u1(0x10310020, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    log_i("TAU1201 configured: 10 Hz, automotive mode, all constellations + L5");
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void gps_init() {
    // Configure UART parameters
    uart_config_t uart_config = {};
    uart_config.baud_rate  = GPS_BAUD;
    uart_config.data_bits  = UART_DATA_8_BITS;
    uart_config.parity     = UART_PARITY_DISABLE;
    uart_config.stop_bits  = UART_STOP_BITS_1;
    uart_config.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    uart_config.source_clk = UART_SCLK_DEFAULT;

    ESP_ERROR_CHECK(uart_param_config(GPS_UART_PORT, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(GPS_UART_PORT, GPS_TX_PIN, GPS_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(GPS_UART_PORT, GPS_RX_BUF_SIZE * 2,
                                        0, 0, nullptr, 0));

    log_i("Tau1201 UART%d started: RX=GPIO%d TX=GPIO%d @%d baud",
          GPS_UART_PORT, GPS_RX_PIN, GPS_TX_PIN, GPS_BAUD);

    // Configure PPS pin as input (for future use)
    gpio_config_t pps_cfg = {};
    pps_cfg.pin_bit_mask = (1ULL << GPS_PPS_PIN);
    pps_cfg.mode         = GPIO_MODE_INPUT;
    pps_cfg.pull_up_en   = GPIO_PULLUP_DISABLE;
    pps_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    pps_cfg.intr_type    = GPIO_INTR_DISABLE;
    gpio_config(&pps_cfg);

    log_i("PPS pin: GPIO%d", GPS_PPS_PIN);

    // Clear parser state
    memset(&s_datetime, 0, sizeof(s_datetime));
    s_nmea_idx = 0;
    s_nmea_in_sentence = false;

    // Configure TAU1201 via UBX protocol (10 Hz, automotive, all GNSS)
    gps_configure_tau1201();
}

void gps_poll() {
    // Read all available bytes from UART — non-blocking
    int len = uart_read_bytes(GPS_UART_PORT, s_uart_buf, sizeof(s_uart_buf),
                              0);  // timeout = 0 ticks → non-blocking
    if (len <= 0) return;

    for (int i = 0; i < len; i++) {
        nmea_feed_char((char)s_uart_buf[i]);
    }
}

GpsDateTime gps_get_datetime() {
    return s_datetime;
}

uint8_t gps_get_update_rate() {
    return s_update_rate;
}
