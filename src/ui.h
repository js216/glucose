// SPDX-License-Identifier: GPL-3.0
// ui.h --- On-screen rendering + touch input (the interactive UI layer)
// Copyright 2026 Jakob Kastelic

#ifndef STEALO_UI_H
#define STEALO_UI_H

#include "ndk.h"
#include "sensors.h" /* sensor types/kinds the model and renderer share */
#include <stdint.h>

/* ---- pixel/text primitives (font-based, no shared state) ----
 * These render directly into a locked framebuffer, so the offline harness can
 * drive them with a plain malloc'd buffer (see test/uitest.c). */

/* Draw string s at (ox,oy), cell scale sc, in ARGB color. Clipped to buf. */
void draw_str(uint32_t *px, const struct ANativeWindow_Buffer *buf, int ox,
              int oy, int sc, const char *s, uint32_t color);

/* Draw a 1px rectangle outline (x,y,w,h) in ARGB color; no-op if off-buffer. */
void draw_frame(uint32_t *px, const struct ANativeWindow_Buffer *buf, int x,
                int y, int w, int h, uint32_t c);

/* Format glucose for display; units: 0 = mg/dL, 1 = mmol/L. Pure. */
void fmt_glu(int mgdl, int units, char *out, int n);

/* Format a trend (tenths of mg/dL per minute; 127 = unknown -> "--"). Pure. */
void fmt_trend(int tr, char *out, int n);

/* Format epoch seconds as HH:MM:SS local time (tz = offset seconds). Pure. */
void fmt_hms(long epoch, long tz, char *out, int n);

/* Format epoch seconds as "MM-DD HH:MM" local time. Pure. */
void fmt_date(long epoch, long tz, char *out, int n);

/* Format how long ago `then` was, coarsely ("234 S" / "2 H" / "6 D"), or
 * "NEVER" when then <= 0. Pure. */
void fmt_ago(long now, long then, char *out, int n);

/* Format a duration as "3 D 4 H" / "5 H 20 M" / "45 M". Pure. */
void fmt_dur(long seconds, char *out, int n);

/* ---- sensor presentation (shared by the list, the detail screen, the plot) */
#define UI_MAX_SLOTS MAX_SLOTS
/* Refuse to render a sensor list shorter than this rather than silently
 * truncating it, so a cramped screen is a visible error, not a quiet lie. */
#define UI_MIN_SLOTS 3

/* Rows the settings screen consumes ABOVE the sensor entries: title (2),
 * DISPLAY (6), ALARM (5), PERMISSIONS (8), the SENSORS header (1) and the
 * trailing ADD row (1). Keep in step with render_settings.
 *
 * Exported deliberately. It used to be private to ui.c while test/uitest.c
 * carried its own literal 23 for the same quantity, so the two could drift
 * apart silently -- and adding rows to render_settings without bumping this is
 * exactly the mistake that leaves sensor rows and their tap targets below the
 * bottom of the screen, permanently unreachable because there is no
 * scrolling. One definition, both users. */
#define UI_SET_ABOVE 23

/* How many sensor rows fit in the settings screen at this geometry, given
 * everything above the SENSORS section. The whole UI never scrolls, so this is
 * what bounds the list. Pure, and exposed so the shell can log a warning. */
/* Layout scale for the settings screen: bounded by BOTH width and height, so
 * the sensor list and ADD row stay on screen on 16:9 phones too. */
int ui_settings_scale(int w, int h);
/* Largest scale at which `rows` rows fit in height h (also bounded by width).
 * Every full-screen menu must size itself through this. */
int ui_fit_scale(int w, int h, int rows);

/* Glyph-cell clipping counter, for the offline harness. Clipping is otherwise
 * invisible: content laid out past an edge is silently dropped. Reset before a
 * render, read after; a non-zero count means text did not fit. */
void ui_clip_reset(void);
long ui_clipped(void);
int ui_sensor_capacity(int w, int h);

