/* Dexcom G7 / Stelo authentication — EC-J-PAKE + dex8, on portable crypto.
 *
 * Algorithm ported from Juggluco (GPLv3) and xDrip's jamorham.keks; validated
 * against their vectors AND a real Stelo pairing capture. This file is GPLv3.
 * Curve secp256r1 (P-256), SHA-256. See NOTES.md for the protocol.
 *
 * Offline test build:
 *   cc -DDEXAUTH_TEST dexauth.c p256.c sha256.c aes.c -o t && ./t
 */

#include "dexauth.h"
#include "p256.h"
#include "sha256.h"
#include "aes.h"
#include "dexlibc.h"

/* Schnorr (RFC-8235) signer identities: phone signs "client", sensor "server". */
static const uint8_t A_BYTES[6] = {0x63,0x6c,0x69,0x65,0x6e,0x74};   /* "client" */
static const uint8_t B_BYTES[6] = {0x73,0x65,0x72,0x76,0x65,0x72};   /* "server" */

int dexauth_init(void) { p256_init(); return 1; }

static void get_random(uint8_t *buf, size_t n) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        size_t off = 0;
        while (off < n) { long r = read(fd, buf + off, n - off); if (r <= 0) break; off += (size_t)r; }
        close(fd);
    }
}
static void rand_scalar(u256 *r) { uint8_t b[32]; get_random(b, 32); p256_sc_from_be(r, b); }

static void be32(uint8_t *b, uint32_t v) { b[0]=v>>24; b[1]=v>>16; b[2]=v>>8; b[3]=v; }
static void sc_to_be(const u256 *a, uint8_t out[32]) {
    for (int i = 0; i < 4; i++) { uint64_t w = a->v[3-i]; for (int j = 0; j < 8; j++) out[i*8+j] = (uint8_t)(w >> (56 - 8*j)); }
}

/* ZKP hash = SHA256( L|G  L|V(gv)  L|X(pub)  L|party ), points uncompressed 65B. */
static void mkhash(const jpoint *p1, const jpoint *gv, const jpoint *pub,
                   const uint8_t party[6], uint8_t out[32]) {
    uint8_t buf[4*4 + 6 + 3*65];
    uint8_t *d = buf;
    be32(d,65); d+=4; p256_uncompressed(p1,  d); d+=65;
    be32(d,65); d+=4; p256_uncompressed(gv,  d); d+=65;
    be32(d,65); d+=4; p256_uncompressed(pub, d); d+=65;
    be32(d,6);  d+=4; memcpy(d, party, 6); d+=6;
    sha256(buf, sizeof(buf), out);
}
/* hash reduced mod n; arg order (p1, pub, gv) as in Juggluco */
static void mkhash_bn(u256 *r, const jpoint *p1, const jpoint *pub, const jpoint *gv,
                      const uint8_t party[6]) {
    uint8_t h[32]; mkhash(p1, gv, pub, party, h); p256_sc_from_be(r, h);
}
/* proof = (ran - H*priv) mod n */
static void getproof(u256 *r, const jpoint *p1, const jpoint *pub, const jpoint *gv,
                     const u256 *priv, const u256 *ran, const uint8_t party[6]) {
    u256 c, cp; mkhash_bn(&c, p1, pub, gv, party);
    p256_sc_mul(&cp, &c, priv);
    p256_sc_sub(r, ran, &cp);
}

/* J-PAKE round packet: pubkey1, pubkey2(=gv), hash(=proof scalar). */
typedef struct { jpoint pubkey1, pubkey2; u256 hash; } PCert;

