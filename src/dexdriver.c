/* stealo protocol driver — transport-agnostic Dexcom pairing/reconnect state
 * machine. See dexdriver.h. Event-driven: each transport callback advances the
 * state and issues the next operation via the drv_* hooks. Heavily logged.
 *
 * Flow (validated against a real Stelo capture):
 *   connect -> subscribe auth+round -> [fresh: J-PAKE rounds] -> 02/03/04/05 auth
 *   -> certificate exchange (0b/0c/0d + ECDSA) to establish a streamable bond
 *   -> 06 1e -> subscribe ctrl+data -> 4e getdata -> stream EGV/backfill.
 * A bonded reconnect (auth==1) skips rounds and certs and streams directly.
 */
#include "dexdriver.h"
#include "dexauth.h"
#include "dexdata.h"
#include "dexcerts.h"
#include "dexlibc.h"

int __android_log_print(int prio, const char *tag, const char *fmt, ...);
#define LOGI(...) __android_log_print(4, "stealo", __VA_ARGS__)

static void loghex(const char *tag, const uint8_t *d, int n){
    char b[3*40+8]; int L=0, cap = n>40?40:n;
    for(int i=0;i<cap;i++) L+=snprintf(b+L,sizeof(b)-(size_t)L,"%02x",d[i]);
    LOGI("%s [%d] %s%s", tag, n, b, n>cap?"..":"");
}
enum { P_IDLE, P_SUB1, P_ROUNDS, P_AUTH, P_CERT, P_KEYCHAL, P_SUB2, P_STREAM, P_FAIL };
static const char *phase_name(int p){
    static const char *N[]={"IDLE","SUB1","ROUNDS","AUTH","CERT","KEYCHAL","SUB2","STREAM","FAIL"};
    return (p>=0 && p<=8)?N[p]:"?";
}

static int      phase;
static char     g_mac[24];
static uint8_t  g_code[8]; static int g_codelen;
static int      streamed;
static dex_pairing *pairing;
static uint8_t  shared_key[16]; static int have_key;
static uint8_t  token[8];
static int      round_idx;
static uint8_t  rxbuf[160]; static int rxlen;
static int      tx_left, sub_idx;
static int      did_rounds;
static uint32_t last_clock;     /* sensor session-time from the latest 4e */
static uint16_t last_age;       /* age of that current reading, seconds */
static int      last_glucose, last_trend, last_predicted, last_seq;
static int      g_bonded;       /* last AuthStatus was the fast (auth==1) path */

void driver_get_session(dex_session *out){
    memset(out, 0, sizeof *out);
    int i = 0; for (; g_mac[i] && i < 23; i++) out->mac[i] = g_mac[i]; out->mac[i] = 0;
    out->bonded = g_bonded;
    out->paired = have_key;
    out->have_reading = (last_clock != 0);
    out->session_seconds = last_clock;
    out->glucose = last_glucose; out->trend = last_trend; out->age = last_age;
    out->predicted = last_predicted; out->sequence = last_seq;
}
static int      cert_idx, cert_size, cert_rx;   /* certificate exchange */

static const struct { const char *uuid; int indicate; } SUB1[]={{U_AUTH,1},{U_ROUND,0}};
static const struct { const char *uuid; int indicate; } SUB2[]={{U_CTRL,1},{U_DATA,0}};
#define NSUB1 2
#define NSUB2 2

static uint32_t le32(const uint8_t *p){ return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }

