# Collect all .c and .cpp source files from main/ and subdirectories
file(GLOB_RECURSE SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "." "ui" "data" "gps" "timing" "obd" "wifi" "storage" "assets"
    PRIV_REQUIRES nvs_flash driver esp_wifi esp_http_server esp_timer
                  esp_event esp_netif fatfs vfs json
)

# LVGL configuration
target_compile_options(${COMPONENT_LIB} PRIVATE -DLV_LVGL_H_INCLUDE_SIMPLE)