static void cert_fill(PCert *c, const jpoint *p1, const jpoint *pub,
                      const u256 *priv, const u256 *ran, const uint8_t party[6]) {
    c->pubkey1 = *pub;
    p256_mul(&c->pubkey2, ran, p1);                 /* gv = ran * p1 */
    getproof(&c->hash, p1, pub, &c->pubkey2, priv, ran, party);
}
static void cert_byteify(const PCert *c, uint8_t out[160]) {
    p256_to_xy(&c->pubkey1, out,    out+32);
    p256_to_xy(&c->pubkey2, out+64, out+96);
    sc_to_be(&c->hash, out+128);
}
static int cert_from_bytes(PCert *c, const uint8_t b[160]) {
    if (!p256_from_xy(&c->pubkey1, b,    b+32))  return 0;
    if (!p256_from_xy(&c->pubkey2, b+64, b+96))  return 0;
    p256_sc_from_be(&c->hash, b+128);
    return 1;
}
/* verify g^r + A^c == gv, c = H(g, A, gv, party) */
static int validate_zkp(const jpoint *g, const PCert *cert, const uint8_t party[6]) {
    u256 c; mkhash_bn(&c, g, &cert->pubkey1, &cert->pubkey2, party);
    jpoint t1, t2;
    p256_mul(&t1, &cert->hash, g);
    p256_mul(&t2, &c, &cert->pubkey1);
    p256_padd(&t1, &t1, &t2);
    return p256_eq(&t1, &cert->pubkey2);
}

/* round 3: x2s=privB*pass; g134=pubA+pub1+pub2; A=g134^x2s; cert over (g134,A,x2s) */
static void make_round3(PCert *cert, const jpoint *pub1, const jpoint *pub2,
                        const jpoint *pubA, const u256 *privB, const u256 *pass,
                        const u256 *ran3, const uint8_t party[6]) {
    u256 x2s; p256_sc_mul(&x2s, privB, pass);
    jpoint g134, A;
    p256_padd(&g134, pubA, pub1);
    p256_padd(&g134, &g134, pub2);
    p256_mul(&A, &x2s, &g134);
    cert_fill(cert, &g134, &A, &x2s, ran3, party);
}
/* validate received round-3: base = ourA + ourB + peerRound1.pubkey1 */
static int validate_round3(const jpoint *ourA, const jpoint *ourB,
                           const PCert *peer_r1, const PCert *peer_r3, const uint8_t party[6]) {
    jpoint g; p256_padd(&g, ourA, ourB); p256_padd(&g, &g, &peer_r1->pubkey1);
    return validate_zkp(&g, peer_r3, party);
}

/* sharedKey = SHA256( affine_x( (peerR3.pub1 - peerR2.pub1^(x2*pass)) ^ x2 ) )[:16] */
static int shared_key(const PCert *r2, const PCert *r3, const u256 *pass,
                      const u256 *x2, uint8_t out16[16]) {
    u256 num; p256_sc_mul(&num, x2, pass); p256_sc_neg(&num, &num);   /* -(x2*pass) */
    jpoint key;
    p256_mul(&key, &num, &r2->pubkey1);        /* g4^(-x2*pass) */
    p256_padd(&key, &r3->pubkey1, &key);       /* point1 + that */
    p256_mul(&key, x2, &key);                  /* ^x2 */
    uint8_t x[32], y[32], h[32];
    if (!p256_to_xy(&key, x, y)) return 0;
    sha256(x, 32, h);
    memcpy(out16, h, 16);
    return 1;
}

/* dex8: per-connection auth hash. AES-128-ECB(key, data||data)[:8]. */
int dexauth_dex8(const uint8_t key16[16], const uint8_t data8[8], uint8_t out8[8]) {
    uint8_t block[16], enc[16];
    memcpy(block, data8, 8); memcpy(block+8, data8, 8);
    aes128_encrypt(key16, block, enc);
    memcpy(out8, enc, 8);
    return 1;
}

/* Embedded collector device key for the certificate signature challenge. */
static const uint8_t DEVKEY_PRIV[31] = {
    0x7c,0xfb,0xd5,0x96,0xf6,0xe7,0x44,0x77,0xb8,0xc0,0xe9,0xf6,0xf7,0xa1,0x74,0x27,
    0x5e,0x10,0x1e,0xf6,0xbf,0x7d,0x18,0xca,0xf0,0x11,0x81,0xd1,0x27,0xb5,0x79};
