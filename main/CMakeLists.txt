set(wlh_sources
    "app/app_main.c"
    "backends/wifi_backend.c"
    "backends/ota_backend.c"
    "services/adc_service.c"
    "services/device_info.c"
    "services/io_service.c"
    "services/kv_service.c"
    "services/pin_profile.c"
    "services/user_passthrough.c"
)
set(wlh_private_requirements
    bt
    esp_adc
    esp_driver_gpio
    esp_event
    esp_hw_support
    esp_netif
    esp_wifi
    nvs_flash
    app_update
    mbedtls
)

if(CONFIG_WLH_ENABLE_BLUETOOTH_CONTROLLER)
    list(APPEND wlh_sources "backends/bluetooth_backend.c")
endif()

if(CONFIG_WLH_TRANSPORT_USB OR
   (NOT DEFINED CONFIG_WLH_TRANSPORT_USB AND IDF_TARGET STREQUAL "esp32s3"))
    list(APPEND wlh_sources "transports/usb/transport_usb.c")
    list(APPEND wlh_private_requirements
        esp_ringbuf
        cherry-embedded__cherryusb
    )
elseif(CONFIG_WLH_TRANSPORT_SDIO OR
       (NOT DEFINED CONFIG_WLH_TRANSPORT_SDIO AND
        (IDF_TARGET STREQUAL "esp32c6" OR IDF_TARGET STREQUAL "esp32c5")))
    list(APPEND wlh_sources "transports/sdio/transport_sdio.c")
    list(APPEND wlh_private_requirements esp_driver_sdio)
else()
    message(FATAL_ERROR "Select a supported WL-hosted transport")
endif()

idf_component_register(
    SRCS ${wlh_sources}
    PRIV_INCLUDE_DIRS
        "app"
        "backends"
        "services"
        "transports"
    PRIV_REQUIRES ${wlh_private_requirements}
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Werror)

# Platform-agnostic Coprocessor Core (plain CMake static library). Protocol
# and Common are nested submodules of coproc-core; tests stay disabled in the
# ESP-IDF build.
set(BUILD_TESTING OFF)
add_subdirectory(
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/coproc-core
    ${CMAKE_BINARY_DIR}/core-coproc-build
    EXCLUDE_FROM_ALL
)
target_link_libraries(${COMPONENT_LIB} PUBLIC wlh_coproc_core)

# FreeRTOS OSAL from Common.
wlh_common_enable_freertos_osal()
target_link_libraries(${COMPONENT_LIB} PUBLIC wlh::freertos_osal)

# ESP-IDF log backend for the core logging abstraction.
wlh_common_enable_log(BACKEND ESP)
target_link_libraries(${COMPONENT_LIB} PUBLIC wlh::log)

# The nested Core libraries are PIC by default for desktop consumers. ESP-IDF
# firmware is linked at fixed addresses, and RISC-V targets reject PIC GOT/PLT
# sections, so override the property only in this Adapter build.
foreach(wlh_embedded_target
        wlh_coproc_core
        wlh_protocol
        wlh_nanopb_runtime
        wlh_protocol_nanopb
        wlh_sim_nanopb
        wlh_freertos_osal
        wlh_log_backend)
    if(TARGET ${wlh_embedded_target})
        set_target_properties(
            ${wlh_embedded_target} PROPERTIES POSITION_INDEPENDENT_CODE OFF
        )
    endif()
endforeach()
