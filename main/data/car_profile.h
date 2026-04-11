#pragma once
#include <stdint.h>
#include <stdbool.h>

/**
 * car_profile -- BRL vehicle profile parser & manager
 *
 * Reads encrypted .brl files from SD card (/cars/),
 * decrypts AES-256-CBC payload, and parses CAN sensor definitions.
 */

#ifdef __cplusplus
extern "C" {
#endif

#define CAR_MAX_SENSORS   80
#define CAR_NAME_LEN      32
#define CAR_ENGINE_LEN    16
#define CAR_MODEL_LEN     24

// Single CAN sensor definition (parsed from .brl JSON)
typedef struct {
    char     name[CAR_NAME_LEN]; // e.g. "RPM", "WaterT", "Boost"
    uint32_t can_id;             // CAN message ID (e.g. 0x00A5)
    uint8_t  proto;              // 0=standard CAN, 2=UDS/diagnostic
    uint8_t  start;              // start byte in CAN frame
    uint8_t  len;                // data length in bytes (1 or 2)
    uint8_t  is_unsigned;        // 1=unsigned, 0=signed
    float    scale;              // raw * scale + offset = value
    float    offset;
    float    min_val;
    float    max_val;
    uint8_t  type;               // 0=generic, 1=pressure, 2=temp, 3=speed, 4=lambda
    uint8_t  slot;               // original slot index from TRX
} CarSensor;

// Vehicle profile (one loaded .brl file)
typedef struct {
    char       engine[CAR_ENGINE_LEN];   // e.g. "N47"
    char       name[CAR_NAME_LEN];       // e.g. "F-Series"
    char       make[CAR_NAME_LEN];       // e.g. "BMW"
    char       model[CAR_MODEL_LEN];     // e.g. "3er"
    uint16_t   year_from;
    uint16_t   year_to;
    char       can_bus[16];              // e.g. "PT-CAN"
    uint16_t   bitrate;                  // kBit/s (e.g. 500)
    CarSensor  sensors[CAR_MAX_SENSORS];
    int        sensor_count;
    bool       loaded;
} CarProfile;

extern CarProfile g_car_profile;

// Load a .brl profile from SD card. Returns true on success.
bool car_profile_load(const char *filename);

// List available .brl profiles on SD. Returns count.
// filenames[i] will contain just the filename (e.g. "N47F.brl")
int  car_profile_list(char filenames[][CAR_NAME_LEN], int max_count);

// Get the currently active profile filename from NVS (empty if none)
void car_profile_get_active(char *buf, int buf_len);

// Set and persist the active profile filename to NVS
void car_profile_set_active(const char *filename);

// Find a sensor by name. Returns pointer or NULL.
const CarSensor *car_profile_find_sensor(const char *name);

// Server profile entry (parsed from list.txt: "filename;make;display_name")
typedef struct {
    char filename[CAR_NAME_LEN];    // e.g. "N47F.brl"
    char make[CAR_NAME_LEN];        // e.g. "BMW"
    char display[CAR_NAME_LEN];     // e.g. "N47 F-Series"
} CarProfileEntry;

// Download profile list from server. Returns count of entries.
int  car_profile_fetch_list(CarProfileEntry *entries, int max_count);

// Download a .brl file from server and save to SD. Returns true on success.
bool car_profile_download(const char *filename);

// Delete a .brl file from SD.
bool car_profile_delete(const char *filename);

#ifdef __cplusplus
}
#endif