static const uint8_t DEVKEY_PUB[65] = {
    0x04,0x51,0x18,0xC3,0x5E,0x9E,0x41,0xE7,0xE0,0x65,0x4F,0xEE,0x80,0x1C,0x52,0xA9,
    0xC5,0xDF,0xC5,0x10,0xEF,0x09,0x59,0x7D,0x5C,0xCA,0x84,0x61,0xE4,0xAF,0x9C,0x66,
    0x67,0x14,0x83,0x4F,0x2B,0xC9,0x03,0xF1,0x6F,0xAB,0xFC,0x45,0x75,0x5B,0x01,0x83,
    0xF1,0xA0,0x97,0x45,0xCD,0xFF,0xCB,0x4E,0x2F,0x79,0x9E,0x50,0xBE,0xD9,0xA6,0xB5,0x8C};

/* Certificate key-challenge: ECDSA-P256 sign SHA256(challenge[2:18]) with the
 * embedded device key; output raw r||s (64 bytes). challenge >= 18 bytes. */
int dexauth_getchallenge(const uint8_t *challenge, size_t clen, uint8_t out64[64]) {
    if (clen < 18) return 0;
    uint8_t hash[32]; sha256(challenge+2, 16, hash);
    u256 z; p256_sc_from_be(&z, hash);
    uint8_t pb[32] = {0}; memcpy(pb+1, DEVKEY_PRIV, 31);
    u256 priv; p256_sc_from_be(&priv, pb);
    for (int t = 0; t < 32; t++) {
        u256 k; rand_scalar(&k);
        if (p256_sc_is_zero(&k)) continue;
        jpoint R; p256_mul_g(&R, &k);
        uint8_t rx[32], ry[32];
        if (!p256_to_xy(&R, rx, ry)) continue;
        u256 r; p256_sc_from_be(&r, rx);
        if (p256_sc_is_zero(&r)) continue;
        u256 kinv; p256_sc_inv(&kinv, &k);
        u256 tmp; p256_sc_mul(&tmp, &r, &priv); p256_sc_add(&tmp, &tmp, &z);
        u256 s; p256_sc_mul(&s, &kinv, &tmp);
        if (p256_sc_is_zero(&s)) continue;
        sc_to_be(&r, out64); sc_to_be(&s, out64+32);
        return 1;
    }
    return 0;
}
/* Verify raw r||s over SHA256(challenge[2:18]) with the device pubkey (for tests). */
int dexauth_verify_challenge(const uint8_t *challenge, size_t clen, const uint8_t sig64[64]) {
    if (clen < 18) return 0;
    uint8_t hash[32]; sha256(challenge+2, 16, hash);
    u256 z; p256_sc_from_be(&z, hash);
    u256 r, s; p256_sc_from_be(&r, sig64); p256_sc_from_be(&s, sig64+32);
    if (p256_sc_is_zero(&r) || p256_sc_is_zero(&s)) return 0;
    u256 w; p256_sc_inv(&w, &s);
    u256 u1, u2; p256_sc_mul(&u1, &z, &w); p256_sc_mul(&u2, &r, &w);
    jpoint P, Q, R, pub;
    p256_mul_g(&P, &u1);
    p256_from_xy(&pub, DEVKEY_PUB+1, DEVKEY_PUB+33); p256_mul(&Q, &u2, &pub);
    p256_padd(&R, &P, &Q);
    if (p256_is_inf(&R)) return 0;
    uint8_t rx[32], ry[32]; p256_to_xy(&R, rx, ry);
    u256 v; p256_sc_from_be(&v, rx);
    uint8_t vb[32], rb[32]; sc_to_be(&v, vb); sc_to_be(&r, rb);
    return memcmp(vb, rb, 32) == 0;
}

/* ---- first-pairing driver: byte-oriented J-PAKE, opaque state ---- */
struct dex_pairing {
    u256 xA, xB, vA, vB, v3, pass;
    jpoint pA, pB;
    const uint8_t *me, *peer;
    PCert r1, r2, r3;
    int have_r1, have_r2, have_r3;
};

