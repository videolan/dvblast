#include "softcam_csa.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <ctype.h>

static softcam_entry_t g_keys[SOFTCAM_MAX_KEYS];
static int g_num_keys = 0;
static pthread_mutex_t g_csa_mutex = PTHREAD_MUTEX_INITIALIZER;

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_hex_string(const char *hex, uint8_t *out, int max_bytes) {
    if (!hex || !out || max_bytes <= 0) return 0;
    char clean[128];
    int clean_len = 0;
    for (int i = 0; hex[i] && clean_len < (int)sizeof(clean) - 1; i++) {
        if (isxdigit((unsigned char)hex[i])) {
            clean[clean_len++] = hex[i];
        }
    }
    clean[clean_len] = '\0';

    int bytes = 0;
    for (int i = 0; i + 1 < clean_len && bytes < max_bytes; i += 2) {
        int hi = hex_nibble(clean[i]);
        int lo = hex_nibble(clean[i + 1]);
        if (hi < 0 || lo < 0) break;
        out[bytes++] = (uint8_t)((hi << 4) | lo);
    }
    return bytes;
}

static bool parse_cw_key(const char *key_hex, uint8_t *cw_out) {
    memset(cw_out, 0, 16);
    uint8_t raw[16] = {0};
    int n = parse_hex_string(key_hex, raw, 16);

    if (n >= 16) {
        /* Full 16-byte Constant CW (Even 8B + Odd 8B) */
        memcpy(cw_out, raw, 16);
        return true;
    } else if (n >= 8) {
        /* 8-byte BISS-1 key with parity already present (Even == Odd) */
        memcpy(cw_out, raw, 8);
        memcpy(cw_out + 8, raw, 8);
        return true;
    } else if (n >= 6) {
        /* 6-byte (12 hex digits) BISS-1: Calculate DVB-CSA parity bytes 3 and 7 */
        cw_out[0] = raw[0];
        cw_out[1] = raw[1];
        cw_out[2] = raw[2];
        cw_out[3] = (uint8_t)((raw[0] + raw[1] + raw[2]) & 0xFF);
        cw_out[4] = raw[3];
        cw_out[5] = raw[4];
        cw_out[6] = raw[5];
        cw_out[7] = (uint8_t)((raw[3] + raw[4] + raw[5]) & 0xFF);
        memcpy(cw_out + 8, cw_out, 8);
        return true;
    }
    return false;
}

void softcam_csa_init(void) {
    pthread_mutex_lock(&g_csa_mutex);
    for (int i = 0; i < g_num_keys; i++) {
        if (g_keys[i].csa_even) dvbcsa_key_free(g_keys[i].csa_even);
        if (g_keys[i].csa_odd) dvbcsa_key_free(g_keys[i].csa_odd);
    }
    memset(g_keys, 0, sizeof(g_keys));
    g_num_keys = 0;
    pthread_mutex_unlock(&g_csa_mutex);
}

void softcam_csa_cleanup(void) {
    softcam_csa_init();
}

bool softcam_csa_set_key(const char *id, const char *name, uint16_t sid, const char *key_hex, bool enabled) {
    if (!id || !*id || !key_hex || !*key_hex) return false;

    pthread_mutex_lock(&g_csa_mutex);
    softcam_entry_t *entry = NULL;

    for (int i = 0; i < g_num_keys; i++) {
        if (strcmp(g_keys[i].id, id) == 0 || (sid != 0 && g_keys[i].service_id == sid)) {
            entry = &g_keys[i];
            break;
        }
    }

    if (!entry) {
        if (g_num_keys >= SOFTCAM_MAX_KEYS) {
            pthread_mutex_unlock(&g_csa_mutex);
            return false;
        }
        entry = &g_keys[g_num_keys++];
    }

    snprintf(entry->id, sizeof(entry->id), "%s", id);
    if (name && *name) snprintf(entry->name, sizeof(entry->name), "%s", name);
    else snprintf(entry->name, sizeof(entry->name), "Key-%s", id);

    entry->service_id = sid;
    snprintf(entry->key_hex, sizeof(entry->key_hex), "%s", key_hex);
    entry->enabled = enabled;

    uint8_t cw[16] = {0};
    entry->valid = parse_cw_key(key_hex, cw);
    if (entry->valid) {
        memcpy(entry->cw, cw, 16);
        if (!entry->csa_even) entry->csa_even = dvbcsa_key_alloc();
        if (!entry->csa_odd)  entry->csa_odd  = dvbcsa_key_alloc();
        if (entry->csa_even) dvbcsa_key_set(entry->cw, entry->csa_even);
        if (entry->csa_odd)  dvbcsa_key_set(entry->cw + 8, entry->csa_odd);
    }

    pthread_mutex_unlock(&g_csa_mutex);
    return true;
}

