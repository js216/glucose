// SPDX-License-Identifier: GPL-3.0
// main.c --- Native app core: UI rendering, state, and JNI wiring
// Copyright 2026 Jakob Kastelic

/* stealo native core, plain C -- no NDK glue.
 * Implements ANativeActivity_onCreate directly; links against stub
 * libc/libandroid/liblog (stub_*.c) -- the phone binds the real bionic ones.
 * jni.h comes from the host JDK (same ABI as Android's).
 *
 * Owns the whole UI (direct struct ANativeWindow pixel rendering),
 * settings/alarm state, the reading history and stats, and the JNI wiring to
 * the BLE pipe. All rendering runs on the main looper thread (see on_main); BLE
 * binder-thread updates just mark the screen dirty for the next 1 Hz repaint.
 */
#include "dexdriver.h"
#include "dexlibc.h"
#include "ndk.h"
#include "plot.h"
#include "settings.h"
#include "stats.h"
#include "stealo.h"
#include "store.h"
#include "ui.h"
#include "util.h"
#include <jni.h>
#include <jni_md.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int __android_log_print(int prio, const char *tag, const char *fmt, ...);
#define LOGI(...) __android_log_print(4, "stealo", __VA_ARGS__)

/* ---- app configuration constants (tunables collected here) ---- */
#define MAX_LINES 16  /* text lines on the pre-reading status screen */
#define MAX_COLS  33  /* character columns the UI lays out to */
#define MAX_DEVS  12  /* sensors held in the PAIR NEW SENSOR list */
#define MENU_MAX  16  /* touch hit-boxes tracked per drawn menu */
#define NPERMS    3   /* runtime permissions requested at once */
#define NOTIFY_W  512 /* lock-screen plot bitmap width (px) */
#define NOTIFY_H  160 /* lock-screen plot bitmap height (px) */

/* POSIX file I/O + kernel-timer calls (no standard header; libc binds at
 * runtime). CLOCK_MONOTONIC is for the repaint timerfd below. */
#define CLOCK_MONOTONIC 1

struct itimerspec {
   struct timespec it_interval, it_value;
};

int timerfd_create(int clockid, int flags);
int timerfd_settime(int fd, int flags, const struct itimerspec *nv,
                    struct itimerspec *ov);

/* --- screen model: a handful of text lines, redrawn on change --- */

struct dev {
   char name[9];
   char mac[18];
   int rssi;
   unsigned count;
   long long last_log_ms;
   long seen_t;
};

static struct ANativeActivity *g_act;
/* Render/thread sync (imperative-shell concern): the surface, the main looper
 * thread id, and a busy flag that both serializes draws and locks a BLE-thread
 * reading update against the main-thread draw. All rendering runs on the main
 * thread; a BLE reading just marks the UI dirty for the 1 Hz repaint. */
static struct ANativeWindow *volatile g_win;
static volatile int g_draw_busy;
static volatile int g_main_tid;
static volatile int g_ui_dirty;

static int on_main(void)
{
   return g_main_tid != 0 && gettid() == g_main_tid;
}

static void hist_lock(void)
{
   while (__atomic_exchange_n(&g_draw_busy, 1, __ATOMIC_SEQ_CST))
      ;
}

static void hist_unlock(void)
{
   __atomic_store_n(&g_draw_busy, 0, __ATOMIC_SEQ_CST);
}

static int g_scanning;
static jclass g_ble; /* global ref to com.jk.stealo.Ble */
static jmethodID g_scan, g_stop;
static jmethodID m_set_orient, m_perm_granted, m_req_perm,
    m_open_settings; /* settings-menu ops */
static jmethodID m_batt_ok, m_req_batt, m_bucket,
    m_bg_restricted;             /* background-run ops */
static jmethodID m_show_glucose; /* push value+plot to the notification */
static jmethodID
    m_bonded_stelo;        /* resolve the bonded Stelo's MAC from bond list */
static int g_notify_dirty; /* a new reading is waiting to be shown */
static char g_status[MAX_COLS + 1] = "STARTING";
/* Execution checkpoint: set to a static label at the top of each hot code path,
 * so the crash handler can record WHERE we were when a fault hit (debuggerd
 * tombstones are SELinux-locked here). volatile so it isn't optimised away. */
static volatile const char *g_where = "boot";
static struct dev g_devs[MAX_DEVS];
static int g_ndevs;
static unsigned g_total; /* all adverts heard, pipe health */
static char g_lines[MAX_LINES][MAX_COLS + 1];
static int g_nlines;
static int g_pairing_started;

/* dexble transport prototypes come from stealo.h; driver_* from dexdriver.h */

/* reading history + current-reading snapshot live in store.c (see store.h) */
static long g_tz_off;      /* local timezone offset, seconds */
static int g_startup_bf;   /* per run: backward 24h fill attempted */
static int g_conn_rssi;    /* live connection RSSI (readRemoteRssi) */
static long g_conn_rssi_t; /* wall-clock of that measurement */
static long g_devinfo_req; /* wall-clock of last DIS read request */

static int g_plot_hours = 3; /* selected plot span (hours); the tab list and
                              * its hit boxes live in ui.c */

/* alarm thresholds (mg/dL, adjustable in the UI) + their button hit boxes */

/* alarm kind passed to dexble_alarm() / Alarm.trigger() (keep in sync with
 * Alarm.java) */
enum { ALARM_LOW, ALARM_HIGH, ALARM_STALE };

static int g_alarm_state;    /* last reading's zone: 0 ok, 1 low, 2 high */
static int g_alarm_sounding; /* an audible alarm is currently active */

/* settings menu (opened by tapping the big-number band) */
enum {
   MENU_NONE,
   MENU_SETTINGS,
   MENU_KEYPAD,
   MENU_DEVLIST
}; /* g_menu / g_kp_return values */

/* Smart pairing (PAIR NEW SENSOR): scans for candidates while the code is
 * typed, WITHOUT touching the currently-bonded sensor. On OK, pick by
 * proximity/count (see select_candidate); ambiguous -> MENU_DEVLIST for the
 * user to choose. */
static int g_smart_pairing;

static int g_menu;         /* which modal screen is open */
static int g_gate;         /* first-run permission-rationale screen */
static int g_want_battery; /* pop battery-opt prompt after perms */
static const int disc_min[] = {0, 10, 30, 60}; /* DISCONNECT-alarm minutes */
static long g_launch_t;    /* for the stale-alarm grace period */
static int g_disc_alarmed; /* stale alarm currently latched */

static const char *perms[] = {"android.permission.BLUETOOTH_SCAN",
                              "android.permission.BLUETOOTH_CONNECT",
                              "android.permission.POST_NOTIFICATIONS"};
/* Cached system states for the settings menu. JNI via the activity's env is
 * only legal on the main thread, so sys_* are never called from a render (which
 * can be requested off a BLE binder thread); sys_refresh samples them from the
 * main thread (menu open / after an action) and build_model just copies them.
 */
static int g_sys_perm[NPERMS], g_sys_batt, g_sys_bucket, g_sys_bg;
static char g_entry[8];
static int g_entrylen;     /* keypad entry buffer */
static int g_kp_mode;      /* keypad: 0 = pair code, 1 = plot max */
static int g_kp_return;    /* keypad close target: 0 = main, 1 = settings */
static int g_al_held = -1; /* alarm button held for auto-repeat (0:LOW- 1:LOW+
                            * 2:HIGH- 3:HIGH+); geometry lives in ui.c */
static int g_timerfd = -1; /* shared repaint / repeat timer */
static struct ALooper
    *g_looper;       /* main looper, to remove the timer fd on destroy */
static int g_inited; /* process-wide one-time init done (relaunch guard) */
/* press-and-hold plot scrub (the plot rect itself comes from the recorded
 * ACT_SCRUB hit box via plot_rect) */
static int g_scrub_idx = -1; /* highlighted point, -1 = none */
static int g_scrubbing;      /* a plot drag is in progress */

/* fmt_glu / fmt_trend / fmt_hms are UI display formatters (declared in ui.h);
 * white_color is the notification plot's dot-colour callback. */
static uint32_t white_color(int g)
{
   (void)g;
   return 0xFFFFFFFF;
} /* plot dots */

/* draw_str / fmt_glu / str_snapshot live in ui.c (rendering primitives) */
#define UNIT_LBL (g_units ? "MMOL/L" : "MG/DL")

static void start_scan(struct ANativeActivity *a);
static void stop_scan(struct ANativeActivity *a);
static void request_ble_permissions(struct ANativeActivity *a);

/* touch targets recorded by the last main-screen ui_render(); read by on_input
 * to map a tap to an action. Rebuilt on the main thread only. */
static struct hits g_hits;

/* Snapshot the shell's mutable state into an immutable frame for the pure UI.
 * Called on the main thread just before ui_render(); the borrowed hist/dev
 * arrays are static so they outlive the render call. */