/* issue a buffer to the round-transport char in 20-byte chunks; tx_left counts acks */
static void send_chunks(const uint8_t *buf, int len){
    tx_left = (len + 19) / 20;
    LOGI("== send %d bytes in %d chunks ==", len, tx_left);
    for(int o=0;o<len;o+=20){ int c = len-o>20?20:len-o; drv_write(U_ROUND,buf+o,c,1); }
}
static void gen_token(void){
    int fd=open("/dev/urandom",O_RDONLY);
    if(fd>=0){ if(read(fd,token,8)!=8){} close(fd); }
}
static void send_authrequest(void){
    gen_token();
    uint8_t m[10]; m[0]=0x02; memcpy(m+1,token,8); m[9]=0x02;
    LOGI("== AuthRequest (02 token 02) ==");
    drv_write(U_AUTH,m,10,0);
    phase=P_AUTH;
}
static void request_round(void){
    LOGI("== request J-PAKE round %d (0a %02x) ==", round_idx+1, round_idx);
    uint8_t m[2]={0x0a,(uint8_t)round_idx}; rxlen=0;
    drv_write(U_AUTH,m,2,0);
}
static void send_our_round(void){
    uint8_t pkt[160];
    int ok = round_idx==0?dexpair_round1(pairing,pkt)
           : round_idx==1?dexpair_round2(pairing,pkt)
           :              dexpair_round3(pairing,pkt);
    if(!ok){ LOGI("!! round%d build failed",round_idx+1); phase=P_FAIL; return; }
    LOGI("== send our round %d ==", round_idx+1);
    send_chunks(pkt,160);
}
/* announce a certificate (0b idx len) and expect the sensor's cert on the round char */
static void start_cert(int idx){
    cert_idx=idx; cert_rx=0; cert_size=0;
    int len = idx==0?DEX_CERT0_LEN:DEX_CERT1_LEN;
    uint8_t m[6]={0x0b,(uint8_t)idx,(uint8_t)len,(uint8_t)(len>>8),(uint8_t)(len>>16),(uint8_t)(len>>24)};
    LOGI("== certificate %d: announce (0b %02x len=%d) ==", idx, idx, len);
    drv_write(U_AUTH,m,6,0);
    phase=P_CERT;
}
static void start_keychallenge(void){
    LOGI("== key challenge (0c + random16) ==");
    uint8_t m[17]; m[0]=0x0c;
    int fd=open("/dev/urandom",O_RDONLY); if(fd>=0){ if(read(fd,m+1,16)!=16){} close(fd); }
    cert_rx=0;                       /* accumulate the sensor's 64-byte challenge blob */
    drv_write(U_AUTH,m,17,0);
    phase=P_KEYCHAL;
}
static void goto_stream(void){
    LOGI("phase -> SUB2 (enable ctrl+data CCCDs, then getdata)");
    phase=P_SUB2; sub_idx=0;
    drv_subscribe(SUB2[0].uuid,SUB2[0].indicate);
}

void driver_init(void){
    dexauth_init();
    uint8_t k[16];
    if(drv_key_load(k)){ memcpy(shared_key,k,16); have_key=1; LOGI("loaded saved key"); }
}
void driver_start(const char *mac, const char *code){
    int i=0; for(; mac[i] && i<23; i++) g_mac[i]=mac[i]; g_mac[i]=0;
    g_codelen=0; for(int j=0; code[j] && g_codelen<8; j++) g_code[g_codelen++]=(uint8_t)code[j];
    phase=P_IDLE;
    drv_status(have_key ? "WAITING" : "PAIRING");
    LOGI("driver_start mac=%s code(len=%d) have_key=%d", g_mac, g_codelen, have_key);
    drv_connect(g_mac);
}
void driver_on_connected(void){
    LOGI("<< connected: services ready"); drv_status("CONNECTED");
    phase=P_SUB1; sub_idx=0;
    LOGI("phase -> SUB1 (enable auth+round CCCDs)");
    drv_subscribe(SUB1[0].uuid,SUB1[0].indicate);
}

/* Continuous reconnect: once paired, reconnect for every sensor cycle (~5 min),
 * indefinitely and without ever giving up — this app is meant to run 24/7 like
 * the official one. drv_connect uses autoConnect=true, a passive wait for the
 * sensor's next advertisement, so even a long run of failures is gentle: there
 * is no active scanning and no connect storm, just one armed reconnect at a
 * time. fails is kept only for logging the current streak. */
#define MAX_FAILS 15            /* connect-but-no-stream cap; pause to stop hammering */
static int fails;               /* consecutive connects that never streamed */
static int authfails;           /* subset: failures after we reached auth/cert */
void driver_on_disconnected(int status){
    LOGI("<< disconnected status=%d in phase=%s (streamed=%d)",status,phase_name(phase),streamed);
    int did_stream = streamed;
    streamed=0;
    if(pairing){ dexpair_free(pairing); pairing=NULL; }
    int was=phase; phase=P_IDLE;
    if(did_stream){ fails=0; authfails=0; }
    else {
        fails++;
        /* Authenticated (reached auth/cert/keychal) but never streamed. If this
         * repeats with a stored key, the key is stale — another app (e.g. the
         * official Stelo app) re-paired the sensor. Drop the key so the next
         * connect re-pairs from scratch via the J-PAKE rounds (have_key=0 path). */
        if(have_key && (was==P_AUTH || was==P_CERT || was==P_KEYCHAL) && ++authfails>=3){
            LOGI("!! %d post-auth failures with a key -> discard key, re-pair", authfails);
            have_key=0; authfails=0; drv_key_clear();
        }
    }
    /* Note: out-of-range does NOT count here — autoConnect just waits for the
     * next advert without a failed connection. fails climbs only when we connect
     * yet can't stream, so the cap catches a genuine loop, not a quiet sensor. */
    if(fails >= MAX_FAILS){
        LOGI("!! %d straight failures — pausing to avoid hammering (relaunch to retry)", fails);
        drv_status("CONNECTION ERROR");
    } else if(have_key || g_codelen>0){
        drv_status(have_key ? "WAITING" : "RE-PAIRING");
        LOGI("reconnect to %s (fail streak %d, was=%s)", g_mac, fails, phase_name(was));
        drv_connect(g_mac);
    } else {
        LOGI("no key/code — not reconnecting (was=%s)", phase_name(was));
        drv_status("CONNECTION ERROR");
    }
}

