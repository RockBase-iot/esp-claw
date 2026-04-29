idf_component_register(
    SRCS "spi_bus_arbiter.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos esp_common log
)
