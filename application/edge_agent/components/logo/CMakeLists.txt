idf_component_register(
    SRCS "logo.c"
    INCLUDE_DIRS "include"
    REQUIRES
        espressif__esp_board_manager
        esp_lcd
        esp_timer
        nanosvg
        display_arbiter
        spi_bus_arbiter
)
