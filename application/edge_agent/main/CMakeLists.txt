set(main_requires
    app_claw
    app_config
    captive_dns
    cmd_wifi
    http_server
    wifi_manager
    display_arbiter
    esp_painter
    sdcard_mount
    app_sd_mirror
    claw_event_router
    driver
    esp_lcd
    esp_app_format
    freertos
    spi_bus_arbiter
    settings
)

if(CONFIG_APP_CLAW_CAP_IM_WECHAT)
    list(APPEND main_requires cap_im_wechat)
endif()

if(CONFIG_APP_CLAW_CAP_MESHTASTIC)
    list(APPEND main_requires cap_meshtastic)
endif()

idf_component_register(
    SRC_DIRS
        "."
    INCLUDE_DIRS
        "."
)