/* Recover a gap: request records from (now - span) up to just before the
 * current reading. Endpoints are sensor session-time, mapped from the latest
 * 4e (last_clock/last_age). span is clamped to the sensor's ~24h buffer. */
void driver_request_backfill(long span){
    if(phase!=P_STREAM || last_clock==0) return;
    long end = (long)last_clock - (long)last_age;   /* current reading, session-time */
    if(end<=1) return;
    if(span > 86400) span = 86400;
    long start = end - span;
    if(start < 0) start = 0;
    end -= 1;                                        /* exclude the current reading */
    if(start >= end) return;
    uint32_t s=(uint32_t)start, e=(uint32_t)end;
    uint8_t m[9]={0x59, (uint8_t)s,(uint8_t)(s>>8),(uint8_t)(s>>16),(uint8_t)(s>>24),
                        (uint8_t)e,(uint8_t)(e>>8),(uint8_t)(e>>16),(uint8_t)(e>>24)};
    LOGI("== backfill request 59 [%u..%u] (span %lds) ==", s, e, span);
    drv_write(U_CTRL, m, 9, 0);
}

void driver_on_written(const char *uuid, int status){
    LOGI("<< onWritten %.8s status=%d phase=%s",uuid,status,phase_name(phase));
    if(phase==P_SUB1){
        if(strcmp(uuid,SUB1[sub_idx].uuid)!=0){ LOGI("   (ignore stray ack)"); return; }
        if(++sub_idx < NSUB1) drv_subscribe(SUB1[sub_idx].uuid,SUB1[sub_idx].indicate);
        else if(have_key){ did_rounds=0; send_authrequest(); }
        else { did_rounds=1; pairing=dexpair_new(g_code,g_codelen,1); round_idx=0; phase=P_ROUNDS; request_round(); }
    } else if(phase==P_ROUNDS){
        if(!strcmp(uuid,U_ROUND) && tx_left>0 && --tx_left==0){
            if(++round_idx<3) request_round();
            else if(!dexpair_shared_key(pairing,shared_key)){ LOGI("!! sharedkey fail"); phase=P_FAIL; }
            else { have_key=1; loghex("SHAREDKEY(derived)",shared_key,16); drv_key_save(shared_key); send_authrequest(); }
        }
    } else if(phase==P_CERT){
        /* our cert chunks being acked; when all sent, next cert or key challenge */
        if(!strcmp(uuid,U_ROUND) && tx_left>0 && --tx_left==0){
            if(cert_idx==0) start_cert(1);
            else start_keychallenge();
        }
    } else if(phase==P_KEYCHAL){
        /* our signature chunks acked; when all sent, write 0d 00 02 */
        if(!strcmp(uuid,U_ROUND) && tx_left>0 && --tx_left==0){
            uint8_t m[3]={0x0d,0x00,0x02}; LOGI("   -> challenge out (0d 00 02)");
            drv_write(U_AUTH,m,3,0);
        }
    } else if(phase==P_SUB2){
        if(strcmp(uuid,SUB2[sub_idx].uuid)!=0){ LOGI("   (ignore stray ack)"); return; }
        if(++sub_idx < NSUB2) drv_subscribe(SUB2[sub_idx].uuid,SUB2[sub_idx].indicate);
        else { LOGI("== get data (write 4e) =="); uint8_t c=0x4e; drv_write(U_CTRL,&c,1,0); phase=P_STREAM; }
    }
}

