idf_component_register(
    SRCS "https_ota_pack.c" "partition_utils.c"
    INCLUDE_DIRS "include"
    REQUIRES
        nvs_flash
        esp_netif
        esp_event
        esp_wifi
        esp_http_client
        esp_https_ota
        app_update
        esp_partition
        spi_flash
        driver
        mbedtls
        esp_hw_support
        esp_system
        soc
)
