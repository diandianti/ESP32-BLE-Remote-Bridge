#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * User-defined display names for the physical remote buttons.
 *
 * A remote reports a button *index*; the firmware turns that into a physical
 * *slot* (index - 1) and everything downstream is keyed on the slot. Which
 * physical button sits in which slot is a property of the remote, not of the
 * protocol, so the label shown for a slot is the user's to choose: this module
 * stores an optional per-slot name that overrides the built-in label.
 *
 * Names are display-only. What a button *does* is the keymap's job.
 */

/** Number of physical key slots; matches the keymap's slot numbering. */
#define KEY_NAME_SLOT_COUNT 16

/** Longest accepted name, in UTF-8 bytes, excluding the terminator. */
#define KEY_NAME_MAX_BYTES  31

/** Per-slot storage stride: the name plus its NUL terminator. */
#define KEY_NAME_SLOT_SIZE  (KEY_NAME_MAX_BYTES + 1)

/** @brief Load the stored names. Call once after config_store_init(). */
void key_names_init(void);

/**
 * @brief Custom name for a slot.
 *
 * @return The stored name, or an empty string when the slot has no custom name
 *         or the slot is out of range. Never NULL.
 */
const char *key_names_get(int slot);

/**
 * @brief Store (or clear) the custom name for a slot.
 *
 * An empty or whitespace-only name clears the slot back to its built-in label.
 * Over-long names are truncated on a UTF-8 character boundary rather than
 * rejected, so a name is never split mid-character.
 *
 * @return false when the slot is out of range; nothing is written then.
 */
bool key_names_set(int slot, const char *name);

/** @brief Clear every custom name. */
void key_names_reset(void);

/** @brief Serialize all slots as {"names":["",...]}, in slot order. */
size_t key_names_to_json(char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
