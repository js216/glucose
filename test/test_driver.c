/* Offline end-to-end test of the protocol driver (dexdriver.c) with NO hardware.
 *
 * A simulated Stelo runs the real J-PAKE server side (dexpair is_client=0) and
 * answers the driver's writes; the final glucose is decoded from REAL captured
 * bytes. Exercises: subscribe sequencing, round request/reassembly/chunking,
 * 02/03/04/05 auth, shared-key agreement + persistence, and EGV decode.
 *
 *   cc -DDEXDRIVER_TEST test_driver.c ../dexdriver.c dexauth.c dexdata.c \
 *      p256.c sha256.c aes.c -I.. -I. -o t && ./t
 */
#include "../dexdriver.h"
#include "dexauth.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* driver logs go through this */
int __android_log_print(int prio, const char *tag, const char *fmt, ...){
    (void)prio;(void)tag; va_list ap; va_start(ap,fmt); vprintf(fmt,ap); printf("\n"); va_end(ap); return 0;
}

/* ---- event queue (mirrors Ble's serialised delivery; avoids reentrancy) ---- */
enum { EV_CONN, EV_WRITTEN, EV_NOTIFY, EV_DISC };
struct ev { int type; char uuid[48]; uint8_t data[256]; int len; int status; };
static struct ev Q[1024]; static int qh, qt;
static void q_conn(void){ Q[qt].type=EV_CONN; qt++; }
static void q_written(const char*u,int s){ struct ev*e=&Q[qt++]; e->type=EV_WRITTEN; snprintf(e->uuid,48,"%s",u); e->status=s; }
static void q_notify(const char*u,const uint8_t*d,int n){ struct ev*e=&Q[qt++]; e->type=EV_NOTIFY; snprintf(e->uuid,48,"%s",u); memcpy(e->data,d,n); e->len=n; }

/* ---- simulated sensor ---- */
static dex_pairing *sensor;
static uint8_t skey[16];
static uint8_t preset_key[16]; static int reconnect_mode;
static uint8_t drv_round[160]; static int drlen; static int mock_round;
static const uint8_t schallenge[8] = {0xde,0xad,0xbe,0xef,0x01,0x02,0x03,0x04};
static int glucose_seen = -1, auth_ok = 0, key_saved_matches = -1;

/* ---- drv_* hooks: the "transport" the driver talks to ---- */
void drv_connect(const char *mac){ (void)mac; q_conn(); }
void drv_subscribe(const char *uuid, int indicate){ (void)indicate; q_written(uuid,0); }
void drv_status(const char *s){ printf("   [status] %s\n", s); }
void drv_glucose(int mg, int trend, int age){ (void)trend;(void)age; glucose_seen = mg; }
void drv_backfill(int mg, int trend, int age){ (void)trend;(void)age; (void)mg; }
int  drv_key_load(uint8_t key[16]){ if(reconnect_mode){ memcpy(key,preset_key,16); return 1; } return 0; }
void drv_key_save(const uint8_t key[16]){ key_saved_matches = (memcmp(key,skey,16)==0); }

