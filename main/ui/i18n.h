#pragma once
#include <stdint.h>

// ---------------------------------------------------------------------------
// Translation keys -- add new strings at the end (before TR_COUNT)
// ---------------------------------------------------------------------------
enum TrKey : uint8_t {
    // Timing screen -- data card titles
    TR_SPEED = 0,
    TR_LAPTIME,
    TR_BESTLAP,
    TR_LIVE_DELTA,
    TR_LAP,
    TR_SECTOR1,
    TR_SECTOR2,
    TR_SECTOR3,
    TR_RPM,
    TR_THROTTLE,
    TR_BOOST,
    TR_LAMBDA,
    TR_BRAKE,
    TR_COOLANT,
    TR_GEAR,
    TR_STEERING,
    // Timing screen -- header buttons
    TR_MENU_BTN,
    TR_LAYOUT_BTN,
    TR_CUSTOMIZE_LAYOUT,
    TR_START_BTN,
    TR_STOP_BTN,
    TR_SAVE_BTN,
    TR_CANCEL_BTN,
    // Layout editor -- widget checkbox names
    TR_WNAME_SPEED,
    TR_WNAME_LAPTIME,
    TR_WNAME_BESTLAP,
    TR_WNAME_DELTA,
    TR_WNAME_LAPNR,
    TR_WNAME_SEC1,
    TR_WNAME_SEC2,
    TR_WNAME_SEC3,
    TR_WNAME_RPM,
    TR_WNAME_THROTTLE,
    TR_WNAME_BOOST,
    TR_WNAME_LAMBDA,
    TR_WNAME_BRAKE,
    TR_WNAME_COOLANT,
    TR_WNAME_GEAR,
    TR_WNAME_STEERING,
    TR_WNAME_MAP,
    // Main menu tiles
    TR_TILE_TIMING,
    TR_TILE_TIMING_SUB,
    TR_TILE_TRACKS,
    TR_TILE_TRACKS_SUB,
    TR_TILE_HISTORY,
    TR_TILE_HISTORY_SUB,
    TR_TILE_SETTINGS,
    TR_TILE_SETTINGS_SUB,
    // Track selection screen
    TR_SELECT_TRACK,
    TR_NEW_TRACK,
    TR_NO_GPS_HINT,
    TR_NO_TRACK,
    // Track creator
    TR_CIRCUIT_BTN,
    TR_AB_BTN,
    TR_SEC_NAME,
    TR_SEC_TRACKTYPE,
    TR_SEC_SF,
    TR_SEC_FINISH,
    TR_SEC_SECTORS,
    TR_ADD_SECTOR,
    TR_SAVE_START_TIMING,
    TR_NAME_HINT,
    // History screen
    TR_HISTORY_TITLE,
    TR_NO_LAPS,
    TR_SET_REF,
    TR_IS_REF,
    // Settings screen
    TR_SETTINGS_TITLE,
    TR_OBD_SUB,
    TR_NOT_CONNECTED,
    TR_CONNECTED,
    TR_CONNECT_BTN,
    TR_SCANNING,
    TR_DISCONNECT_BTN,
    TR_WIFI_AP_TITLE,
    TR_WIFI_AP_SUB,
    TR_WIFI_AP_OFF,
    TR_WIFI_AP_ON,
    TR_WIFI_STA_TITLE,
    TR_WIFI_STA_SUB,
    TR_CONFIGURE_BTN,
    TR_LANGUAGE_LABEL,
    TR_UNITS_LABEL,
    // WiFi credential dialog
    TR_WIFI_DLG_TITLE,
    TR_SSID_LABEL,
    TR_PASS_LABEL,
    TR_CONNECT_DLG,
    TR_CANCEL_DLG,
    // User-created track
    TR_CUSTOM_COUNTRY,
    // Track filter
    TR_ALL_COUNTRIES,
    // Storage / Disk
    TR_STORAGE_TITLE,
    TR_STORAGE_UNAVAIL,
    TR_STORAGE_USED,
    // History screen sections
    TR_HIST_CURRENT,
    TR_HIST_SAVED,
    TR_HIST_NO_SAVED,
    TR_HIST_BEST,
    TR_HIST_LAPS,
    // History screen -- filter & delete
    TR_HIST_ALL_TRACKS,
    TR_HIST_DELETE_CONFIRM,
    // Car profile manager
    TR_CAR_PROFILES,
    TR_CAR_PROFILES_SUB,
    TR_CAR_LOADING,
    TR_CAR_DOWNLOAD,
    TR_CAR_ACTIVE,
    TR_CAR_ON_DEVICE,
    TR_CAR_ACTIVATE,
    TR_CAR_NO_WIFI,
    TR_CAR_DOWNLOAD_OK,
    TR_CAR_DOWNLOAD_FAIL,
    TR_CAR_ALL_MAKES,
    // Vehicle connection mode
    TR_VEH_CONN_TITLE,
    TR_VEH_CONN_SUB,
    TR_VEH_OBD_BLE,
    TR_VEH_CAN_DIRECT,
    TR_VEH_CAN_NO_PROFILE,
    // GPS info screen
    TR_GPS_INFO,
    TR_GPS_INFO_SUB,
    TR_GPS_STATUS,
    TR_GPS_NO_FIX,
    TR_GPS_FIX_OK,
    TR_GPS_SATS,
    TR_GPS_HDOP,
    TR_GPS_COORDS,
    TR_GPS_ALTITUDE,
    TR_GPS_SPEED,
    TR_GPS_HEADING,
    TR_GPS_TIME,
    TR_GPS_PPS,
    // WiFi scan
    TR_WIFI_SCAN,
    TR_WIFI_SCANNING,
    TR_WIFI_NO_NETWORKS,
    TR_WIFI_MANUAL,
    // Timing screen -- field picker & dialogs
    TR_PICK_FIELD,
    TR_SESSION_NAME,
    TR_AUTO_START,
    TR_USE_DEFAULT,
    TR_DELTA_SCALE,
    // OBD -- intake air temp
    TR_INTAKE,
    TR_COUNT
};

// Set active language (0 = Deutsch, 1 = English). Call on startup and on toggle.
void     i18n_set_language(uint8_t lang);
uint8_t  i18n_get_language();

// Return translated string for key. Never returns nullptr.
const char* tr(TrKey key);
