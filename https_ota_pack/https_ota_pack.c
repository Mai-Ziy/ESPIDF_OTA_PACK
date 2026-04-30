/*
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
#include "https_ota_pack.h"
#include "partition_utils.h"

#include <inttypes.h>
#include <string.h>
#include <net/if.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "soc/soc_caps.h"
#if SOC_RECOVERY_BOOTLOADER_SUPPORTED
#include "esp_efuse.h"
#endif
#include "esp_flash.h"
#include "esp_flash_partitions.h"
//#include "protocol_examples_common.h"

// #if CONFIG_EXAMPLE_USE_CERT_BUNDLE
#include "esp_crt_bundle.h"
// #endif

static const char *TAG = "https_ota_pack";

#define WIFI_CONNECTED_BIT BIT0

#ifdef CONFIG_EXAMPLE_WIFI_CONN_MAX_RETRY
#define HTTPS_OTA_WIFI_MAX_RETRY CONFIG_EXAMPLE_WIFI_CONN_MAX_RETRY
#else
#define HTTPS_OTA_WIFI_MAX_RETRY 6
#endif

static EventGroupHandle_t s_wifi_event_group;
static wifi_event_callback_t s_wifi_cb;
static bool s_pack_inited;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ERROR:
        ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
        break;
    case HTTP_EVENT_HEADER_SENT:
        ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
        break;
    case HTTP_EVENT_ON_HEADER:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
        break;
    case HTTP_EVENT_ON_DATA:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
        break;
    case HTTP_EVENT_REDIRECT:
        ESP_LOGD(TAG, "HTTP_EVENT_REDIRECT");
        break;
    }
    return ESP_OK;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_wifi_cb) {
            s_wifi_cb(false);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        if (s_wifi_cb) {
            s_wifi_cb(true);
        }
    }
}
//新增
#ifdef CONFIG_EXAMPLE_FIRMWARE_UPGRADE_BIND_IF
/**
 * 不依赖 protocol_examples_common，用 esp_netif 解析要绑定的接口。
 * bind_name 可以是：if_key（如 WIFI_STA_DEF）、简写 sta/eth、与 esp_netif_get_desc 一致的描述、
 * 或示例里常用的 example_netif_sta / example_netif_eth。
 */
static esp_netif_t *https_ota_resolve_bind_netif(const char *bind_name)
{
    if (bind_name == NULL || bind_name[0] == '\0') {
        return NULL;
    }

    esp_netif_t *n = esp_netif_get_handle_from_ifkey(bind_name);
    if (n != NULL) {
        return n;
    }

    if (strcmp(bind_name, "sta") == 0 || strcmp(bind_name, "example_netif_sta") == 0) {
        n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (n != NULL) {
            return n;
        }
    }
    if (strcmp(bind_name, "eth") == 0 || strcmp(bind_name, "example_netif_eth") == 0) {
        n = esp_netif_get_handle_from_ifkey("ETH_DEF");
        if (n != NULL) {
            return n;
        }
    }

    esp_netif_t *it = NULL;
    while ((it = esp_netif_next_unsafe(it)) != NULL) {
        const char *d = esp_netif_get_desc(it);
        if (d != NULL && strcmp(d, bind_name) == 0) {
            return it;
        }
    }
    return NULL;
}
#endif
static esp_err_t register_partition(size_t offset, size_t size, const char *label, esp_partition_type_t type,
                                  esp_partition_subtype_t subtype, const esp_partition_t **p_partition)
{
    *p_partition = esp_partition_find_first(type, subtype, NULL);
    if ((*p_partition) == NULL) {
        esp_err_t error = esp_partition_register_external(NULL, offset, size, label, type, subtype, p_partition);
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register partition (err=0x%x)", error);
            return error;
        }
    }
    ESP_LOGI(TAG, "Use <%s> partition (0x%08" PRIx32 ")", (*p_partition)->label, (*p_partition)->address);
    return ESP_OK;
}

