/* Dexcom G7 / Stelo authentication session driver.
 *
 * Implements the per-connection challenge-response gate (the 02/03/04/05 exchange
 * that runs on every reconnect once a sharedKey exists). The message sequencing is
 * ported from xDrip's jamorham.keks Plugin.java and Juggluco DexGattCallback.java.
 * The full first-pairing J-PAKE flow is driven from the same states; its live
 * validation waits on a real pairing capture.
 *
 * Offline test build (mock sensor, no hardware):
 *   cc -DDEXSESSION_TEST dexsession.c dexauth.c -lcrypto -o dexsession_test && ./dexsession_test
 */

#include "dexlibc.h"
#include <stdint.h>
#include <stddef.h>
#include "dexauth.h"
#include "dexsession.h"

/* AuthRequest: 02 <token8> 02. Caller supplies the 8 random token bytes. */
int dexsess_start(dex_session *s, const uint8_t saved_key[16],
                  const uint8_t token[8], uint8_t out[10]) {
    memcpy(s->saved_key, saved_key, 16);
    memcpy(s->token, token, 8);
    s->state = DEX_REQUEST_AUTH;
    out[0] = 0x02;
    memcpy(out+1, token, 8);
    out[9] = 0x02;
    return 10;
}

/* Handle AuthChallenge (03 <tokenHash8> <challenge8>). Verifies the sensor proved
 * knowledge of the shared key, then emits ChallengeReply (04 <challengeHash8>).
 * Returns 10 (reply length) on success, DEX_ERR on verification failure. */
int dexsess_on_challenge(dex_session *s, const uint8_t *data, size_t len, uint8_t out[9]) {
    if (s->state != DEX_REQUEST_AUTH || len < 17 || data[0] != 0x03)
        return DEX_ERR;
    uint8_t expect[8];
    dexauth_dex8(s->saved_key, s->token, expect);      /* sensor's tokenHash must match */
    if (memcmp(expect, data+1, 8) != 0) {
        s->state = DEX_FAIL;
        return DEX_ERR;
    }
    memcpy(s->challenge, data+9, 8);
    out[0] = 0x04;
    dexauth_dex8(s->saved_key, s->challenge, out+1);   /* our challengeHash */
    s->state = DEX_CHALLENGE_REPLY;
    return 9;
}

/* Handle AuthStatus (05 <auth> <bond>). Returns DEX_AUTHED, DEX_NEED_PAIR, or DEX_ERR. */
int dexsess_on_status(dex_session *s, const uint8_t *data, size_t len) {
    if (s->state != DEX_CHALLENGE_REPLY || len < 3 || data[0] != 0x05)
        return DEX_ERR;
    int authed = data[1] != 0, bonded = data[2] != 0;
    if (authed && bonded) { s->state = DEX_AUTHED;    return DEX_AUTHED; }
    if (authed)           { s->state = DEX_NEED_PAIR; return DEX_NEED_PAIR; }
    s->state = DEX_FAIL;
    return DEX_ERR;
}

/* The control command that starts the glucose stream once authenticated. */
int dexsess_getdata_cmd(uint8_t out[1]) { out[0] = 0x4e; return 1; }

/* Backfill request: 59 <startU32LE> <endU32LE>. */
int dexsess_backfill_cmd(uint32_t start, uint32_t end, uint8_t out[9]) {
    out[0] = 0x59;
    out[1]=start; out[2]=start>>8; out[3]=start>>16; out[4]=start>>24;
    out[5]=end;   out[6]=end>>8;   out[7]=end>>16;   out[8]=end>>24;
    return 9;
}

#ifdef DEXSESSION_TEST
#include <stdio.h>

/* Mock sensor holding the same sharedKey: answers 02->03, checks 04, replies 05. */
static void mock_sensor_challenge(const uint8_t key[16], const uint8_t *authreq,
                                  const uint8_t fixed_challenge[8], uint8_t out[17]) {
    out[0] = 0x03;
    dexauth_dex8(key, authreq+1, out+1);        /* tokenHash over our token */
    memcpy(out+9, fixed_challenge, 8);          /* sensor's challenge */
}
static int mock_sensor_verify_reply(const uint8_t key[16], const uint8_t challenge[8],
                                    const uint8_t *reply) {
    uint8_t expect[8];
    dexauth_dex8(key, challenge, expect);
    return reply[0] == 0x04 && memcmp(expect, reply+1, 8) == 0;
}

int main(void) {
    if (!dexauth_init()) { printf("init failed\n"); return 1; }
    int all = 1;

    uint8_t key[16]   = {0x6f,0x83,0x26,0x74,0x4b,0xef,0x03,0xfa,0xa5,0x20,0xad,0x9c,0x5c,0xff,0x67,0x3f};
    uint8_t token[8]  = {0x24,0x62,0x8e,0x4b,0x01,0x94,0x9a,0xb5};
    uint8_t chal[8]   = {0xa8,0x3f,0x50,0x0f,0x19,0x1b,0xcc,0x69};

    printf("== reconnect auth against a mock sensor (shared key %02x..%02x) ==\n", key[0], key[15]);
    dex_session s;
    uint8_t authreq[10];
    dexsess_start(&s, key, token, authreq);
    printf("  -> AuthRequest %02x..%02x (len 10)\n", authreq[0], authreq[9]);

    uint8_t rx03[17];
    mock_sensor_challenge(key, authreq, chal, rx03);
    uint8_t reply[9];
    int r = dexsess_on_challenge(&s, rx03, 17, reply);
    int ok_reply = (r == 9) && mock_sensor_verify_reply(key, chal, reply);
    printf("  [%s] verified sensor tokenHash, produced valid ChallengeReply\n", ok_reply?"PASS":"FAIL");
    all &= ok_reply;

    uint8_t rx05[3] = {0x05, 0x01, 0x01};
    int st = dexsess_on_status(&s, rx05, 3);
    printf("  [%s] AuthStatus -> authenticated & bonded (GetData)\n", st==DEX_AUTHED?"PASS":"FAIL");
    all &= (st == DEX_AUTHED);

    printf("== negative: tampered tokenHash must be rejected ==\n");
    {
        dex_session s2; uint8_t ar[10]; dexsess_start(&s2, key, token, ar);
        uint8_t bad[17]; mock_sensor_challenge(key, ar, chal, bad);
        bad[1] ^= 0xff;                       /* corrupt the sensor's proof */
        uint8_t rep[9];
        int rr = dexsess_on_challenge(&s2, bad, 17, rep);
        printf("  [%s] bad sensor proof rejected\n", rr==DEX_ERR?"PASS":"FAIL");
        all &= (rr == DEX_ERR);
    }

    printf("== backfill command framing ==\n");
    {
        uint8_t bf[9]; dexsess_backfill_cmd(0x0008082d, 0x00080a85, bf);
        int ok = bf[0]==0x59 && bf[1]==0x2d && bf[2]==0x08 && bf[5]==0x85 && bf[6]==0x0a;
        printf("  [%s] 59 <startLE> <endLE>\n", ok?"PASS":"FAIL");
        all &= ok;
    }

    printf("\n%s\n", all ? "ALL SESSION TESTS PASSED" : "SOME TESTS FAILED");
    return all ? 0 : 1;
}
#endif
