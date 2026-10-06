#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Initialize NVS (with auto-recovery) and the storage layer. */
esp_err_t config_store_init(void);

/** @brief Write a NUL-terminated string value into an NVS namespace. */
esp_err_t config_store_set_str(const char *ns, const char *key, const char *value);

/**
 * @brief Read a string value. Returns the string length (0 if missing).
 * The output is always NUL-terminated when @p out_len > 0.
 */
size_t config_store_get_str(const char *ns, const char *key, char *out, size_t out_len);

/** @brief Remove a single key from a namespace. */
esp_err_t config_store_erase_key(const char *ns, const char *key);

/** @brief Write a binary blob into an NVS namespace. */
esp_err_t config_store_set_blob(const char *ns, const char *key, const void *data, size_t len);

/** @brief Read a binary blob. Returns the number of bytes read (0 if missing). */
size_t config_store_get_blob(const char *ns, const char *key, void *out, size_t out_len);

/** @brief Remove every key in a namespace. */
esp_err_t config_store_erase_ns(const char *ns);

/** @brief Erase the whole NVS partition (factory reset). */
esp_err_t config_store_erase_all(void);

/**
 * @brief NVS entry usage, as a flash-wear gauge.
 *
 * Partition-level on purpose: a counter inside this module would miss the
 * NimBLE bond store, which writes to NVS directly. @p used grows with every
 * write from any source, so a flat value across normal use means nothing is
 * programming flash. Any out pointer may be NULL.
 *
 * @return false when the statistics are unavailable.
 */
bool config_store_get_usage(uint32_t *used, uint32_t *free_entries, uint32_t *total);

/** @brief Log the NVS entry usage under @p tag. */
void config_store_log_usage(const char *tag);

#ifdef __cplusplus
}
#endif
