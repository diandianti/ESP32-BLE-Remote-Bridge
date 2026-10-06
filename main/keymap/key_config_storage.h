#pragma once

#include "key_state_machine.h"
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Load the keymap from NVS, falling back to factory defaults. */
void key_config_storage_init(key_mapper_engine_t *engine);

/** @brief Persist the current keymap to NVS. */
bool key_config_storage_save(key_mapper_engine_t *engine);

/**
 * @brief Request a deferred (post-response) keymap save.
 *
 * Only for configuration changes. Runtime state (the selected layer, held keys)
 * is intentionally never persisted: writing it turned routine keypresses into
 * flash writes.
 */
void key_config_storage_request_save(void);

/** @brief Load the keymap from NVS (does not apply defaults on failure). */
bool key_config_storage_load(key_mapper_engine_t *engine);

/** @brief Erase the stored keymap and reload factory defaults. */
void key_config_storage_reset_defaults(key_mapper_engine_t *engine);

/** @brief Serialize the full multi-layer keymap into @p out as JSON. */
size_t key_config_to_json(const key_mapper_engine_t *engine, char *out, size_t out_len);

/** @brief Apply a multi-layer keymap JSON document to the engine. */
bool key_config_from_json(key_mapper_engine_t *engine, const char *json_str);

/** @brief Serialize the latest key telemetry event as JSON. */
size_t key_telemetry_to_json(const key_mapper_engine_t *engine, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
