// SPDX-License-Identifier: GPL-3.0
// p256.c --- Portable NIST P-256 elliptic-curve arithmetic
// Copyright 2026 Jakob Kastelic

/* Portable NIST P-256. See p256.h. Correctness-first (not constant-time). */
#include "p256.h"
#include <stddef.h>
#include <stdint.h>

/* ---- 256-bit little-endian limb arithmetic ---- */
static int u256_iszero(const struct u256 *a)
{
   return (a->v[0] | a->v[1] | a->v[2] | a->v[3]) == 0;
}

static int u256_cmp(const struct u256 *a, const struct u256 *b)
{ /* -1,0,1 */
   for (int i = 3; i >= 0; i--) {
      if (a->v[i] < b->v[i])
         return -1;
      if (a->v[i] > b->v[i])
         return 1;
   }
   return 0;
}

static uint64_t u256_add(struct u256 *r, const struct u256 *a,
                         const struct u256 *b)
{ /* returns carry */
   unsigned __int128 c = 0;
   for (int i = 0; i < 4; i++) {
      c += (unsigned __int128)a->v[i] + b->v[i];
      r->v[i] = (uint64_t)c;
      c >>= 64U;
   }
   return (uint64_t)c;
}

static uint64_t u256_sub(struct u256 *r, const struct u256 *a,
                         const struct u256 *b)
{ /* returns borrow */
   unsigned __int128 br = 0;
   for (int i = 0; i < 4; i++) {
      unsigned __int128 t = (unsigned __int128)a->v[i] - b->v[i] - br;
      r->v[i]             = (uint64_t)t;
      br                  = (t >> 64U) & 1U;
   }
   return (uint64_t)br;
}

static void u256_from_be(struct u256 *r, const uint8_t b[32])
{
   for (int i = 0; i < 4; i++) {
      const uint8_t *p = b + ((size_t)(3 - i) * 8);
      r->v[i]          = ((uint64_t)p[0] << 56U) | ((uint64_t)p[1] << 48U) |
                         ((uint64_t)p[2] << 40U) | ((uint64_t)p[3] << 32U) |
                         ((uint64_t)p[4] << 24U) | ((uint64_t)p[5] << 16U) |
                         ((uint64_t)p[6] << 8U) | (uint64_t)p[7];
   }
}

static void u256_to_be(const struct u256 *a, uint8_t b[32])
{
   for (int i = 0; i < 4; i++) {
      uint8_t *p = b + ((size_t)(3 - i) * 8);
      uint64_t x = a->v[i];
      for (int j = 0; j < 8; j++)
         p[j] = (uint8_t)(x >> (unsigned)(56 - (8 * j)));
   }
}

static int u256_bit(const struct u256 *a, int i)
{
   return (int)((a->v[(unsigned)i >> 6U] >> ((unsigned)i & 63U)) & 1U);
}

static uint64_t u256_shl1(struct u256 *a, int carry_in)
{ /* a = (a<<1)|carry_in; returns bit shifted out */
   uint64_t c = (unsigned)carry_in & 1U;
   for (int i = 0; i < 4; i++) {
      uint64_t nc = a->v[i] >> 63U;
      a->v[i]     = (a->v[i] << 1U) | c;
      c           = nc;
   }
   return c;
}

/* 256x256 -> 512 (as struct u256 lo, hi) */
static void u256_mul(const struct u256 *a, const struct u256 *b,
                     struct u256 *lo, struct u256 *hi)
{
   uint64_t r[8] = {0};
   for (int i = 0; i < 4; i++) {
      unsigned __int128 carry = 0;
      for (int j = 0; j < 4; j++) {
         unsigned __int128 t =
             ((unsigned __int128)a->v[i] * b->v[j]) + r[i + j] + carry;
         r[i + j] = (uint64_t)t;
         carry    = t >> 64U;
      }
      r[i + 4] += (uint64_t)carry;
   }
   for (int i = 0; i < 4; i++) {
      lo->v[i] = r[i];
      hi->v[i] = r[i + 4];
   }
}

/* reduce a 512-bit value (lo,hi) mod m, bitwise. Result < m. */
static void mod512(const struct u256 *lo, const struct u256 *hi,
                   const struct u256 *m, struct u256 *out)
{
   struct u256 r = {
       {0, 0, 0, 0}
   };
   for (int i = 511; i >= 0; i--) {
      int bit = (i < 256) ? u256_bit(lo, i) : u256_bit(hi, i - 256);
      uint64_t co =
          u256_shl1(&r, bit); /* co==1 means value overflowed 256 bits */
      if (co || u256_cmp(&r, m) >= 0) {
         struct u256 t;
         u256_sub(&t, &r, m);
         r = t;
      }
   }
   *out = r;
}