static void build_model(struct screen *m)
{
   static struct ui_point pts[NHIST];
   static struct ui_dev devs[MAX_DEVS];
   long now = realtime_s();

   *m = (struct screen){0};
   if (g_gate)
      m->scr = SCR_GATE;
   else if (g_menu == MENU_SETTINGS)
      m->scr = SCR_SETTINGS;
   else if (g_menu == MENU_KEYPAD)
      m->scr = SCR_KEYPAD;
   else if (g_menu == MENU_DEVLIST)
      m->scr = SCR_DEVLIST;
   else
      m->scr = SCR_MAIN;
   m->now    = now;
   m->tz_off = g_tz_off;

   m->glu          = g_cur_glu;
   m->trend        = g_cur_trend;
   m->t            = g_cur_time;
   m->rssi         = g_cur_rssi;
   m->rssi_ok      = g_cur_rssi_ok;
   m->stale        = (g_cur_glu >= 0) && (now - g_cur_time > 360);
   m->disc_alarmed = g_disc_alarmed;

   int nh = g_nhist < NHIST ? g_nhist : NHIST;
   for (int i = 0; i < nh; i++) {
      pts[i].t   = g_hist[i].t;
      pts[i].glu = g_hist[i].glu;
   }
   m->hist       = pts;
   m->nhist      = nh;
   m->scrub      = g_scrub_idx;
   m->plot_hours = g_plot_hours;
   m->plot_max   = g_plot_max;

   struct dex_session s;
   driver_get_session(&s);
   m->bonded          = s.bonded;
   m->have_reading    = s.have_reading;
   m->predicted       = s.predicted;
   m->sequence        = s.sequence;
   m->session_seconds = (long)s.session_seconds;

   m->units      = g_units;
   m->alarm_low  = g_alarm_low;
   m->alarm_high = g_alarm_high;

   /* settings + device info (globals persist; s.mac lives on our stack, so it
    * is copied into a static the borrowed pointer can safely outlast) */
   m->sound_on = g_sound_on;
   m->vib_on   = g_vib_on;
   m->orient   = g_orient;
   m->disc     = g_disc;
   m->code     = g_code_str;
   m->model    = g_model;
   m->fw       = g_fw;
   m->mfr      = g_mfr;
   static char macbuf[20];
   str_snapshot(macbuf, sizeof macbuf, s.mac);
   m->mac = macbuf;
   for (int i = 0; i < NPERMS; i++)
      m->perm[i] = g_sys_perm[i];
   m->batt_ok        = g_sys_batt;
   m->standby_bucket = g_sys_bucket;
   m->bg_restricted  = g_sys_bg;

   /* keypad: mode + the digits typed so far (copied so the pointer is stable)
    */
   m->kp_mode = g_kp_mode;
   static char entrybuf[8];
   int el = g_entrylen < (int)sizeof entrybuf - 1 ? g_entrylen
                                                  : (int)sizeof entrybuf - 1;
   for (int i = 0; i < el; i++)
      entrybuf[i] = g_entry[i];
   entrybuf[el] = 0;
   m->entry     = entrybuf;

   static const int win[5] = {1, 3, 7, 30, 90};
   for (int i = 0; i < 5; i++) {
      int tir = 0;
      int avg = 0;
      if (stat_window(win[i], &tir, &avg)) {
         m->stat[i].have = 1;
         m->stat[i].tir  = tir;
         m->stat[i].avg  = avg;
      }
   }

   m->stored    = g_stored;
   m->status    = g_status;
   m->adv_total = g_total;

   int nd = g_ndevs < MAX_DEVS ? g_ndevs : MAX_DEVS;
   for (int i = 0; i < nd; i++) {
      str_snapshot(devs[i].name, sizeof devs[i].name, g_devs[i].name);
      str_snapshot(devs[i].mac, sizeof devs[i].mac, g_devs[i].mac);
      devs[i].rssi = g_devs[i].rssi;
   }
   m->devs = devs;
   m->ndev = nd;
}

static void draw_impl(struct ANativeWindow *win);

/* draw() is called from the main looper (on_timer at 1 Hz, on_input) AND from
 * BLE binder threads (status/reading updates flow through set_status/
 * stealo_glucose -> draw). struct ANativeWindow (a BufferQueue producer) is NOT
 * safe for concurrent access: two threads locking the surface at once corrupts
 * the returned buffer and segfaults. Serialise with a lock-free guard -- if a
 * draw is already running on another thread, drop this frame; the 1 Hz timer
 * (and the next event) repaint the latest state immediately after.
 *
 * The guard also closes a draw-vs-destroy use-after-free: a BLE thread can hold
 * an old window pointer while on_window_destroyed runs and the framework frees
 * the surface. We take g_draw_busy first, then re-check that the window we were
 * handed is STILL the live g_win; on_window_destroyed clears g_win and spins on
 * g_draw_busy, so it cannot return (and let the framework free the surface)
 * while a draw_impl is in flight. Fixes the intermittent main-screen SIGSEGV.
 */
static void draw(struct ANativeWindow *win)
{
   if (!win)
      return;
   /* Render only on the main looper thread. A draw requested from a BLE binder
    * thread (a reading/status/advert) is coalesced into g_ui_dirty and painted
    * by the next on_timer tick -- so draw_impl and the hit-box geometry it
    * rebuilds are never touched concurrently with on_input. */
   if (!on_main()) {
      g_ui_dirty = 1;
      return;
   }
   /* SEQ_CST on both this exchange and the g_win load below (paired with the
    * SEQ_CST store/load in on_window_destroyed) forbids the store-buffer
    * outcome where this thread sees the old g_win AND the destroyer sees
    * g_draw_busy==0 -- which would let the surface be freed mid-draw. */
   if (__atomic_exchange_n(&g_draw_busy, 1, __ATOMIC_SEQ_CST))
      return;
   if (win ==
       __atomic_load_n(&g_win, __ATOMIC_SEQ_CST)) /* still the live one */
      draw_impl(win);
   __atomic_store_n(&g_draw_busy, 0, __ATOMIC_SEQ_CST);
}

static void draw_impl(struct ANativeWindow *win)
{
   struct ANativeWindow_Buffer buf;

   if (!win)
      return;
   ANativeWindow_setBuffersGeometry(win, 0, 0, WINDOW_FORMAT_RGBA_8888);
   if (ANativeWindow_lock(win, &buf, NULL) != 0)
      return;

   /* Every screen is the pure UI now: build the immutable frame model, render
    * it, and record the touch targets. build_model picks the screen from
    * g_gate / g_menu; ui_render clears the framebuffer itself. */
   struct screen sm;
   build_model(&sm);
   ui_render(&buf, &sm, &g_hits);

   ANativeWindow_unlockAndPost(win);
}

/* Rebuild the text lines; redraw only if something visible changed, and at
 * most ~5 times/second so radio chatter can't saturate the main thread. */
static void update_screen(void)
{
   /* Off the main thread (a BLE-thread status/advert update), don't rebuild the
    * shared text lines or draw -- just mark dirty; on_timer rebuilds+paints. */
   if (!on_main()) {
      g_ui_dirty = 1;
      return;
   }
   char next[MAX_LINES][MAX_COLS + 1];
   int n = 0;

   /* g_status is written by set_status on a BLE binder thread; snapshot it with
    * a bound so this read can never scan off the end during a racing write. */
   char st[MAX_COLS + 1];
   str_snapshot(st, sizeof st, g_status);
   (void)snprintf(next[n++], sizeof next[0], "STEALO  %s", st);
   /* total rounded to 10s so ambient chatter doesn't redraw every advert */
   (void)snprintf(next[n++], sizeof next[0], "ADV %u  DX %d", g_total / 10 * 10,
                  g_ndevs);

   int changed = (n != g_nlines);
   for (int i = 0; !changed && i < n; i++)
      changed = strcmp(next[i], g_lines[i]) != 0;
   if (!changed)
      return;

   g_nlines = n;
   for (int i = 0; i < n; i++)
      (void)snprintf(g_lines[i], sizeof g_lines[0], "%s", next[i]);

   static long long last_draw_ms;
   long long now = now_ms();
   if (now - last_draw_ms < 200)
      return; /* next change will repaint */
   last_draw_ms = now;
   draw(g_win);
}

/* --- Java -> C: one advertisement heard (main thread) --- */

