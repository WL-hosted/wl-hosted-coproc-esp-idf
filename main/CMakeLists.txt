idf_component_register(
    SRCS
        "app_main.c"
        "device_info.c"
        "transport_usb.c"
        "user_passthrough.c"
        "wifi_backend.c"
    INCLUDE_DIRS
        "."
    PRIV_REQUIRES
        esp_event
        esp_netif
        esp_ringbuf
        esp_wifi
        nvs_flash
        cherry-embedded__cherryusb
)

# Platform-agnostic Coprocessor Core (plain CMake static library). Protocol
# and Common are nested submodules of coproc-core; tests stay disabled in the
# ESP-IDF build.
set(BUILD_TESTING OFF)
add_subdirectory(
    ${CMAKE_CURRENT_SOURCE_DIR}/../coproc-core
    ${CMAKE_BINARY_DIR}/coproc-core-build
    EXCLUDE_FROM_ALL
)
target_link_libraries(${COMPONENT_LIB} PUBLIC wlh_coproc_core)

# FreeRTOS OSAL from Common.
wlh_common_enable_freertos_osal()
target_link_libraries(${COMPONENT_LIB} PUBLIC wlh::freertos_osal)
