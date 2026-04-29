set(main_requires
    app_claw
    app_config
    captive_dns
    http_server
    wifi_manager
    display_arbiter
    esp_painter
    sdcard_mount
    claw_event_router
    driver
    esp_lcd
    esp_app_format
    freertos
    spi_bus_arbiter
)

if(CONFIG_APP_CLAW_CAP_IM_WECHAT)
    list(APPEND main_requires cap_im_wechat)
endif()

idf_component_register(
    SRC_DIRS
        "."
    INCLUDE_DIRS
        "."
    REQUIRES
        ${main_requires}
)