bool softcam_csa_del_key(const char *id_or_sid) {
    if (!id_or_sid) return false;
    pthread_mutex_lock(&g_csa_mutex);

    uint16_t target_sid = (uint16_t)atoi(id_or_sid);
    int found_idx = -1;

    for (int i = 0; i < g_num_keys; i++) {
        if (strcmp(g_keys[i].id, id_or_sid) == 0 || (target_sid != 0 && g_keys[i].service_id == target_sid)) {
            found_idx = i;
            break;
        }
    }

    if (found_idx >= 0) {
        if (g_keys[found_idx].csa_even) dvbcsa_key_free(g_keys[found_idx].csa_even);
        if (g_keys[found_idx].csa_odd) dvbcsa_key_free(g_keys[found_idx].csa_odd);

        for (int i = found_idx; i < g_num_keys - 1; i++) {
            g_keys[i] = g_keys[i + 1];
        }
        g_num_keys--;
        memset(&g_keys[g_num_keys], 0, sizeof(softcam_entry_t));
        pthread_mutex_unlock(&g_csa_mutex);
        return true;
    }

    pthread_mutex_unlock(&g_csa_mutex);
    return false;
}

const softcam_entry_t *softcam_csa_find(uint16_t sid) {
    pthread_mutex_lock(&g_csa_mutex);
    for (int i = 0; i < g_num_keys; i++) {
        if (g_keys[i].enabled && g_keys[i].valid && g_keys[i].service_id == sid) {
            pthread_mutex_unlock(&g_csa_mutex);
            return &g_keys[i];
        }
    }
    /* Fallback: if there is a universal key with sid == 0 */
    for (int i = 0; i < g_num_keys; i++) {
        if (g_keys[i].enabled && g_keys[i].valid && g_keys[i].service_id == 0) {
            pthread_mutex_unlock(&g_csa_mutex);
            return &g_keys[i];
        }
    }
    pthread_mutex_unlock(&g_csa_mutex);
    return NULL;
}

const softcam_entry_t *softcam_csa_get_at(int index) {
    if (index < 0 || index >= g_num_keys) return NULL;
    return &g_keys[index];
}

int softcam_csa_count(void) {
    return g_num_keys;
}

int softcam_csa_descramble(uint16_t sid, uint8_t *ts_packet) {
    if (!ts_packet || ts_packet[0] != 0x47) return 0;

    int tsc = (ts_packet[3] >> 6) & 0x03;
    if (tsc == 0) return 0; /* Clear (not scrambled) */

    const softcam_entry_t *k = softcam_csa_find(sid);
    if (!k || !k->valid) return 0;

    const dvbcsa_key_t *csa_key = NULL;
    if (tsc == 2) {
        csa_key = k->csa_even;
    } else if (tsc == 3) {
        csa_key = k->csa_odd ? k->csa_odd : k->csa_even;
    } else if (tsc == 1) {
        csa_key = k->csa_even;
    }
    if (!csa_key) return 0;

    int afc = (ts_packet[3] >> 4) & 0x03;
    size_t offset = 4;
    if (afc == 0 || afc == 2) return 0; /* No payload */
    if (afc == 3) {
        uint8_t afl = ts_packet[4];
        if (afl > 183) return 0;
        offset = 4 + 1 + afl;
    }
    if (offset >= 188) return 0;

    size_t payload_len = 188 - offset;
    dvbcsa_decrypt(csa_key, ts_packet + offset, (unsigned int)payload_len);

    /* Clear scrambling control bits to 00 */
    ts_packet[3] &= 0x3F;

    ((softcam_entry_t *)k)->packets_descrambled++;
    return 1;
}

json_t *softcam_csa_to_json(void) {
    pthread_mutex_lock(&g_csa_mutex);
    json_t *arr = json_array();
    for (int i = 0; i < g_num_keys; i++) {
        json_t *item = json_object();
        json_object_set_new(item, "id", json_string(g_keys[i].id));
        json_object_set_new(item, "name", json_string(g_keys[i].name));
        json_object_set_new(item, "service_id", json_integer(g_keys[i].service_id));
        json_object_set_new(item, "key", json_string(g_keys[i].key_hex));
        json_object_set_new(item, "enabled", json_boolean(g_keys[i].enabled));
        json_object_set_new(item, "valid", json_boolean(g_keys[i].valid));
        json_object_set_new(item, "descrambled_packets", json_integer((json_int_t)g_keys[i].packets_descrambled));
        json_array_append_new(arr, item);
    }
    pthread_mutex_unlock(&g_csa_mutex);
    return arr;
}

bool softcam_csa_from_json(json_t *root) {
    if (!json_is_array(root)) return false;
    softcam_csa_init();

    size_t idx;
    json_t *val;
    json_array_foreach(root, idx, val) {
        const char *id = json_string_value(json_object_get(val, "id"));
        const char *name = json_string_value(json_object_get(val, "name"));
        uint16_t sid = (uint16_t)json_integer_value(json_object_get(val, "service_id"));
        const char *key = json_string_value(json_object_get(val, "key"));
        bool enabled = !json_is_false(json_object_get(val, "enabled"));
        if (id && key) {
            softcam_csa_set_key(id, name, sid, key, enabled);
        }
    }
    return true;
}
