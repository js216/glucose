// SPDX-License-Identifier: GPL-3.0
// p256.h --- Portable NIST P-256 (API)
// Copyright 2026 Jakob Kastelic

/* Portable NIST P-256 (secp256r1): the field/scalar/point operations the
 * Dexcom J-PAKE needs. No external dependencies. Not constant-time (this is a
 * personal reader talking to its own sensor, not an adversarial setting). */
#ifndef DEX_P256_H
#define DEX_P256_H
#include <stdint.h>

struct u256 { /* little-endian 64-bit limbs */
   uint64_t v[4];
};

struct jpoint { /* Jacobian; Z==0 => infinity */
   struct u256 X, Y, Z;
};

void p256_init(void); /* call once */

/* scalars mod n (curve order) */
void p256_sc_from_be(struct u256 *r,
                     const uint8_t be[32]); /* load + reduce mod n */
void p256_sc_mul(struct u256 *r, const struct u256 *a, const struct u256 *b);
void p256_sc_add(struct u256 *r, const struct u256 *a, const struct u256 *b);
void p256_sc_sub(struct u256 *r, const struct u256 *a, const struct u256 *b);
void p256_sc_neg(struct u256 *r, const struct u256 *a);
void p256_sc_inv(struct u256 *r,
                 const struct u256 *a); /* modular inverse mod n */
int p256_sc_is_zero(const struct u256 *a);

/* points */
extern struct jpoint p256_g;
void p256_mul(struct jpoint *r, const struct u256 *k,
              const struct jpoint *P);                   /* k*P */
void p256_mul_g(struct jpoint *r, const struct u256 *k); /* k*G */
void p256_padd(struct jpoint *r, const struct jpoint *P,
               const struct jpoint *Q);
void p256_pneg(struct jpoint *r, const struct jpoint *P);
int p256_is_inf(const struct jpoint *P);
int p256_eq(const struct jpoint *A, const struct jpoint *B);
/* affine X||Y (32+32); returns 1 ok, 0 if infinity / not on curve */
int p256_to_xy(const struct jpoint *P, uint8_t x[32], uint8_t y[32]);
int p256_from_xy(struct jpoint *P, const uint8_t x[32], const uint8_t y[32]);
void p256_uncompressed(const struct jpoint *P, uint8_t out[65]); /* 04||X||Y */

#endif