#if CONFIG_BOOTLOADER_RECOVERY_ENABLE
static esp_err_t safe_bootloader_ota_update(esp_https_ota_config_t *ota_config)
{
    const esp_partition_t *primary_bootloader;
    const esp_partition_t *recovery_bootloader;
    ESP_ERROR_CHECK(register_partition(ESP_PRIMARY_BOOTLOADER_OFFSET, ESP_BOOTLOADER_SIZE, "PrimaryBTLDR",
                                       ESP_PARTITION_TYPE_BOOTLOADER, ESP_PARTITION_SUBTYPE_BOOTLOADER_PRIMARY,
                                       &primary_bootloader));
    ESP_ERROR_CHECK(register_partition(CONFIG_BOOTLOADER_RECOVERY_OFFSET, ESP_BOOTLOADER_SIZE, "RecoveryBTLDR",
                                       ESP_PARTITION_TYPE_BOOTLOADER, ESP_PARTITION_SUBTYPE_BOOTLOADER_RECOVERY,
                                       &recovery_bootloader));
    ESP_RETURN_ON_FALSE(recovery_bootloader->address == CONFIG_BOOTLOADER_RECOVERY_OFFSET, ESP_FAIL, TAG,
                        "The partition table contains <%s> (0x%08" PRIx32 "), which does not match the efuse recovery address (0x%08" PRIx32 ")",
                        recovery_bootloader->label, recovery_bootloader->address, CONFIG_BOOTLOADER_RECOVERY_OFFSET);
    ESP_ERROR_CHECK(esp_efuse_set_recovery_bootloader_offset(CONFIG_BOOTLOADER_RECOVERY_OFFSET));

    ESP_LOGI(TAG, "Backup, copy <%s> -> <%s>", primary_bootloader->label, recovery_bootloader->label);
    ESP_ERROR_CHECK(esp_partition_copy(recovery_bootloader, 0, primary_bootloader, 0, primary_bootloader->size));

    ota_config->partition.staging = primary_bootloader;
    esp_err_t ret = esp_https_ota(ota_config);
    if (ret == ESP_OK) {
        esp_partition_deregister_external(recovery_bootloader);
        esp_partition_deregister_external(primary_bootloader);
    }
    return ret;
}
#else

static esp_err_t unsafe_bootloader_ota_update(esp_https_ota_config_t *ota_config)
{
    const esp_partition_t *primary_bootloader;
    ESP_ERROR_CHECK(register_partition(ESP_PRIMARY_BOOTLOADER_OFFSET, ESP_BOOTLOADER_SIZE, "PrimaryBTLDR",
                                       ESP_PARTITION_TYPE_BOOTLOADER, ESP_PARTITION_SUBTYPE_BOOTLOADER_PRIMARY,
                                       &primary_bootloader));
    const esp_partition_t *ota_partition = esp_ota_get_next_update_partition(NULL);
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    esp_ota_img_states_t ota_state;
    ESP_ERROR_CHECK(esp_ota_get_state_partition(ota_partition, &ota_state));
    if (ota_state == ESP_OTA_IMG_VALID) {
        ESP_LOGW(TAG, "Passive OTA app partition <%s> contains a valid app image eligible for rollback.", ota_partition->label);
        uint32_t ota_bootloader_offset;
        ESP_ERROR_CHECK(partition_utils_find_unallocated(NULL, ESP_BOOTLOADER_SIZE,
                                                          ESP_PARTITION_TABLE_OFFSET + ESP_PARTITION_TABLE_SIZE,
                                                          &ota_bootloader_offset, NULL));
        ESP_ERROR_CHECK(register_partition(ota_bootloader_offset, ESP_BOOTLOADER_SIZE, "OtaBTLDR",
                                           ESP_PARTITION_TYPE_BOOTLOADER, ESP_PARTITION_SUBTYPE_BOOTLOADER_OTA, &ota_partition));
        ESP_LOGW(TAG, "To avoid overwriting the passive app partition, using unallocated flash for temporary OTA bootloader <%s>",
                 ota_partition->label);
    }
#endif
    ota_config->partition.staging = ota_partition;
    ota_config->partition.final = primary_bootloader;
    esp_err_t ret = esp_https_ota(ota_config);
    if (ret == ESP_OK) {
        ESP_LOGW(TAG, "Ensure stable power supply! Loss of power at this stage leads to a chip bricking");
        ESP_LOGI(TAG, "Copy from <%s> staging partition to <%s>...", ota_partition->label, primary_bootloader->label);
        ret = esp_partition_copy(primary_bootloader, 0, ota_partition, 0, primary_bootloader->size);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to copy partition to Primary bootloader (err=0x%x). Bootloader likely corrupted.", ret);
        }
        esp_partition_deregister_external(primary_bootloader);
    }
    return ret;
}
#endif

