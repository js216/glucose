/* stealo native core, plain C — no NDK glue.
 * Implements ANativeActivity_onCreate directly; links against stub
 * libc/libandroid/liblog (stub_*.c) — the phone binds the real bionic ones.
 * jni.h comes from the host JDK (same ABI as Android's).
 *
 * Milestone A: receive-only BLE scan; lists DXCM* advertisements on screen.
 */
#include <stdint.h>
#include <stddef.h>
#include <jni.h>
#include "plot.h"
#include "dexdriver.h"

int __android_log_print(int prio, const char *tag, const char *fmt, ...);
#define LOGI(...) __android_log_print(4, "stealo", __VA_ARGS__)

/* bionic libc, declared by hand (no Android headers on the host) */
int    snprintf(char *s, size_t n, const char *fmt, ...);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
struct timespec { long tv_sec, tv_nsec; };
int    clock_gettime(int clk, struct timespec *ts);
#define CLOCK_MONOTONIC 1

/* file I/O for the persistent reading log */
void  *malloc(unsigned long n);
void   free(void *p);
int    open(const char *path, int flags, ...);
long   read(int fd, void *buf, unsigned long n);
long   write(int fd, const void *buf, unsigned long n);
int    close(int fd);
long   lseek(int fd, long off, int whence);
#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT  0100
#define O_TRUNC  01000
#define O_APPEND 02000
#define SEEK_SET 0
#define SEEK_END 2

/* periodic timer (timerfd) so AGE / stale-state repaint without a touch */
struct itimerspec { struct timespec it_interval, it_value; };
int  timerfd_create(int clockid, int flags);
int  timerfd_settime(int fd, int flags, const struct itimerspec *nv, struct itimerspec *ov);

/* --- minimal declarations normally provided by NDK headers --- */

typedef struct ANativeWindow ANativeWindow;

typedef struct {
    int32_t width, height, stride, format;
    void *bits;
    uint32_t reserved[6];
} ANativeWindow_Buffer;

typedef struct { int32_t left, top, right, bottom; } ARect;

int32_t ANativeWindow_setBuffersGeometry(ANativeWindow *w, int32_t width, int32_t height, int32_t format);
int32_t ANativeWindow_lock(ANativeWindow *w, ANativeWindow_Buffer *out, ARect *dirty);
int32_t ANativeWindow_unlockAndPost(ANativeWindow *w);

typedef struct ALooper ALooper;
typedef struct AInputQueue AInputQueue;
typedef struct AInputEvent AInputEvent;
typedef int (*ALooper_callbackFunc)(int fd, int events, void *data);
ALooper *ALooper_forThread(void);
int     ALooper_addFd(ALooper *l, int fd, int ident, int events,
                      ALooper_callbackFunc cb, void *data);
#define ALOOPER_EVENT_INPUT 1
void    AInputQueue_attachLooper(AInputQueue *q, ALooper *l, int ident,
                                 ALooper_callbackFunc cb, void *data);
void    AInputQueue_detachLooper(AInputQueue *q);
int32_t AInputQueue_getEvent(AInputQueue *q, AInputEvent **ev);
int32_t AInputQueue_preDispatchEvent(AInputQueue *q, AInputEvent *ev);
void    AInputQueue_finishEvent(AInputQueue *q, AInputEvent *ev, int handled);
int32_t AInputEvent_getType(const AInputEvent *ev);
int32_t AMotionEvent_getAction(const AInputEvent *ev);
float   AMotionEvent_getX(const AInputEvent *ev, unsigned long idx);
float   AMotionEvent_getY(const AInputEvent *ev, unsigned long idx);
unsigned long AMotionEvent_getHistorySize(const AInputEvent *ev);
float   AMotionEvent_getHistoricalX(const AInputEvent *ev, unsigned long idx, unsigned long h);
float   AMotionEvent_getHistoricalY(const AInputEvent *ev, unsigned long idx, unsigned long h);
#define AINPUT_EVENT_TYPE_MOTION    2
#define AMOTION_EVENT_ACTION_DOWN   0
#define AMOTION_EVENT_ACTION_UP     1
#define AMOTION_EVENT_ACTION_MOVE   2
#define AMOTION_EVENT_ACTION_CANCEL 3
#define AMOTION_EVENT_ACTION_MASK   0xff

typedef struct ANativeActivity {
    struct ANativeActivityCallbacks *callbacks;
    JavaVM *vm;
    JNIEnv *env;
    jobject clazz;   /* actually the NativeActivity instance, not its class */
    const char *internalDataPath, *externalDataPath;
    int32_t sdkVersion;
    void *instance, *assetManager;
    const char *obbPath;
} ANativeActivity;

void ANativeActivity_setWindowFlags(ANativeActivity *a, uint32_t add, uint32_t remove);
#define AWINDOW_FLAG_KEEP_SCREEN_ON 0x00000080

typedef struct ANativeActivityCallbacks {
    void  (*onStart)(ANativeActivity *);
    void  (*onResume)(ANativeActivity *);
    void *(*onSaveInstanceState)(ANativeActivity *, size_t *);
    void  (*onPause)(ANativeActivity *);
    void  (*onStop)(ANativeActivity *);
    void  (*onDestroy)(ANativeActivity *);
    void  (*onWindowFocusChanged)(ANativeActivity *, int);
    void  (*onNativeWindowCreated)(ANativeActivity *, ANativeWindow *);
    void  (*onNativeWindowResized)(ANativeActivity *, ANativeWindow *);
    void  (*onNativeWindowRedrawNeeded)(ANativeActivity *, ANativeWindow *);
    void  (*onNativeWindowDestroyed)(ANativeActivity *, ANativeWindow *);
    void  (*onInputQueueCreated)(ANativeActivity *, AInputQueue *);
    void  (*onInputQueueDestroyed)(ANativeActivity *, AInputQueue *);
    void  (*onContentRectChanged)(ANativeActivity *, const ARect *);
    void  (*onConfigurationChanged)(ANativeActivity *);
    void  (*onLowMemory)(ANativeActivity *);
} ANativeActivityCallbacks;

#define WINDOW_FORMAT_RGBA_8888 1

/* --- 5x7 font: digits, A-Z, a few punctuation marks --- */

