/* stealo BLE transport: the thin JNI layer between the Ble.java dumb pipe and
 * the transport-agnostic protocol driver (dexdriver.c). It implements the drv_*
 * hooks via Ble's static methods and forwards Ble's callbacks into the driver.
 *
 * All driver work happens synchronously inside a native callback (or the pair
 * kickoff), each of which carries a JNIEnv; we stash it in cur_env for the
 * duration so the drv_* hooks can call back into Java without a thread attach. */
#include <jni.h>
#include "dexdriver.h"
#include "dexlibc.h"

int __android_log_print(int prio, const char *tag, const char *fmt, ...);
#define LOGI(...) __android_log_print(4, "stealo", __VA_ARGS__)

/* UI hooks implemented in main.c */
void stealo_status(const char *s);
void stealo_glucose(int mg_dl, int trend, int age_s);
void stealo_backfill(int mg_dl, int trend, int age_s);
void stealo_rssi(int rssi);
void stealo_devinfo(const char *uuid, const char *val);

static jclass    g_ble;
static jobject   g_ctx;
static jmethodID m_connect, m_subscribe, m_write, m_readrssi, m_read, m_startsvc;
static char      g_keypath[256];
static JNIEnv   *cur_env;
static JavaVM   *g_vm;                          /* for a JNIEnv on any thread */
static jclass    g_alarm_cls;                   /* com.jk.stealo.Alarm */
static jmethodID m_alarm_trigger, m_alarm_silence;

/* a JNIEnv valid on the calling thread (main-loop touches and binder callbacks
 * both drive the alarm), attaching if necessary */
static JNIEnv *any_env(void){
    JNIEnv *e=0;
    if(!g_vm) return 0;
    if((*g_vm)->GetEnv(g_vm,(void**)&e,JNI_VERSION_1_6)==JNI_OK) return e;
    if((*g_vm)->AttachCurrentThread(g_vm,(void**)&e,0)==0) return e;
    return 0;
}
void dexble_alarm(int high, int sound, int vibrate){
    JNIEnv *e=any_env();
    if(!e||!g_alarm_cls||!m_alarm_trigger){ LOGI("alarm: cannot fire (e=%p cls=%p m=%p)",(void*)e,(void*)g_alarm_cls,(void*)m_alarm_trigger); return; }
    LOGI("alarm: fire trigger high=%d sound=%d vib=%d",high,sound,vibrate);
    (*e)->CallStaticVoidMethod(e,g_alarm_cls,m_alarm_trigger,g_ctx,(jboolean)high,(jboolean)sound,(jboolean)vibrate);
    if((*e)->ExceptionCheck(e)){ LOGI("alarm: java threw"); (*e)->ExceptionClear(e); }
}
void dexble_alarm_silence(void){
    JNIEnv *e=any_env(); if(!e||!g_alarm_cls||!m_alarm_silence) return;
    (*e)->CallStaticVoidMethod(e,g_alarm_cls,m_alarm_silence,g_ctx);
    if((*e)->ExceptionCheck(e)) (*e)->ExceptionClear(e);
}

