/* Portable NIST P-256. See p256.h. Correctness-first (not constant-time). */
#include "p256.h"

typedef unsigned __int128 u128;

/* ---- 256-bit little-endian limb arithmetic ---- */
static int u256_iszero(const u256 *a){ return (a->v[0]|a->v[1]|a->v[2]|a->v[3])==0; }
static int u256_cmp(const u256 *a, const u256 *b){          /* -1,0,1 */
    for (int i = 3; i >= 0; i--) { if (a->v[i] < b->v[i]) return -1; if (a->v[i] > b->v[i]) return 1; }
    return 0;
}
static uint64_t u256_add(u256 *r, const u256 *a, const u256 *b){ /* returns carry */
    u128 c = 0;
    for (int i = 0; i < 4; i++){ c += (u128)a->v[i] + b->v[i]; r->v[i] = (uint64_t)c; c >>= 64; }
    return (uint64_t)c;
}
static uint64_t u256_sub(u256 *r, const u256 *a, const u256 *b){ /* returns borrow */
    u128 br = 0;
    for (int i = 0; i < 4; i++){ u128 t = (u128)a->v[i] - b->v[i] - br; r->v[i] = (uint64_t)t; br = (t >> 64) & 1; }
    return (uint64_t)br;
}
static void u256_from_be(u256 *r, const uint8_t b[32]){
    for (int i = 0; i < 4; i++){
        const uint8_t *p = b + (3 - i) * 8;
        r->v[i] = ((uint64_t)p[0]<<56)|((uint64_t)p[1]<<48)|((uint64_t)p[2]<<40)|((uint64_t)p[3]<<32)
                | ((uint64_t)p[4]<<24)|((uint64_t)p[5]<<16)|((uint64_t)p[6]<<8)|(uint64_t)p[7];
    }
}
static void u256_to_be(const u256 *a, uint8_t b[32]){
    for (int i = 0; i < 4; i++){
        uint8_t *p = b + (3 - i) * 8; uint64_t x = a->v[i];
        for (int j = 0; j < 8; j++) p[j] = (uint8_t)(x >> (56 - 8*j));
    }
}
static int u256_bit(const u256 *a, int i){ return (int)((a->v[i>>6] >> (i & 63)) & 1); }
static uint64_t u256_shl1(u256 *a, int carry_in){  /* a = (a<<1)|carry_in; returns bit shifted out */
    uint64_t c = carry_in & 1;
    for (int i = 0; i < 4; i++){ uint64_t nc = a->v[i] >> 63; a->v[i] = (a->v[i] << 1) | c; c = nc; }
    return c;
}

/* 256x256 -> 512 (as u256 lo, hi) */
static void u256_mul(const u256 *a, const u256 *b, u256 *lo, u256 *hi){
    uint64_t r[8] = {0};
    for (int i = 0; i < 4; i++){
        u128 carry = 0;
        for (int j = 0; j < 4; j++){
            u128 t = (u128)a->v[i] * b->v[j] + r[i+j] + carry;
            r[i+j] = (uint64_t)t; carry = t >> 64;
        }
        r[i+4] += (uint64_t)carry;
    }
    for (int i = 0; i < 4; i++){ lo->v[i] = r[i]; hi->v[i] = r[i+4]; }
}

/* reduce a 512-bit value (lo,hi) mod m, bitwise. Result < m. */
static void mod512(const u256 *lo, const u256 *hi, const u256 *m, u256 *out){
    u256 r = {{0,0,0,0}};
    for (int i = 511; i >= 0; i--){
        int bit = (i < 256) ? u256_bit(lo, i) : u256_bit(hi, i - 256);
        uint64_t co = u256_shl1(&r, bit);   /* co==1 means value overflowed 256 bits */
        if (co || u256_cmp(&r, m) >= 0){ u256 t; u256_sub(&t, &r, m); r = t; }
    }
    *out = r;
}

/* ---- modulus context (p for field, n for scalars) ---- */
static u256 P, N;
static u256 ZERO = {{0,0,0,0}};
static u256 ONE  = {{1,0,0,0}};

static void modmul(u256 *r, const u256 *a, const u256 *b, const u256 *m){
    u256 lo, hi; u256_mul(a, b, &lo, &hi); mod512(&lo, &hi, m, r);
}
static void modadd(u256 *r, const u256 *a, const u256 *b, const u256 *m){
    u256 t; uint64_t c = u256_add(&t, a, b);
    if (c || u256_cmp(&t, m) >= 0){ u256 u; u256_sub(&u, &t, m); *r = u; } else *r = t;
}
static void modsub(u256 *r, const u256 *a, const u256 *b, const u256 *m){
    u256 t; uint64_t br = u256_sub(&t, a, b);
    if (br){ u256 u; u256_add(&u, &t, m); *r = u; } else *r = t;
}
/* modular inverse via Fermat: a^(m-2) mod m (m prime) */
static void modinv(u256 *r, const u256 *a, const u256 *m){
    u256 e, two = {{2,0,0,0}}; u256_sub(&e, m, &two);   /* e = m-2 */
    u256 res = ONE, base = *a;
    for (int i = 0; i < 256; i++){
        if (u256_bit(&e, i)){ u256 t; modmul(&t, &res, &base, m); res = t; }
        u256 t; modmul(&t, &base, &base, m); base = t;
    }
    *r = res;
}