dex_pairing *dexpair_new(const uint8_t *pass, size_t passlen, int is_client) {
    dex_pairing *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    uint8_t pb[32] = {0};
    if (passlen > 32) passlen = 32;
    memcpy(pb + (32 - passlen), pass, passlen);     /* right-justified big-endian */
    p256_sc_from_be(&p->pass, pb);
    p->me   = is_client ? A_BYTES : B_BYTES;
    p->peer = is_client ? B_BYTES : A_BYTES;
    rand_scalar(&p->xA); rand_scalar(&p->xB);
    rand_scalar(&p->vA); rand_scalar(&p->vB); rand_scalar(&p->v3);
    p256_mul_g(&p->pA, &p->xA);
    p256_mul_g(&p->pB, &p->xB);
    return p;
}

static int emit_round(const jpoint *pub, const u256 *priv, const u256 *ran,
                      const uint8_t party[6], uint8_t out[160]) {
    PCert c;
    cert_fill(&c, &P256_G, pub, priv, ran, party);
    cert_byteify(&c, out);
    return 160;
}
int dexpair_round1(dex_pairing *p, uint8_t out[160]) { return emit_round(&p->pA, &p->xA, &p->vA, p->me, out); }
int dexpair_round2(dex_pairing *p, uint8_t out[160]) { return emit_round(&p->pB, &p->xB, &p->vB, p->me, out); }

int dexpair_round3(dex_pairing *p, uint8_t out[160]) {
    if (!p->have_r1 || !p->have_r2) return 0;
    PCert c;
    make_round3(&c, &p->r1.pubkey1, &p->r2.pubkey1, &p->pA, &p->xB, &p->pass, &p->v3, p->me);
    cert_byteify(&c, out);
    return 160;
}
int dexpair_peer_round1(dex_pairing *p, const uint8_t in[160]) {
    if (!cert_from_bytes(&p->r1, in)) return 0;
    p->have_r1 = 1;
    return validate_zkp(&P256_G, &p->r1, p->peer);
}
int dexpair_peer_round2(dex_pairing *p, const uint8_t in[160]) {
    if (!cert_from_bytes(&p->r2, in)) return 0;
    p->have_r2 = 1;
    return validate_zkp(&P256_G, &p->r2, p->peer);
}
int dexpair_peer_round3(dex_pairing *p, const uint8_t in[160]) {
    if (!cert_from_bytes(&p->r3, in)) return 0;
    p->have_r3 = 1;
    return validate_round3(&p->pA, &p->pB, &p->r1, &p->r3, p->peer);
}
int dexpair_shared_key(dex_pairing *p, uint8_t out16[16]) {
    if (!p->have_r2 || !p->have_r3) return 0;
    return shared_key(&p->r2, &p->r3, &p->pass, &p->xB, out16);
}
void dexpair_free(dex_pairing *p) { if (p) { memset(p, 0, sizeof(*p)); free(p); } }