static void jni_on_advert(JNIEnv *env, jclass cls, jstring jname, jstring jmac,
                          jint rssi)
{
   (void)cls;
   const char *name = (*env)->GetStringUTFChars(env, jname, NULL);
   const char *mac  = (*env)->GetStringUTFChars(env, jmac, NULL);
   /* GetStringUTFChars returns NULL on OOM (with an exception pending); on this
    * per-advert hot path a NULL deref would crash. Bail cleanly instead. */
   if (!name || !mac) {
      if (name)
         (*env)->ReleaseStringUTFChars(env, jname, name);
      if (mac)
         (*env)->ReleaseStringUTFChars(env, jmac, mac);
      if ((*env)->ExceptionCheck(env))
         (*env)->ExceptionClear(env);
      return;
   }

   g_total++;
   /* This app is Stelo-only. Stelo advertises "DX01"; the Dexcom G7 advertises
    * "DXCM" -- we must NEVER touch a G7 (the user's medical safety net) or any
    * other stranger's sensor. So auto-connect is gated two ways:
    *   - once we have bonded, ONLY to that exact sensor's MAC (s.mac), and
    *   - before first pairing, ONLY to a Stelo (DX01), never a G7.
    * PAIR NEW SENSOR (g_smart_pairing) suppresses this and uses code+proximity.
    */
   int is_stelo = strncmp(name, "DX01", 4) == 0;
   struct dex_session s;
   driver_get_session(&s);
   if (!g_pairing_started && !g_smart_pairing) {
      int locked = s.mac[0] != 0;
      int match  = locked ? (strcmp(mac, s.mac) == 0) : is_stelo;
      if (match) {
         g_pairing_started = 1;
         LOGI("Stelo %s %s -> %s", name, mac, locked ? "reconnect" : "pairing");
         (*env)->CallStaticVoidMethod(env, g_ble, g_stop);
         g_scanning = 0;
         dexble_pair(env, mac, g_code_str);
      }
   }
   if (is_stelo) {
      int i = 0;
      for (i = 0; i < g_ndevs; i++)
         if (strcmp(g_devs[i].mac, mac) == 0)
            break;
      if (i == g_ndevs && g_ndevs < MAX_DEVS) {
         g_ndevs++;
         LOGI("new DXCM device: %s %s %d", name, mac, rssi);
      }
      if (i < g_ndevs) {
         (void)snprintf(g_devs[i].name, sizeof g_devs[i].name, "%s", name);
         (void)snprintf(g_devs[i].mac, sizeof g_devs[i].mac, "%s", mac);
         g_devs[i].rssi   = rssi;
         g_devs[i].seen_t = realtime_s();
         g_devs[i].count++;
         /* one cadence line per device per 30 s, to time advert bursts */
         long long now = now_ms();
         if (now - g_devs[i].last_log_ms > 30000) {
            g_devs[i].last_log_ms = now;
            LOGI("DXCM adv: %s %s rssi %d count %u", name, mac, rssi,
                 g_devs[i].count);
         }
      }
   }

   (*env)->ReleaseStringUTFChars(env, jname, name);
   (*env)->ReleaseStringUTFChars(env, jmac, mac);
   update_screen();
}

/* --- permissions --- */

static const char *perm_modern[] = {"android.permission.BLUETOOTH_SCAN",
                                    "android.permission.BLUETOOTH_CONNECT"};
static const char *perm_legacy[] = {"android.permission.ACCESS_FINE_LOCATION"};

static int has_ble_permissions(struct ANativeActivity *a)
{
   JNIEnv *env       = a->env;
   const char **want = a->sdkVersion >= 31 ? perm_modern : perm_legacy;
   jsize n           = a->sdkVersion >= 31 ? 2 : 1;

   jclass act      = (*env)->GetObjectClass(env, a->clazz);
   jmethodID check = (*env)->GetMethodID(env, act, "checkSelfPermission",
                                         "(Ljava/lang/String;)I");
   int ok          = 1;
   for (jsize i = 0; i < n; i++) {
      jstring s = (*env)->NewStringUTF(env, want[i]);
      if ((*env)->CallIntMethod(env, a->clazz, check, s) != 0)
         ok = 0;
      (*env)->DeleteLocalRef(env, s);
   }
   return ok;
}

/* Ask for every runtime permission the app wants, in one dialog sequence: the
 * BLE pair needed to reach the sensor plus notifications so alarms can alert.
 * The battery-optimisation exemption isn't a runtime permission (it's a
 * settings intent) -- g_want_battery makes on_resume pop it right afterwards.
 * The result callback never reaches native code; grant state is re-checked on
 * resume. */
static void request_ble_permissions(struct ANativeActivity *a)
{
   JNIEnv *env = a->env;
   const char *want[4];
   jsize n = 0;
   if (a->sdkVersion >= 31) {
      want[n++] = "android.permission.BLUETOOTH_SCAN";
      want[n++] = "android.permission.BLUETOOTH_CONNECT";
   } else {
      want[n++] = "android.permission.ACCESS_FINE_LOCATION";
   }
   if (a->sdkVersion >= 33)
      want[n++] = "android.permission.POST_NOTIFICATIONS";

   jclass act       = (*env)->GetObjectClass(env, a->clazz);
   jmethodID req    = (*env)->GetMethodID(env, act, "requestPermissions",
                                          "([Ljava/lang/String;I)V");
   jobjectArray arr = (*env)->NewObjectArray(
       env, n, (*env)->FindClass(env, "java/lang/String"), NULL);
   for (jsize i = 0; i < n; i++) {
      jstring s = (*env)->NewStringUTF(env, want[i]);
      (*env)->SetObjectArrayElement(env, arr, i, s);
      (*env)->DeleteLocalRef(env, s);
   }
   (*env)->CallVoidMethod(env, a->clazz, req, arr, (jint)1);
   g_want_battery = 1; /* pop the battery-exemption prompt on the next resume */
}

/* FindClass inside struct ANativeActivity callbacks resolves via the
 * framework's class loader, which can't see app classes; go through the
 * activity's own loader instead. Takes a dotted name ("com.jk.stealo.Ble"). */