/* ---- drv_* transport hooks (called by the driver, cur_env valid) ---- */
void drv_connect(const char *mac){
    JNIEnv *e=cur_env; if(!e) return;
    jstring m=(*e)->NewStringUTF(e,mac);
    jstring err=(*e)->CallStaticObjectMethod(e,g_ble,m_connect,g_ctx,m);
    (*e)->DeleteLocalRef(e,m);
    if(err){ const char*s=(*e)->GetStringUTFChars(e,err,0); LOGI("connect: %s",s); stealo_status(s); (*e)->ReleaseStringUTFChars(e,err,s); }
}
void drv_subscribe(const char *uuid, int indicate){
    JNIEnv *e=cur_env; if(!e) return;
    jstring u=(*e)->NewStringUTF(e,uuid);
    (*e)->CallStaticVoidMethod(e,g_ble,m_subscribe,u,(jboolean)indicate);
    (*e)->DeleteLocalRef(e,u);
}
void drv_write(const char *uuid, const uint8_t *d, int n, int no_resp){
    JNIEnv *e=cur_env; if(!e) return;
    jstring u=(*e)->NewStringUTF(e,uuid);
    jbyteArray a=(*e)->NewByteArray(e,n);
    (*e)->SetByteArrayRegion(e,a,0,n,(const jbyte*)d);
    (*e)->CallStaticVoidMethod(e,g_ble,m_write,u,a,(jboolean)no_resp);
    (*e)->DeleteLocalRef(e,u); (*e)->DeleteLocalRef(e,a);
}
void drv_status(const char *s){ stealo_status(s); }
static void ble_read_rssi(void){
    JNIEnv *e=cur_env; if(!e || !m_readrssi) return;
    (*e)->CallStaticVoidMethod(e,g_ble,m_readrssi);   /* result -> onRssi */
}
/* One-shot read of the Device Information Service (0x180A) strings the Stelo
 * actually exposes: model 0x2A24, firmware 0x2A26, manufacturer 0x2A29. (It has
 * no 0x2A25 serial / 0x2A28 software characteristic.) Results arrive on onRead. */
void dexble_request_devinfo(void){
    JNIEnv *e=cur_env; if(!e || !m_read) return;
    static const char *uuids[3]={
        "00002a24-0000-1000-8000-00805f9b34fb",   /* model number   */
        "00002a26-0000-1000-8000-00805f9b34fb",   /* firmware rev.  */
        "00002a29-0000-1000-8000-00805f9b34fb",   /* manufacturer   */
    };
    for(int i=0;i<3;i++){
        jstring u=(*e)->NewStringUTF(e,uuids[i]);
        (*e)->CallStaticVoidMethod(e,g_ble,m_read,u);   /* result -> onRead */
        (*e)->DeleteLocalRef(e,u);
    }
}
void drv_glucose(int mg, int trend, int age){ stealo_glucose(mg,trend,age); }
void drv_backfill(int mg, int trend, int age){ stealo_backfill(mg,trend,age); }
int  drv_key_load(uint8_t key[16]){
    int fd=open(g_keypath,O_RDONLY); if(fd<0) return 0;
    int ok=(read(fd,key,16)==16); close(fd); return ok;
}
void drv_key_save(const uint8_t key[16]){
    int fd=open(g_keypath,O_WRONLY|O_CREAT|O_TRUNC,0600);
    if(fd>=0){ if(write(fd,key,16)!=16){} close(fd); }
}
void drv_key_clear(void){ unlink(g_keypath); }   /* stale key: force a fresh pairing */

/* ---- Ble.java callbacks (each stashes its env, then drives the state machine) ---- */
static void jni_connected(JNIEnv *e, jclass c){ (void)c; cur_env=e; driver_on_connected(); ble_read_rssi(); cur_env=0; }
static void jni_disconnected(JNIEnv *e, jclass c, jint s){ (void)c; cur_env=e; driver_on_disconnected(s); cur_env=0; }
static void jni_written(JNIEnv *e, jclass c, jstring ju, jint s){
    (void)c; cur_env=e; const char*u=(*e)->GetStringUTFChars(e,ju,0);
    driver_on_written(u,s); (*e)->ReleaseStringUTFChars(e,ju,u); cur_env=0;
}
static void jni_notify(JNIEnv *e, jclass c, jstring ju, jbyteArray jd){
    (void)c; cur_env=e; const char*u=(*e)->GetStringUTFChars(e,ju,0);
    jsize n=(*e)->GetArrayLength(e,jd); uint8_t buf[256]; if(n>256)n=256;
    (*e)->GetByteArrayRegion(e,jd,0,n,(jbyte*)buf);
    driver_on_notify(u,buf,n); (*e)->ReleaseStringUTFChars(e,ju,u); cur_env=0;
}
static void jni_rssi(JNIEnv *e, jclass c, jint rssi){ (void)e; (void)c; stealo_rssi(rssi); }
static void jni_read(JNIEnv *e, jclass c, jstring ju, jbyteArray jd){
    (void)c; cur_env=e; const char*u=(*e)->GetStringUTFChars(e,ju,0);
    jsize n=(*e)->GetArrayLength(e,jd); char buf[64]; if(n>63)n=63;
    (*e)->GetByteArrayRegion(e,jd,0,n,(jbyte*)buf); buf[n<0?0:n]=0;
    stealo_devinfo(u,buf);
    (*e)->ReleaseStringUTFChars(e,ju,u); cur_env=0;
}

