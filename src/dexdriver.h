/* stealo protocol driver: the Dexcom pairing/reconnect state machine, with NO
 * Android/JNI dependency. The transport (BLE writes/subscribes) and UI/storage
 * are provided by the host via the drv_* hooks below — implemented by dexble.c
 * on the phone and by a mock harness in the offline tests. */
#ifndef DEXDRIVER_H
#define DEXDRIVER_H
#include <stdint.h>

/* Dexcom GATT characteristic UUIDs. */
#define U_CTRL  "f8083534-849e-531c-c594-30f1f86a4ea5"
#define U_AUTH  "f8083535-849e-531c-c594-30f1f86a4ea5"
#define U_DATA  "f8083536-849e-531c-c594-30f1f86a4ea5"
#define U_ROUND "f8083538-849e-531c-c594-30f1f86a4ea5"

/* ---- provided BY the transport layer (dexble.c / test harness) ---- */
void drv_connect(const char *mac);
void drv_subscribe(const char *uuid, int indicate);
void drv_write(const char *uuid, const uint8_t *data, int n, int no_resp);
void drv_status(const char *s);
void drv_glucose(int mg_dl, int trend, int age_s);    /* current reading */
void drv_backfill(int mg_dl, int trend, int age_s);   /* recovered older reading */
int  drv_key_load(uint8_t key[16]);           /* returns 1 if a key was loaded */
void drv_key_save(const uint8_t key[16]);
void drv_key_clear(void);                     /* delete the stored key (force re-pair) */

/* ---- driver API (called by the transport layer) ---- */
void driver_init(void);                        /* dexauth_init + load saved key */
void driver_start(const char *mac, const char *code);   /* set target + code, connect */
void driver_on_connected(void);
void driver_on_disconnected(int status);
void driver_on_written(const char *uuid, int status);
void driver_on_notify(const char *uuid, const uint8_t *data, int n);
void driver_request_backfill(long span_seconds);   /* recover a gap ending at now */

/* Snapshot of what we know about the connected sensor and session. */
typedef struct {
    char     mac[24];
    int      bonded;             /* authenticated on the fast saved-key path */
    int      paired;             /* we hold a shared key */
    int      have_reading;       /* a 4e EGV has been decoded this run */
    uint32_t session_seconds;    /* sensor clock at the last reading */
    int      glucose, trend, age, predicted, sequence;
} dex_session;
void driver_get_session(dex_session *out);

#endif