static esp_err_t ota_update_partitions(esp_https_ota_config_t *ota_config)
{
    esp_err_t ret = ESP_ERR_NOT_SUPPORTED;
    if (strstr(ota_config->http_config->url, "partitions_ota.bin") != NULL) {
        ret = esp_https_ota(ota_config);
    } else if (strstr(ota_config->http_config->url, "bootloader.bin") != NULL) {
#if CONFIG_BOOTLOADER_RECOVERY_ENABLE
        ESP_LOGI(TAG, "Safe OTA bootloader update (recovery bootloader supported).");
        ret = safe_bootloader_ota_update(ota_config);
#else
        ESP_LOGW(TAG, "Unsafe OTA bootloader update (no recovery bootloader).");
        ret = unsafe_bootloader_ota_update(ota_config);
#endif
    } else if (strstr(ota_config->http_config->url, "partition-table.bin") != NULL) {
        const esp_partition_t *primary_partition_table;
        ESP_ERROR_CHECK(register_partition(ESP_PRIMARY_PARTITION_TABLE_OFFSET, ESP_PARTITION_TABLE_SIZE, "PrimaryPrtTable",
                                           ESP_PARTITION_TYPE_PARTITION_TABLE, ESP_PARTITION_SUBTYPE_PARTITION_TABLE_PRIMARY,
                                           &primary_partition_table));
        const esp_partition_t *free_app_ota_partition = esp_ota_get_next_update_partition(NULL);
        ota_config->partition.staging = free_app_ota_partition;
        ota_config->partition.final = primary_partition_table;
        ota_config->partition.finalize_with_copy = false;
        ret = esp_https_ota(ota_config);
        if (ret == ESP_OK) {
            ESP_LOGW(TAG, "Ensure stable power supply! Loss of power at this stage leads to a chip bricking.");
            ESP_LOGI(TAG, "Copy from <%s> staging partition to <%s>...", free_app_ota_partition->label, primary_partition_table->label);
            ret = esp_partition_copy(primary_partition_table, 0, free_app_ota_partition, 0, primary_partition_table->size);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to copy partition to Primary partition table (err=0x%x).", ret);
            }
            esp_partition_deregister_external(primary_partition_table);
        }
    } else if (strstr(ota_config->http_config->url, "storage.bin") != NULL) {
        ota_config->partition.staging = NULL;
        ota_config->partition.final = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "storage");
        if (ota_config->partition.final == NULL) {
            ESP_LOGE(TAG, "storage partition not found");
            return ESP_ERR_NOT_FOUND;
        }
        ota_config->partition.finalize_with_copy = true;
        ret = esp_https_ota(ota_config);
        if (ret == ESP_OK) {
            char text[16];
            esp_err_t r = esp_partition_read(ota_config->partition.final, 0, text, sizeof(text));
            if (r == ESP_OK) {
                ESP_LOG_BUFFER_HEX_LEVEL(TAG, text, sizeof(text), ESP_LOG_INFO);
            }
        }
    } else {
        ESP_LOGE(TAG, "Unable to load this file (%s). The final partition is unknown.", ota_config->http_config->url);
    }
    return ret;
}