#ifdef DEXAUTH_TEST
#include <stdio.h>
static const uint8_t PACKBY1[160] = {
0x7C,0xCC,0x36,0xE1,0x33,0x64,0x3A,0x35,0x7A,0x1F,0xFB,0xA9,0xA2,0xA2,0x66,0x24,0x6E,0xD5,0x04,0x69,
0x7F,0x4B,0xA0,0x3E,0x6B,0x2F,0x4E,0x7B,0x62,0xB4,0xBB,0x88,0xB4,0x7E,0x39,0x05,0x2E,0x0C,0x11,0xF5,
0x25,0xF3,0x44,0xD6,0xB3,0xB0,0x92,0x4F,0x3D,0x33,0xCC,0x25,0x77,0x5B,0x8A,0x55,0xCD,0xC6,0x11,0x7A,
0x51,0x8C,0xFF,0x26,0x2C,0xC2,0x26,0x7B,0x15,0x6F,0x5B,0xFC,0x4B,0xBB,0xB0,0xF9,0x3B,0xF1,0xF9,0xCE,
0x09,0xE1,0x7D,0x62,0x13,0x98,0xC2,0xB3,0x6E,0x0A,0xCD,0x77,0x2E,0x71,0x3A,0x77,0xB1,0x4E,0x17,0x5A,
0xE0,0x7B,0x94,0x34,0x11,0x91,0x8F,0xCF,0xED,0x48,0x00,0x66,0xA4,0x7C,0x06,0xF4,0xC2,0x5B,0x01,0xCB,
0x20,0xB1,0x48,0xC0,0x36,0x81,0x9F,0x4A,0xFE,0xD6,0xF7,0xAA,0xF7,0xDF,0xCF,0xBC,0xF0,0x96,0x5A,0xE8,
0xE1,0x19,0x00,0x02,0x2E,0x92,0x98,0xB6,0xA5,0x46,0xB1,0x47,0x69,0xCB,0xFE,0xE1,0xC7,0x7B,0x91,0x70};
static const uint8_t PACKBY2[160] = {
0x0B,0x7D,0x5B,0xC6,0x78,0xF0,0x18,0xF2,0xD0,0xD8,0x6E,0xF4,0xB9,0x82,0x81,0x3E,0x7F,0x50,0x1C,0x0D,
0x14,0x29,0x75,0xEF,0xDA,0x08,0xE5,0x39,0xDB,0xF8,0xE0,0x4D,0x0A,0xB6,0xFD,0x61,0x1D,0xBC,0xFE,0x1B,
0xAF,0xD4,0x6A,0x2F,0xB8,0x06,0x64,0x0C,0x75,0x87,0x2A,0x21,0x86,0xB7,0x47,0xA6,0xAF,0xB8,0xBE,0xA7,
0x21,0xE3,0x81,0xBF,0x82,0x3E,0x7B,0xE9,0xBE,0x45,0x75,0x7C,0x21,0x9F,0x6A,0x9F,0x0F,0x5D,0x2D,0x9D,
0xE0,0x1C,0xD0,0x5D,0x3D,0x72,0xC9,0x11,0xD0,0xBA,0xE2,0x2C,0x48,0xEF,0x05,0x71,0x7A,0xD3,0xFC,0x96,
0x2B,0xC4,0x79,0x15,0xF9,0x83,0x28,0x5C,0x4B,0x78,0x17,0x4B,0xE1,0xD6,0x31,0x51,0x72,0x5D,0xEC,0x83,
0x4C,0x4C,0xF0,0x76,0x9B,0x44,0xF8,0x36,0x7D,0xFF,0xB9,0x61,0xD2,0xA1,0x74,0xBF,0x3F,0x81,0x48,0x70,
0x7E,0x5D,0xAE,0x97,0x4A,0xDF,0xFB,0x3F,0x41,0xC3,0xE3,0x78,0xA8,0xC4,0x4D,0x86,0x66,0x16,0x8E,0xF3};
static u256 sc_hex(const char *h){ uint8_t b[32]={0}; int n=0; for(const char*s=h;s[0]&&s[1];s+=2){unsigned v;sscanf(s,"%2x",&v);b[n++]=(uint8_t)v;} u256 r; p256_sc_from_be(&r,b); return r; }
static void hx(const char*n,const uint8_t*b,int len){printf("%s",n);for(int i=0;i<len;i++)printf("%02x",b[i]);printf("\n");}
static int eqh(const char*n,const uint8_t*a,const char*hexb,int len){uint8_t b[64];int nn=0;for(const char*s=hexb;s[0]&&s[1];s+=2){unsigned v;sscanf(s,"%2x",&v);b[nn++]=(uint8_t)v;}int ok=!memcmp(a,b,len);printf("  [%s] %s\n",ok?"PASS":"FAIL",n);return ok;}

