# ESPIDF_OTA_PACK
针对IDF框架的WIFI_OTA空中升级组件(基于IDF5.5.3版本)

使用教程
1.根目录添加components\文件夹，将http_ota_pack组件放进文件夹内
2.设置根目录CMakeLists.txt,将componments组件包含进项目
'''
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_SOURCE_DIR}/components"
)

'''
3.在main里面的CMakeLists.txt添加http_ota_pack组件
'''

PRIV_REQUIRES spi_flash https_ota_pack

'''
4.在调用的文件内添加头文件
'''

#include "https_ota_pack.h"
#include "partition_utils.h"（可选：仅当你在应用层直接调用 partition_utils 的接口）

'''
5.调用库文件，调用示例
'''
#include "https_ota_pack.h"

void app_main(void)
{
    ESP_ERROR_CHECK(https_ota_init());

    https_ota_config_t cfg;
    https_ota_create_default_config(
        &cfg,
        "SSID",                         /* WiFi 名称 */
        "PASSWORD",                     /* WiFi 密码 */
        "https://example.com/partitions_ota.bin");  /* OTA 固件 HTTPS URL */

    https_ota_perform_update(&cfg);    /* 升级成功后会重启，函数通常不会返回 */
}

'''

6.注意事项：1.分区表设置问题：分区表不能为默认"Single factory app,no OTA",可进入menuconfig的分区表修改配置
           2.默认https证书使用证书 bundle，如须使用自己的 CA PEM，需要引入esp_crt_bundle.h，以及在use_cert_bundle = false，并设置 custom_cert_pem 指向有效 PEM。

详细说明(可忽略)：
分区表（必选）
不能使用仅含 “Single factory app, no OTA” 的默认表。应用 OTA 需要在 menuconfig → Partition Table 中选择带 OTA 分区的表，或使用自定义 partitions.csv（至少包含 ota_0 / ota_1 等应用 OTA 槽，具体以乐鑫文档与目标芯片为准）。否则运行期会出现类似 “Passive OTA / staging partition not found” 的报错。

HTTPS 证书（二选一）
证书 bundle（推荐、与 https_ota_create_default_config 一致）：在 menuconfig 中启用 mbedTLS Certificate Bundle（CONFIG_MBEDTLS_CERTIFICATE_BUNDLE）。应用层一般不必再 #include "esp_crt_bundle.h"，由 https_ota_pack 在 use_cert_bundle == true 时内部使用。
自签 / 私有 CA（PEM 字符串）：设置 use_cert_bundle = false，并将 custom_cert_pem 指向有效 PEM 文本（例如嵌入证书符号或静态字符串）。应用层不需要为“用 bundle”而包含 esp_crt_bundle.h；仅当你自己在别处调用 esp_crt_bundle_attach 等 API 时才需要该头文件。
WiFi / 路径
确保 sdkconfig 中已启用 WiFi 等与 STA 相关的选项；若工程里没有示例专用的 CONFIG_EXAMPLE_*，请依赖 https_ota_create_default_config 与 CONFIG_MBEDTLS_CERTIFICATE_BUNDLE 对齐后的默认行为，或手动设置 cfg.use_cert_bundle / cfg.custom_cert_pem。


如果觉得不错，请给我一个STAR!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!