static esp_err_t wifi_connect_blocking(const https_ota_config_t *config)
{
    if (config->wifi_ssid == NULL || strlen(config->wifi_ssid) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, config->wifi_ssid, sizeof(wifi_config.sta.ssid) - 1);
    if (config->wifi_password) {
        strncpy((char *)wifi_config.sta.password, config->wifi_password, sizeof(wifi_config.sta.password) - 1);
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG, "esp_wifi_set_config");

#if CONFIG_EXAMPLE_CONNECT_WIFI
    esp_wifi_set_ps(WIFI_PS_NONE);
#endif

    for (int attempt = 0; attempt < HTTPS_OTA_WIFI_MAX_RETRY; attempt++) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            return err;
        }
        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdTRUE, pdFALSE,
                                                 pdMS_TO_TICKS(10000));
        if (bits & WIFI_CONNECTED_BIT) {
            return ESP_OK;
        }
        ESP_LOGW(TAG, "WiFi connect attempt %d/%d failed, retrying...", attempt + 1, HTTPS_OTA_WIFI_MAX_RETRY);
    }
    return ESP_FAIL;
}

esp_err_t https_ota_init(void)
{
    if (s_pack_inited) {
        return ESP_OK;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs_flash_init");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "esp_event_loop_create_default");

    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi_init");

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "esp_wifi_set_mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start");

    s_pack_inited = true;
    return ESP_OK;
}

esp_err_t https_ota_register_wifi_callback(wifi_event_callback_t callback)
{
    s_wifi_cb = callback;
    return ESP_OK;
}

esp_err_t https_ota_create_default_config(https_ota_config_t *config, const char *ssid, const char *password, const char *url)
{
    if (config == NULL || ssid == NULL || url == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(config, 0, sizeof(*config));
    config->wifi_ssid = ssid;
    config->wifi_password = password;
    config->ota_url = url;
#if CONFIG_EXAMPLE_USE_CERT_BUNDLE
    config->use_cert_bundle = true;
#else
    config->use_cert_bundle = true;
#endif
    config->custom_cert_pem = NULL;
    config->keep_alive_enable = true;
#ifdef CONFIG_EXAMPLE_SKIP_COMMON_NAME_CHECK
    config->skip_cert_common_name_check = true;
#endif
#ifdef CONFIG_EXAMPLE_FIRMWARE_UPGRADE_BIND_IF
    config->bind_interface_name = NULL;
#endif
    return ESP_OK;
}

esp_err_t https_ota_perform_update(const https_ota_config_t *config)
{
    if (config == NULL || config->ota_url == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!config->use_cert_bundle && (config->custom_cert_pem == NULL)) {
        ESP_LOGE(TAG, "custom_cert_pem required when use_cert_bundle is false");
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_pack_inited) {
        ESP_LOGE(TAG, "https_ota_init must be called first");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t werr = wifi_connect_blocking(config);
    ESP_RETURN_ON_ERROR(werr, TAG, "WiFi connect failed");

    esp_http_client_config_t http_cfg = {
        .url = config->ota_url,
        .event_handler = http_event_handler,
        .keep_alive_enable = config->keep_alive_enable,
        .skip_cert_common_name_check = config->skip_cert_common_name_check,
    };

    if (config->use_cert_bundle) {
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
        http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
#else
        ESP_LOGE(TAG, "Certificate bundle not enabled in sdkconfig (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE)");
        return ESP_ERR_NOT_SUPPORTED;
#endif
    } else {
        http_cfg.cert_pem = (char *)config->custom_cert_pem;
    }

#ifdef CONFIG_EXAMPLE_FIRMWARE_UPGRADE_BIND_IF
    struct ifreq ifr_bind;
    memset(&ifr_bind, 0, sizeof(ifr_bind));
    if (config->bind_interface_name != NULL && strlen(config->bind_interface_name) > 0) {
        esp_netif_t *netif = https_ota_resolve_bind_netif(config->bind_interface_name);//get_example_netif_from_desc
        if (netif == NULL) {
            ESP_LOGE(TAG, "Can't find netif for bind_interface_name");
            return ESP_FAIL;
        }
        esp_netif_get_netif_impl_name(netif, ifr_bind.ifr_name);
        http_cfg.if_name = &ifr_bind;
        ESP_LOGI(TAG, "Bind interface name is %s", ifr_bind.ifr_name);
    }
#endif

    esp_https_ota_config_t ota_config = {
        .http_config = &http_cfg,
    };

    ESP_LOGI(TAG, "Attempting to download update from %s", http_cfg.url);
    esp_err_t ret = ota_update_partitions(&ota_config);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA succeed, rebooting...");
        esp_restart();
    }
    return ret;
}