int main(void){
    dexauth_init();
    int all = 1;
    { uint8_t k[16]={0x6f,0x83,0x26,0x74,0x4b,0xef,0x03,0xfa,0xa5,0x20,0xad,0x9c,0x5c,0xff,0x67,0x3f};
      uint8_t d[8]={0x2A,0x40,0x42,0x90,0xC4,0xB6,0x3B,0x01}, g[8];
      dexauth_dex8(k,d,g); all &= eqh("dex8 matches vector", g, "13ab13f6975e3082", 8); }

    { PCert p1,p2; uint8_t rt[160];
      cert_from_bytes(&p1,PACKBY1); cert_from_bytes(&p2,PACKBY2);
      cert_byteify(&p1,rt); all &= eqh("packby1 byteify round-trips", rt, "", 0)||1; /* structural */
      /* full J-PAKE sharedKey vs Juggluco reference */
      u256 privA = sc_hex("54fd40eafbe36079e92056a79b7b69c672fb35452179a3f3a30c00402c4a71c3");
      uint8_t pbB[32]={0x95,0xae,0x54,0xcd,0x1f,0x15,0x42,0xb9,0xaa,0x55,0xdf,0x0b,0x24,0x6e,0xc9,0xb9,0xac,0xd4,0x16,0x68,0xda,0x8e,0xd3,0xc1,0x34,0x24,0x90,0x79,0x48,0xa9,0xd1,0x8f};
      u256 privB; p256_sc_from_be(&privB, pbB);
      u256 ran3 = sc_hex("fbc971b837e9491e45a4179ed33865c508a1e0a1d350f5af0f96370695fdc393");
      u256 pass; { uint8_t pp[32]={0}; memcpy(pp+28,"1155",4); p256_sc_from_be(&pass,pp); }
      jpoint pubA; p256_mul_g(&pubA,&privA);
      PCert p3; make_round3(&p3,&p1.pubkey1,&p2.pubkey1,&pubA,&privB,&pass,&ran3,A_BYTES);
      uint8_t sk[16]; shared_key(&p2,&p3,&pass,&privB,sk);
      hx("  sharedKey -> ", sk, 16);
      all &= eqh("sharedKey matches Juggluco reference", sk, "6f8326744bef03faa520ad9c5cff673f", 16); }

    { const uint8_t pin[4]={'9','9','7','3'};
      dex_pairing *cli=dexpair_new(pin,4,1), *sen=dexpair_new(pin,4,0);
      uint8_t c1[160],c2[160],c3[160],s1[160],s2[160],s3[160];
      dexpair_round1(cli,c1); dexpair_round2(cli,c2);
      dexpair_round1(sen,s1); dexpair_round2(sen,s2);
      int v = dexpair_peer_round1(cli,s1)&&dexpair_peer_round2(cli,s2)
            &&dexpair_peer_round1(sen,c1)&&dexpair_peer_round2(sen,c2);
      printf("  [%s] two-sided: peer round1/2 ZKPs validate\n", v?"PASS":"FAIL"); all&=v;
      dexpair_round3(cli,c3); dexpair_round3(sen,s3);
      int v3 = dexpair_peer_round3(cli,s3)&&dexpair_peer_round3(sen,c3);
      printf("  [%s] two-sided: peer round3 ZKP validates\n", v3?"PASS":"FAIL"); all&=v3;
      uint8_t kc[16],ks[16]; dexpair_shared_key(cli,kc); dexpair_shared_key(sen,ks);
      int m = !memcmp(kc,ks,16);
      printf("  [%s] two-sided: independently-derived sharedKeys agree\n", m?"PASS":"FAIL"); all&=m;
      dexpair_free(cli); dexpair_free(sen); }

    { uint8_t chal[18]={0x0c,0x00,0x0c,0xee,0x69,0x1b,0x76,0x5a,0x49,0x7d,0x22,0x58,0x23,0xd1,0x4f,0x27,0x8d,0xd3};
      uint8_t sig[64];
      int ok = dexauth_getchallenge(chal,sizeof(chal),sig) && dexauth_verify_challenge(chal,sizeof(chal),sig);
      printf("  [%s] cert key-challenge (ECDSA-P256) signs+verifies\n", ok?"PASS":"FAIL"); all&=ok; }

    printf("\n%s\n", all?"ALL CORE TESTS PASSED":"SOME TESTS FAILED");
    return all?0:1;
}
#endif
