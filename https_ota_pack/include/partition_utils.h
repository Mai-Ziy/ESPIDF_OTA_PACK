/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_partition.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Find unallocated space in the partition table.
 */
esp_err_t partition_utils_find_unallocated(esp_flash_t *flash_chip, size_t required_size, uint32_t start_offset, uint32_t *found_offset, size_t *found_size);

#ifdef __cplusplus
}
#endif
