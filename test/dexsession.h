/* Dexcom G7 / Stelo authentication session — public C API. See NOTES.md. */
#ifndef DEXSESSION_H
#define DEXSESSION_H

#include <stdint.h>
#include <stddef.h>

enum {
    DEX_INIT = 0,
    DEX_REQUEST_AUTH,
    DEX_CHALLENGE_REPLY,
    DEX_AUTHED,
    DEX_NEED_PAIR,
    DEX_FAIL
};

/* Return codes shared by the step handlers. */
#define DEX_ERR       (-1)
/* dexsess_on_status also returns DEX_AUTHED / DEX_NEED_PAIR from the enum above. */

typedef struct {
    uint8_t saved_key[16];
    uint8_t token[8];
    uint8_t challenge[8];
    int     state;
} dex_session;

/* Reconnect challenge-response (needs an established sharedKey). */
int dexsess_start(dex_session *s, const uint8_t saved_key[16],
                  const uint8_t token[8], uint8_t out[10]);
int dexsess_on_challenge(dex_session *s, const uint8_t *data, size_t len, uint8_t out[9]);
int dexsess_on_status(dex_session *s, const uint8_t *data, size_t len);

/* Control commands. */
int dexsess_getdata_cmd(uint8_t out[1]);
int dexsess_backfill_cmd(uint32_t start, uint32_t end, uint8_t out[9]);

#endif