/* ---- public API (called from main.c) ---- */
void dexble_init(const char *data_dir){
    int i=0; for(; data_dir[i] && i<200; i++) g_keypath[i]=data_dir[i];
    const char *f="/stelo.key"; for(int j=0; f[j]; j++) g_keypath[i++]=f[j]; g_keypath[i]=0;
    driver_init();
}
int dexble_register(JNIEnv *e, jclass ble, jobject ctx){
    g_ble=(*e)->NewGlobalRef(e,ble);
    g_ctx=(*e)->NewGlobalRef(e,ctx);
    static const JNINativeMethod m[]={
        {"onConnected","()V",(void*)jni_connected},
        {"onDisconnected","(I)V",(void*)jni_disconnected},
        {"onNotify","(Ljava/lang/String;[B)V",(void*)jni_notify},
        {"onWritten","(Ljava/lang/String;I)V",(void*)jni_written},
        {"onRssi","(I)V",(void*)jni_rssi},
        {"onRead","(Ljava/lang/String;[B)V",(void*)jni_read},
    };
    if((*e)->RegisterNatives(e,ble,m,6)!=0) return 0;
    m_connect  =(*e)->GetStaticMethodID(e,ble,"connect","(Landroid/content/Context;Ljava/lang/String;)Ljava/lang/String;");
    m_subscribe=(*e)->GetStaticMethodID(e,ble,"subscribe","(Ljava/lang/String;Z)V");
    m_write    =(*e)->GetStaticMethodID(e,ble,"write","(Ljava/lang/String;[BZ)V");
    m_readrssi =(*e)->GetStaticMethodID(e,ble,"readRssi","()V");
    m_read     =(*e)->GetStaticMethodID(e,ble,"read","(Ljava/lang/String;)V");
    m_startsvc =(*e)->GetStaticMethodID(e,ble,"startService","(Landroid/content/Context;)V");
    if(m_startsvc) (*e)->CallStaticVoidMethod(e,g_ble,m_startsvc,g_ctx);   /* keep alive in bg */
    (*e)->GetJavaVM(e,&g_vm);
    return m_connect && m_subscribe && m_write && m_readrssi && m_read;
}
/* Wire the Alarm class. FindClass here would resolve via the framework loader,
 * which can't see app classes, so main.c loads it via the activity's classloader
 * (find_app_class) and hands it in. */
void dexble_set_alarm(JNIEnv *e, jclass alarm_cls){
    if(!alarm_cls){ LOGI("alarm class not wired (null)"); return; }
    g_alarm_cls=(*e)->NewGlobalRef(e,alarm_cls);
    m_alarm_trigger=(*e)->GetStaticMethodID(e,alarm_cls,"trigger","(Landroid/content/Context;ZZZ)V");
    m_alarm_silence=(*e)->GetStaticMethodID(e,alarm_cls,"silence","(Landroid/content/Context;)V");
    LOGI("alarm class wired (trigger=%p silence=%p)",(void*)m_alarm_trigger,(void*)m_alarm_silence);
}
void dexble_pair(JNIEnv *e, const char *mac, const char *code){
    cur_env=e; driver_start(mac,code); cur_env=0;
}
