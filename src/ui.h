// SPDX-License-Identifier: GPL-3.0
// ui.h --- On-screen rendering + touch input (the interactive UI layer)
// Copyright 2026 Jakob Kastelic

#ifndef STEALO_UI_H
#define STEALO_UI_H

#include "ndk.h"
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

/* Bounded, always-terminated copy of a string the caller may be racing a writer
 * on (pass cap = source buffer size). Pure. */
void str_snapshot(char *dst, int cap, const char *src);

/* ================= functional-core UI =================================
 * The UI is a pure function of an immutable snapshot: the shell (main.c) fills
 * a `struct screen` each frame, ui_render() draws it and records the touch
 * targets into `struct hits`, and ui_hit() maps a later tap to the action the
 * shell should perform. No globals, no callbacks -- so the whole UI is driven
 * and checked offline from test/ (feed a model -> PNG; feed a tap -> action).
 */

enum ui_screen { SCR_MAIN, SCR_SETTINGS, SCR_KEYPAD, SCR_DEVLIST, SCR_GATE };

struct ui_point {
   long t;
   int glu;
}; /* one plotted reading */

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
   const struct ui_point *hist; /* plot history, newest-first (borrowed) */
   const struct ui_dev *devs;   /* scanned sensors (borrowed) */
   const char *status;          /* top status text */
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
   /* modal/UX state */
   int alarm_sounding, stored, ndev;
   int kp_mode; /* keypad: 0 = pairing code, 1 = plot-max entry */
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