/* ---- modulus context (p for field, n for scalars) ---- */
static struct u256 field_p, order_n;
static struct u256 zero = {
    {0, 0, 0, 0}
};
static struct u256 one = {
    {1, 0, 0, 0}
};

static void modmul(struct u256 *r, const struct u256 *a, const struct u256 *b,
                   const struct u256 *m)
{
   struct u256 lo;
   struct u256 hi;
   u256_mul(a, b, &lo, &hi);
   mod512(&lo, &hi, m, r);
}

static void modadd(struct u256 *r, const struct u256 *a, const struct u256 *b,
                   const struct u256 *m)
{
   struct u256 t;
   uint64_t c = u256_add(&t, a, b);
   if (c || u256_cmp(&t, m) >= 0) {
      struct u256 u;
      u256_sub(&u, &t, m);
      *r = u;
   } else {
      *r = t;
   }
}

static void modsub(struct u256 *r, const struct u256 *a, const struct u256 *b,
                   const struct u256 *m)
{
   struct u256 t;
   uint64_t br = u256_sub(&t, a, b);
   if (br) {
      struct u256 u;
      u256_add(&u, &t, m);
      *r = u;
   } else {
      *r = t;
   }
}

/* modular inverse via Fermat: a^(m-2) mod m (m prime) */
static void modinv(struct u256 *r, const struct u256 *a, const struct u256 *m)
{
   struct u256 e;
   struct u256 two = {
       {2, 0, 0, 0}
   };
   u256_sub(&e, m, &two); /* e = m-2 */
   struct u256 res  = one;
   struct u256 base = *a;
   for (int i = 0; i < 256; i++) {
      if (u256_bit(&e, i)) {
         struct u256 t;
         modmul(&t, &res, &base, m);
         res = t;
      }
      struct u256 t;
      modmul(&t, &base, &base, m);
      base = t;
   }
   *r = res;
}

/* field (mod p) shortcuts */
static void fadd(struct u256 *r, const struct u256 *a, const struct u256 *b)
{
   modadd(r, a, b, &field_p);
}

static void fsub(struct u256 *r, const struct u256 *a, const struct u256 *b)
{
   modsub(r, a, b, &field_p);
}

static void fmul(struct u256 *r, const struct u256 *a, const struct u256 *b)
{
   modmul(r, a, b, &field_p);
}

static void finv(struct u256 *r, const struct u256 *a)
{
   modinv(r, a, &field_p);
}

/* ---- curve ---- */
static struct u256 curve_b;
struct jpoint p256_g;