/* The characters the rename keypad offers, in grid order. Exposed so the shell
 * can map an MA_CHAR code back to the character that was tapped. */
extern const char ui_label_chars[];
int ui_label_nchars(void);

const char *ui_marker_name(int marker);
const char *ui_color_name(int color);
uint32_t ui_sensor_color(int color);

/* ================= functional-core UI =================================
 * The UI is a pure function of an immutable snapshot: the shell (main.c) fills
 * a `struct screen` each frame, ui_render() draws it and records the touch
 * targets into `struct hits`, and ui_hit() maps a later tap to the action the
 * shell should perform. No globals, no callbacks -- so the whole UI is driven
 * and checked offline from test/ (feed a model -> PNG; feed a tap -> action).
 */

enum ui_screen {
   SCR_MAIN,
   SCR_SETTINGS,
   SCR_KEYPAD,
   SCR_DEVLIST,
   SCR_GATE,
   SCR_SENSOR,   /* one sensor: attributes above, actions below */
   SCR_CAL,      /* that sensor's calibration panel */
   SCR_CALPEND,  /* a calibration is already queued: REPLACE / CANCEL */
   SCR_SENSTYPE, /* pick a sensor type when adding */
   SCR_FORGET,   /* confirm forgetting a sensor */
   SCR_LABEL,     /* rename a sensor (letter keypad) */
   SCR_MARKPICK,  /* marker-shape picker */
   SCR_COLORPICK, /* colour picker */
   SCR_METERHELP, /* OneTouch: how-to-connect + Scan button */
   SCR_N
};

struct ui_point {
   long t;
   int glu;
   int src;  /* sensor id, for marker/colour lookup */
   int kind; /* KIND_CGM plots as a line vertex, KIND_BGM as a marker */
}; /* one plotted reading */

/* One configured sensor: everything the list row AND the detail screen need,
 * so the shell hands over a single self-contained snapshot per sensor. */
struct ui_sensor {
   long last;            /* last reading (CGM) or sync (BGM); 0 = never */
   long session_seconds; /* CGM session length */
   int id, type, kind;
   int color, marker, primary, size;
   int glu, trend, predicted, sequence;
   int rssi, rssi_ok, connected;
   long rssi_t;      /* wall-clock of the RSSI sample, for its "N M AGO" age */
   long meter_sync_t; /* meter only: when the app last synced it (vs last datapoint) */
   /* Calibration state for this CGM's LAST CAL row. cal_pending!=0 means a
    * calibration is queued and not yet accepted (cal_pending is its mg/dL);
    * otherwise cal_t>0 gives the last RESOLVED calibration -- cal_mgdl mg/dL if
    * cal_ok, else a failure. */
   int cal_pending;
   int cal_mgdl;
   int cal_ok;
   long cal_t;
   /* label must hold a full sensor_slot.label (sensors.h) -- at 12 it truncated
    * the default meter name "ONETOUCH-AB:CD" to "ONETOUCH-AB", cutting off
    * exactly the MAC tail that tells two meters apart. */
   char label[20];
   char status[12];
   char mac[20], model[24], fw[24], serial[24], code[8];
};

struct ui_dev {
   char name[12];
   char mac[20];
   int rssi;
}; /* a scanned sensor */

struct ui_stat {
   int have, tir, avg;
}; /* one rolling-window stat column */

/* Everything a frame needs to draw itself. Built fresh by the shell; the UI
 * only reads it. Pointers are borrowed for the duration of the render call.
 * Fields are grouped widest-first (8-byte, then int, then the stats array) so
 * the struct packs without padding holes; the comments mark the logical sets.
 */
struct screen {
   /* --- 8-byte members (times, then borrowed pointers) --- */
   long now;             /* realtime_s() at frame time */
   long t, tz_off;       /* reading time; local timezone offset (seconds) */
   long session_seconds; /* current session length from the driver */
   const struct ui_point *hist;     /* plot history, newest-first (borrowed) */
   const struct ui_dev *devs;       /* scanned sensors (borrowed) */
   const struct ui_sensor *sensors; /* configured sensors (borrowed) */
   const char *status;              /* top status text */
   const char *mac, *model, *fw, *mfr, *code; /* device-info strings */
   const char *entry;                         /* keypad digits typed so far */

