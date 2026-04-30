/*
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
#ifndef HTTPS_OTA_PACK_H
#define HTTPS_OTA_PACK_H

#include <stdbool.h>
#include "esp_err.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief OTA Configuration Structure
 *
 * This structure encapsulates all the parameters needed for HTTPS OTA updates
 * across different ESP32 variants. It simplifies OTA integration by providing
 * a unified interface for WiFi configuration and OTA parameters.
 *
 * Pointer fields (ssid, password, url, custom_cert_pem) must remain valid for the
 * duration of https_ota_perform_update() unless otherwise documented.
 */
typedef struct {
    /* WiFi Configuration */
    const char *wifi_ssid;              /**< WiFi SSID */
    const char *wifi_password;          /**< WiFi password */

    /* OTA Configuration */
    const char *ota_url;                /**< OTA image URL (must be HTTPS) */
    bool use_cert_bundle;               /**< Use built-in certificate bundle (recommended for all ESP32) */
    const char *custom_cert_pem;        /**< Custom certificate PEM string (if use_cert_bundle=false, required) */

    /* Advanced Configuration (Optional) */
    bool skip_cert_common_name_check;   /**< Skip certificate common name check (default: false) */
    bool keep_alive_enable;             /**< Enable HTTP keep-alive (default: true) */

#ifdef CONFIG_EXAMPLE_FIRMWARE_UPGRADE_BIND_IF
    const char *bind_interface_name;    /**< Bind to specific network interface (e.g., "sta", "eth", "thread") */
#endif
} https_ota_config_t;

/**
 * @brief Initialize HTTPS OTA System
 *
 * This function initializes the NVS, network interfaces, and event loops required for OTA.
 * Must be called before any OTA operations.
 *
 * @return
 *    - ESP_OK: Initialization successful
 *    - ESP_FAIL: Initialization failed
 */
esp_err_t https_ota_init(void);

/**
 * @brief Perform HTTPS OTA Update
 *
 * This is the main OTA function that handles all types of updates:
 * - Application updates
 * - Bootloader updates (with recovery support if available)
 * - Partition table updates
 * - Storage/Data partition updates
 *
 * The update type is automatically detected from the URL filename.
 *
 * @param[in] config Pointer to https_ota_config_t structure with WiFi and OTA settings
 *
 * @return
 *    - ESP_OK: OTA update completed successfully, device will reboot
 *    - ESP_FAIL: OTA update failed
 *    - ESP_ERR_INVALID_ARG: Invalid arguments provided
 *    - ESP_ERR_NOT_SUPPORTED: OTA type not supported or partition not found
 */
esp_err_t https_ota_perform_update(const https_ota_config_t *config);

/**
 * @brief WiFi Connection Callback Type
 *
 * Callback function type for WiFi connection status updates
 */
typedef void (*wifi_event_callback_t)(bool connected);

/**
 * @brief Register WiFi Event Callback
 *
 * Register a callback function to be notified when WiFi connection status changes.
 *
 * @param[in] callback Function pointer to call on WiFi status change
 *
 * @return
 *    - ESP_OK: Callback registered successfully
 *    - ESP_FAIL: Failed to register callback
 */
esp_err_t https_ota_register_wifi_callback(wifi_event_callback_t callback);

/**
 * @brief Utility Function: Create Default Config
 *
 * Creates a default OTA configuration with recommended settings.
 * Use this as a starting point and modify fields as needed.
 *
 * @param[out] config Pointer to https_ota_config_t to fill with defaults
 * @param[in] ssid WiFi SSID
 * @param[in] password WiFi password
 * @param[in] url OTA URL
 *
 * @return
 *    - ESP_OK: Config created successfully
 *    - ESP_ERR_INVALID_ARG: Invalid arguments (NULL pointers)
 */
esp_err_t https_ota_create_default_config(https_ota_config_t *config,
                                          const char *ssid,
                                          const char *password,
                                          const char *url);

#ifdef __cplusplus
}
#endif

#endif /* HTTPS_OTA_PACK_H */