static int mock_phase;   /* 0=rounds, 1=cert, 2=keychal */
void drv_write(const char *uuid, const uint8_t *d, int n, int no_resp){
    (void)no_resp;
    q_written(uuid,0);
    if(!strcmp(uuid,U_AUTH) && n>=2 && d[0]==0x0a){          /* round request */
        mock_phase=0; mock_round = d[1]; drlen = 0;
        uint8_t pkt[160];
        int ok = mock_round==0?dexpair_round1(sensor,pkt)
               : mock_round==1?dexpair_round2(sensor,pkt)
               :               dexpair_round3(sensor,pkt);
        if(!ok){ printf("   !! sensor round%d build failed\n",mock_round+1); return; }
        for(int i=0;i<8;i++) q_notify(U_ROUND,pkt+i*20,20);   /* sensor's round */
    } else if(!strcmp(uuid,U_ROUND) && mock_phase==0){        /* driver's round chunk */
        if(drlen+n<=160){ memcpy(drv_round+drlen,d,n); drlen+=n; }
        if(drlen>=160){
            int ok = mock_round==0?dexpair_peer_round1(sensor,drv_round)
                   : mock_round==1?dexpair_peer_round2(sensor,drv_round)
                   :               dexpair_peer_round3(sensor,drv_round);
            printf("   [sensor] driver round%d ZKP %s\n", mock_round+1, ok?"VALID":"INVALID");
            if(mock_round==2){ if(!dexpair_shared_key(sensor,skey)) printf("   !! sensor sharedkey fail\n"); }
        }
    } else if(!strcmp(uuid,U_AUTH) && n>=10 && d[0]==0x02){   /* AuthRequest: token */
        uint8_t rx[17]; rx[0]=0x03;
        dexauth_dex8(skey, d+1, rx+1);
        memcpy(rx+9, schallenge, 8);
        q_notify(U_AUTH,rx,17);
    } else if(!strcmp(uuid,U_AUTH) && n>=9 && d[0]==0x04){    /* ChallengeReply */
        uint8_t expect[8]; dexauth_dex8(skey, schallenge, expect);
        auth_ok = (memcmp(expect,d+1,8)==0);
        /* reconnect answers 05 01 01 (stream directly); pairing answers 02 (do certs) */
        uint8_t rx[3]={0x05, (uint8_t)(reconnect_mode?0x01:0x02), 0x01};
        q_notify(U_AUTH,rx,3);
    } else if(!strcmp(uuid,U_AUTH) && n>=2 && d[0]==0x0b){    /* certificate announce */
        mock_phase=1;
        uint8_t rx[7]={0x0b,0x00,d[1],1,0,0,0};              /* sensor cert idx, size=1 */
        q_notify(U_AUTH,rx,7);
        uint8_t c=0xab; q_notify(U_ROUND,&c,1);             /* 1-byte sensor cert */
    } else if(!strcmp(uuid,U_AUTH) && n>=17 && d[0]==0x0c){   /* key-challenge announce */
        mock_phase=2;
        uint8_t blob[64]={0}; q_notify(U_ROUND,blob,64);    /* sensor's 64-byte blob (ignored) */
        uint8_t rx[18]={0x0c,0x00}; memcpy(rx+2,schallenge,16);
        q_notify(U_AUTH,rx,18);                             /* 0c 00 <16> to sign */
    } else if(!strcmp(uuid,U_AUTH) && n>=3 && d[0]==0x0d){    /* challenge out -> accept */
        uint8_t rx[8]={0x0d,0x00,0x00,1,2,3,4,5};
        q_notify(U_AUTH,rx,8);
    } else if(!strcmp(uuid,U_ROUND)){                         /* cert/sig chunks: ignore */
        /* auto-acked by q_written above */
    } else if(!strcmp(uuid,U_CTRL) && n>=1 && d[0]==0x4e){    /* getdata -> real EGV bytes */
        static const uint8_t egv[]={0x4e,0x00,0x31,0x08,0x08,0x00,0xdd,0x06,0x00,0x01,
                                    0x04,0x00,0xa5,0x00,0x06,0xfe,0xa5,0x00,0x0f}; /* glucose 165 */
        q_notify(U_CTRL,egv,sizeof(egv));
    }
}

static void pump(void){
    while(qh<qt){
        struct ev *e=&Q[qh++];
        if(e->type==EV_CONN) driver_on_connected();
        else if(e->type==EV_WRITTEN) driver_on_written(e->uuid,e->status);

        else driver_on_notify(e->uuid,e->data,e->len);
    }
}

int main(void){
    int all=1;
    dexauth_init();

    printf("========== PAIRING (no saved key) ==========\n");
    sensor=dexpair_new((const uint8_t*)"9973",4,0);   /* server side */
    qh=qt=0; glucose_seen=-1; auth_ok=0; key_saved_matches=-1; reconnect_mode=0;
    driver_init();
    driver_start("F8:DA:3F:EA:B5:F0","9973");
    pump();
    printf("---- pairing result ----\n");
    printf("  [%s] sensor accepted our ChallengeReply (mutual auth)\n", auth_ok?"PASS":"FAIL"); all&=auth_ok;
    printf("  [%s] saved key equals sensor's derived key (J-PAKE agreed)\n", key_saved_matches==1?"PASS":"FAIL"); all&=(key_saved_matches==1);
    printf("  [%s] decoded glucose from stream = %d (expect 165)\n", glucose_seen==165?"PASS":"FAIL", glucose_seen); all&=(glucose_seen==165);
    memcpy(preset_key, skey, 16);   /* reuse the agreed key for the reconnect test */
    dexpair_free(sensor);

    printf("\n========== RECONNECT (saved key, no rounds) ==========\n");
    reconnect_mode=1; memcpy(skey,preset_key,16);
    qh=qt=0; glucose_seen=-1; auth_ok=0;
    driver_init();     /* loads preset key */
    driver_start("F8:DA:3F:EA:B5:F0","9973");
    pump();
    printf("---- reconnect result ----\n");
    printf("  [%s] authenticated with saved key (skipped J-PAKE rounds)\n", auth_ok?"PASS":"FAIL"); all&=auth_ok;
    printf("  [%s] decoded glucose = %d (expect 165)\n", glucose_seen==165?"PASS":"FAIL", glucose_seen); all&=(glucose_seen==165);

    printf("\n%s\n", all?"ALL DRIVER TESTS PASSED":"SOME TESTS FAILED");
    return all?0:1;
}