static jclass find_app_class(struct ANativeActivity *a, const char *name)
{
   JNIEnv *env       = a->env;
   jclass act_cls    = (*env)->GetObjectClass(env, a->clazz);
   jmethodID get_cl  = (*env)->GetMethodID(env, act_cls, "getClassLoader",
                                           "()Ljava/lang/ClassLoader;");
   jobject loader    = (*env)->CallObjectMethod(env, a->clazz, get_cl);
   jclass loader_cls = (*env)->GetObjectClass(env, loader);
   jmethodID load    = (*env)->GetMethodID(
       env, loader_cls, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
   jstring jname = (*env)->NewStringUTF(env, name);
   jclass cls    = (*env)->CallObjectMethod(env, loader, load, jname);
   if ((*env)->ExceptionCheck(env)) {
      (*env)->ExceptionClear(env);
      return NULL;
   }
   return cls;
}

/* --- scan lifecycle (all on main thread) --- */

static void set_status(const char *s)
{
   (void)snprintf(g_status, sizeof g_status, "%s", s);
   update_screen();
}

/* Re-evaluate the alarm against the latest fresh reading and current
 * thresholds. Chime+vibrate once on the transition from NOT-alarmed to alarmed
 * -- whether that transition comes from a new glucose value or from moving a
 * threshold. Never re-fires while already alarmed (low<->high is not a new
 * entry); silences when the value returns in range. Safe to call on every
 * reading or threshold tap. */
static void alarm_reeval(void)
{
   int fresh = (g_cur_glu >= 0 && realtime_s() - g_cur_time <= 360);
   int zone  = 0;
   if (fresh && g_cur_glu < g_alarm_low)
      zone = 1;
   else if (fresh && g_cur_glu > g_alarm_high)
      zone = 2;
   int was_alarmed = (g_alarm_state != 0);
   if (!was_alarmed && zone) { /* not-alarmed -> alarmed: fire once */
      g_alarm_sounding = 1;
      dexble_alarm(zone == 2 ? ALARM_HIGH : ALARM_LOW, g_sound_on, g_vib_on);
   } else if (was_alarmed && !zone) { /* back in range: stop */
      if (g_alarm_sounding) {
         g_alarm_sounding = 0;
         dexble_alarm_silence();
      }
   }
   g_alarm_state = zone;
}

/* Stale-data ("DISCONNECT") alarm: fire when the newest reading is older than
 * the chosen threshold. A freshly opened app gets a grace period equal to the
 * threshold (data may be stale until the first sync). Evaluated on the 1 Hz
 * timer because it's the ABSENCE of new data that triggers it. */
static void disc_reeval(void)
{
   if (g_disc == 0) { /* OFF */
      if (g_disc_alarmed) {
         g_disc_alarmed = 0;
         if (g_alarm_sounding) {
            g_alarm_sounding = 0;
            dexble_alarm_silence();
         }
      }
      return;
   }
   long thr  = (long)disc_min[(unsigned)g_disc & 3U] * 60;
   long now  = realtime_s();
   int grace = (now - g_launch_t < thr);
   int stale = !grace && (g_cur_glu < 0 || now - g_cur_time > thr);
   if (stale && !g_disc_alarmed) {
      g_disc_alarmed   = 1;
      g_alarm_sounding = 1;
      dexble_alarm(ALARM_STALE, g_sound_on, g_vib_on);
   } else if (!stale && g_disc_alarmed) {
      g_disc_alarmed = 0;
      if (g_alarm_sounding) {
         g_alarm_sounding = 0;
         dexble_alarm_silence();
      }
   }
}

/* --- hooks called by the BLE driver (dexble.c) --- */
void stealo_status(const char *s)
{
   set_status(s);
}

/* current reading from the 4e stream */
void stealo_glucose(int mg_dl, int trend, int age_s)
{
   g_where   = "stealo_glucose";
   long t    = realtime_s() - age_s;
   long prev = g_nhist ? g_hist[0].t : 0;
   int has   = (g_conn_rssi_t &&
                realtime_s() - g_conn_rssi_t < 120); /* this connection */
   /* Mutate the shared history / current-reading state under the same guard the
    * renderer holds (hist_history_lock), so a main-thread draw never reads a
    * half-shifted g_hist, torn stats, or a mismatched g_cur_glu/g_cur_time. */
   hist_lock();
   int isnew = hist_insert(t, mg_dl, trend);
   if (isnew) {
      stat_add(t, mg_dl);
      if (has) {
         g_cur_rssi    = g_conn_rssi;
         g_cur_rssi_ok = 1;
      } /* keep it like glu/trend */
   }
   g_cur_glu   = g_hist[0].glu;
   g_cur_trend = g_hist[0].trend;
   g_cur_time  = g_hist[0].t;
   hist_unlock();
   if (isnew) /* file I/O outside the lock -- touches no draw-shared state */
      store_append(t, mg_dl, trend, g_conn_rssi, has);
   LOGI("glucose %d mg/dL trend %d age %d", mg_dl, trend, age_s);

   alarm_reeval(); /* chime on entering the alarmed state (see alarm_reeval) */
   /* Rendering is deferred to the main-thread 1 Hz timer (see on_main); just
    * mark the screen and notification dirty. */
   g_ui_dirty     = 1;
   g_notify_dirty = 1;

   /* Read the sensor's serial / firmware / software strings. Deferred to here
    * (after the first reading) so it runs post-auth, when the reads succeed.
    * The sensor closes the cycle within a few seconds, often before all three
    * reads land, so we retry each reconnect until we have them all -- throttled
    * to at most once a minute, and stopping entirely once complete. */
   if ((!g_model[0] || !g_fw[0] || !g_mfr[0]) &&
       realtime_s() - g_devinfo_req > 60) {
      g_devinfo_req = realtime_s();
      dexble_request_devinfo();
   }

   int did_bf = 0;
   /* Backward fill, at most once per launch: pull history back to the start of
    * the available window = min(24h, session age). Gating on session duration
    * means a young session stops re-requesting once we hold its whole span,
    * instead of forever chasing a 24h it can never reach. */
   if (!g_startup_bf) {
      g_startup_bf = 1;
      struct dex_session s;
      driver_get_session(&s);
      long target = 24L * 3600;
      if (s.have_reading && (long)s.session_seconds < target)
         target = (long)s.session_seconds;
      long oldest = g_nhist ? g_hist[g_nhist - 1].t : t;
      if (target > 600 && realtime_s() - oldest < target - 300) {
         LOGI("backward fill: %ld s backfill (have %ld s of %ld s window)",
              target, realtime_s() - oldest, target);
         driver_request_backfill(target);
         did_bf = 1;
      }
   }
   /* Ongoing: recover a genuine gap since the last reading (one request/gap).
    */
   if (!did_bf && isnew && prev) {
      long span = t - prev;
      if (span > 450) {
         LOGI("gap %ld s since last reading -> request backfill", span);
         driver_request_backfill(span);
      }
   }
}

/* live connection signal strength from readRemoteRssi (no sensor-battery cost)
 */
void stealo_rssi(int rssi)
{
   g_conn_rssi   = rssi;
   g_conn_rssi_t = realtime_s();
   LOGI("rssi %d dbm", rssi);
   draw(g_win);
}

/* device-info string (serial / firmware / software) read from DIS 0x180A */
void stealo_devinfo(const char *uuid, const char *val)
{
   if (!val || !val[0] || !uuid)
      return;
   /* uuid is the full 128-bit form "0000XXXX-0000-1000-8000-00805f9b34fb";
    * the 16-bit assigned number sits at offset 4. Guard the length before
    * indexing uuid+4 so a short/empty string can't read out of bounds. */
   int ulen = 0;
   while (uuid[ulen] && ulen < 8)
      ulen++;
   if (ulen < 8)
      return;
   char *dst = 0;
   if (strncmp(uuid + 4, "2a24", 4) == 0)
      dst = g_model; /* model number      */
   else if (strncmp(uuid + 4, "2a26", 4) == 0)
      dst = g_fw; /* firmware revision */
   else if (strncmp(uuid + 4, "2a29", 4) == 0)
      dst = g_mfr; /* manufacturer name */
   if (!dst)
      return;
   int i = 0;
   for (; val[i] && i < 22; i++)
      dst[i] = val[i];
   dst[i] = 0;
   LOGI("devinfo %s = %s", uuid, dst);
   info_save();
   draw(g_win);
}

/* --- settings-menu system ops via Ble.java (main thread; g_act->env valid) ---
 */
static void sys_set_orientation(int mode)
{
   if (!g_act || !m_set_orient)
      return;
   JNIEnv *e = g_act->env;
   (*e)->CallStaticVoidMethod(e, g_ble, m_set_orient, g_act->clazz, (jint)mode);
}

static int sys_perm_granted(const char *perm)
{
   if (!g_act || !m_perm_granted)
      return 0;
   JNIEnv *e = g_act->env;
   jstring p = (*e)->NewStringUTF(e, perm);
   jboolean r =
       (*e)->CallStaticBooleanMethod(e, g_ble, m_perm_granted, g_act->clazz, p);
   (*e)->DeleteLocalRef(e, p);
   return r;
}

static void sys_request_perm(const char *perm)
{
   if (!g_act || !m_req_perm)
      return;
   JNIEnv *e = g_act->env;
   jstring p = (*e)->NewStringUTF(e, perm);
   (*e)->CallStaticVoidMethod(e, g_ble, m_req_perm, g_act->clazz, p);
   (*e)->DeleteLocalRef(e, p);
}

static void
sys_open_settings(void) /* app details page: grant or revoke anything */
{
   if (!g_act || !m_open_settings)
      return;
   JNIEnv *e = g_act->env;
   (*e)->CallStaticVoidMethod(e, g_ble, m_open_settings, g_act->clazz);
}

static int sys_call_bool(jmethodID m)
{
   if (!g_act || !m)
      return 0;
   JNIEnv *e = g_act->env;
   return (*e)->CallStaticBooleanMethod(e, g_ble, m, g_act->clazz);
}

static void sys_request_battery(void)
{
   if (!g_act || !m_req_batt)
      return;
   JNIEnv *e = g_act->env;
   (*e)->CallStaticVoidMethod(e, g_ble, m_req_batt, g_act->clazz);
}

static int sys_standby_bucket(void)
{
   if (!g_act || !m_bucket)
      return -1;
   JNIEnv *e = g_act->env;
   return (*e)->CallStaticIntMethod(e, g_ble, m_bucket, g_act->clazz);
}

/* Sample the system states the settings screen shows into the g_sys_* cache.
 * MAIN THREAD ONLY (JNI via the activity env): call on menu-open and after an
 * action, never from a render -- build_model just copies the cache. */
static void sys_refresh(void)
{
   for (int i = 0; i < NPERMS; i++)
      g_sys_perm[i] = sys_perm_granted(perms[i]);
   g_sys_batt   = sys_call_bool(m_batt_ok);
   g_sys_bucket = sys_standby_bucket();
   g_sys_bg     = sys_call_bool(m_bg_restricted);
}

/* Close the keypad/device-list back to wherever pairing was launched from: the
 * settings menu (g_kp_return==MENU_SETTINGS) or the main screen (MENU_NONE,
 * restoring the chosen orientation). */
static void keypad_close(void)
{
   g_menu = g_kp_return;
   if (!g_kp_return)
      sys_set_orientation(g_orient);
}

/* Begin collecting pairing candidates WITHOUT disturbing the current sensor: a
 * passive scan only (the existing bond keeps reconnecting by MAC on its own).
 * g_smart_pairing suppresses the first-DX auto-pair so nothing is touched until
 * the user commits to a specific sensor. */
static void pair_scan_start(void)
{
   g_smart_pairing = 1;
   g_ndevs         = 0; /* fresh candidate list */
   if (g_act)
      start_scan(g_act);
}

/* Abandon pairing: stop the candidate scan; the existing bond is untouched. */
static void pair_cancel(void)
{
   g_smart_pairing = 0;
   if (g_act)
      stop_scan(g_act);
}

/* Choose which scanned sensor to pair:
 *   0 found  -> -1 (show the list; it fills as the scan continues)
 *   1 found  -> that one
 *   >1 found -> the strongest IF it beats the next by >= 20 dB (clearly the one
 *               on your body); otherwise -1 (ambiguous -> let the user pick).
 */
static int select_candidate(void)
{
   if (g_ndevs <= 0)
      return -1;
   if (g_ndevs == 1)
      return 0;
   int best = 0;
   for (int i = 1; i < g_ndevs; i++)
      if (g_devs[i].rssi > g_devs[best].rssi)
         best = i;
   int second = -1;
   for (int i = 0; i < g_ndevs; i++)
      if (i != best && (second < 0 || g_devs[i].rssi > g_devs[second].rssi))
         second = i;
   if (second < 0 || g_devs[best].rssi - g_devs[second].rssi >= 20)
      return best;
   return -1;
}

/* Commit to a specific sensor: NOW drop the old bond and pair the chosen MAC
 * with the entered code. Only reached after the code is in and a candidate is
 * chosen (auto or from the list). */
static void commit_pair(const char *mac)
{
   driver_forget();
   g_smart_pairing   = 0;
   g_pairing_started = 1; /* lock onto this MAC; don't auto-grab others */
   if (g_act)
      stop_scan(g_act);
   set_status("PAIRING");
   if (g_act)
      dexble_pair(g_act->env, mac, g_code_str);
   keypad_close();
   LOGI("pair new sensor %s with code %s", mac, g_code_str);
}

static void menu_action(int action)
{
   if (action == 0) {
      g_orient = (int)(((unsigned)g_orient + 1U) & 3U);
      settings_save();
   } /* applied on close */
   else if (action == 1) {
      g_sound_on = !g_sound_on;
      settings_save();
   } else if (action == 2) {
      g_vib_on = !g_vib_on;
      settings_save();
   } else if (action == 3) {
      g_units = !g_units;
      settings_save();
   } else if (action == 4) {
      g_disc = (int)(((unsigned)g_disc + 1U) & 3U);
      settings_save();
   } else if (action == 20) { /* battery optimisation: request if optimised,
                                 else settings */
      if (g_sys_batt)
         sys_open_settings();
      else
         sys_request_battery();
      sys_refresh();
   } else if (action == 22) {
      sys_open_settings();
      sys_refresh();
   } /* bg-exec: change in settings */
   else if (action >= 10 && action < 10 + NPERMS) {
      /* denied -> request dialog; granted -> app settings (only place to
       * revoke) */
      if (g_sys_perm[action - 10])
         sys_open_settings();
      else
         sys_request_perm(perms[action - 10]);
      sys_refresh();
   } else if (action == 99) {
      g_menu = MENU_NONE;
      sys_set_orientation(g_orient);
   } /* apply orient */
   /* --- keypad (opened from settings rows: return there on close) --- */
   else if (action == 30) {
      g_menu      = MENU_KEYPAD;
      g_kp_mode   = 0;
      g_kp_return = MENU_SETTINGS;
      g_entrylen  = 0;
      pair_scan_start(); /* scan under the code entry to hide the delay */
   } else if (action == 31) {
      g_menu      = MENU_KEYPAD;
      g_kp_mode   = 1;
      g_kp_return = MENU_SETTINGS;
      g_entrylen  = 0;
   } else if (action >= 100 && action <= 109) { /* digit */
      int cap = g_kp_mode ? 3 : 4;
      if (g_entrylen < cap)
         g_entry[g_entrylen++] = (char)('0' + (action - 100));
   } else if (action == 110) {
      if (g_entrylen > 0)
         g_entrylen--;
   } /* backspace */
   else if (action == 113) {
      if (g_smart_pairing)
         pair_cancel(); /* abandon pairing, keep the old bond */
      keypad_close();
   } /* X -> close */
   else if (action == 199) { /* device list: cancel */
      pair_cancel();
      keypad_close();
   } else if (action >= 200 &&
              action < 200 + MAX_DEVS) { /* device list: pick */
      /* Only honour a device-pick while the list is actually open and the index
       * is a real device. The hit-box array is rebuilt by draw() (which can run
       * on a BLE thread), so a tap racing a repaint could otherwise map to a
       * phantom pick and commit_pair -> driver_forget would drop the live bond.
       */
      int idx = action - 200;
      if (g_menu == MENU_DEVLIST && idx < g_ndevs)
         commit_pair(g_devs[idx].mac);
   } else if (action == 111) { /* OK */
      if (g_kp_mode) {         /* PLOT MAX: entry is in the display unit */
         if (g_entrylen > 0) {
            int v = 0;
            for (int i = 0; i < g_entrylen; i++)
               v = (v * 10) + (g_entry[i] - '0');
            int mgdl = g_units ? v * 18 : v; /* mmol/L -> mg/dL */
            if (mgdl < 100)
               mgdl = 100;
            if (mgdl > 400)
               mgdl = 400;
            g_plot_max = mgdl;
            plot_set_max(mgdl);
            settings_save();
            keypad_close();
         }
      } else if (g_entrylen == 4) { /* PAIR: code in, now pick the sensor */
         for (int i = 0; i < 4; i++)
            g_code_str[i] = g_entry[i];
         g_code_str[4] = 0;
         code_save();
         int idx = select_candidate();
         if (idx >= 0)
            commit_pair(g_devs[idx].mac); /* clear winner: pair it */
         else
            g_menu = MENU_DEVLIST; /* ambiguous/none: let the user choose */
      }
   }
   if (g_win)
      draw(g_win);
}

/* Plot rectangle recorded by the last render (the ACT_SCRUB target), so a drag
 * can resolve to a datapoint even after the finger leaves the plot. */
static int plot_rect(int *x, int *y, int *w, int *h)
{
   for (int i = 0; i < g_hits.n; i++)
      if (g_hits.box[i].kind == ACT_SCRUB) {
         *x = g_hits.box[i].x;
         *y = g_hits.box[i].y;
         *w = g_hits.box[i].w;
         *h = g_hits.box[i].h;
         return 1;
      }
   return 0;
}

/* adjust the threshold for button i by +/-5, clamped and kept low<=high; saves
 */
static void alarm_adjust(int i)
{
   int *v = (i < 2) ? &g_alarm_low : &g_alarm_high;
   *v += ((unsigned)i & 1U) ? 5 : -5; /* even=minus, odd=plus */
   if (*v < 40)
      *v = 40;
   if (*v > 400)
      *v = 400;
   if (g_alarm_low > g_alarm_high) { /* keep low <= high */
      if (i < 2)
         g_alarm_low = g_alarm_high;
      else
         g_alarm_high = g_alarm_low;
   }
   alarm_save();
   alarm_reeval(); /* a threshold move can itself enter/leave the alarmed state
                    */
}

/* an older reading recovered via backfill: store it, place it in history, but
 * don't disturb the current value unless it turns out to be the newest */
void stealo_backfill(int mg_dl, int trend, int age_s)
{
   long t = realtime_s() - age_s;
   hist_lock();
   int isnew = hist_insert(t, mg_dl, trend);
   if (isnew)
      stat_add(t, mg_dl);
   g_cur_glu   = g_hist[0].glu;
   g_cur_trend = g_hist[0].trend;
   g_cur_time  = g_hist[0].t;
   hist_unlock();
   if (isnew)
      store_append(t, mg_dl, trend, 0, 0); /* no RSSI for backfilled points */
   LOGI("backfill reading %d mg/dL age %d -> t=%ld", mg_dl, age_s, t);
   /* A gap recovered by backfill can be the newest reading (a missed live
    * cycle); re-evaluate the alarm and refresh the notification rather than
    * waiting for the next live reading. Rendering is on the 1 Hz timer. */
   alarm_reeval();
   g_ui_dirty     = 1;
   g_notify_dirty = 1;
}

static void start_scan(struct ANativeActivity *a)
{
   JNIEnv *env = a->env;

   if (g_scanning || !g_ble)
      return;
   if (!has_ble_permissions(a)) {
      set_status("NO PERMISSION");
      return;
   }
   jstring err = (*env)->CallStaticObjectMethod(env, g_ble, g_scan, a->clazz);
   if ((*env)->ExceptionCheck(env)) {
      (*env)->ExceptionClear(env);
      set_status("SCAN THREW");
      return;
   }
   if (err) {
      const char *e = (*env)->GetStringUTFChars(env, err, NULL);
      LOGI("scan: %s", e);
      set_status(e);
      (*env)->ReleaseStringUTFChars(env, err, e);
      return;
   }
   g_scanning = 1;
   LOGI("scanning (receive-only)");
   /* only surface SCANNING before we're operational; once paired/streaming the
    * driver's own status (WAITING/CONNECTED/...) is the meaningful one and the
    * background scan must not mask it */
   struct dex_session s;
   driver_get_session(&s);
   if (!s.paired && !s.have_reading)
      set_status("SCANNING");
}

static void stop_scan(struct ANativeActivity *a)
{
   JNIEnv *env = a->env;

   if (!g_scanning)
      return;
   (*env)->CallStaticVoidMethod(env, g_ble, g_stop);
   if ((*env)->ExceptionCheck(env))
      (*env)->ExceptionClear(env);
   g_scanning = 0;
   /* don't surface "PAUSED": stopping the background scan is an internal detail
    * (it happens on pause and on every orientation flip); the driver's own
    * connection status stays the meaningful thing to show */
}

/* --- input: drain the queue so the ANR watchdog stays fed --- */

/* reprogram the shared timer: first tick after `first_ms`, then every
 * `repeat_ms`. Used to switch between the 1 Hz repaint cadence and the hold-to-
 * repeat cadence -- which waits before repeating so a quick tap doesn't repeat.
 */
static void timer_set(long first_ms, long repeat_ms)
{
   if (g_timerfd < 0)
      return;
   struct itimerspec its;
   its.it_value.tv_sec     = first_ms / 1000;
   its.it_value.tv_nsec    = (first_ms % 1000) * 1000000L;
   its.it_interval.tv_sec  = repeat_ms / 1000;
   its.it_interval.tv_nsec = (repeat_ms % 1000) * 1000000L;
   timerfd_settime(g_timerfd, 0, &its, 0);
}

/* Push the live value + a 3H plot into the ongoing notification (lock screen /
 * shade). Main thread only (uses g_act->env). The plot is grayscale (white dots
 * on dark), so the ARGB bitmap needs no colour swizzle. Called from on_timer
 * when a new reading has arrived, so we rebuild the bitmap at most once a
 * cycle, not every second. */
static uint32_t g_notify_px[NOTIFY_W * NOTIFY_H];

static void notify_update(void)
{
   if (!g_act || !m_show_glucose || !g_ble)
      return;
   JNIEnv *e = g_act->env;
   char title[48];
   char text[48];
   int stale = realtime_s() - g_cur_time > 360;
   if (g_cur_glu < 0 || stale) {
      (void)snprintf(title, sizeof title, "--- %s", UNIT_LBL);
      (void)snprintf(text, sizeof text, "no recent reading");
   } else {
      char gv[12];
      char tr[8];
      char hm[16];
      fmt_glu(g_cur_glu, g_units, gv, sizeof gv);
      fmt_trend(g_cur_trend, tr, sizeof tr);
      fmt_hms(g_cur_time, g_tz_off, hm, sizeof hm);
      hm[5] = 0; /* HH:MM */
      (void)snprintf(title, sizeof title, "%s %s  %s", gv, UNIT_LBL, tr);
      (void)snprintf(text, sizeof text, "at %s", hm);
   }
   for (int i = 0; i < NOTIFY_W * NOTIFY_H; i++)
      g_notify_px[i] = 0xFF181818;
   static struct plot_pt pts[NHIST];
   hist_lock(); /* consistent snapshot vs a BLE-thread hist_insert */
   int np = g_nhist;
   for (int i = 0; i < np; i++) {
      pts[i].t   = g_hist[i].t;
      pts[i].glu = g_hist[i].glu;
   }
   hist_unlock();
   plot_render(g_notify_px, NOTIFY_W, NOTIFY_W, NOTIFY_H, 0, 0, NOTIFY_W,
               NOTIFY_H, pts, np, realtime_s(), 3, 3, white_color, -1, 0);
   jstring jt    = (*e)->NewStringUTF(e, title);
   jstring js    = (*e)->NewStringUTF(e, text);
   jintArray arr = (*e)->NewIntArray(e, NOTIFY_W * NOTIFY_H);
   /* On OOM any of these is NULL with an exception pending; SetIntArrayRegion
    * on a NULL array aborts the VM, so bail and clean up instead. */
   if (jt && js && arr) {
      (*e)->SetIntArrayRegion(e, arr, 0, NOTIFY_W * NOTIFY_H,
                              (const jint *)g_notify_px);
      (*e)->CallStaticVoidMethod(e, g_ble, m_show_glucose, g_act->clazz, jt, js,
                                 arr, (jint)NOTIFY_W, (jint)NOTIFY_H);
   }
   if ((*e)->ExceptionCheck(e))
      (*e)->ExceptionClear(e);
   if (jt)
      (*e)->DeleteLocalRef(e, jt);
   if (js)
      (*e)->DeleteLocalRef(e, js);
   if (arr)
      (*e)->DeleteLocalRef(e, arr);
}

/* timer tick: repaint so AGE / stale state stay live; while a +/- button is
 * held (fast cadence) also step the threshold, for hold-to-repeat */
static int on_timer(int fd, int events, void *data)
{
   g_where = "on_timer";
   (void)events;
   (void)data;
   /* on_timer IS the render thread; reaffirm g_main_tid here so on_main() can
    * never be wrong about who may draw -- guarantees the 1 Hz repaint always
    * runs even if the onCreate capture were somehow off, so the UI can't wedge.
    */
   g_main_tid     = gettid();
   uint64_t ticks = 0;
   read(fd, &ticks, sizeof ticks); /* single read clears the expiration count */
   if (g_al_held >= 0)
      alarm_adjust(g_al_held);
   disc_reeval(); /* stale-data alarm depends on elapsed time */
   /* Stall watchdog: if we're paired but no reading has arrived in ~12 min (2+
    * missed cycles), the link is likely stranded (e.g. an orphaned GATT client
    * left by a crash/force-stop). Force a fresh connect to self-heal -- no BT
    * toggle. Throttled to once every 5 min so it never hammers the sensor. */
   {
      static long last_kick;
      struct dex_session s;
      driver_get_session(&s);
      long now = realtime_s();
      long age = (g_cur_time > 0) ? now - g_cur_time : now - g_launch_t;
      if (s.paired && age > 700 && now - last_kick > 300 && g_act) {
         last_kick = now;
         LOGI("watchdog: %ld s since last reading -> force reconnect", age);
         dexble_reconnect(g_act->env);
      }
   }
   if (g_notify_dirty) { /* refresh the lock-screen notification on the main
                            thread */
      g_notify_dirty = 0;
      notify_update();
   }
   /* Rebuild the text lines here (BLE-thread updates only marked g_ui_dirty),
    * then repaint unconditionally so AGE / stale state stay live. Both run on
    * the main thread, so the hit-box geometry can't race on_input. */
   g_ui_dirty = 0;
   update_screen();
   if (g_win)
      draw(g_win);
   return 1;
}

static int on_input(int fd, int events, void *data)
{
   g_where = "on_input";
   (void)fd;
   (void)events;
   struct AInputQueue *q  = data;
   struct AInputEvent *ev = NULL;
   while (AInputQueue_getEvent(q, &ev) >= 0) {
      if (AInputQueue_preDispatchEvent(q, ev))
         continue; /* IME took it; finished elsewhere */
      int handled = 0;
      if (AInputEvent_getType(ev) == AINPUT_EVENT_TYPE_MOTION) {
         int action = (int)((unsigned)AMotionEvent_getAction(ev) &
                            (unsigned)AMOTION_EVENT_ACTION_MASK);
         int tx     = (int)AMotionEvent_getX(ev, 0);
         int ty     = (int)AMotionEvent_getY(ev, 0);
         /* A sounding alarm is silenced by ANY press anywhere in the app --
          * main screen, settings, keypad, gate -- and that press does nothing
          * else. Checked before every modal handler so it always wins. */
         if (action == AMOTION_EVENT_ACTION_DOWN && g_alarm_sounding) {
            g_alarm_sounding = 0;
            dexble_alarm_silence();
            AInputQueue_finishEvent(q, ev, 1);
            continue;
         }
         /* the first-run rationale screen is modal: a tap on CONTINUE fires the
          * permission request; anything else is ignored */
         if (g_gate) {
            if (action == AMOTION_EVENT_ACTION_DOWN &&
                ui_hit(&g_hits, tx, ty).kind == ACT_GATE_CONTINUE) {
               g_gate = 0;
               if (g_act)
                  request_ble_permissions(g_act);
               if (g_win)
                  draw(g_win);
            }
            AInputQueue_finishEvent(q, ev, 1);
            continue;
         }
         /* All modal menus (settings / keypad / device list) are pure now: a
          * tap maps via the recorded ACT_MENU targets to a menu_action code. */
         if (g_menu) {
            if (action == AMOTION_EVENT_ACTION_DOWN) {
               struct action a = ui_hit(&g_hits, tx, ty);
               if (a.kind == ACT_MENU)
                  menu_action(a.arg);
            }
            AInputQueue_finishEvent(q, ev, 1);
            continue;
         }
         /* main screen: resolve the tap against the targets recorded by the
          * last ui_render(), then run the shell-side gesture/timer state. */
         struct action act = ui_hit(&g_hits, tx, ty);
         /* A drag begins on a press inside the plot; once begun, keep scrubbing
          * for every MOVE -- even when the finger leaves the plot rectangle --
          * using its X position (plot_hit picks by time/X only). */
         int begin =
             (action == AMOTION_EVENT_ACTION_DOWN && act.kind == ACT_SCRUB);
         int cont = (action == AMOTION_EVENT_ACTION_MOVE && g_scrubbing);
         int rx   = 0;
         int ry   = 0;
         int rw   = 0;
         int rh   = 0;
         if ((begin || cont) && plot_rect(&rx, &ry, &rw, &rh)) {
            g_scrubbing = 1;
            /* Average the current sample with the batched historical ones so
             * the pick tracks the centre of the contact, not a jittery edge. */
            unsigned long hs = AMotionEvent_getHistorySize(ev);
            long ax          = tx;
            long ay          = ty;
            long n           = 1;
            for (unsigned long h = 0; h < hs; h++) {
               ax += (long)AMotionEvent_getHistoricalX(ev, 0, h);
               ay += (long)AMotionEvent_getHistoricalY(ev, 0, h);
               n++;
            }
            int fx = (int)(ax / n);
            int fy = (int)(ay / n);
            static struct plot_pt pts[NHIST];
            int np = g_nhist < NHIST ? g_nhist : NHIST;
            for (int i = 0; i < np; i++) {
               pts[i].t   = g_hist[i].t;
               pts[i].glu = g_hist[i].glu;
            }
            int idx = plot_hit(rx, ry, rw, rh, pts, np, realtime_s(),
                               g_plot_hours, fx, fy);
            if (idx != g_scrub_idx) {
               g_scrub_idx = idx;
               draw(g_win);
            }
            handled = 1;
         } else if (action == AMOTION_EVENT_ACTION_DOWN) {
            if (act.kind == ACT_OPEN_SETTINGS) {
               sys_refresh(); /* snapshot system state before draw (main
                                 thread)*/
               g_menu = MENU_SETTINGS;
               sys_set_orientation(0);
               if (g_win)
                  draw(g_win);
               handled = 1;
            } else if (act.kind == ACT_PAIR_NEW) { /* "SENSOR EXPIRED" prompt */
               g_menu      = MENU_KEYPAD;
               g_kp_mode   = 0;
               g_kp_return = MENU_NONE;
               g_entrylen  = 0;
               sys_set_orientation(0);
               if (g_win)
                  draw(g_win);
               handled = 1;
            } else if (act.kind == ACT_ALARM_LOW ||
                       act.kind == ACT_ALARM_HIGH) {
               /* step once now; repeat only after a 400 ms hold, then 120 ms.
                * button index: 0 LOW- 1 LOW+ 2 HIGH- 3 HIGH+ */
               int btn =
                   (act.kind == ACT_ALARM_HIGH ? 2 : 0) + (act.arg > 0 ? 1 : 0);
               g_al_held = btn;
               alarm_adjust(btn);
               timer_set(400, 120);
               draw(g_win);
               handled = 1;
            }
         } else if (action == AMOTION_EVENT_ACTION_UP ||
                    action == AMOTION_EVENT_ACTION_CANCEL) {
            g_scrubbing = 0;
            if (g_al_held >= 0) { /* release stops the auto-repeat */
               g_al_held = -1;
               timer_set(1000, 1000);
               handled = 1;
            } else if (g_scrub_idx >= 0) { /* release clears the highlight */
               g_scrub_idx = -1;
               draw(g_win);
               handled = 1;
            } else if (act.kind == ACT_PLOT_TAB) { /* a tab tap (arg = hours) */
               g_plot_hours = act.arg;
               draw(g_win);
               handled = 1;
            }
         }
      }
      AInputQueue_finishEvent(q, ev, handled);
   }
   return 1; /* keep the callback registered */
}

/* --- activity callbacks --- */

/* The activity can be destroyed (back-press) while the foreground service keeps
 * the process alive. Without this, the 1 Hz timer keeps firing and derefs the
 * freed g_act (watchdog / notify_update) -- a use-after-free. Remove and close
 * the timer fd and clear g_act/g_win so nothing touches the dead activity; the
 * one-time init is guarded so a later relaunch re-runs onCreate cleanly. */
static void on_destroy(struct ANativeActivity *a)
{
   (void)a;
   if (g_looper && g_timerfd >= 0)
      ALooper_removeFd(g_looper, g_timerfd);
   if (g_timerfd >= 0)
      close(g_timerfd);
   g_timerfd = -1;
   __atomic_store_n(&g_win, NULL, __ATOMIC_SEQ_CST);
   while (
       __atomic_load_n(&g_draw_busy, __ATOMIC_SEQ_CST)) /* let a draw finish */
      ;
   g_act = NULL;
}

static void on_queue_created(struct ANativeActivity *a, struct AInputQueue *q)
{
   (void)a;
   AInputQueue_attachLooper(q, ALooper_forThread(), 1, on_input, q);
}

static void on_queue_destroyed(struct ANativeActivity *a, struct AInputQueue *q)
{
   (void)a;
   AInputQueue_detachLooper(q);
}

static void on_resume(struct ANativeActivity *a)
{
   start_scan(a);
   sys_refresh();        /* a permission/settings dialog may have returned */
   if (g_want_battery) { /* first-boot: chain the battery-opt prompt once */
      g_want_battery = 0;
      if (!g_sys_batt)
         sys_request_battery();
   }
   if (g_menu && g_win)
      draw(g_win); /* so the menu reflects the new state at once */
}

static void on_pause(struct ANativeActivity *a)
{
   stop_scan(a);
}

static void on_window_created(struct ANativeActivity *a,
                              struct ANativeWindow *win)
{
   (void)a;
   __atomic_store_n(&g_win, win, __ATOMIC_SEQ_CST);
   g_nlines = 0;
   update_screen();
   draw(win); /* force the first frame; update_screen's throttle may skip it */
}

static void on_window_destroyed(struct ANativeActivity *a,
                                struct ANativeWindow *win)
{
   (void)a;
   (void)win;
   /* Stop new draws targeting this surface, then wait for any in-flight draw
    * (possibly on a BLE thread) to finish before we return -- the framework
    * frees the struct ANativeWindow once this callback returns, so returning
    * while a draw_impl still holds it would be a use-after-free. draw_impl does
    * no blocking work, so this spin is sub-millisecond. */
   __atomic_store_n(&g_win, NULL, __ATOMIC_SEQ_CST);
   while (__atomic_load_n(&g_draw_busy, __ATOMIC_SEQ_CST))
      ;
}

static void on_redraw_needed(struct ANativeActivity *a,
                             struct ANativeWindow *win)
{
   (void)a;
   draw(win);
}

static void on_window_resized(struct ANativeActivity *a,
                              struct ANativeWindow *win)
{
   (void)a;
   draw(win);
}

static char g_crash_path[256];

/* native crash logger: append signal + a little app context, then re-raise so
 * the OS still records its tombstone. Retrieve with run-as cat files/crash.log
 */
static void crash_handler(int sig)
{
   char b[200];
   int n = snprintf(
       b, sizeof b,
       "CRASH sig=%d t=%ld where=%.40s status=%.24s glu=%d menu=%d nhist=%d\n",
       sig, realtime_s(), g_where, g_status, g_cur_glu, g_menu, g_nhist);
   n      = clampn(n, sizeof b);
   int fd = open(g_crash_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
   if (fd >= 0) {
      if (write(fd, b, n) != n) {
      }
      close(fd);
   }
   (void)signal(sig, SIG_DFL);
   (void)raise(sig);
}

static void crash_install(const char *dir)
{
   int i = 0;
   for (; dir[i] && i < 230; i++)
      g_crash_path[i] = dir[i];
   const char *f = "/crash.log";
   for (int j = 0; f[j]; j++)
      g_crash_path[i++] = f[j];
   g_crash_path[i] = 0;
   int sigs[]      = {4 /*ILL*/, 6 /*ABRT*/, 7 /*BUS*/, 8 /*FPE*/, 11 /*SEGV*/};
   for (unsigned k = 0; k < sizeof sigs / sizeof sigs[0]; k++)
      (void)signal(sigs[k], crash_handler);
}

/* Read the device's local UTC offset (seconds) into g_tz_off for on-screen
 * timestamps. Split out of ANativeActivity_onCreate; self-contained JNI. */
static void init_tz_offset(JNIEnv *env)
{
   jclass tzc  = (*env)->FindClass(env, "java/util/TimeZone");
   jclass sysc = (*env)->FindClass(env, "java/lang/System");
   if (tzc && sysc) {
      jmethodID get_def = (*env)->GetStaticMethodID(env, tzc, "getDefault",
                                                    "()Ljava/util/TimeZone;");
      jmethodID ctm =
          (*env)->GetStaticMethodID(env, sysc, "currentTimeMillis", "()J");
      jmethodID get_off = (*env)->GetMethodID(env, tzc, "getOffset", "(J)I");
      jobject tz        = (*env)->CallStaticObjectMethod(env, tzc, get_def);
      jlong now         = (*env)->CallStaticLongMethod(env, sysc, ctm);
      if (tz) /* getDefault can return null; don't call getOffset on it */
         g_tz_off = (*env)->CallIntMethod(env, tz, get_off, now) / 1000;
   }
   if ((*env)->ExceptionCheck(env))
      (*env)->ExceptionClear(env);
}

/* NDK entry point: resolved by name by the Android runtime when the .so loads.
 * Explicitly exported (we build -fvisibility=hidden); it is external by
 * necessity, which is also why it cannot be given internal linkage. */
__attribute__((visibility("default"))) void
ANativeActivity_onCreate(struct ANativeActivity *activity, void *saved,
                         size_t saved_size)
{
   (void)saved;
   (void)saved_size;
   JNIEnv *env = activity->env;

   /* This runs on the main/UI looper thread; record it so
    * draw()/update_screen() can tell it apart from BLE binder threads (see
    * on_main). The looper callbacks (on_timer/on_input) are registered on this
    * same thread below. */
   g_main_tid = gettid();
   g_act      = activity;
   g_launch_t = realtime_s(); /* stale-alarm grace starts now */
   crash_install(activity->internalDataPath ? activity->internalDataPath
                                            : "/data/local/tmp");

   /* local timezone offset (seconds), for on-screen timestamps */
   init_tz_offset(env);

   /* keep the screen (and thus the foreground scan) alive between the
    * sensors' advertising bursts, which can be many minutes apart */
   ANativeActivity_setWindowFlags(activity, AWINDOW_FLAG_KEEP_SCREEN_ON, 0);
   activity->callbacks->onResume                   = on_resume;
   activity->callbacks->onPause                    = on_pause;
   activity->callbacks->onNativeWindowCreated      = on_window_created;
   activity->callbacks->onNativeWindowDestroyed    = on_window_destroyed;
   activity->callbacks->onNativeWindowRedrawNeeded = on_redraw_needed;
   activity->callbacks->onNativeWindowResized      = on_window_resized;
   activity->callbacks->onInputQueueCreated        = on_queue_created;
   activity->callbacks->onInputQueueDestroyed      = on_queue_destroyed;
   activity->callbacks->onDestroy                  = on_destroy;

   /* Process-wide, one-time setup: JNI globals, the BLE driver, and the loaded
    * history/settings. The foreground service can outlive the activity, so a
    * relaunch re-enters onCreate in the same process -- guard this so we don't
    * leak the g_ble global ref, re-register natives, or reload the history. */
   if (!g_inited) {
      jclass ble = find_app_class(activity, "com.jk.stealo.Ble");
      if (!ble) {
         LOGI("Ble class NOT found");
         set_status("NO BLE CLASS!");
         return;
      }
      g_ble = (*env)->NewGlobalRef(env, ble);

      /* char[] (not literals) so the char* JNINativeMethod fields need no const
       * cast */
      static char nm_advert[] = "onAdvert";
      static char sg_advert[] = "(Ljava/lang/String;Ljava/lang/String;I)V";
      static const JNINativeMethod methods[] = {
          {nm_advert, sg_advert, (void *)jni_on_advert},
      };
      if ((*env)->RegisterNatives(env, g_ble, methods, 1) != 0) {
         LOGI("RegisterNatives failed");
         set_status("JNI REG FAILED!");
         return;
      }
      g_scan = (*env)->GetStaticMethodID(
          env, g_ble, "scan", "(Landroid/content/Context;)Ljava/lang/String;");
      g_stop = (*env)->GetStaticMethodID(env, g_ble, "stop", "()V");
      m_show_glucose =
          (*env)->GetStaticMethodID(env, g_ble, "showGlucose",
                                    "(Landroid/content/Context;Ljava/lang/"
                                    "String;Ljava/lang/String;[III)V");
      m_set_orient = (*env)->GetStaticMethodID(env, g_ble, "setOrientation",
                                               "(Landroid/content/Context;I)V");
      m_perm_granted = (*env)->GetStaticMethodID(
          env, g_ble, "permGranted",
          "(Landroid/content/Context;Ljava/lang/String;)Z");
      m_req_perm = (*env)->GetStaticMethodID(
          env, g_ble, "requestPerm",
          "(Landroid/content/Context;Ljava/lang/String;)V");
      m_open_settings = (*env)->GetStaticMethodID(
          env, g_ble, "openAppSettings", "(Landroid/content/Context;)V");
      m_batt_ok = (*env)->GetStaticMethodID(env, g_ble, "isBatteryUnrestricted",
                                            "(Landroid/content/Context;)Z");
      m_req_batt = (*env)->GetStaticMethodID(env, g_ble, "requestBatteryOpt",
                                             "(Landroid/content/Context;)V");
      m_bucket   = (*env)->GetStaticMethodID(env, g_ble, "standbyBucket",
                                             "(Landroid/content/Context;)I");
      m_bg_restricted = (*env)->GetStaticMethodID(
          env, g_ble, "isBgRestricted", "(Landroid/content/Context;)Z");
      m_bonded_stelo = (*env)->GetStaticMethodID(
          env, g_ble, "bondedStelo",
          "(Landroid/content/Context;)Ljava/lang/String;");

      /* wire up the BLE protocol driver (registers its own Ble callbacks) */
      dexble_init(activity->internalDataPath ? activity->internalDataPath
                                             : "/data/local/tmp");
      if (!dexble_register(env, g_ble, activity->clazz))
         LOGI("dexble_register failed");

      /* Robust reconnect: if we hold a key (bonded) but have no saved sensor
       * MAC
       * -- e.g. after an app update that added MAC persistence, or if stelo.mac
       * was lost -- resolve the sensor's address from the system bond list
       * (reliable names) and lock onto it, so we never fall back to guessing
       * from adverts (whose local name is usually absent) and never touch the
       * G7. */
      {
         struct dex_session s;
         driver_get_session(&s);
         if (!s.mac[0] && s.paired && m_bonded_stelo) {
            jstring jm = (*env)->CallStaticObjectMethod(
                env, g_ble, m_bonded_stelo, activity->clazz);
            /* getBondedDevices() throws SecurityException if BLUETOOTH_CONNECT
             * was revoked after pairing; clear it so the next JNI call
             * (find_app_class for the Alarm class) is not made with an
             * exception pending. */
            if ((*env)->ExceptionCheck(env))
               (*env)->ExceptionClear(env);
            if (jm) {
               const char *bm = (*env)->GetStringUTFChars(env, jm, NULL);
               if (bm && bm[0]) {
                  LOGI("locked to bonded Stelo %s", bm);
                  drv_mac_save(bm);
                  driver_lock_mac(bm);
               }
               if (bm)
                  (*env)->ReleaseStringUTFChars(env, jm, bm);
               (*env)->DeleteLocalRef(env, jm);
            }
         }
      }
      /* Alarm class must come through the app's own classloader (see
       * find_app_class) */
      dexble_set_alarm(env, find_app_class(activity, "com.jk.stealo.Alarm"));

      /* persistent reading log: remember our own datapoints across restarts.
       * Internal storage; the app is debuggable, so retrieve with
       *   adb shell run-as com.jk.stealo cat files/readings.csv > readings.csv
       */
      {
         const char *dir = activity->internalDataPath
                               ? activity->internalDataPath
                               : "/data/local/tmp";
         int i           = 0;
         for (; dir[i] && i < 230; i++)
            g_store_path[i] = dir[i];
         const char *f = "/readings.csv";
         for (int j = 0; f[j]; j++)
            g_store_path[i++] = f[j];
         g_store_path[i] = 0;
         int k           = 0;
         for (; dir[k] && k < 230; k++)
            g_info_path[k] = dir[k];
         const char *g = "/stelo.info";
         for (int j = 0; g[j]; j++)
            g_info_path[k++] = g[j];
         g_info_path[k] = 0;
         int m          = 0;
         for (; dir[m] && m < 230; m++)
            g_alarm_path[m] = dir[m];
         const char *h = "/alarm.cfg";
         for (int j = 0; h[j]; j++)
            g_alarm_path[m++] = h[j];
         g_alarm_path[m] = 0;
         int p           = 0;
         for (; dir[p] && p < 230; p++)
            g_settings_path[p] = dir[p];
         const char *sf = "/settings.cfg";
         for (int j = 0; sf[j]; j++)
            g_settings_path[p++] = sf[j];
         g_settings_path[p] = 0;
         int cp             = 0;
         for (; dir[cp] && cp < 230; cp++)
            g_code_path[cp] = dir[cp];
         const char *cf = "/paircode.txt";
         for (int j = 0; cf[j]; j++)
            g_code_path[cp++] = cf[j];
         g_code_path[cp] = 0;
         store_load();
         stat_load(g_store_path);
         info_load();
         alarm_load();
         settings_load();
         code_load();
         sys_set_orientation(g_orient); /* restore last-chosen orientation */
         g_stored = store_count();
         LOGI("reading log: %s (%d in memory, %d stored)", g_store_path,
              g_nhist, g_stored);
      }
      g_inited = 1;
   }

   /* 1 Hz repaint timer so AGE / stale state stay current without a touch.
    * On an activity relaunch in the same process, tear down any prior timer
    * first so we don't leak an fd + looper callback that fires with a stale
    * g_act (see on_destroy). */
   g_looper = ALooper_forThread();
   if (g_timerfd >= 0) {
      ALooper_removeFd(g_looper, g_timerfd);
      close(g_timerfd);
      g_timerfd = -1;
   }
   {
      int tfd = timerfd_create(CLOCK_MONOTONIC, 04000 /* TFD_NONBLOCK */);
      if (tfd >= 0) {
         g_timerfd = tfd;
         struct itimerspec its;
         its.it_value.tv_sec     = 1;
         its.it_value.tv_nsec    = 0;
         its.it_interval.tv_sec  = 1;
         its.it_interval.tv_nsec = 0;
         timerfd_settime(tfd, 0, &its, 0);
         ALooper_addFd(g_looper, tfd, 3, ALOOPER_EVENT_INPUT, on_timer, 0);
      }
   }

   /* Don't fire the system permission dialog on cold start. Show the
    * rationale screen first; CONTINUE (in on_input) issues the actual
    * request. */
   if (!has_ble_permissions(activity))
      g_gate = 1;
}
