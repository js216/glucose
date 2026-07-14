/* Dexcom G7 / Stelo authentication — public C API. Self-contained (portable
 * SHA-256 / AES-128 / P-256; no external crypto lib). See NOTES.md. */
#ifndef DEXAUTH_H
#define DEXAUTH_H

#include <stddef.h>
#include <stdint.h>

/* Initialise the curve context. Call once before anything else. Returns 1. */
int dexauth_init(void);

/* Per-connection auth hash: AES-128-ECB(key16, data8||data8)[:8]. */
int dexauth_dex8(const uint8_t key16[16], const uint8_t data8[8], uint8_t out8[8]);

/* Certificate key-challenge: ECDSA-P256 sign SHA256(challenge[2:18]) with the
 * embedded device key -> raw r||s (64 bytes). challenge must be >= 18 bytes. */
int dexauth_getchallenge(const uint8_t *challenge, size_t clen, uint8_t out64[64]);
int dexauth_verify_challenge(const uint8_t *challenge, size_t clen, const uint8_t sig64[64]);

/* ---- First-pairing EC-J-PAKE driver (byte-oriented; opaque state) ----
 * Exchange 160-byte round packets with the peer, then derive the 16-byte sharedKey.
 * is_client=1 for the phone side, 0 for the sensor side (selects the signer id).
 * The peer_* calls return 1 if the received ZKP validates, 0 otherwise. */
typedef struct dex_pairing dex_pairing;
dex_pairing *dexpair_new(const uint8_t *pass, size_t passlen, int is_client);
int  dexpair_round1(dex_pairing *p, uint8_t out[160]);
int  dexpair_round2(dex_pairing *p, uint8_t out[160]);
int  dexpair_round3(dex_pairing *p, uint8_t out[160]);
int  dexpair_peer_round1(dex_pairing *p, const uint8_t in[160]);
int  dexpair_peer_round2(dex_pairing *p, const uint8_t in[160]);
int  dexpair_peer_round3(dex_pairing *p, const uint8_t in[160]);
int  dexpair_shared_key(dex_pairing *p, uint8_t out16[16]);
void dexpair_free(dex_pairing *p);

#endif