   /* --- int members --- */
   enum ui_screen scr;
   /* current reading (glu < 0 => no reading yet) */
   int glu, trend, rssi, rssi_ok, stale;
   int disc_alarmed; /* stale-data alarm latched (drives the STALE banner) */
   /* plot: point count, scrub cursor (-1 = none), span, vertical max */
   int nhist, scrub, plot_hours, plot_max;
   /* session facts from the driver */
   int bonded, paired, have_reading, predicted, sequence;
   /* settings (values, not the module's globals) */
   int units, alarm_low, alarm_high, sound_on, vib_on, orient, disc;
   int screen_on;    /* 1 = hold the screen awake while open, 0 = follow the OS */
   int newdata_beep; /* 1 = beep on each new primary-CGM datapoint */
   /* sensor registry: the list in settings, and which one a detail screen is
    * showing (sel indexes `sensors`; -1 when no detail screen is open) */
   int nsensors, sel;
   /* last 0x32/0x34 answers for the selected sensor */
   int cal_have, cal_permitted, cal_status, cal_last_bg, cal_result;
   int cal_pending; /* value awaiting CONFIRM, mg/dL; 0 = none */
   /* modal/UX state */
   int stored, ndev;
   int kp_mode; /* keypad: 0 = pairing code, 1 = plot-max entry */
   /* Type being added (ADD SENSOR flow), for the PAIR / SELECT titles. */
   const char *add_type; /* display name, e.g. "STELO", "G7" */
   int add_kind;         /* KIND_CGM / KIND_BGM of that type */
   unsigned adv_total;
   /* system snapshot for the settings screen (perm[]: BT scan/connect/notify)
    */
   int perm[3], batt_ok, bg_restricted, standby_bucket;

   /* rolling stats: 1d / 3d / 7d / 30d / 90d */
   struct ui_stat stat[5];
};

/* One touch target and the action code the shell acts on. `arg` carries a tab
 * index / device index / +-1 delta as needed. */
struct action {
   int kind, arg;
};

enum {
   ACT_NONE = 0,
   ACT_OPEN_SETTINGS,
   ACT_PLOT_TAB,      /* arg = plot span in hours */
   ACT_ALARM_LOW,     /* arg = +-1 (minus/plus) */
   ACT_ALARM_HIGH,    /* arg = +-1 */
   ACT_PAIR_NEW,      /* "SENSOR EXPIRED" prompt -> pairing keypad */
   ACT_SCRUB,         /* a press inside the plot; shell resolves the point */
   ACT_GATE_CONTINUE, /* first-run rationale: request permissions */
   /* Modal screens (settings / keypad / device list) speak the shell's
    * menu_action protocol; arg carries that integer code so menu_action stays
    * the single dispatch point for their JNI/pairing side effects. */
   ACT_MENU,
};

/* The integer codes ACT_MENU carries. These were bare literals shared by hand
 * between the renderer and the shell's menu_action(); naming them keeps the two
 * ends honest now that there are enough to be easy to collide. Ranges (MA_PERM,
 * MA_SENSOR, MA_DIGIT, MA_TYPE, MA_DEV_PICK) are bases with an index added, so
 * leave the gaps after them alone. Values are historical -- do not renumber. */
