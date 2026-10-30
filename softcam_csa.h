#ifndef DVBLAST_SOFTCAM_CSA_H
#define DVBLAST_SOFTCAM_CSA_H

#include <stdint.h>
#include <stdbool.h>
#include <jansson.h>
#include "dvbcsa/dvbcsa.h"

#define SOFTCAM_MAX_KEYS 64
#define SOFTCAM_NAME_LEN 64
#define SOFTCAM_HEX_LEN  64

typedef struct {
    char id[SOFTCAM_NAME_LEN];
    char name[SOFTCAM_NAME_LEN];
    uint16_t service_id;          /* 0 means applies to all or specified by ID */
    char key_hex[SOFTCAM_HEX_LEN];
    uint8_t cw[16];               /* 8 bytes Even + 8 bytes Odd */
    bool enabled;
    bool valid;
    dvbcsa_key_t *csa_even;
    dvbcsa_key_t *csa_odd;
    uint64_t packets_descrambled;
} softcam_entry_t;

void softcam_csa_init(void);
void softcam_csa_cleanup(void);

bool softcam_csa_set_key(const char *id, const char *name, uint16_t sid, const char *key_hex, bool enabled);
bool softcam_csa_del_key(const char *id_or_sid);
const softcam_entry_t *softcam_csa_find(uint16_t sid);
const softcam_entry_t *softcam_csa_get_at(int index);
int  softcam_csa_count(void);

int  softcam_csa_descramble(uint16_t sid, uint8_t *ts_packet);

json_t *softcam_csa_to_json(void);
bool    softcam_csa_from_json(json_t *root);

#endif /* DVBLAST_SOFTCAM_CSA_H */
