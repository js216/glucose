/* Portable NIST P-256 (secp256r1): the field/scalar/point operations the
 * Dexcom J-PAKE needs. No external dependencies. Not constant-time (this is a
 * personal reader talking to its own sensor, not an adversarial setting). */
#ifndef DEX_P256_H
#define DEX_P256_H
#include <stdint.h>

typedef struct { uint64_t v[4]; } u256;          /* little-endian 64-bit limbs */
typedef struct { u256 X, Y, Z; } jpoint;         /* Jacobian; Z==0 => infinity */

void p256_init(void);                             /* call once */

/* scalars mod n (curve order) */
void p256_sc_from_be(u256 *r, const uint8_t be[32]); /* load + reduce mod n */
void p256_sc_mul(u256 *r, const u256 *a, const u256 *b);
void p256_sc_add(u256 *r, const u256 *a, const u256 *b);
void p256_sc_sub(u256 *r, const u256 *a, const u256 *b);
void p256_sc_neg(u256 *r, const u256 *a);
void p256_sc_inv(u256 *r, const u256 *a);        /* modular inverse mod n */
int  p256_sc_is_zero(const u256 *a);

/* points */
extern jpoint P256_G;
void p256_mul(jpoint *r, const u256 *k, const jpoint *P);   /* k*P */
void p256_mul_g(jpoint *r, const u256 *k);                  /* k*G */
void p256_padd(jpoint *r, const jpoint *P, const jpoint *Q);
void p256_pneg(jpoint *r, const jpoint *P);
int  p256_is_inf(const jpoint *P);
int  p256_eq(const jpoint *P, const jpoint *Q);
/* affine X||Y (32+32); returns 1 ok, 0 if infinity / not on curve */
int  p256_to_xy(const jpoint *P, uint8_t x[32], uint8_t y[32]);
int  p256_from_xy(jpoint *P, const uint8_t x[32], const uint8_t y[32]);
void p256_uncompressed(const jpoint *P, uint8_t out[65]);  /* 04||X||Y */

#endif