enum ui_menu {
   MA_ORIENT      = 0,
   MA_SOUND       = 1,
   MA_VIB         = 2,
   MA_UNITS       = 3,
   MA_DISC        = 4,
   MA_SCREEN      = 5,
   MA_NEWDATA     = 6, /* toggle the new-datapoint beep */
   MA_METERSCAN   = 7, /* start scanning from the OneTouch instructions screen */
   MA_PERM        = 10, /* + permission index (0..2) */
   MA_BATTERY     = 20,
   MA_BGEXEC      = 22,
   MA_PAIR_CODE   = 30,
   MA_PLOTMAX     = 31,
   MA_SENSOR      = 40, /* + slot index; opens that sensor's screen */
   MA_SENSOR_BACK = 48,
   MA_ADDSENSOR   = 49,
   MA_PRIMARY     = 50,
   MA_MARKER      = 51,
   MA_COLOR       = 52,
   MA_LABEL       = 53,
   MA_CAL_OPEN    = 54,
   MA_SYNC        = 55,
   MA_FORGET      = 56, /* opens the confirmation screen; does not act */
   MA_FORGET_YES  = 57,
   MA_FORGET_NO   = 58,
   MA_SIZE        = 59, /* cycle marker size */
   MA_CAL_REFRESH = 60,
   MA_CAL_ENTER   = 61,
   MA_CAL_BACK    = 62,
   MA_CAL_REPLACE = 63, /* pending-cal screen: enter a new value (supersedes) */
   MA_CAL_CANCEL  = 64, /* pending-cal screen: discard the queued calibration */
   MA_TYPE        = 70, /* + sensor type (SENSOR_STELO..) */
   MA_CLOSE       = 99,
   MA_DIGIT       = 100, /* + digit 0..9 */
   MA_OK          = 111, /* keypad / label confirm */
   MA_BACKSPACE   = 110,
   MA_KP_CLOSE    = 113,
   MA_DEV_CANCEL  = 199,
   MA_DEV_PICK    = 200, /* + scanned-device index */
   MA_MARK_PICK   = 220, /* + marker enum value */
   MA_COLOR_PICK  = 240, /* + colour index */
   MA_SIZE_PICK   = 250, /* + size 1..MARK_SIZE_MAX */
   MA_CHAR        = 300  /* + index into ui_label_chars[] */
};

/* THE RANGES MUST NOT OVERLAP, and the compiler is the only thing that will
 * notice if they start to.
 *
 * Five of these codes are a base with a runtime index added, and the bases were
 * chosen by hand with gaps that are not obviously big enough: MA_SENSOR + slot
 * has three spare before MA_SENSOR_BACK, and MA_DIGIT + digit lands EXACTLY on
 * MA_BACKSPACE with none. A collision does not crash or warn -- menu_action
 * simply dispatches the wrong branch, so bumping MAX_SLOTS from 5 to 9 would
 * silently turn "open sensor 8" into "close the sensor screen", and a wrong
 * branch here reaches sensor_forget_slot and the calibration write.
 *
 * This is the same failure the alarm levels had: two numbering spaces sharing
 * one range, agreeing only by luck, with nothing checking. Assert it instead.
 * If one of these fires, MOVE THE LATER BASE UP -- do not renumber history. */
/* MA_PERM and MA_DEV_PICK are asserted in main.c, which owns NPERMS and
 * MAX_DEVS. */
_Static_assert(MA_SENSOR + MAX_SLOTS <= MA_SENSOR_BACK,
               "MA_SENSOR range hits MA_SENSOR_BACK");
_Static_assert(MA_TYPE + SENSOR_NTYPES <= MA_CLOSE,
               "MA_TYPE range hits MA_CLOSE");
_Static_assert(MA_DIGIT + 10 <= MA_BACKSPACE,
               "MA_DIGIT range hits MA_BACKSPACE");

/* Up to this many touch targets per frame. */
#define UI_MAX_HITS 48

struct hits {
   struct {
      int x, y, w, h, kind, arg;
   } box[UI_MAX_HITS];

   int n;
};

/* Render model `m` into framebuffer `fb`, recording touch targets into `h`. */
void ui_render(struct ANativeWindow_Buffer *fb, const struct screen *m,
               struct hits *h);
/* Map a tap at (x,y) against the targets from the last render. Pure. */
struct action ui_hit(const struct hits *h, int x, int y);

#endif
