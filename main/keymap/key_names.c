#include "key_names.h"

#include <string.h>

#include "app_log.h"
#include "config_store.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

// One namespace, one blob: the whole 16-slot table is small enough that a
// partial write is impossible, which matters because a half-written name would
// silently run into the next slot.
#define KEY_NAMES_NS  "key_names"
#define KEY_NAMES_KEY "names"

// How long a burst of renames is allowed to accumulate before the table is
// written. The config page saves one slot per request, so without this, naming
// all sixteen buttons meant sixteen NVS commits - and it is the commit, not the
// 512 bytes, that programs a flash page.
#define KEY_NAMES_SAVE_DELAY_MS 200

static char s_names[KEY_NAME_SLOT_COUNT][KEY_NAME_SLOT_SIZE];

// Guards s_names: the WebUSB task renames slots while the save task reads the
// table out, so without this a save could capture a half-applied rename.
static SemaphoreHandle_t s_mutex = NULL;
static SemaphoreHandle_t s_save_sem = NULL;
static volatile bool s_dirty = false;

static bool slot_valid(int slot)
{
    return slot >= 0 && slot < KEY_NAME_SLOT_COUNT;
}

static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// Copy at most KEY_NAME_MAX_BYTES bytes of a UTF-8 string, dropping a trailing
// multi-byte sequence that the limit cut in half. Truncating blindly would emit
// a broken character, which some renderers show as a replacement glyph.
static void utf8_copy(char *dst, const char *src)
{
    size_t n = strlen(src);
    if (n > KEY_NAME_MAX_BYTES) n = KEY_NAME_MAX_BYTES;

    if (n > 0) {
        // Walk back over continuation bytes (10xxxxxx) to the lead byte, then
        // drop the sequence if it does not fit in the bytes we kept.
        size_t start = n - 1;
        while (start > 0 && ((unsigned char)src[start] & 0xC0) == 0x80) start--;
        unsigned char lead = (unsigned char)src[start];
        size_t need = 1;
        if ((lead & 0xE0) == 0xC0)      need = 2;
        else if ((lead & 0xF0) == 0xE0) need = 3;
        else if ((lead & 0xF8) == 0xF0) need = 4;
        if (start + need > n) n = start;
    }

    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void key_names_write(void)
{
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_err_t err = config_store_set_blob(KEY_NAMES_NS, KEY_NAMES_KEY, s_names, sizeof(s_names));
    if (s_mutex) xSemaphoreGive(s_mutex);

    if (err != ESP_OK) {
        app_log("KEYMAP", "Failed to save key names: %s", esp_err_to_name(err));
    }
}

static void key_names_save_task(void *arg)
{
    (void)arg;
    while (1) {
        if (xSemaphoreTake(s_save_sem, portMAX_DELAY) == pdTRUE) {
            vTaskDelay(pdMS_TO_TICKS(KEY_NAMES_SAVE_DELAY_MS));
            if (!s_dirty) continue;   // cleared by a reset that happened meanwhile
            s_dirty = false;
            key_names_write();
        }
    }
}

void key_names_init(void)
{
    memset(s_names, 0, sizeof(s_names));
    (void)config_store_get_blob(KEY_NAMES_NS, KEY_NAMES_KEY, s_names, sizeof(s_names));

    // Terminate every slot even if the stored blob was short or came from a
    // different stride: a name must never read into the following slot.
    for (int i = 0; i < KEY_NAME_SLOT_COUNT; i++) {
        s_names[i][KEY_NAME_MAX_BYTES] = '\0';
    }

    if (!s_mutex)   s_mutex = xSemaphoreCreateMutex();
    if (!s_save_sem) {
        s_save_sem = xSemaphoreCreateBinary();
        xTaskCreate(key_names_save_task, "keyname_save", 4096, NULL, 3, NULL);
    }

    int custom = 0;
    for (int i = 0; i < KEY_NAME_SLOT_COUNT; i++) {
        if (s_names[i][0]) custom++;
    }
    app_log("KEYMAP", "Key names ready (%d/%d custom)", custom, KEY_NAME_SLOT_COUNT);
}

const char *key_names_get(int slot)
{
    if (!slot_valid(slot)) return "";
    return s_names[slot];
}

bool key_names_set(int slot, const char *name)
{
    if (!slot_valid(slot)) return false;

    if (!name) name = "";
    while (is_space(*name)) name++;

    size_t len = strlen(name);
    while (len > 0 && is_space(name[len - 1])) len--;

    // Normalise into a scratch buffer first so the comparison below is against
    // exactly what would be stored - otherwise a rename that only differs by
    // surrounding whitespace would look like a change and cost a flash write.
    char next[KEY_NAME_SLOT_SIZE];
    if (len == 0) {
        next[0] = '\0';
    } else {
        char trimmed[KEY_NAME_SLOT_SIZE];
        size_t n = (len < KEY_NAME_MAX_BYTES) ? len : KEY_NAME_MAX_BYTES;
        memcpy(trimmed, name, n);
        trimmed[n] = '\0';
        utf8_copy(next, trimmed);
    }

    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool changed = (strcmp(s_names[slot], next) != 0);
    if (changed) {
        memcpy(s_names[slot], next, KEY_NAME_SLOT_SIZE);
    }
    if (s_mutex) xSemaphoreGive(s_mutex);

    // Unchanged means there is nothing to record: writing it anyway would only
    // program a flash page for no reason.
    if (!changed) return true;

    app_log("KEYMAP", "Key name slot %d -> \"%s\"", slot, next);
    s_dirty = true;
    if (s_save_sem) xSemaphoreGive(s_save_sem);
    return true;
}

void key_names_reset(void)
{
    // Cancel any pending flush first, so it cannot write back the table we are
    // about to clear.
    s_dirty = false;

    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    memset(s_names, 0, sizeof(s_names));
    if (s_mutex) xSemaphoreGive(s_mutex);

    (void)config_store_erase_key(KEY_NAMES_NS, KEY_NAMES_KEY);
    app_log("KEYMAP", "Key names reset to built-in labels");
}

// Append a JSON string body, escaping what JSON requires. UTF-8 bytes pass
// through untouched, which is what makes Chinese names work.
static size_t json_escape_into(char *out, size_t out_len, size_t pos, const char *src)
{
    static const char hex[] = "0123456789abcdef";
    for (const char *p = src; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            if (pos + 2 >= out_len) break;
            out[pos++] = '\\';
            out[pos++] = (char)c;
        } else if (c < 0x20) {
            if (pos + 6 >= out_len) break;
            out[pos++] = '\\';
            out[pos++] = 'u';
            out[pos++] = '0';
            out[pos++] = '0';
            out[pos++] = hex[(c >> 4) & 0x0F];
            out[pos++] = hex[c & 0x0F];
        } else {
            if (pos + 1 >= out_len) break;
            out[pos++] = (char)c;
        }
    }
    return pos;
}

size_t key_names_to_json(char *out, size_t out_len)
{
    if (!out || out_len == 0) return 0;

    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);

    size_t pos = 0;
#define APPEND_LITERAL(s)                        \
    do {                                         \
        const char *q_ = (s);                    \
        while (*q_ && pos + 1 < out_len) {       \
            out[pos++] = *q_++;                  \
        }                                        \
    } while (0)

    APPEND_LITERAL("{\"names\":[");
    for (int i = 0; i < KEY_NAME_SLOT_COUNT; i++) {
        if (i > 0) APPEND_LITERAL(",");
        APPEND_LITERAL("\"");
        pos = json_escape_into(out, out_len, pos, s_names[i]);
        APPEND_LITERAL("\"");
    }
    APPEND_LITERAL("]}");
#undef APPEND_LITERAL

    if (s_mutex) xSemaphoreGive(s_mutex);

    // Every append above stops while pos + 1 < out_len, so this write is in
    // bounds even when the buffer filled up.
    out[pos] = '\0';
    return pos;
}
