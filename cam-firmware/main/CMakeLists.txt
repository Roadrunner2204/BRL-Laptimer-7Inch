# Cam-firmware main component.
file(GLOB_RECURSE SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "." "cam_link" "recorder" "wifi_sta" "http_server" "capture"
    PRIV_REQUIRES nvs_flash driver esp_http_server esp_timer esp_event
                  fatfs vfs json esp_wifi esp_netif lwip
                  esp_driver_jpeg esp_driver_sdmmc
                  # esp_video / esp_cam_sensor / esp_hosted / esp_wifi_remote
                  # come in via idf_component.yml in this directory.
)