/* field (mod p) shortcuts */
static void fadd(u256 *r, const u256 *a, const u256 *b){ modadd(r, a, b, &P); }
static void fsub(u256 *r, const u256 *a, const u256 *b){ modsub(r, a, b, &P); }
static void fmul(u256 *r, const u256 *a, const u256 *b){ modmul(r, a, b, &P); }
static void finv(u256 *r, const u256 *a){ modinv(r, a, &P); }

/* ---- curve ---- */
static u256 B_;
jpoint P256_G;

void p256_init(void){
    static const uint8_t p_be[32]={0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x01,0,0,0,0,0,0,0,0,0,0,0,0,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff};
    static const uint8_t n_be[32]={0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xbc,0xe6,0xfa,0xad,0xa7,0x17,0x9e,0x84,0xf3,0xb9,0xca,0xc2,0xfc,0x63,0x25,0x51};
    static const uint8_t b_be[32]={0x5a,0xc6,0x35,0xd8,0xaa,0x3a,0x93,0xe7,0xb3,0xeb,0xbd,0x55,0x76,0x98,0x86,0xbc,0x65,0x1d,0x06,0xb0,0xcc,0x53,0xb0,0xf6,0x3b,0xce,0x3c,0x3e,0x27,0xd2,0x60,0x4b};
    static const uint8_t gx[32]={0x6b,0x17,0xd1,0xf2,0xe1,0x2c,0x42,0x47,0xf8,0xbc,0xe6,0xe5,0x63,0xa4,0x40,0xf2,0x77,0x03,0x7d,0x81,0x2d,0xeb,0x33,0xa0,0xf4,0xa1,0x39,0x45,0xd8,0x98,0xc2,0x96};
    static const uint8_t gy[32]={0x4f,0xe3,0x42,0xe2,0xfe,0x1a,0x7f,0x9b,0x8e,0xe7,0xeb,0x4a,0x7c,0x0f,0x9e,0x16,0x2b,0xce,0x33,0x57,0x6b,0x31,0x5e,0xce,0xcb,0xb6,0x40,0x68,0x37,0xbf,0x51,0xf5};
    u256_from_be(&P, p_be); u256_from_be(&N, n_be); u256_from_be(&B_, b_be);
    u256_from_be(&P256_G.X, gx); u256_from_be(&P256_G.Y, gy); P256_G.Z = ONE;
}

int p256_is_inf(const jpoint *P_){ return u256_iszero(&P_->Z); }

/* Jacobian doubling, a = -3 */
static void jdouble(jpoint *r, const jpoint *p){
    if (u256_iszero(&p->Z) || u256_iszero(&p->Y)){ r->X=ONE; r->Y=ONE; r->Z=ZERO; return; }
    u256 A,Bv,C,D,t,t2,z2,z4;
    fmul(&z2,&p->Z,&p->Z); fmul(&z4,&z2,&z2);
    fmul(&A,&p->X,&p->X);                 /* X^2 */
    u256 three_x2; fadd(&three_x2,&A,&A); fadd(&three_x2,&three_x2,&A); /* 3X^2 */
    u256 three_z4; fadd(&three_z4,&z4,&z4); fadd(&three_z4,&three_z4,&z4);/* 3Z^4 */
    u256 M; fsub(&M,&three_x2,&three_z4);  /* M = 3X^2 - 3Z^4  (a=-3) */
    fmul(&t,&p->Y,&p->Y);                  /* Y^2 */
    fmul(&Bv,&p->X,&t); fadd(&Bv,&Bv,&Bv); fadd(&Bv,&Bv,&Bv);  /* S = 4*X*Y^2 */
    fmul(&C,&t,&t); fadd(&C,&C,&C); fadd(&C,&C,&C); fadd(&C,&C,&C); /* 8*Y^4 */
    fmul(&D,&M,&M); u256 twoB; fadd(&twoB,&Bv,&Bv); fsub(&D,&D,&twoB); /* X' = M^2 - 2S */
    fsub(&t2,&Bv,&D); fmul(&t2,&M,&t2); fsub(&t2,&t2,&C);       /* Y' = M(S-X') - 8Y^4 */
    u256 Zr; fmul(&Zr,&p->Y,&p->Z); fadd(&Zr,&Zr,&Zr);         /* Z' = 2YZ */
    r->X=D; r->Y=t2; r->Z=Zr;
}