void p256_init(void)
{
   static const uint8_t p_be[32] = {
       0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x01, 0,    0,    0,
       0,    0,    0,    0,    0,    0,    0,    0,    0,    0xff, 0xff,
       0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
   static const uint8_t n_be[32] = {
       0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff,
       0xff, 0xff, 0xff, 0xff, 0xff, 0xbc, 0xe6, 0xfa, 0xad, 0xa7, 0x17,
       0x9e, 0x84, 0xf3, 0xb9, 0xca, 0xc2, 0xfc, 0x63, 0x25, 0x51};
   static const uint8_t b_be[32] = {
       0x5a, 0xc6, 0x35, 0xd8, 0xaa, 0x3a, 0x93, 0xe7, 0xb3, 0xeb, 0xbd,
       0x55, 0x76, 0x98, 0x86, 0xbc, 0x65, 0x1d, 0x06, 0xb0, 0xcc, 0x53,
       0xb0, 0xf6, 0x3b, 0xce, 0x3c, 0x3e, 0x27, 0xd2, 0x60, 0x4b};
   static const uint8_t gx[32] = {
       0x6b, 0x17, 0xd1, 0xf2, 0xe1, 0x2c, 0x42, 0x47, 0xf8, 0xbc, 0xe6,
       0xe5, 0x63, 0xa4, 0x40, 0xf2, 0x77, 0x03, 0x7d, 0x81, 0x2d, 0xeb,
       0x33, 0xa0, 0xf4, 0xa1, 0x39, 0x45, 0xd8, 0x98, 0xc2, 0x96};
   static const uint8_t gy[32] = {
       0x4f, 0xe3, 0x42, 0xe2, 0xfe, 0x1a, 0x7f, 0x9b, 0x8e, 0xe7, 0xeb,
       0x4a, 0x7c, 0x0f, 0x9e, 0x16, 0x2b, 0xce, 0x33, 0x57, 0x6b, 0x31,
       0x5e, 0xce, 0xcb, 0xb6, 0x40, 0x68, 0x37, 0xbf, 0x51, 0xf5};
   u256_from_be(&field_p, p_be);
   u256_from_be(&order_n, n_be);
   u256_from_be(&curve_b, b_be);
   u256_from_be(&p256_g.X, gx);
   u256_from_be(&p256_g.Y, gy);
   p256_g.Z = one;
}

int p256_is_inf(const struct jpoint *P_)
{
   return u256_iszero(&P_->Z);
}

/* Jacobian doubling, a = -3 */
static void jdouble(struct jpoint *r, const struct jpoint *p)
{
   if (u256_iszero(&p->Z) || u256_iszero(&p->Y)) {
      r->X = one;
      r->Y = one;
      r->Z = zero;
      return;
   }
   struct u256 a;
   struct u256 bv;
   struct u256 c;
   struct u256 d;
   struct u256 t;
   struct u256 t2;
   struct u256 z2;
   struct u256 z4;
   fmul(&z2, &p->Z, &p->Z);
   fmul(&z4, &z2, &z2);
   fmul(&a, &p->X, &p->X); /* X^2 */
   struct u256 three_x2;
   fadd(&three_x2, &a, &a);
   fadd(&three_x2, &three_x2, &a); /* 3X^2 */
   struct u256 three_z4;
   fadd(&three_z4, &z4, &z4);
   fadd(&three_z4, &three_z4, &z4); /* 3Z^4 */
   struct u256 m;
   fsub(&m, &three_x2, &three_z4); /* M = 3X^2 - 3Z^4  (a=-3) */
   fmul(&t, &p->Y, &p->Y);         /* Y^2 */
   fmul(&bv, &p->X, &t);
   fadd(&bv, &bv, &bv);
   fadd(&bv, &bv, &bv); /* S = 4*X*Y^2 */
   fmul(&c, &t, &t);
   fadd(&c, &c, &c);
   fadd(&c, &c, &c);
   fadd(&c, &c, &c); /* 8*Y^4 */
   fmul(&d, &m, &m);
   struct u256 two_b;
   fadd(&two_b, &bv, &bv);
   fsub(&d, &d, &two_b); /* X' = M^2 - 2S */
   fsub(&t2, &bv, &d);
   fmul(&t2, &m, &t2);
   fsub(&t2, &t2, &c); /* Y' = M(S-X') - 8Y^4 */
   struct u256 zr;
   fmul(&zr, &p->Y, &p->Z);
   fadd(&zr, &zr, &zr); /* Z' = 2YZ */
   r->X = d;
   r->Y = t2;
   r->Z = zr;
}

/* Jacobian add */
void p256_padd(struct jpoint *r, const struct jpoint *P_,
               const struct jpoint *Q)
{
   if (u256_iszero(&P_->Z)) {
      *r = *Q;
      return;
   }
   if (u256_iszero(&Q->Z)) {
      *r = *P_;
      return;
   }
   struct u256 z1z1;
   struct u256 z2z2;
   struct u256 u1;
   struct u256 u2;
   struct u256 s1;
   struct u256 s2;
   struct u256 t;
   fmul(&z1z1, &P_->Z, &P_->Z);
   fmul(&z2z2, &Q->Z, &Q->Z);
   fmul(&u1, &P_->X, &z2z2);
   fmul(&u2, &Q->X, &z1z1);
   fmul(&t, &Q->Z, &z2z2);
   fmul(&s1, &P_->Y, &t);
   fmul(&t, &P_->Z, &z1z1);
   fmul(&s2, &Q->Y, &t);
   if (u256_cmp(&u1, &u2) == 0) {
      if (u256_cmp(&s1, &s2) == 0) {
         jdouble(r, P_);
         return;
      }
      r->X = one;
      r->Y = one;
      r->Z = zero;
      return; /* infinity */
   }
   struct u256 h;
   struct u256 rr;
   struct u256 h2;
   struct u256 h3;
   struct u256 u1h2;
   struct u256 x3;
   struct u256 y3;
   struct u256 z3;
   fsub(&h, &u2, &u1);
   fsub(&rr, &s2, &s1);
   fmul(&h2, &h, &h);
   fmul(&h3, &h2, &h);
   fmul(&u1h2, &u1, &h2);
   fmul(&x3, &rr, &rr);
   fsub(&x3, &x3, &h3);
   struct u256 two_u1h2;
   fadd(&two_u1h2, &u1h2, &u1h2);
   fsub(&x3, &x3, &two_u1h2);
   fsub(&y3, &u1h2, &x3);
   fmul(&y3, &rr, &y3);
   struct u256 s1h3;
   fmul(&s1h3, &s1, &h3);
   fsub(&y3, &y3, &s1h3);
   fmul(&z3, &P_->Z, &Q->Z);
   fmul(&z3, &z3, &h);
   r->X = x3;
   r->Y = y3;
   r->Z = z3;
}

void p256_mul(struct jpoint *r, const struct u256 *k, const struct jpoint *P_)
{
   struct jpoint acc;
   acc.X = one;
   acc.Y = one;
   acc.Z = zero; /* infinity */
   for (int i = 255; i >= 0; i--) {
      struct jpoint d;
      jdouble(&d, &acc);
      acc = d;
      if (u256_bit(k, i)) {
         struct jpoint a;
         p256_padd(&a, &acc, P_);
         acc = a;
      }
   }
   *r = acc;
}

void p256_mul_g(struct jpoint *r, const struct u256 *k)
{
   p256_mul(r, k, &p256_g);
}

void p256_pneg(struct jpoint *r, const struct jpoint *P_)
{
   r->X = P_->X;
   fsub(&r->Y, &field_p, &P_->Y);
   r->Z = P_->Z;
}

int p256_to_xy(const struct jpoint *P_, uint8_t x[32], uint8_t y[32])
{
   if (u256_iszero(&P_->Z))
      return 0;
   struct u256 zi;
   struct u256 zi2;
   struct u256 zi3;
   struct u256 ax;
   struct u256 ay;
   finv(&zi, &P_->Z);
   fmul(&zi2, &zi, &zi);
   fmul(&zi3, &zi2, &zi);
   fmul(&ax, &P_->X, &zi2);
   fmul(&ay, &P_->Y, &zi3);
   u256_to_be(&ax, x);
   u256_to_be(&ay, y);
   return 1;
}

int p256_eq(const struct jpoint *A, const struct jpoint *B)
{
   int ia = u256_iszero(&A->Z);
   int ib = u256_iszero(&B->Z);
   if (ia || ib)
      return ia && ib;
   uint8_t xa[32];
   uint8_t ya[32];
   uint8_t xb[32];
   uint8_t yb[32];
   p256_to_xy(A, xa, ya);
   p256_to_xy(B, xb, yb);
   for (int i = 0; i < 32; i++)
      if (xa[i] != xb[i] || ya[i] != yb[i])
         return 0;
   return 1;
}

int p256_from_xy(struct jpoint *P_, const uint8_t x[32], const uint8_t y[32])
{
   u256_from_be(&P_->X, x);
   u256_from_be(&P_->Y, y);
   P_->Z = one;
   if (u256_cmp(&P_->X, &field_p) >= 0 || u256_cmp(&P_->Y, &field_p) >= 0)
      return 0;
   /* on-curve: y^2 == x^3 - 3x + b */
   struct u256 y2;
   struct u256 x3;
   struct u256 t;
   struct u256 rhs;
   fmul(&y2, &P_->Y, &P_->Y);
   fmul(&x3, &P_->X, &P_->X);
   fmul(&x3, &x3, &P_->X);
   fadd(&t, &P_->X, &P_->X);
   fadd(&t, &t, &P_->X); /* 3x */
   fsub(&rhs, &x3, &t);
   fadd(&rhs, &rhs, &curve_b);
   return u256_cmp(&y2, &rhs) == 0;
}

void p256_uncompressed(const struct jpoint *P_, uint8_t out[65])
{
   out[0] = 0x04;
   p256_to_xy(P_, out + 1, out + 33);
}

/* scalars mod n */
void p256_sc_from_be(struct u256 *r, const uint8_t be[32])
{
   struct u256 t;
   u256_from_be(&t, be);
   if (u256_cmp(&t, &order_n) >= 0) {
      struct u256 u;
      u256_sub(&u, &t, &order_n);
      *r = u;
   } else {
      *r = t;
   }
}

void p256_sc_mul(struct u256 *r, const struct u256 *a, const struct u256 *b)
{
   modmul(r, a, b, &order_n);
}

void p256_sc_add(struct u256 *r, const struct u256 *a, const struct u256 *b)
{
   modadd(r, a, b, &order_n);
}

void p256_sc_sub(struct u256 *r, const struct u256 *a, const struct u256 *b)
{
   modsub(r, a, b, &order_n);
}

void p256_sc_inv(struct u256 *r, const struct u256 *a)
{
   modinv(r, a, &order_n);
}

void p256_sc_neg(struct u256 *r, const struct u256 *a)
{
   if (u256_iszero(a)) {
      *r = *a;
      return;
   }
   u256_sub(r, &order_n, a);
}

int p256_sc_is_zero(const struct u256 *a)
{
   return u256_iszero(a);
}