void driver_on_notify(const char *uuid, const uint8_t *buf, int n){
    LOGI("<< onNotify %.8s phase=%s",uuid,phase_name(phase)); loghex("   ",buf,n);
    if(phase==P_ROUNDS && !strcmp(uuid,U_ROUND)){
        if(rxlen+n<=160){ memcpy(rxbuf+rxlen,buf,n); rxlen+=n; }
        if(rxlen>=160){
            int ok = round_idx==0?dexpair_peer_round1(pairing,rxbuf)
                   : round_idx==1?dexpair_peer_round2(pairing,rxbuf)
                   :              dexpair_peer_round3(pairing,rxbuf);
            LOGI("   peer round%d ZKP %s",round_idx+1, ok?"VALID":"INVALID(continuing)");
            send_our_round();
        }
    } else if(phase==P_AUTH && !strcmp(uuid,U_AUTH)){
        if(n>=17 && buf[0]==0x03){
            LOGI("   AuthChallenge (03) -> ChallengeReply (04)");
            uint8_t reply[9]; reply[0]=0x04; dexauth_dex8(shared_key, buf+9, reply+1);
            drv_write(U_AUTH,reply,9,0);
        } else if(n>=3 && buf[0]==0x05){
            int auth=buf[1], bond=buf[2];
            g_bonded = (auth==1);
            LOGI("   AuthStatus (05) auth=%02x bond=%02x",auth,bond);
            if(auth==0){ LOGI("!! auth failed"); drv_status("AUTH FAILED"); phase=P_FAIL; }
            else if(did_rounds || auth!=1){
                /* establish/refresh the bond via the certificate exchange */
                drv_status(did_rounds?"PAIRED":"BONDING");
                start_cert(0);
            } else {
                LOGI("   bonded reconnect -> stream");
                drv_status("AUTHENTICATED");
                goto_stream();
            }
        }
    } else if(phase==P_CERT){
        if(!strcmp(uuid,U_AUTH) && n>=7 && buf[0]==0x0b){
            /* size announce; don't reset cert_rx — the sensor streams some cert
             * chunks before this arrives (start_cert already zeroed it). */
            cert_size = (int)le32(buf+3);
            LOGI("   sensor cert %d size=%d (rx so far %d)", cert_idx, cert_size, cert_rx);
        } else if(!strcmp(uuid,U_ROUND)){
            cert_rx += n;
            if(cert_size>0 && cert_rx >= cert_size){
                LOGI("   sensor cert %d received (%d); sending ours", cert_idx, cert_rx);
                send_chunks(cert_idx==0?DEX_CERT0:DEX_CERT1, cert_idx==0?DEX_CERT0_LEN:DEX_CERT1_LEN);
            }
        }
    } else if(phase==P_KEYCHAL){
        if(!strcmp(uuid,U_AUTH) && n>=18 && buf[0]==0x0c){
            LOGI("   sensor key-challenge; signing (ECDSA)");
            uint8_t sig[64];
            if(dexauth_getchallenge(buf,(size_t)n,sig)) send_chunks(sig,64);
            else { LOGI("!! sign failed"); phase=P_FAIL; }
        } else if(!strcmp(uuid,U_AUTH) && n>=3 && buf[0]==0x0d){
            LOGI("   challenge accepted (0d); -> time-extended (06 1e)");
            uint8_t m[2]={0x06,0x1e}; drv_write(U_AUTH,m,2,0);
            goto_stream();
        }
        /* sensor's 64-byte challenge blob on the round char is accumulated/ignored */
    } else if(phase==P_STREAM){
        if(!strcmp(uuid,U_CTRL) && n>=19 && buf[0]==0x4e){
            dex_egv ev; if(dexdata_egv(buf,(size_t)n,&ev)){
                last_clock=ev.clock; last_age=ev.age;
                last_glucose=ev.glucose; last_trend=ev.trend;
                last_predicted=ev.predicted; last_seq=ev.sequence;
                LOGI("   EGV glucose=%d age=%d trend=%d clock=%u",ev.glucose,ev.age,ev.trend,ev.clock);
                drv_glucose(ev.glucose,ev.trend,ev.age); streamed=1;
            }
        } else if(!strcmp(uuid,U_DATA)){
            dex_record r[8]; int k=dexdata_records(buf,(size_t)n,r,8);
            LOGI("   %d backfill record(s)",k);
            for(int i=0;i<k;i++){
                LOGI("     rec ts=%u glu=%d",r[i].timestamp,r[i].glucose);
                /* age = how long ago this record was taken, from the sensor clock */
                long age = last_clock ? (long)last_clock - (long)r[i].timestamp : 0;
                if(age<0) age=0;
                drv_backfill(r[i].glucose, 127, (int)age);   /* 127 = trend unavailable */
            }
            if(k>0) streamed=1;
        }
    }
}