/* Jacobian add */
void p256_padd(jpoint *r, const jpoint *P_, const jpoint *Q){
    if (u256_iszero(&P_->Z)){ *r=*Q; return; }
    if (u256_iszero(&Q->Z)){ *r=*P_; return; }
    u256 z1z1,z2z2,u1,u2,s1,s2,t;
    fmul(&z1z1,&P_->Z,&P_->Z); fmul(&z2z2,&Q->Z,&Q->Z);
    fmul(&u1,&P_->X,&z2z2); fmul(&u2,&Q->X,&z1z1);
    fmul(&t,&Q->Z,&z2z2); fmul(&s1,&P_->Y,&t);
    fmul(&t,&P_->Z,&z1z1); fmul(&s2,&Q->Y,&t);
    if (u256_cmp(&u1,&u2)==0){
        if (u256_cmp(&s1,&s2)==0){ jdouble(r,P_); return; }
        r->X=ONE; r->Y=ONE; r->Z=ZERO; return;   /* infinity */
    }
    u256 H,R,h2,h3,u1h2,X3,Y3,Z3;
    fsub(&H,&u2,&u1); fsub(&R,&s2,&s1);
    fmul(&h2,&H,&H); fmul(&h3,&h2,&H); fmul(&u1h2,&u1,&h2);
    fmul(&X3,&R,&R); fsub(&X3,&X3,&h3); u256 two_u1h2; fadd(&two_u1h2,&u1h2,&u1h2); fsub(&X3,&X3,&two_u1h2);
    fsub(&Y3,&u1h2,&X3); fmul(&Y3,&R,&Y3); u256 s1h3; fmul(&s1h3,&s1,&h3); fsub(&Y3,&Y3,&s1h3);
    fmul(&Z3,&P_->Z,&Q->Z); fmul(&Z3,&Z3,&H);
    r->X=X3; r->Y=Y3; r->Z=Z3;
}

void p256_mul(jpoint *r, const u256 *k, const jpoint *P_){
    jpoint acc; acc.X=ONE; acc.Y=ONE; acc.Z=ZERO;   /* infinity */
    for (int i = 255; i >= 0; i--){
        jpoint d; jdouble(&d,&acc); acc=d;
        if (u256_bit(k,i)){ jpoint a; p256_padd(&a,&acc,P_); acc=a; }
    }
    *r=acc;
}
void p256_mul_g(jpoint *r, const u256 *k){ p256_mul(r,k,&P256_G); }

void p256_pneg(jpoint *r, const jpoint *P_){ r->X=P_->X; fsub(&r->Y,&P,&P_->Y); r->Z=P_->Z; }

int p256_to_xy(const jpoint *P_, uint8_t x[32], uint8_t y[32]){
    if (u256_iszero(&P_->Z)) return 0;
    u256 zi,zi2,zi3,ax,ay;
    finv(&zi,&P_->Z); fmul(&zi2,&zi,&zi); fmul(&zi3,&zi2,&zi);
    fmul(&ax,&P_->X,&zi2); fmul(&ay,&P_->Y,&zi3);
    u256_to_be(&ax,x); u256_to_be(&ay,y);
    return 1;
}
int p256_eq(const jpoint *A, const jpoint *B){
    int ia=u256_iszero(&A->Z), ib=u256_iszero(&B->Z);
    if (ia||ib) return ia&&ib;
    uint8_t xa[32],ya[32],xb[32],yb[32];
    p256_to_xy(A,xa,ya); p256_to_xy(B,xb,yb);
    for (int i=0;i<32;i++) if (xa[i]!=xb[i]||ya[i]!=yb[i]) return 0;
    return 1;
}
int p256_from_xy(jpoint *P_, const uint8_t x[32], const uint8_t y[32]){
    u256_from_be(&P_->X,x); u256_from_be(&P_->Y,y); P_->Z=ONE;
    if (u256_cmp(&P_->X,&P)>=0 || u256_cmp(&P_->Y,&P)>=0) return 0;
    /* on-curve: y^2 == x^3 - 3x + b */
    u256 y2,x3,t,rhs;
    fmul(&y2,&P_->Y,&P_->Y);
    fmul(&x3,&P_->X,&P_->X); fmul(&x3,&x3,&P_->X);
    fadd(&t,&P_->X,&P_->X); fadd(&t,&t,&P_->X);     /* 3x */
    fsub(&rhs,&x3,&t); fadd(&rhs,&rhs,&B_);
    return u256_cmp(&y2,&rhs)==0;
}
void p256_uncompressed(const jpoint *P_, uint8_t out[65]){
    out[0]=0x04; p256_to_xy(P_, out+1, out+33);
}

/* scalars mod n */
void p256_sc_from_be(u256 *r, const uint8_t be[32]){
    u256 t; u256_from_be(&t,be);
    if (u256_cmp(&t,&N)>=0){ u256 u; u256_sub(&u,&t,&N); *r=u; } else *r=t;
}
void p256_sc_mul(u256 *r, const u256 *a, const u256 *b){ modmul(r,a,b,&N); }
void p256_sc_add(u256 *r, const u256 *a, const u256 *b){ modadd(r,a,b,&N); }
void p256_sc_sub(u256 *r, const u256 *a, const u256 *b){ modsub(r,a,b,&N); }
void p256_sc_inv(u256 *r, const u256 *a){ modinv(r,a,&N); }
void p256_sc_neg(u256 *r, const u256 *a){ if (u256_iszero(a)) { *r=*a; return; } u256_sub(r,&N,a); }
int  p256_sc_is_zero(const u256 *a){ return u256_iszero(a); }