static const uint8_t font_digit[10][7] = {
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, /* 0 */
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, /* 1 */
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, /* 2 */
    {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}, /* 3 */
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, /* 4 */
    {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, /* 5 */
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, /* 6 */
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, /* 7 */
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, /* 8 */
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, /* 9 */
};

static const uint8_t font_upper[26][7] = {
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, /* A */
    {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, /* B */
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, /* C */
    {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}, /* D */
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, /* E */
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, /* F */
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, /* G */
    {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, /* H */
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, /* I */
    {0x07,0x02,0x02,0x02,0x02,0x12,0x0C}, /* J */
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, /* K */
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}, /* L */
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, /* M */
    {0x11,0x11,0x19,0x15,0x13,0x11,0x11}, /* N */
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, /* O */
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, /* P */
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, /* Q */
    {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}, /* R */
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, /* S */
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, /* T */
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, /* U */
    {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}, /* V */
    {0x11,0x11,0x11,0x15,0x15,0x15,0x0A}, /* W */
    {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, /* X */
    {0x11,0x11,0x11,0x0A,0x04,0x04,0x04}, /* Y */
    {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}, /* Z */
};

static const uint8_t font_minus[7] = {0x00,0x00,0x00,0x0E,0x00,0x00,0x00};
static const uint8_t font_colon[7] = {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00};
static const uint8_t font_dot[7]   = {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C};
static const uint8_t font_bang[7]  = {0x04,0x04,0x04,0x04,0x04,0x00,0x04};
static const uint8_t font_slash[7] = {0x01,0x02,0x02,0x04,0x08,0x08,0x10};
static const uint8_t font_plus[7]  = {0x00,0x04,0x04,0x1F,0x04,0x04,0x00};
static const uint8_t font_lpar[7]  = {0x04,0x08,0x10,0x10,0x10,0x08,0x04};
static const uint8_t font_rpar[7]  = {0x04,0x02,0x01,0x01,0x01,0x02,0x04};
static const uint8_t font_pct[7]   = {0x19,0x1A,0x02,0x04,0x08,0x0B,0x13};

static const uint8_t *glyph_for(char c)
{
    if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
    if (c >= 'A' && c <= 'Z') return font_upper[c - 'A'];
    if (c >= '0' && c <= '9') return font_digit[c - '0'];
    if (c == '-') return font_minus;
    if (c == ':') return font_colon;
    if (c == '.') return font_dot;
    if (c == '!') return font_bang;
    if (c == '/') return font_slash;
    if (c == '+') return font_plus;
    if (c == '(') return font_lpar;
    if (c == ')') return font_rpar;
    if (c == '%') return font_pct;
    return NULL;  /* space / unknown: blank cell */
}

/* --- screen model: a handful of text lines, redrawn on change --- */

#define MAX_LINES 16
#define MAX_COLS  33
#define MAX_DEVS  12

struct dev { char name[9]; char mac[18]; int rssi;
             unsigned count; long long last_log_ms; long seen_t; };

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static ANativeActivity *g_act;
static ANativeWindow   *g_win;
static int       g_scanning;
static jclass    g_ble;                /* global ref to com.jk.stealo.Ble */
static jmethodID g_scan, g_stop;
static char      g_status[MAX_COLS + 1] = "STARTING";
static struct dev g_devs[MAX_DEVS];
static int       g_ndevs;
static unsigned  g_total;              /* all adverts heard, pipe health */
static char      g_lines[MAX_LINES][MAX_COLS + 1];
static int       g_nlines;
static char      g_glucose[MAX_COLS + 1];   /* latest reading line, if any */
static int       g_pairing_started;

/* dexble driver (dexble.c) */
void dexble_init(const char *data_dir);
int  dexble_register(JNIEnv *env, jclass ble, jobject ctx);
void dexble_pair(JNIEnv *env, const char *mac, const char *code);
void dexble_request_devinfo(void);
void dexble_alarm(int high);
void dexble_alarm_silence(void);
#define PAIR_CODE "9973"   /* Stelo applicator pairing code (rebuild to change) */

/* latest reading + short history for the big on-screen value */
#define CLOCK_REALTIME 0
static int  g_cur_glu = -1, g_cur_trend;
static long g_cur_time;                       /* epoch seconds of latest reading */
#define NHIST 2100                             /* master history (~7d at 5-min spacing) */
#define RECENT_ROWS 8                          /* rows shown in the RECENT table */
static struct { int glu, trend; long t; } g_hist[NHIST];
static int  g_nhist;
static long g_tz_off;                          /* local timezone offset, seconds */
static char g_store_path[256];                 /* persistent reading log */
static int  g_stored;                          /* total readings in the log */
static int  g_startup_bf;                      /* per run: backward 24h fill attempted */
static int  g_conn_rssi;                       /* live connection RSSI (readRemoteRssi) */
static long g_conn_rssi_t;                      /* wall-clock of that measurement */
static char g_info_path[256];                  /* cached device-info strings */
static char g_model[24], g_fw[24], g_mfr[24];  /* DIS: model / firmware / manufacturer */
static long g_devinfo_req;                      /* wall-clock of last DIS read request */

/* plot window tabs (hours) and their on-screen hit boxes, set during draw() */
#define PLOT_TABS 6
static const int PLOT_HRS[PLOT_TABS] = {3, 6, 12, 24, 72, 168};   /* 3H..24H, 3D, 7D */
static int  g_plot_hours = 3;
static int  g_tab_x[PLOT_TABS], g_tab_w[PLOT_TABS], g_tab_y, g_tab_h;
/* alarm thresholds (mg/dL, adjustable in the UI) + their button hit boxes */
static int  g_alarm_low = 110, g_alarm_high = 300;
static char g_alarm_path[256];
static int  g_alarm_state;                      /* last reading's zone: 0 ok, 1 low, 2 high */
static int  g_alarm_sounding;                   /* an audible alarm is currently active */
static int  g_al_x[4], g_al_w[4], g_al_y, g_al_h;   /* 0:LOW- 1:LOW+ 2:HIGH- 3:HIGH+ */
static int  g_al_held = -1;                     /* alarm button held for auto-repeat */
static int  g_timerfd = -1;                     /* shared repaint / repeat timer */
/* plot rectangle (set during draw) + press-and-hold scrub selection */
static int  g_plot_x, g_plot_y, g_plot_w, g_plot_h;
static int  g_scrub_idx = -1;                  /* highlighted point, -1 = none */
#define COL_HILITE 0xFFAAAAAA                   /* scrub highlight dot (gray) */

static long realtime_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec;
}
static void fmt_hms(long epoch, char *out, int n)
{
    long t = (epoch + g_tz_off) % 86400;
    if (t < 0) t += 86400;
    snprintf(out, n, "%02ld:%02ld:%02ld", t / 3600, (t % 3600) / 60, t % 60);
}
static void fmt_trend(int tr, char *out, int n)
{
    if (tr == 127) { snprintf(out, n, "--"); return; }
    int a = tr < 0 ? -tr : tr;
    snprintf(out, n, "%c%d.%d", tr < 0 ? '-' : '+', a / 10, a % 10);
}

/* big-number colour by range (RGBA_8888 framebuffer: 0xAABBGGRR) */
static uint32_t glu_color(int g)
{
    if (g < 50)  return 0xFF0000FF;   /* red    */
    if (g < 70)  return 0xFF0080FF;   /* orange */
    if (g < 180) return 0xFF33FF88;   /* green  */
    return 0xFFFFFFFF;                /* white  */
}
static uint32_t white_color(int g) { (void)g; return 0xFFFFFFFF; }   /* plot dots */

/* Insert a reading into the display history, kept newest-first and deduped
 * (readings within 150 s are the same 5-min sample). Handles out-of-order
 * inserts so backfilled (older) records land in the right slot. */
/* returns 1 if a genuinely new point was inserted, 0 if it deduped an existing
 * one (within 150 s) or was older than everything kept */
static int hist_insert(long t, int glu, int trend)
{
    for (int i = 0; i < g_nhist; i++) {
        long d = t - g_hist[i].t; if (d < 0) d = -d;
        if (d < 150) { g_hist[i].glu = glu; g_hist[i].trend = trend; return 0; }
    }
    if (g_nhist == NHIST && t <= g_hist[NHIST - 1].t) return 0;   /* older than all kept */
    if (g_nhist < NHIST) g_nhist++;
    int i = g_nhist - 1;
    while (i > 0 && g_hist[i - 1].t < t) { g_hist[i] = g_hist[i - 1]; i--; }
    g_hist[i].t = t; g_hist[i].glu = glu; g_hist[i].trend = trend;
    return 1;
}

/* ---- persistent reading log: append-only CSV "epoch,glucose,trend10" ----
 * epoch      = Unix seconds when the reading was taken
 * glucose    = mg/dL
 * trend10    = trend x10 (mg/dL/min x10; signed; 127 = unavailable) */
/* CSV row: "epoch,glucose,trend10,rssi" — rssi left empty when not measured
 * for this reading (backfilled history has none; we never invent it) */
static void store_append(long t, int glu, int trend, int rssi, int has_rssi)
{
    int fd = open(g_store_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) return;
    char line[48];
    int n = has_rssi ? snprintf(line, sizeof line, "%ld,%d,%d,%d\n", t, glu, trend, rssi)
                     : snprintf(line, sizeof line, "%ld,%d,%d,\n",   t, glu, trend);
    if (write(fd, line, n) != n) { /* ignore */ }
    close(fd);
    g_stored++;
}
/* count the rows currently in the log (one pass over the file) */
static int store_count(void)
{
    int fd = open(g_store_path, O_RDONLY, 0);
    if (fd < 0) return 0;
    char b[4096]; long n; int c = 0;
    while ((n = read(fd, b, sizeof b)) > 0)
        for (long i = 0; i < n; i++) if (b[i] == '\n') c++;
    close(fd);
    return c;
}

/* ---- rolling glucose stats via daily buckets: O(1) per reading, O(90) to
 * read, and never a full-file rescan — so it stays cheap with years of data. */
struct daybucket { long day; int count, in_range; long sum; };
static struct daybucket g_days[128];       /* ring keyed by day % 128 (>90 days) */
static long g_stat_oldest;                 /* oldest reading fed into the stats */

static void stat_add(long t, int glu)
{
    long day = t / 86400;
    int idx = (int)(day & 127);
    if (g_days[idx].day != day) {           /* recycle a slot >=128 days stale */
        g_days[idx].day = day; g_days[idx].count = 0;
        g_days[idx].in_range = 0; g_days[idx].sum = 0;
    }
    g_days[idx].count++;
    g_days[idx].sum += glu;
    if (glu >= 70 && glu <= 180) g_days[idx].in_range++;
    if (!g_stat_oldest || t < g_stat_oldest) g_stat_oldest = t;
}

/* time-in-range (%) and average over the last `days`; 0 => "--" (window not
 * yet covered by our data) */
static int stat_window(int days, int *tir, int *avg)
{
    long now = realtime_s();
    if (!g_stat_oldest || now - g_stat_oldest < (long)(days - 1) * 86400) return 0;
    long today = now / 86400, cnt = 0, inr = 0, sum = 0;
    for (int d = 0; d < days; d++) {
        long day = today - d; int idx = (int)(day & 127);
        if (g_days[idx].day == day) { cnt += g_days[idx].count; inr += g_days[idx].in_range; sum += g_days[idx].sum; }
    }
    if (cnt == 0) return 0;
    *tir = (int)(100 * inr / cnt);
    *avg = (int)(sum / cnt);
    return 1;
}

/* Seed the stats from the last ~90 days of the log — a bounded tail read, so it
 * stays O(90 days) regardless of how large the file has grown. */
static void stat_load(void)
{
    int fd = open(g_store_path, O_RDONLY, 0);
    if (fd < 0) return;
    long sz = lseek(fd, 0, SEEK_END);
    long want = 1024 * 1024;                /* ~90 days of rows */
    long off = sz > want ? sz - want : 0;
    lseek(fd, off, SEEK_SET);
    char *buf = malloc((unsigned long)want + 1);
    if (!buf) { close(fd); return; }
    long n = read(fd, buf, want);
    close(fd);
    if (n <= 0) { free(buf); return; }
    buf[n] = 0;
    char *p = buf;
    if (off > 0) { while (*p && *p != '\n') p++; if (*p == '\n') p++; }
    while (*p) {
        long t = 0; int glu = 0; char *q = p;
        while (*q >= '0' && *q <= '9') t = t * 10 + (*q++ - '0');
        if (*q == ',') q++;
        while (*q >= '0' && *q <= '9') glu = glu * 10 + (*q++ - '0');
        if (t > 0 && glu > 0) stat_add(t, glu);
        while (*p && *p != '\n') p++; if (*p == '\n') p++;
    }
    free(buf);
}
/* load: read the tail of the CSV and keep the most-recent NHIST rows (newest first) */
static void store_load(void)
{
    int fd = open(g_store_path, O_RDONLY, 0);
    if (fd < 0) return;
    long sz = lseek(fd, 0, SEEK_END);
    /* keep ~7 days on screen (2016 rows x ~25 B); read a generous 64 KB tail */
    long off = sz > 65536 ? sz - 65536 : 0;
    lseek(fd, off, SEEK_SET);
    static char buf[65537];
    long n = read(fd, buf, 65536);
    close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    char *p = buf;
    if (off > 0) { while (*p && *p != '\n') p++; if (*p == '\n') p++; }  /* skip partial line */
    while (*p) {
        char *e = p; while (*e && *e != '\n') e++;
        long t = 0, glu = 0, tr = 0; int neg = 0; char *q = p;
        while (q < e && *q >= '0' && *q <= '9') t = t * 10 + (*q++ - '0');
        if (q < e && *q == ',') q++;
        while (q < e && *q >= '0' && *q <= '9') glu = glu * 10 + (*q++ - '0');
        if (q < e && *q == ',') q++;
        if (q < e && *q == '-') { neg = 1; q++; }
        while (q < e && *q >= '0' && *q <= '9') tr = tr * 10 + (*q++ - '0');
        if (neg) tr = -tr;
        if (t > 0) hist_insert(t, (int)glu, (int)tr);
        p = (*e == '\n') ? e + 1 : e;
    }
    if (g_nhist > 0) {
        g_cur_glu = g_hist[0].glu; g_cur_trend = g_hist[0].trend; g_cur_time = g_hist[0].t;
    }
}

/* Render a string at (ox,oy) with cell scale sc, in the given ARGB colour. */
static void draw_str(uint32_t *px, const ANativeWindow_Buffer *buf,
                     int ox, int oy, int sc, const char *s, uint32_t color)
{
    for (int ci = 0; s[ci]; ci++) {
        const uint8_t *g = glyph_for(s[ci]);
        if (!g)
            continue;
        for (int row = 0; row < 7; row++)
            for (int col = 0; col < 5; col++) {
                if (!(g[row] & (0x10 >> col)))
                    continue;
                int bx = ox + (ci * 6 + col) * sc, by = oy + row * sc;
                for (int dy = 0; dy < sc; dy++)
                    for (int dx = 0; dx < sc; dx++) {
                        int x = bx + dx, y = by + dy;
                        if (x >= 0 && x < buf->width && y >= 0 && y < buf->height)
                            px[y * buf->stride + x] = color;
                    }
            }
    }
}
static int str_len(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void draw(ANativeWindow *win)
{
    ANativeWindow_Buffer buf;

    if (!win)
        return;
    ANativeWindow_setBuffersGeometry(win, 0, 0, WINDOW_FORMAT_RGBA_8888);
    if (ANativeWindow_lock(win, &buf, NULL) != 0)
        return;

    uint32_t *px = buf.bits;
    for (int32_t y = 0; y < buf.height; y++)
        for (int32_t x = 0; x < buf.width; x++)
            px[y * buf.stride + x] = 0xFF181818;

    int sc = buf.width / (MAX_COLS * 6);
    if (sc < 1) sc = 1;
    int y = buf.height / 20 + 2 * sc;    /* clear the system status bar */

    /* big current value: the 3-digit glucose at 10x, centred, then the recent
     * readings (each with its own timestamp + trend). */
    if (g_cur_glu >= 0) {
        int scrub = (g_scrub_idx >= 0 && g_scrub_idx < g_nhist);
        long agev = realtime_s() - g_cur_time;
        int stale = agev > 360;                    /* older than 6 min: not the latest */
        char big[8]; uint32_t bigcol;
        /* the big number ALWAYS shows the most recent reading; XXX when stale.
         * (status now lives in the panel's STATE row, so no banner here) */
        if (stale) { snprintf(big, sizeof big, "---");           bigcol = 0xFF888888; }
        else       { snprintf(big, sizeof big, "%d", g_cur_glu); bigcol = glu_color(g_cur_glu); }
        y += 12 * sc;                              /* padding above big number */
        int bigsc = sc * 10;
        int big_w = str_len(big) * 6 * bigsc;
        /* right column: unit / trend / age stacked beside the number */
        char tr[8], agestr[12];
        if (stale) snprintf(tr, sizeof tr, "---");   /* no fresh trend when stale */
        else       fmt_trend(g_cur_trend, tr, sizeof tr);
        long a = realtime_s() - g_cur_time; if (a < 0) a = 0;
        if (a < 600) snprintf(agestr, sizeof agestr, "%ld S", a);
        else         snprintf(agestr, sizeof agestr, "%ld M", a / 60);
        int col_w = 5 * 6 * sc;                    /* "MG/DL" is the widest line */
        int gap = 6 * sc;
        int bx = (buf.width - (big_w + gap + col_w)) / 2;
        if (bx < 2 * sc) bx = 2 * sc;
        draw_str(px, &buf, bx, y, bigsc, big, bigcol);
        int colx = bx + big_w + gap;
        int gh = 7 * sc;                           /* glyph height at scale sc */
        int bh = 8 * bigsc;                        /* big-number height */
        int mid = y + (bh - gh) / 2;               /* trend at the vertical centre */
        int spread = (bh - gh) / 4;                /* half the full-height spacing */
        draw_str(px, &buf, colx, mid - spread, sc, "MG/DL", 0xFFCCCCCC);
        draw_str(px, &buf, colx, mid,          sc, tr,      0xFFCCCCCC);
        draw_str(px, &buf, colx, mid + spread, sc, agestr,  0xFFCCCCCC);
        y += 8 * bigsc + 18 * sc;                  /* big number + padding below (doubled) */

        /* Row above the plot: normally the window tabs; while swiping the plot it
         * is replaced by the picked datapoint's numbers (2x font, centred). */
        {
            int colw = buf.width / PLOT_TABS;
            int rowh = 14 * sc;                       /* 2x the 7px glyph height */
            /* extend the tap band up through the empty gap to the big number, so
             * the whole space above each label is part of the target */
            g_tab_y = y - 18 * sc; g_tab_h = rowh + 18 * sc;
            if (scrub) {
                char ts[16], line[32];
                fmt_hms(g_hist[g_scrub_idx].t, ts, sizeof ts); ts[5] = '\0';   /* HH:MM */
                snprintf(line, sizeof line, "%d MG/DL  %s", g_hist[g_scrub_idx].glu, ts);
                int lw = str_len(line) * 6 * 2 * sc;
                int lx = (buf.width - lw) / 2; if (lx < 2 * sc) lx = 2 * sc;
                draw_str(px, &buf, lx, y, 2 * sc, line, 0xFFFFFFFF);
            } else {
                int laby = y + (rowh - 7 * sc) / 2;   /* label vertically centred */
                for (int i = 0; i < PLOT_TABS; i++) {
                    char lab[6];                       /* <=24h shown in H, larger in D */
                    if (PLOT_HRS[i] < 48) snprintf(lab, sizeof lab, "%dH", PLOT_HRS[i]);
                    else                  snprintf(lab, sizeof lab, "%dD", PLOT_HRS[i] / 24);
                    int lw = str_len(lab) * 6 * sc;
                    int cx = i * colw + (colw - lw) / 2;   /* centre label in its column */
                    draw_str(px, &buf, cx, laby, sc, lab,
                             PLOT_HRS[i] == g_plot_hours ? 0xFFFFFFFF : 0xFF888888);
                    g_tab_x[i] = i * colw; g_tab_w[i] = colw;   /* whole column is tappable */
                }
            }
            y += rowh;
        }

        /* crude 50-300 plot, 50% taller than the big number */
        {
            int ph = 12 * bigsc;
            g_plot_x = 2 * sc; g_plot_y = y; g_plot_w = buf.width - 4 * sc; g_plot_h = ph;
            static plot_pt pts[NHIST];
            for (int i = 0; i < g_nhist; i++) { pts[i].t = g_hist[i].t; pts[i].glu = g_hist[i].glu; }
            /* shrink the dots on the dense multi-day windows so they don't merge */
            int prad = 3 * sc / 2;
            if      (g_plot_hours >= 168) prad = prad / 2;       /* 7D: 50% smaller */
            else if (g_plot_hours >= 72)  prad = prad * 3 / 4;   /* 3D: 25% smaller */
            plot_render(px, buf.stride, buf.width, buf.height,
                        g_plot_x, g_plot_y, g_plot_w, g_plot_h,
                        pts, g_nhist, realtime_s(), g_plot_hours, prad, white_color,
                        scrub ? g_scrub_idx : -1, COL_HILITE);
            /* (scrub numbers are drawn in the tab row above, not near the dot) */
            y += ph + 9 * sc;                      /* padding between plot and table */
        }

        /* alarm config row: "ALARM  LOW - 110 +  HIGH - 300 +" — only the two
         * numbers are white, everything else gray; the +/- are tap targets. The
         * nine tokens are justified with equal gaps so the row spans full width
         * and each +/- has equal space around it (folded into its hit box). */
        {
            const uint32_t gy = 0xFF888888, wt = 0xFFFFFFFF;
            int cw = 6 * sc;
            char lo[8], hi[8];
            snprintf(lo, sizeof lo, "%d", g_alarm_low);
            snprintf(hi, sizeof hi, "%d", g_alarm_high);
            const char *tok[9] = { "ALARM", "LOW", "-", lo, "+", "HIGH", "-", hi, "+" };
            uint32_t    tcol[9] = { gy, gy, gy, wt, gy, gy, gy, wt, gy };
            int total = 0; for (int i = 0; i < 9; i++) total += str_len(tok[i]) * cw;
            /* 10 equal gaps: one before the first token, one after the last, and
             * eight between — so every +/- (including the final one) gets the
             * same breathing room and tap padding, edge included */
            int gap = (buf.width - total) / 10;
            if (gap < cw) gap = cw;
            int ax = gap, btn = 0;
            /* button tap band runs from just above the text all the way down to
             * the data table (the doubled blank below is part of the target) */
            g_al_y = y - 3 * sc; g_al_h = 3 * sc + 7 * sc + 18 * sc;
            for (int i = 0; i < 9; i++) {
                draw_str(px, &buf, ax, y, sc, tok[i], tcol[i]);
                int tw = str_len(tok[i]) * cw;
                if (tok[i][0] == '-' || tok[i][0] == '+') {   /* button + half-gap each side */
                    g_al_x[btn] = ax - gap / 2; g_al_w[btn] = tw + gap; btn++;
                }
                ax += tw + gap;
            }
            y += 7 * sc + 18 * sc;                            /* text + doubled blank line */
        }

        /* sensor + session info panel (replaces the old recent-readings table) */
        {
            dex_session s; driver_get_session(&s);
            int rssi = 0, have_rssi = 0; long seen = 0;
            for (int i = 0; i < g_ndevs; i++)
                if (strcmp(g_devs[i].mac, s.mac) == 0) {
                    rssi = g_devs[i].rssi; seen = g_devs[i].seen_t; have_rssi = 1; break;
                }
            char row[64];
            const uint32_t col = 0xFFCCCCCC;

            /* when not bonded we're scanning/pairing — show the running advert
             * count so a dead/stuck scan is obvious (frozen count = scan wedged) */
            if (s.bonded)
                snprintf(row, sizeof row, "STATE   %s (BONDED)", g_status);
            else
                snprintf(row, sizeof row, "STATE   %s  %u ADV", g_status, g_total);
            draw_str(px, &buf, 2 * sc, y, sc, row, col); y += 9 * sc;

            snprintf(row, sizeof row, "STORED  %d readings", g_stored);
            draw_str(px, &buf, 2 * sc, y, sc, row, col); y += 9 * sc;

            snprintf(row, sizeof row, "STELO   %s %s", PAIR_CODE, s.mac[0] ? s.mac : "--");
            draw_str(px, &buf, 2 * sc, y, sc, row, col); y += 9 * sc;

            if (g_model[0]) { snprintf(row, sizeof row, "SW      %s", g_model);
                draw_str(px, &buf, 2 * sc, y, sc, row, col); y += 9 * sc; }
            if (g_fw[0]) { snprintf(row, sizeof row, "FW      %s", g_fw);
                draw_str(px, &buf, 2 * sc, y, sc, row, col); y += 9 * sc; }
            /* manufacturer is always Dexcom — read (for the DIS retry gate) but not shown */

            if (s.have_reading) {
                long ss = (long)s.session_seconds, left = 15L * 86400 - ss; if (left < 0) left = 0;
                snprintf(row, sizeof row, "SESSION %ldD %ldH   LEFT %ldD %ldH",
                         ss / 86400, (ss % 86400) / 3600, left / 86400, (left % 86400) / 3600);
            } else snprintf(row, sizeof row, "SESSION --");
            draw_str(px, &buf, 2 * sc, y, sc, row, col); y += 9 * sc;

            if (g_conn_rssi_t) {          /* live connection RSSI, with its time */
                char st[16]; fmt_hms(g_conn_rssi_t, st, sizeof st); st[5] = '\0';
                snprintf(row, sizeof row, "SIGNAL  %d DBM  @ %s", g_conn_rssi, st);
            } else if (have_rssi) {       /* fall back to the last advert RSSI */
                char st[16]; fmt_hms(seen, st, sizeof st); st[5] = '\0';
                snprintf(row, sizeof row, "SIGNAL  %d DBM  @ %s", rssi, st);
            } else snprintf(row, sizeof row, "SIGNAL  --");
            draw_str(px, &buf, 2 * sc, y, sc, row, col); y += 9 * sc;

            snprintf(row, sizeof row, "PRED    %d MG/DL   SEQ %d", s.predicted, s.sequence);
            draw_str(px, &buf, 2 * sc, y, sc, row, col); y += 9 * sc;
        }

        /* blank line, then TIR / AVG over 3/7/30/90 days (O(90) daily buckets) */
        {
            static const int WIN[5] = {1, 3, 7, 30, 90};
            char tc[5][8], ac[5][8], hc[5][8];
            for (int i = 0; i < 5; i++) {
                int tir, avg;
                if (stat_window(WIN[i], &tir, &avg)) {
                    snprintf(tc[i], sizeof tc[i], "%d", tir);   /* unit shown in its own column */
                    snprintf(ac[i], sizeof ac[i], "%d", avg);
                    /* GMI (glucose management indicator) = 3.31 + 0.02392*avg */
                    int te = (331 + (2392 * avg) / 1000 + 5) / 10;   /* A1C in tenths % */
                    snprintf(hc[i], sizeof hc[i], "%d.%d", te / 10, te % 10);
                } else {
                    snprintf(tc[i], sizeof tc[i], "--");
                    snprintf(ac[i], sizeof ac[i], "--");
                    snprintf(hc[i], sizeof hc[i], "--");
                }
            }
            char row[72];
            y += 7 * sc;                          /* one blank row (matches the 9sc gap) */
            snprintf(row, sizeof row, "%-4s%-6s%-6s%-6s%-6s%-6s%s", "", "1D", "3D", "7D", "30D", "90D", "");
            draw_str(px, &buf, 2 * sc, y, sc, row, 0xFF888888); y += 9 * sc;
            snprintf(row, sizeof row, "%-4s%-6s%-6s%-6s%-6s%-6s%s", "TIR", tc[0], tc[1], tc[2], tc[3], tc[4], "%");
            draw_str(px, &buf, 2 * sc, y, sc, row, 0xFFCCCCCC); y += 9 * sc;
            snprintf(row, sizeof row, "%-4s%-6s%-6s%-6s%-6s%-6s%s", "AVG", ac[0], ac[1], ac[2], ac[3], ac[4], "MG/DL");
            draw_str(px, &buf, 2 * sc, y, sc, row, 0xFFCCCCCC); y += 9 * sc;
            snprintf(row, sizeof row, "%-4s%-6s%-6s%-6s%-6s%-6s%s", "A1C", hc[0], hc[1], hc[2], hc[3], hc[4], "%");
            draw_str(px, &buf, 2 * sc, y, sc, row, 0xFFCCCCCC); y += 9 * sc;
        }

        /* alarm trigger: when the current (fresh) reading is out of the set
         * range, announce it in large centred letters below the stats table */
        if (realtime_s() - g_cur_time <= 360) {
            const char *msg = 0; uint32_t c = 0;
            if      (g_cur_glu < g_alarm_low)  { msg = "LOW";  c = 0xFF0000FF; }
            else if (g_cur_glu > g_alarm_high) { msg = "HIGH"; c = 0xFF0080FF; }
            if (msg) {
                int msc = 5 * sc;                          /* large letters */
                int w = str_len(msg) * 6 * msc;
                int mx = (buf.width - w) / 2; if (mx < 2 * sc) mx = 2 * sc;
                y += 7 * sc + 9 * sc;                      /* doubled blank above the word */
                draw_str(px, &buf, mx, y, msc, msg, c);
                y += 8 * msc;
            }
        }
    }

    if (g_cur_glu < 0) {
        /* no readings yet: show scan status, advert counter, sensor list */
        for (int li = 0; li < g_nlines; li++) {
            draw_str(px, &buf, 2 * sc, y, sc, g_lines[li], 0xFFFFFFFF);
            y += 9 * sc;
        }
        if (g_ndevs > 0) {
            y += 3 * sc;
            draw_str(px, &buf, 2 * sc, y, sc, "SENSORS", 0xFF888888);
            y += 10 * sc;
            for (int i = 0; i < g_ndevs; i++) {
                char dl[48];
                snprintf(dl, sizeof dl, "%-8s %4d %s",
                         g_devs[i].name, g_devs[i].rssi, g_devs[i].mac);
                draw_str(px, &buf, 2 * sc, y, sc, dl, 0xFFCCCCCC);
                y += 9 * sc;
            }
        }
    }

    ANativeWindow_unlockAndPost(win);
}

/* Rebuild the text lines; redraw only if something visible changed, and at
 * most ~5 times/second so radio chatter can't saturate the main thread. */
static void update_screen(void)
{
    char next[MAX_LINES][MAX_COLS + 1];
    int n = 0;

    snprintf(next[n++], sizeof next[0], "STEALO  %s", g_status);
    /* total rounded to 10s so ambient chatter doesn't redraw every advert */
    snprintf(next[n++], sizeof next[0], "ADV %u  DX %d", g_total / 10 * 10, g_ndevs);

    int changed = (n != g_nlines);
    for (int i = 0; !changed && i < n; i++)
        changed = strcmp(next[i], g_lines[i]) != 0;
    if (!changed)
        return;

    g_nlines = n;
    for (int i = 0; i < n; i++)
        snprintf(g_lines[i], sizeof g_lines[0], "%s", next[i]);

    static long long last_draw_ms;
    long long now = now_ms();
    if (now - last_draw_ms < 200)
        return;                     /* next change will repaint */
    last_draw_ms = now;
    draw(g_win);
}

/* --- Java -> C: one advertisement heard (main thread) --- */

static void jni_on_advert(JNIEnv *env, jclass cls, jstring jname, jstring jmac, jint rssi)
{
    (void)cls;
    const char *name = (*env)->GetStringUTFChars(env, jname, NULL);
    const char *mac  = (*env)->GetStringUTFChars(env, jmac, NULL);

    g_total++;
    /* Stelo advertises as DX01*; on first sighting, stop scanning and pair. */
    if (!g_pairing_started && strncmp(name, "DX01", 4) == 0) {
        g_pairing_started = 1;
        LOGI("Stelo %s %s -> pairing", name, mac);
        (*env)->CallStaticVoidMethod(env, g_ble, g_stop);
        g_scanning = 0;
        dexble_pair(env, mac, PAIR_CODE);
    }
    if (strncmp(name, "DX", 2) == 0) {
        int i;
        for (i = 0; i < g_ndevs; i++)
            if (strcmp(g_devs[i].mac, mac) == 0)
                break;
        if (i == g_ndevs && g_ndevs < MAX_DEVS) {
            g_ndevs++;
            LOGI("new DXCM device: %s %s %d", name, mac, rssi);
        }
        if (i < g_ndevs) {
            snprintf(g_devs[i].name, sizeof g_devs[i].name, "%s", name);
            snprintf(g_devs[i].mac,  sizeof g_devs[i].mac,  "%s", mac);
            g_devs[i].rssi = rssi;
            g_devs[i].seen_t = realtime_s();
            g_devs[i].count++;
            /* one cadence line per device per 30 s, to time advert bursts */
            long long now = now_ms();
            if (now - g_devs[i].last_log_ms > 30000) {
                g_devs[i].last_log_ms = now;
                LOGI("DXCM adv: %s %s rssi %d count %u", name, mac, rssi, g_devs[i].count);
            }
        }
    }

    (*env)->ReleaseStringUTFChars(env, jname, name);
    (*env)->ReleaseStringUTFChars(env, jmac, mac);
    update_screen();
}

/* --- permissions --- */

static const char *perm_modern[] = { "android.permission.BLUETOOTH_SCAN",
                                     "android.permission.BLUETOOTH_CONNECT" };
static const char *perm_legacy[] = { "android.permission.ACCESS_FINE_LOCATION" };

static int has_ble_permissions(ANativeActivity *a)
{
    JNIEnv *env = a->env;
    const char **want = a->sdkVersion >= 31 ? perm_modern : perm_legacy;
    jsize n = a->sdkVersion >= 31 ? 2 : 1;

    jclass act = (*env)->GetObjectClass(env, a->clazz);
    jmethodID check = (*env)->GetMethodID(env, act, "checkSelfPermission",
                                          "(Ljava/lang/String;)I");
    int ok = 1;
    for (jsize i = 0; i < n; i++) {
        jstring s = (*env)->NewStringUTF(env, want[i]);
        if ((*env)->CallIntMethod(env, a->clazz, check, s) != 0)
            ok = 0;
        (*env)->DeleteLocalRef(env, s);
    }
    return ok;
}

/* Ask for the BLE permissions this Android version needs. The result
 * callback never reaches native code; grant state is re-checked on resume. */
static void request_ble_permissions(ANativeActivity *a)
{
    JNIEnv *env = a->env;
    const char **want = a->sdkVersion >= 31 ? perm_modern : perm_legacy;
    jsize n = a->sdkVersion >= 31 ? 2 : 1;

    jclass act = (*env)->GetObjectClass(env, a->clazz);
    jmethodID req = (*env)->GetMethodID(env, act, "requestPermissions",
                                        "([Ljava/lang/String;I)V");
    jobjectArray arr = (*env)->NewObjectArray(env, n,
                           (*env)->FindClass(env, "java/lang/String"), NULL);
    for (jsize i = 0; i < n; i++) {
        jstring s = (*env)->NewStringUTF(env, want[i]);
        (*env)->SetObjectArrayElement(env, arr, i, s);
        (*env)->DeleteLocalRef(env, s);
    }
    (*env)->CallVoidMethod(env, a->clazz, req, arr, (jint)1);
}

/* FindClass inside ANativeActivity callbacks resolves via the framework's
 * class loader, which can't see app classes; go through the activity's own
 * loader instead. Takes a dotted name ("com.jk.stealo.Ble"). */
static jclass find_app_class(ANativeActivity *a, const char *name)
{
    JNIEnv *env = a->env;
    jclass actCls = (*env)->GetObjectClass(env, a->clazz);
    jmethodID getCl = (*env)->GetMethodID(env, actCls, "getClassLoader",
                                          "()Ljava/lang/ClassLoader;");
    jobject loader = (*env)->CallObjectMethod(env, a->clazz, getCl);
    jclass loaderCls = (*env)->GetObjectClass(env, loader);
    jmethodID load = (*env)->GetMethodID(env, loaderCls, "loadClass",
                                 "(Ljava/lang/String;)Ljava/lang/Class;");
    jstring jname = (*env)->NewStringUTF(env, name);
    jclass cls = (*env)->CallObjectMethod(env, loader, load, jname);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        return NULL;
    }
    return cls;
}

/* --- scan lifecycle (all on main thread) --- */

static void set_status(const char *s)
{
    snprintf(g_status, sizeof g_status, "%s", s);
    update_screen();
}

/* --- hooks called by the BLE driver (dexble.c) --- */
void stealo_status(const char *s) { set_status(s); }
/* current reading from the 4e stream */
void stealo_glucose(int mg_dl, int trend, int age_s)
{
    long t = realtime_s() - age_s;
    long prev = g_nhist ? g_hist[0].t : 0;
    int isnew = hist_insert(t, mg_dl, trend);
    if (isnew) {
        int has = (g_conn_rssi_t && realtime_s() - g_conn_rssi_t < 120);  /* this connection */
        store_append(t, mg_dl, trend, g_conn_rssi, has);
        stat_add(t, mg_dl);
    }
    g_cur_glu = g_hist[0].glu; g_cur_trend = g_hist[0].trend; g_cur_time = g_hist[0].t;
    snprintf(g_glucose, sizeof g_glucose, "GLUCOSE %d  TR %d", mg_dl, trend);
    LOGI("glucose %d mg/dL trend %d age %d", mg_dl, trend, age_s);

    /* Edge-triggered audible/vibrating alarm: sound only on the transition INTO
     * an out-of-range zone (not on every out-of-range reading), and stop when a
     * reading returns to range. A press anywhere also silences (see on_input). */
    if (isnew) {
        int st = (g_cur_glu < g_alarm_low) ? 1 : (g_cur_glu > g_alarm_high) ? 2 : 0;
        if (st != g_alarm_state) {
            g_alarm_state = st;
            if (st) { g_alarm_sounding = 1; dexble_alarm(st == 2); }
            else if (g_alarm_sounding) { g_alarm_sounding = 0; dexble_alarm_silence(); }
        }
    }
    update_screen();
    draw(g_win);                 /* force a repaint even if the text line is unchanged */

    /* Read the sensor's serial / firmware / software strings. Deferred to here
     * (after the first reading) so it runs post-auth, when the reads succeed.
     * The sensor closes the cycle within a few seconds, often before all three
     * reads land, so we retry each reconnect until we have them all — throttled
     * to at most once a minute, and stopping entirely once complete. */
    if ((!g_model[0] || !g_fw[0] || !g_mfr[0]) && realtime_s() - g_devinfo_req > 60) {
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
        dex_session s; driver_get_session(&s);
        long target = 24L * 3600;
        if (s.have_reading && (long)s.session_seconds < target) target = (long)s.session_seconds;
        long oldest = g_nhist ? g_hist[g_nhist - 1].t : t;
        if (target > 600 && realtime_s() - oldest < target - 300) {
            LOGI("backward fill: %ld s backfill (have %ld s of %ld s window)",
                 target, realtime_s() - oldest, target);
            driver_request_backfill(target);
            did_bf = 1;
        }
    }
    /* Ongoing: recover a genuine gap since the last reading (one request/gap). */
    if (!did_bf && isnew && prev) {
        long span = t - prev;
        if (span > 450) {
            LOGI("gap %ld s since last reading -> request backfill", span);
            driver_request_backfill(span);
        }
    }
}

/* live connection signal strength from readRemoteRssi (no sensor-battery cost) */
void stealo_rssi(int rssi)
{
    g_conn_rssi = rssi;
    g_conn_rssi_t = realtime_s();
    LOGI("rssi %d dbm", rssi);
    draw(g_win);
}

/* persist the three device-info strings so they survive across launches and
 * show even before the next connect (they change at most on a firmware update) */
static void info_save(void)
{
    int fd = open(g_info_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return;
    char b[96]; int n = snprintf(b, sizeof b, "%s\n%s\n%s\n", g_model, g_fw, g_mfr);
    if (write(fd, b, n) != n) {}
    close(fd);
}
static void info_load(void)
{
    int fd = open(g_info_path, O_RDONLY, 0);
    if (fd < 0) return;
    char b[96]; int n = read(fd, b, sizeof b - 1); close(fd);
    if (n <= 0) return; b[n] = 0;
    char *p = b, *dst[3] = { g_model, g_fw, g_mfr };
    for (int i = 0; i < 3 && p; i++) {
        char *nl = p; while (*nl && *nl != '\n') nl++;
        int len = nl - p; if (len > 22) len = 22;
        for (int j = 0; j < len; j++) dst[i][j] = p[j]; dst[i][len] = 0;
        p = *nl ? nl + 1 : 0;
    }
}

/* device-info string (serial / firmware / software) read from DIS 0x180A */
void stealo_devinfo(const char *uuid, const char *val)
{
    if (!val || !val[0]) return;
    /* uuid is the full 128-bit form "0000XXXX-0000-1000-8000-00805f9b34fb";
     * the 16-bit assigned number sits at offset 4 */
    char *dst = 0;
    if      (strncmp(uuid + 4, "2a24", 4) == 0) dst = g_model;   /* model number      */
    else if (strncmp(uuid + 4, "2a26", 4) == 0) dst = g_fw;      /* firmware revision */
    else if (strncmp(uuid + 4, "2a29", 4) == 0) dst = g_mfr;     /* manufacturer name */
    if (!dst) return;
    int i = 0; for (; val[i] && i < 22; i++) dst[i] = val[i]; dst[i] = 0;
    LOGI("devinfo %s = %s", uuid, dst);
    info_save();
    draw(g_win);
}

/* alarm thresholds persist across launches in a tiny "low high\n" file */
static void alarm_save(void)
{
    int fd = open(g_alarm_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return;
    char b[32]; int n = snprintf(b, sizeof b, "%d %d\n", g_alarm_low, g_alarm_high);
    if (write(fd, b, n) != n) {}
    close(fd);
}
static void alarm_load(void)
{
    int fd = open(g_alarm_path, O_RDONLY, 0);
    if (fd < 0) return;
    char b[32]; int n = read(fd, b, sizeof b - 1); close(fd);
    if (n <= 0) return; b[n] = 0;
    int lo = 0, hi = 0; char *q = b;
    while (*q >= '0' && *q <= '9') lo = lo * 10 + (*q++ - '0');
    while (*q == ' ') q++;
    while (*q >= '0' && *q <= '9') hi = hi * 10 + (*q++ - '0');
    if (lo > 0) g_alarm_low = lo;
    if (hi > 0) g_alarm_high = hi;
}
/* which alarm +/- button (0:LOW- 1:LOW+ 2:HIGH- 3:HIGH+) is under (tx,ty), or -1 */
static int alarm_button_at(int tx, int ty)
{
    if (ty < g_al_y || ty >= g_al_y + g_al_h) return -1;
    for (int i = 0; i < 4; i++)
        if (tx >= g_al_x[i] && tx < g_al_x[i] + g_al_w[i]) return i;
    return -1;
}
/* adjust the threshold for button i by +/-5, clamped and kept low<=high; saves */
static void alarm_adjust(int i)
{
    int *v = (i < 2) ? &g_alarm_low : &g_alarm_high;
    *v += (i & 1) ? 5 : -5;                          /* even=minus, odd=plus */
    if (*v < 40)  *v = 40;
    if (*v > 400) *v = 400;
    if (g_alarm_low > g_alarm_high) {                /* keep low <= high */
        if (i < 2) g_alarm_low = g_alarm_high; else g_alarm_high = g_alarm_low;
    }
    alarm_save();
}

/* an older reading recovered via backfill: store it, place it in history, but
 * don't disturb the current value unless it turns out to be the newest */
void stealo_backfill(int mg_dl, int trend, int age_s)
{
    long t = realtime_s() - age_s;
    if (hist_insert(t, mg_dl, trend)) {              /* store only new */
        store_append(t, mg_dl, trend, 0, 0);         /* no RSSI for backfilled points */
        stat_add(t, mg_dl);
    }
    g_cur_glu = g_hist[0].glu; g_cur_trend = g_hist[0].trend; g_cur_time = g_hist[0].t;
    LOGI("backfill reading %d mg/dL age %d -> t=%ld", mg_dl, age_s, t);
    update_screen();
    draw(g_win);
}

static void start_scan(ANativeActivity *a)
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
    set_status("SCANNING");
}

static void stop_scan(ANativeActivity *a)
{
    JNIEnv *env = a->env;

    if (!g_scanning)
        return;
    (*env)->CallStaticVoidMethod(env, g_ble, g_stop);
    if ((*env)->ExceptionCheck(env))
        (*env)->ExceptionClear(env);
    g_scanning = 0;
    set_status("PAUSED");
}

/* --- input: drain the queue so the ANR watchdog stays fed --- */

/* reprogram the shared timer: first tick after `first_ms`, then every
 * `repeat_ms`. Used to switch between the 1 Hz repaint cadence and the hold-to-
 * repeat cadence — which waits before repeating so a quick tap doesn't repeat. */
static void timer_set(long first_ms, long repeat_ms)
{
    if (g_timerfd < 0) return;
    struct itimerspec its;
    its.it_value.tv_sec  = first_ms / 1000;  its.it_value.tv_nsec  = (first_ms % 1000) * 1000000L;
    its.it_interval.tv_sec = repeat_ms / 1000; its.it_interval.tv_nsec = (repeat_ms % 1000) * 1000000L;
    timerfd_settime(g_timerfd, 0, &its, 0);
}

/* timer tick: repaint so AGE / stale state stay live; while a +/- button is
 * held (fast cadence) also step the threshold, for hold-to-repeat */
static int on_timer(int fd, int events, void *data)
{
    (void)events; (void)data;
    uint64_t ticks;
    read(fd, &ticks, sizeof ticks);   /* single read clears the expiration count */
    if (g_al_held >= 0) alarm_adjust(g_al_held);
    if (g_win) draw(g_win);
    return 1;
}

static int on_input(int fd, int events, void *data)
{
    (void)fd; (void)events;
    AInputQueue *q = data;
    AInputEvent *ev;
    while (AInputQueue_getEvent(q, &ev) >= 0) {
        if (AInputQueue_preDispatchEvent(q, ev))
            continue;               /* IME took it; finished elsewhere */
        int handled = 0;
        if (AInputEvent_getType(ev) == AINPUT_EVENT_TYPE_MOTION) {
            int action = AMotionEvent_getAction(ev) & AMOTION_EVENT_ACTION_MASK;
            int tx = (int)AMotionEvent_getX(ev, 0);
            int ty = (int)AMotionEvent_getY(ev, 0);
            /* any press anywhere silences a sounding alarm first, and that press
             * does nothing else (so you can't accidentally change a tab/threshold
             * while silencing) */
            if (action == AMOTION_EVENT_ACTION_DOWN && g_alarm_sounding) {
                g_alarm_sounding = 0; dexble_alarm_silence();
                AInputQueue_finishEvent(q, ev, 1);
                continue;
            }
            int in_plot = tx >= g_plot_x && tx < g_plot_x + g_plot_w &&
                          ty >= g_plot_y && ty < g_plot_y + g_plot_h;
            if ((action == AMOTION_EVENT_ACTION_DOWN || action == AMOTION_EVENT_ACTION_MOVE) && in_plot) {
                /* Press-and-hold: highlight the datapoint nearest the finger.
                 * Average the current sample with the batched historical ones so
                 * the pick tracks the centre of the contact, not a jittery edge. */
                unsigned long hs = AMotionEvent_getHistorySize(ev);
                long ax = tx, ay = ty, n = 1;
                for (unsigned long h = 0; h < hs; h++) {
                    ax += (long)AMotionEvent_getHistoricalX(ev, 0, h);
                    ay += (long)AMotionEvent_getHistoricalY(ev, 0, h);
                    n++;
                }
                int fx = (int)(ax / n), fy = (int)(ay / n);
                static plot_pt pts[NHIST];
                for (int i = 0; i < g_nhist; i++) { pts[i].t = g_hist[i].t; pts[i].glu = g_hist[i].glu; }
                int idx = plot_hit(g_plot_x, g_plot_y, g_plot_w, g_plot_h,
                                   pts, g_nhist, realtime_s(), g_plot_hours, fx, fy);
                if (idx != g_scrub_idx) { g_scrub_idx = idx; draw(g_win); }
                handled = 1;
            } else if (action == AMOTION_EVENT_ACTION_DOWN) {
                int btn = alarm_button_at(tx, ty);   /* press a +/- : step once now,
                                                      * then auto-repeat on a fast tick */
                if (btn >= 0) {                  /* step once now; repeat only after
                                                  * a 400 ms hold, then every 120 ms */
                    g_al_held = btn; alarm_adjust(btn); timer_set(400, 120); draw(g_win);
                    handled = 1;
                }
            } else if (action == AMOTION_EVENT_ACTION_UP || action == AMOTION_EVENT_ACTION_CANCEL) {
                if (g_al_held >= 0) {               /* release stops the auto-repeat */
                    g_al_held = -1; timer_set(1000, 1000); handled = 1;
                } else if (g_scrub_idx >= 0) {      /* release clears the highlight */
                    g_scrub_idx = -1; draw(g_win); handled = 1;
                } else {                            /* otherwise it may be a tab tap */
                    for (int i = 0; i < PLOT_TABS; i++)
                        if (tx >= g_tab_x[i] && tx < g_tab_x[i] + g_tab_w[i] &&
                            ty >= g_tab_y  && ty < g_tab_y  + g_tab_h) {
                            g_plot_hours = PLOT_HRS[i];
                            draw(g_win);
                            handled = 1;
                            break;
                        }
                }
            }
        }
        AInputQueue_finishEvent(q, ev, handled);
    }
    return 1;                       /* keep the callback registered */
}

/* --- activity callbacks --- */

static void on_queue_created(ANativeActivity *a, AInputQueue *q)
{
    (void)a;
    AInputQueue_attachLooper(q, ALooper_forThread(), 1, on_input, q);
}

static void on_queue_destroyed(ANativeActivity *a, AInputQueue *q)
{
    (void)a;
    AInputQueue_detachLooper(q);
}

static void on_resume(ANativeActivity *a)                                   { start_scan(a); }
static void on_pause(ANativeActivity *a)                                    { stop_scan(a); }
static void on_window_created(ANativeActivity *a, ANativeWindow *win)       { (void)a; g_win = win; g_nlines = 0; update_screen(); }
static void on_window_destroyed(ANativeActivity *a, ANativeWindow *win)     { (void)a; (void)win; g_win = NULL; }
static void on_redraw_needed(ANativeActivity *a, ANativeWindow *win)        { (void)a; draw(win); }
static void on_window_resized(ANativeActivity *a, ANativeWindow *win)       { (void)a; draw(win); }

__attribute__((visibility("default")))
void ANativeActivity_onCreate(ANativeActivity *activity, void *saved, size_t saved_size)
{
    (void)saved; (void)saved_size;
    JNIEnv *env = activity->env;

    g_act = activity;

    /* local timezone offset (seconds), for on-screen timestamps */
    {
        jclass tzc  = (*env)->FindClass(env, "java/util/TimeZone");
        jclass sysc = (*env)->FindClass(env, "java/lang/System");
        if (tzc && sysc) {
            jmethodID getDef = (*env)->GetStaticMethodID(env, tzc, "getDefault", "()Ljava/util/TimeZone;");
            jmethodID ctm    = (*env)->GetStaticMethodID(env, sysc, "currentTimeMillis", "()J");
            jmethodID getOff = (*env)->GetMethodID(env, tzc, "getOffset", "(J)I");
            jobject tz  = (*env)->CallStaticObjectMethod(env, tzc, getDef);
            jlong   now = (*env)->CallStaticLongMethod(env, sysc, ctm);
            g_tz_off = (*env)->CallIntMethod(env, tz, getOff, now) / 1000;
        }
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    }

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

    jclass ble = find_app_class(activity, "com.jk.stealo.Ble");
    if (!ble) {
        LOGI("Ble class NOT found");
        set_status("NO BLE CLASS!");
        return;
    }
    g_ble = (*env)->NewGlobalRef(env, ble);

    static const JNINativeMethod methods[] = {
        { "onAdvert", "(Ljava/lang/String;Ljava/lang/String;I)V", (void *)jni_on_advert },
    };
    if ((*env)->RegisterNatives(env, g_ble, methods, 1) != 0) {
        LOGI("RegisterNatives failed");
        set_status("JNI REG FAILED!");
        return;
    }
    g_scan = (*env)->GetStaticMethodID(env, g_ble, "scan",
                                       "(Landroid/content/Context;)Ljava/lang/String;");
    g_stop = (*env)->GetStaticMethodID(env, g_ble, "stop", "()V");

    /* wire up the BLE protocol driver (registers its own Ble callbacks) */
    dexble_init(activity->internalDataPath ? activity->internalDataPath : "/data/local/tmp");
    if (!dexble_register(env, g_ble, activity->clazz))
        LOGI("dexble_register failed");

    /* persistent reading log: remember our own datapoints across restarts.
     * Internal storage; the app is debuggable, so retrieve with
     *   adb shell run-as com.jk.stealo cat files/readings.csv > readings.csv */
    {
        const char *dir = activity->internalDataPath ? activity->internalDataPath
                        : "/data/local/tmp";
        int i = 0; for (; dir[i] && i < 230; i++) g_store_path[i] = dir[i];
        const char *f = "/readings.csv"; for (int j = 0; f[j]; j++) g_store_path[i++] = f[j];
        g_store_path[i] = 0;
        int k = 0; for (; dir[k] && k < 230; k++) g_info_path[k] = dir[k];
        const char *g = "/stelo.info"; for (int j = 0; g[j]; j++) g_info_path[k++] = g[j];
        g_info_path[k] = 0;
        int m = 0; for (; dir[m] && m < 230; m++) g_alarm_path[m] = dir[m];
        const char *h = "/alarm.cfg"; for (int j = 0; h[j]; j++) g_alarm_path[m++] = h[j];
        g_alarm_path[m] = 0;
        store_load();
        stat_load();
        info_load();
        alarm_load();
        g_stored = store_count();
        LOGI("reading log: %s (%d in memory, %d stored)", g_store_path, g_nhist, g_stored);
    }

    /* 1 Hz repaint timer so AGE / stale state stay current without a touch */
    {
        int tfd = timerfd_create(CLOCK_MONOTONIC, 04000 /* TFD_NONBLOCK */);
        if (tfd >= 0) {
            g_timerfd = tfd;
            struct itimerspec its;
            its.it_value.tv_sec = 1;    its.it_value.tv_nsec = 0;
            its.it_interval.tv_sec = 1; its.it_interval.tv_nsec = 0;
            timerfd_settime(tfd, 0, &its, 0);
            ALooper_addFd(ALooper_forThread(), tfd, 3, ALOOPER_EVENT_INPUT, on_timer, 0);
        }
    }

    if (!has_ble_permissions(activity))
        request_ble_permissions(activity);
}
