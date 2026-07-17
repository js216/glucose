// SPDX-License-Identifier: GPL-3.0
// ui.c --- On-screen rendering + touch input (the interactive UI layer)
// Copyright 2026 Jakob Kastelic

/* The whole UI as a pure function of an immutable `struct screen`: ui_render()
 * draws the current screen into a locked framebuffer and records its touch
 * targets into `struct hits`; ui_hit() maps a later tap to the action the shell
 * (main.c) should perform. No globals, no callbacks -- so every screen builds
 * and runs on the host against a malloc'd buffer (see test/uitest.c, which
 * renders each screen to a PPM and checks its hit-targets with no phone). */
#include "ui.h"
#include "font.h"
#include "ndk.h"
#include "plot.h"
#include <stdint.h>
#include <stdio.h> /* snprintf */

/* Layout constants owned by the UI (not the shell). */
#define UI_COLS   33         /* character columns the layout targets */
#define UI_TABS   6          /* plot-span tabs */
#define UI_HILITE 0xFFAAAAAA /* scrub highlight dot (gray) */
static const int ui_tab_hours[UI_TABS] = {3, 6, 12, 24, 72, 168};

#define UI_LBL(units) ((units) ? "MMOL/L" : "MG/DL")

/* Fill one sc*sc glyph cell at (bx,by), clipped to the buffer. */
static void draw_cell(uint32_t *px, const struct ANativeWindow_Buffer *buf,
                      int bx, int by, int sc, uint32_t color)
{
   for (int dy = 0; dy < sc; dy++)
      for (int dx = 0; dx < sc; dx++) {
         int x = bx + dx;
         int y = by + dy;
         if (x >= 0 && x < buf->width && y >= 0 && y < buf->height)
            px[(y * buf->stride) + x] = color;
      }
}

void draw_str(uint32_t *px, const struct ANativeWindow_Buffer *buf, int ox,
              int oy, int sc, const char *s, uint32_t color)
{
   /* Hard scan bound: draw can run concurrently with a BLE-thread rewrite of a
    * text buffer (g_lines/g_status); a torn write can momentarily drop the NUL,
    * and without this cap the loop could scan off the end. No UI string is this
    * long. */
   for (int ci = 0; ci < 256 && s[ci]; ci++) {
      const uint8_t *g = glyph_for(s[ci]);
      if (!g)
         continue;
      for (int row = 0; row < 7; row++)
         for (int col = 0; col < 5; col++) {
            if (!((unsigned)g[row] & (0x10U >> (unsigned)col)))
               continue;
            int bx = ox + (((ci * 6) + col) * sc);
            int by = oy + (row * sc);
            draw_cell(px, buf, bx, by, sc, color);
         }
   }
}

void draw_frame(uint32_t *px, const struct ANativeWindow_Buffer *buf, int x,
                int y, int w, int h, uint32_t c)
{
   if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > buf->width ||
       y + h > buf->height)
      return;
   for (int i = 0; i < w; i++) {
      px[(y * buf->stride) + x + i]           = c;
      px[((y + h - 1) * buf->stride) + x + i] = c;
   }
   for (int j = 0; j < h; j++) {
      px[((y + j) * buf->stride) + x]         = c;
      px[((y + j) * buf->stride) + x + w - 1] = c;
   }
}

void fmt_glu(int mgdl, int units, char *out, int n)
{
   if (units) { /* mmol/L = mg/dL / 18, one decimal */
      int t = ((mgdl * 10) + 9) / 18;
      (void)snprintf(out, n, "%d.%d", t / 10, t % 10);
   } else {
      (void)snprintf(out, n, "%d", mgdl);
   }
}

void str_snapshot(char *dst, int cap, const char *src)
{
   int i = 0;
   for (; i < cap - 1 && src[i]; i++)
      dst[i] = src[i];
   dst[i] = 0;
}

/* ---- functional core: pure render + pure hit-test ---- */

static void clear_fb(struct ANativeWindow_Buffer *fb, uint32_t c)
{
   uint32_t *px = fb->bits;
   for (int32_t y = 0; y < fb->height; y++)
      for (int32_t x = 0; x < fb->width; x++)
         px[(y * fb->stride) + x] = c;
}

static void add_hit(struct hits *h, int x, int y, int w, int hgt, int kind,
                    int arg)
{
   if (h->n >= UI_MAX_HITS)
      return;
   h->box[h->n].x    = x;
   h->box[h->n].y    = y;
   h->box[h->n].w    = w;
   h->box[h->n].h    = hgt;
   h->box[h->n].kind = kind;
   h->box[h->n].arg  = arg;
   h->n++;
}

/* big-number colour by fixed medical range (0xAABBGGRR) */
static uint32_t glu_color(int g)
{
   if (g < 50)
      return 0xFF0000FF; /* red    */
   if (g < 70)
      return 0xFF0080FF; /* orange */
   if (g < 180)
      return 0xFF33FF88; /* green  */
   return 0xFFFFFFFF;    /* white  */
}

static uint32_t white_color(int g)
{
   (void)g;
   return 0xFFFFFFFF; /* plot dots */
}

void fmt_trend(int tr, char *out, int n)
{
   if (tr == 127) {
      (void)snprintf(out, n, "--");
      return;
   }
   int a = tr < 0 ? -tr : tr;
   (void)snprintf(out, n, "%c%d.%d", tr < 0 ? '-' : '+', a / 10, a % 10);
}

void fmt_hms(long epoch, long tz, char *out, int n)
{
   long t = (epoch + tz) % 86400;
   if (t < 0)
      t += 86400;
   (void)snprintf(out, n, "%02ld:%02ld:%02ld", t / 3600, (t % 3600) / 60,
                  t % 60);
}

/* Left/top column: big number + label column, plot tabs, plot, alarm-config
 * row. Draws into [cx, cx+cw); returns the y just below the last row. Records
 * the big-number band (open settings), the plot rect (scrub), the tab cells,
 * and the two +/- alarm buttons as touch targets. */
static int render_glucose(struct ANativeWindow_Buffer *fb,
                          const struct screen *m, struct hits *h, int cx,
                          int cw, int y, int sc, int bottom)
{
   uint32_t *px  = fb->bits;
   int landscape = bottom > 0;                   /* height-constrained column */
   int pad       = landscape ? 6 * sc : 18 * sc; /* padding around the number */
   int scrub     = (m->scrub >= 0 && m->scrub < m->nhist);

   char big[8];
   uint32_t bigcol = 0;
   if (m->stale) {
      (void)snprintf(big, sizeof big, "---");
      bigcol = 0xFF888888;
   } else {
      fmt_glu(m->glu, m->units, big, sizeof big);
      bigcol = glu_color(m->glu);
   }
   int big_y0 = 0;
   y += landscape ? 4 * sc : 12 * sc;

   char tr[8];
   char agestr[12];
   if (m->stale)
      (void)snprintf(tr, sizeof tr, "---");
   else
      fmt_trend(m->trend, tr, sizeof tr);
   long a = m->now - m->t;
   if (a < 0)
      a = 0;
   if (a < 600)
      (void)snprintf(agestr, sizeof agestr, "%ld S", a);
   else
      (void)snprintf(agestr, sizeof agestr, "%ld M", a / 60);
   char rssistr[12]; /* shares the reading's timestamp (AGE) */
   if (!m->stale && m->rssi_ok)
      (void)snprintf(rssistr, sizeof rssistr, "%d DB", m->rssi);
   else
      (void)snprintf(rssistr, sizeof rssistr, "--");

   int uw    = str_len(UI_LBL(m->units));
   int rw    = str_len(rssistr);
   int col_w = (uw > rw ? uw : rw) * 6 * sc;
   int gap   = 6 * sc;
   int bigsc = sc * 10; /* shrink so number + label column fit cw */
   int fit   = (cw - (4 * sc) - gap - col_w) / (str_len(big) * 6);
   if (bigsc > fit)
      bigsc = fit;
   if (bigsc < 2 * sc)
      bigsc = 2 * sc;
   int big_w = str_len(big) * 6 * bigsc;
   int bx    = cx + ((cw - (big_w + gap + col_w)) / 2);
   if (bx < cx + (2 * sc))
      bx = cx + (2 * sc);
   draw_str(px, fb, bx, y, bigsc, big, bigcol);
   int colx  = bx + big_w + gap;
   int gh    = 7 * sc;    /* a label glyph is 7 rows tall */
   int num_h = 7 * bigsc; /* the big number's exact glyph height */
   /* Four labels span EXACTLY the number's height: the first sits at the
    * number's top, the last is pinned so its bottom row equals the number's
    * bottom row, and the middle two are evenly spread between. */
   int lp = (num_h - gh) / 3;
   draw_str(px, fb, colx, y, sc, UI_LBL(m->units), 0xFFCCCCCC);
   draw_str(px, fb, colx, y + lp, sc, tr, 0xFFCCCCCC);
   draw_str(px, fb, colx, y + (2 * lp), sc, agestr, 0xFFCCCCCC);
   draw_str(px, fb, colx, y + num_h - gh, sc, rssistr, 0xFFCCCCCC);
   y += (8 * bigsc) + pad;

   /* plot-window tabs (or the scrub readout while dragging) */
   int colw = cw / UI_TABS;
   int rowh = 14 * sc;
   /* Tap target reaches up to the big number's lowest pixel and no higher, then
    * down through the tab row. The big-number (settings) band ends exactly
    * there, so the two never fight over the same pixels. */
   int tab_y  = y - bigsc - pad;
   int tab_h  = rowh + bigsc + pad;
   int big_y1 = tab_y;
   /* the big-number band opens the settings menu */
   add_hit(h, 0, big_y0, cw + cx, big_y1 - big_y0, ACT_OPEN_SETTINGS, 0);
   if (scrub) {
      char ts[16];
      char line[40];
      char gv[12];
      fmt_hms(m->hist[m->scrub].t, m->tz_off, ts, sizeof ts);
      ts[5] = '\0';
      fmt_glu(m->hist[m->scrub].glu, m->units, gv, sizeof gv);
      (void)snprintf(line, sizeof line, "%s %s  %s", gv, UI_LBL(m->units), ts);
      int lw = str_len(line) * 6 * 2 * sc;
      int lx = cx + ((cw - lw) / 2);
      if (lx < cx + (2 * sc))
         lx = cx + (2 * sc);
      draw_str(px, fb, lx, y, 2 * sc, line, 0xFFFFFFFF);
   } else {
      int laby = y + ((rowh - (7 * sc)) / 2);
      for (int i = 0; i < UI_TABS; i++) {
         char lab[6];
         if (ui_tab_hours[i] < 48)
            (void)snprintf(lab, sizeof lab, "%dH", ui_tab_hours[i]);
         else
            (void)snprintf(lab, sizeof lab, "%dD", ui_tab_hours[i] / 24);
         int lw   = str_len(lab) * 6 * sc;
         int tabx = cx + (i * colw);
         draw_str(px, fb, tabx + ((colw - lw) / 2), laby, sc, lab,
                  ui_tab_hours[i] == m->plot_hours ? 0xFFFFFFFF : 0xFF888888);
         /* arg carries the plot span in hours, so the shell needn't know the
          * tab list -- it just assigns it. */
         add_hit(h, tabx, tab_y, colw, tab_h, ACT_PLOT_TAB, ui_tab_hours[i]);
      }
   }
   y += rowh;

   /* fixed-scale plot -- fixed height in portrait, fills the column landscape
    */
   int ph = landscape ? (bottom - y - (26 * sc)) : 12 * bigsc;
   if (ph < 20 * sc)
      ph = 20 * sc;
   int plot_x = cx + (2 * sc);
   int plot_y = y;
   int plot_w = cw - (4 * sc);
   /* ~7 days of readings at 5-minute spacing; the shell never sends more. */
#define UI_PLOT_MAX 2100
   static struct plot_pt pts[UI_PLOT_MAX];
   int np = m->nhist < UI_PLOT_MAX ? m->nhist : UI_PLOT_MAX;
   for (int i = 0; i < np; i++) {
      pts[i].t   = m->hist[i].t;
      pts[i].glu = m->hist[i].glu;
   }
   int prad = 3 * sc / 2;
   if (m->plot_hours >= 168)
      prad = prad / 2;
   else if (m->plot_hours >= 72)
      prad = prad * 3 / 4;
   plot_render(px, fb->stride, fb->width, fb->height, plot_x, plot_y, plot_w,
               ph, pts, np, m->now, m->plot_hours, prad, white_color,
               scrub ? m->scrub : -1, UI_HILITE);
   /* the whole plot rect scrubs; the shell resolves the datapoint via plot_hit
    */
   add_hit(h, plot_x, plot_y, plot_w, ph, ACT_SCRUB, 0);
   y += ph + (9 * sc);

   /* alarm config row: "ALARM  LOW - 110 +  HIGH - 300 +", full-column */
   const uint32_t gy = 0xFF888888;
   const uint32_t wt = 0xFFFFFFFF;
   int cwid          = 6 * sc;
   char lo[8];
   char hi[8];
   fmt_glu(m->alarm_low, m->units, lo, sizeof lo);
   fmt_glu(m->alarm_high, m->units, hi, sizeof hi);
   const char *tok[9] = {"ALARM", "LOW", "-", lo, "+", "HIGH", "-", hi, "+"};
   uint32_t tcol[9]   = {gy, gy, gy, wt, gy, gy, gy, wt, gy};
   int total          = 0;
   for (int i = 0; i < 9; i++)
      total += str_len(tok[i]) * cwid;
   int g = (cw - total) / 10;
   if (g < cwid)
      g = cwid;
   int ax   = cx + g;
   int al_y = y - (3 * sc);
   int al_h = (3 * sc) + (7 * sc) + pad;
   /* the four +/- buttons in draw order: LOW-, LOW+, HIGH-, HIGH+ */
   int alact[4] = {ACT_ALARM_LOW, ACT_ALARM_LOW, ACT_ALARM_HIGH,
                   ACT_ALARM_HIGH};
   int alarg[4] = {-1, +1, -1, +1};
   int btn      = 0;
   for (int i = 0; i < 9; i++) {
      draw_str(px, fb, ax, y, sc, tok[i], tcol[i]);
      int tw = str_len(tok[i]) * cwid;
      if (tok[i][0] == '-' || tok[i][0] == '+') {
         add_hit(h, ax - (g / 2), al_y, tw + g, al_h, alact[btn], alarg[btn]);
         btn++;
      }
      ax += tw + g;
   }
   y += (7 * sc) + pad;
   return y;
}

/* Right/bottom column: sensor+session panel, rolling-stats table, alarm banner,
 * and the tappable "SENSOR EXPIRED" prompt. */
static void render_info(struct ANativeWindow_Buffer *fb, const struct screen *m,
                        struct hits *h, int cx, int cw, int y, int sc)
{
   uint32_t *px       = fb->bits;
   int x              = cx + (2 * sc);
   int lh             = 16 * sc; /* row pitch: matches the settings leading */
   const uint32_t col = 0xFFCCCCCC;

   char row[72];
   if (m->bonded) {
      (void)snprintf(row, sizeof row, "STATE   CONNECTED");
   } else {
      char st[UI_COLS + 1];
      str_snapshot(st, sizeof st, m->status ? m->status : "");
      (void)snprintf(row, sizeof row, "STATE   %s  %u ADV", st, m->adv_total);
   }
   draw_str(px, fb, x, y, sc, row, col);
   y += lh;
   (void)snprintf(row, sizeof row, "STORED  %d readings", m->stored);
   draw_str(px, fb, x, y, sc, row, col);
   y += lh;
   if (m->have_reading) {
      long ss   = m->session_seconds;
      long left = (15L * 86400) - ss;
      if (left < 0)
         left = 0;
      (void)snprintf(row, sizeof row, "SESSION %ldD %ldH   LEFT %ldD %ldH",
                     ss / 86400, (ss % 86400) / 3600, left / 86400,
                     (left % 86400) / 3600);
   } else {
      (void)snprintf(row, sizeof row, "SESSION --");
   }
   draw_str(px, fb, x, y, sc, row, col);
   y += lh;
   if (m->have_reading) {
      /* predicted is a 10-bit field; 0x3ff (1023) is the sensor's "no
       * prediction" sentinel and no real value exceeds Dexcom's 400 mg/dL cap
       * -- show "--" rather than the raw sentinel. SEQ is still valid. */
      if (m->predicted <= 0 || m->predicted > 400) {
         (void)snprintf(row, sizeof row, "PRED    --      SEQ %d", m->sequence);
      } else {
         char pv[12];
         fmt_glu(m->predicted, m->units, pv, sizeof pv);
         (void)snprintf(row, sizeof row, "PRED    %s %s   SEQ %d", pv,
                        UI_LBL(m->units), m->sequence);
      }
   } else {
      (void)snprintf(row, sizeof row, "PRED    --      SEQ --");
   }
   draw_str(px, fb, x, y, sc, row, col);
   y += lh;

   /* rolling stats table: TIR / AVG / A1C across 1D/3D/7D/30D/90D */
   char tc[5][8];
   char ac[5][8];
   char hc[5][8];
   for (int i = 0; i < 5; i++) {
      if (m->stat[i].have) {
         (void)snprintf(tc[i], sizeof tc[i], "%d", m->stat[i].tir);
         fmt_glu(m->stat[i].avg, m->units, ac[i], sizeof ac[i]);
         /* ADAG estimate: A1C% = (avg_mg/dL + 46.7) / 28.7, in tenths. */
         int te = ((100 * m->stat[i].avg) + 4670 + 143) / 287;
         (void)snprintf(hc[i], sizeof hc[i], "%d.%d", te / 10, te % 10);
      } else {
         (void)snprintf(tc[i], sizeof tc[i], "--");
         (void)snprintf(ac[i], sizeof ac[i], "--");
         (void)snprintf(hc[i], sizeof hc[i], "--");
      }
   }
   y += 7 * sc;
   (void)snprintf(row, sizeof row, "%-4s%-6s%-6s%-6s%-6s%-6s%s", "", "1D", "3D",
                  "7D", "30D", "90D", "");
   draw_str(px, fb, x, y, sc, row, 0xFF888888);
   y += lh;
   (void)snprintf(row, sizeof row, "%-4s%-6s%-6s%-6s%-6s%-6s%s", "TIR", tc[0],
                  tc[1], tc[2], tc[3], tc[4], "%");
   draw_str(px, fb, x, y, sc, row, col);
   y += lh;
   (void)snprintf(row, sizeof row, "%-4s%-6s%-6s%-6s%-6s%-6s%s", "AVG", ac[0],
                  ac[1], ac[2], ac[3], ac[4], UI_LBL(m->units));
   draw_str(px, fb, x, y, sc, row, col);
   y += lh;
   (void)snprintf(row, sizeof row, "%-4s%-6s%-6s%-6s%-6s%-6s%s", "A1C", hc[0],
                  hc[1], hc[2], hc[3], hc[4], "%");
   draw_str(px, fb, x, y, sc, row, col);
   y += lh;

   /* alarm banner: STALE, else LOW/HIGH if the reading is fresh */
   const char *msg = 0;
   uint32_t c      = 0;
   if (m->disc_alarmed) {
      msg = "STALE";
      c   = 0xFF00A0FF;
   } else if (m->now - m->t <= 360) {
      if (m->glu < m->alarm_low) {
         msg = "LOW";
         c   = 0xFF0000FF;
      } else if (m->glu > m->alarm_high) {
         msg = "HIGH";
         c   = 0xFF0080FF;
      }
   }
   if (msg) {
      int msc = 5 * sc;
      int w   = str_len(msg) * 6 * msc;
      int mx  = cx + ((cw - w) / 2);
      if (mx < cx + (2 * sc))
         mx = cx + (2 * sc);
      y += (7 * sc) + (9 * sc);
      draw_str(px, fb, mx, y, msc, msg, c);
   }

   /* past its rated 15-day life: a tappable prompt to pair a replacement */
   if (m->have_reading && m->session_seconds >= 15L * 86400) {
      const char *l1 = "SENSOR EXPIRED";
      const char *l2 = "PAIR NEW SENSOR ...";
      uint32_t ec    = 0xFF00A0FF;
      int w1         = str_len(l1) * 6 * sc;
      int w2         = str_len(l2) * 6 * sc;
      y += 2 * lh;
      draw_str(px, fb, cx + ((cw - w1) / 2), y, sc, l1, ec);
      draw_str(px, fb, cx + ((cw - w2) / 2), y + lh, sc, l2, ec);
      add_hit(h, cx, y - (3 * sc), cw, lh + (10 * sc), ACT_PAIR_NEW, 0);
   }
}

/* Before any reading arrives: scan status lines + the scanned-sensor list. */
static void render_noreading(struct ANativeWindow_Buffer *fb,
                             const struct screen *m, int y, int sc)
{
   uint32_t *px = fb->bits;
   char line[UI_COLS + 1];
   char st[UI_COLS + 1];
   str_snapshot(st, sizeof st, m->status ? m->status : "");
   (void)snprintf(line, sizeof line, "STEALO  %s", st);
   draw_str(px, fb, 2 * sc, y, sc, line, 0xFFFFFFFF);
   y += 9 * sc;
   /* total rounded to 10s so ambient chatter doesn't churn the line */
   (void)snprintf(line, sizeof line, "ADV %u  DX %d", (m->adv_total / 10) * 10,
                  m->ndev);
   draw_str(px, fb, 2 * sc, y, sc, line, 0xFFFFFFFF);
   y += 9 * sc;

   if (m->ndev > 0) {
      y += 3 * sc;
      draw_str(px, fb, 2 * sc, y, sc, "SENSORS", 0xFF888888);
      y += 10 * sc;
      for (int i = 0; i < m->ndev; i++) {
         char dl[48];
         (void)snprintf(dl, sizeof dl, "%-8s %4d %s", m->devs[i].name,
                        m->devs[i].rssi, m->devs[i].mac);
         draw_str(px, fb, 2 * sc, y, sc, dl, 0xFFCCCCCC);
         y += 9 * sc;
      }
   }
}

/* Compose the main screen: two columns in landscape, stacked in portrait. */
static void render_main(struct ANativeWindow_Buffer *fb, const struct screen *m,
                        struct hits *h)
{
   int landscape = fb->width > fb->height;
   int colw      = landscape ? fb->width / 2 : fb->width;
   int sc        = colw / (UI_COLS * 6);
   if (sc < 1)
      sc = 1;
   int y = (fb->height / 20) + (2 * sc); /* clear the system status bar */

   if (m->glu >= 0) {
      if (landscape) {
         int gw   = 2 * 6 * sc;
         int cwid = (fb->width - gw) / 2;
         render_glucose(fb, m, h, 0, cwid, y, sc, fb->height);
         render_info(fb, m, h, cwid + gw, fb->width - cwid - gw, y, sc);
      } else {
         y = render_glucose(fb, m, h, 0, fb->width, y, sc, 0);
         render_info(fb, m, h, 0, fb->width, y, sc);
      }
   } else {
      render_noreading(fb, m, y, sc);
   }
}

/* ---- settings menu (portrait table; rows carry menu_action codes) ---- */

static const char *ui_orient_lbl[] = {"PORTRAIT", "LANDSCAPE", "GRAVITY",
                                      "SYSTEM"};
static const char *ui_disc_lbl[]   = {"OFF", "10 MIN", "30 MIN", "60 MIN"};
static const char *ui_perm_lbl[]   = {"BT SCAN", "BT CONNECT", "NOTIFY"};

/* App-Standby bucket -> short label. */
static const char *ui_bucket_label(int b)
{
   if (b <= 0)
      return "?";
   if (b <= 5)
      return "EXEMPT";
   if (b <= 10)
      return "ACTIVE";
   if (b <= 20)
      return "WORKING";
   if (b <= 30)
      return "FREQUENT";
   if (b <= 40)
      return "RARE";
   return "RESTRICTED";
}

/* One menu row: name left, value right; records a full-width tap target
 * carrying the menu_action `code` (code < 0 = read-only, no target). */
static void menu_row(struct ANativeWindow_Buffer *fb, struct hits *h, int y,
                     int sc, int lh, const char *name, const char *value,
                     uint32_t valcol, int code)
{
   uint32_t *px = fb->bits;
   int rx       = fb->width - (4 * sc);
   draw_str(px, fb, 4 * sc, y, sc, name, 0xFFCCCCCC);
   int vw = str_len(value) * 6 * sc;
   draw_str(px, fb, rx - vw, y, sc, value, valcol);
   if (code >= 0)
      add_hit(h, 0, y - (3 * sc), fb->width, lh, ACT_MENU, code);
}

static void render_settings(struct ANativeWindow_Buffer *fb,
                            const struct screen *m, struct hits *h)
{
   uint32_t *px = fb->bits;
   int sc       = fb->width / (UI_COLS * 6); /* settings is always portrait */
   if (sc < 1)
      sc = 1;
   int tsc = 2 * sc;
   int lh  = 16 * sc; /* generous pitch: a blank line between rows */
   int x   = 4 * sc;
   int rx  = fb->width - (4 * sc);
   int y   = (fb->height / 20) + (8 * sc);

   /* title with a right-aligned X to close */
   draw_str(px, fb, x, y, tsc, "SETTINGS", 0xFFFFFFFF);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", 0xFFFFFFFF);
   /* generous close target: title + blank line + DISPLAY header */
   add_hit(h, 0, y - (3 * sc), fb->width, 3 * lh, ACT_MENU, 99);
   y += 2 * lh;

   draw_str(px, fb, x, y, sc, "DISPLAY", 0xFF888888);
   y += lh;
   menu_row(fb, h, y, sc, lh, "ORIENTATION",
            ui_orient_lbl[(unsigned)m->orient & 3U], 0xFFFFFFFF, 0);
   y += lh;
   menu_row(fb, h, y, sc, lh, "UNITS", m->units ? "MMOL/L" : "MG/DL",
            0xFFFFFFFF, 3);
   y += lh;
   char pmv[20];
   char pmvv[8];
   fmt_glu(m->plot_max, m->units, pmvv, sizeof pmvv);
   (void)snprintf(pmv, sizeof pmv, "%s %s", pmvv, UI_LBL(m->units));
   menu_row(fb, h, y, sc, lh, "PLOT MAX", pmv, 0xFFFFFFFF, 31);
   y += 2 * lh;

   draw_str(px, fb, x, y, sc, "ALARM", 0xFF888888);
   y += lh;
   menu_row(fb, h, y, sc, lh, "SOUND", m->sound_on ? "ON" : "OFF", 0xFFFFFFFF,
            1);
   y += lh;
   menu_row(fb, h, y, sc, lh, "VIBRATION", m->vib_on ? "ON" : "OFF", 0xFFFFFFFF,
            2);
   y += lh;
   menu_row(fb, h, y, sc, lh, "DISCONNECT", ui_disc_lbl[(unsigned)m->disc & 3U],
            0xFFFFFFFF, 4);
   y += 2 * lh;

   /* permissions + the background controls a CGM needs alive. Values are the
    * shell's cached snapshot -- never live JNI from a render. */
   draw_str(px, fb, x, y, sc, "PERMISSIONS", 0xFF888888);
   y += lh;
   for (int i = 0; i < 3; i++) {
      int g = m->perm[i];
      menu_row(fb, h, y, sc, lh, ui_perm_lbl[i], g ? "GRANTED" : "DENIED",
               g ? 0xFF33FF88 : 0xFF4466FF, 10 + i);
      y += lh;
   }
   menu_row(fb, h, y, sc, lh, "BATTERY",
            m->batt_ok ? "UNRESTRICTED" : "OPTIMIZED",
            m->batt_ok ? 0xFF33FF88 : 0xFF4466FF, 20);
   y += lh;
   menu_row(fb, h, y, sc, lh, "STANDBY", ui_bucket_label(m->standby_bucket),
            (m->standby_bucket > 0 && m->standby_bucket <= 20) ? 0xFF33FF88
                                                               : 0xFFAA8844,
            -1);
   y += lh;
   menu_row(fb, h, y, sc, lh, "BG EXEC",
            m->bg_restricted ? "RESTRICTED" : "ALLOWED",
            m->bg_restricted ? 0xFF4466FF : 0xFF33FF88, 22);
   y += 2 * lh;

   draw_str(px, fb, x, y, sc, "SENSOR", 0xFF888888);
   y += lh;
   menu_row(fb, h, y, sc, lh, "TYPE", "CGM", 0xFFFFFFFF, -1);
   y += lh;
   menu_row(fb, h, y, sc, lh, "CODE", m->code ? m->code : "", 0xFFFFFFFF, -1);
   y += lh;
   menu_row(fb, h, y, sc, lh, "MAC", (m->mac && m->mac[0]) ? m->mac : "--",
            0xFFFFFFFF, -1);
   y += lh;
   if (m->model && m->model[0]) {
      menu_row(fb, h, y, sc, lh, "SW", m->model, 0xFFFFFFFF, -1);
      y += lh;
   }
   if (m->fw && m->fw[0]) {
      menu_row(fb, h, y, sc, lh, "FW", m->fw, 0xFFFFFFFF, -1);
      y += lh;
   }
   menu_row(fb, h, y, sc, lh, "PAIR NEW SENSOR ...", "", 0xFFFFFFFF, 30);
}

/* One keypad key: framed cell, centred label, full-cell ACT_MENU target. */
static void pad_key(struct ANativeWindow_Buffer *fb, struct hits *h, int cx,
                    int cy, int cw, int ch, int ksc, const char *lab, int code)
{
   uint32_t *px = fb->bits;
   draw_frame(px, fb, cx, cy, cw, ch, 0xFF555555);
   int lw  = str_len(lab) * 6 * ksc;
   int lhh = 7 * ksc;
   draw_str(px, fb, cx + ((cw - lw) / 2), cy + ((ch - lhh) / 2), ksc, lab,
            0xFFFFFFFF);
   add_hit(h, cx, cy, cw, ch, ACT_MENU, code);
}

/* Pairing / plot-max keypad: a title, a fixed-width entry field, and a 3x4
 * digit grid. Keys and the close band carry menu_action codes (100-113). */
static void render_keypad(struct ANativeWindow_Buffer *fb,
                          const struct screen *m, struct hits *h)
{
   uint32_t *px = fb->bits;
   int sc       = fb->width / (UI_COLS * 6);
   if (sc < 1)
      sc = 1;
   int tsc = 2 * sc;
   int lh  = 16 * sc;
   int x   = 4 * sc;
   int rx  = fb->width - (4 * sc);
   int y   = (fb->height / 20) + (8 * sc);

   int ty = y;
   draw_str(px, fb, x, y, tsc, m->kp_mode ? "PLOT MAX" : "PAIR NEW SENSOR",
            0xFFFFFFFF);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", 0xFFFFFFFF);
   y += 2 * lh;

   /* Entry field: one underscore per slot, replaced by digits as typed, so the
    * width never shifts. Plot-max shows the unit after the value; pair shows
    * the 4 code digits. dsc is sized for the widest label so the field -- and
    * the keypad below -- is identical in both modes. */
   int nslots     = m->kp_mode ? 3 : 4;
   const char *en = m->entry ? m->entry : "";
   char shown[16];
   int k = 0;
   for (int i = 0; i < nslots; i++) /* typed digits, then '_' for empty slots */
      shown[k++] = *en ? *en++ : '_';
   if (m->kp_mode)
      k += snprintf(shown + k, sizeof shown - k, " %s", UI_LBL(m->units));
   shown[k] = 0;
   int dsc  = (fb->width - (8 * sc)) / (10 * 6); /* fits "___ MMOL/L" */
   if (dsc > 4 * sc)
      dsc = 4 * sc;
   if (dsc < sc)
      dsc = sc;
   int dw = str_len(shown) * 6 * dsc;
   draw_str(px, fb, (fb->width - dw) / 2, y, dsc, shown, 0xFF33FF88);
   y += (7 * dsc) + (12 * sc);

   /* Generous close target: the whole area above the keypad closes it. */
   add_hit(h, 0, ty - (3 * sc), fb->width, y - (ty - (3 * sc)), ACT_MENU, 113);

   /* 3x4 grid: digits, then 0 / DEL / OK. The title's X cancels. */
   int gm     = fb->width / 12;
   int gw     = fb->width - (2 * gm);
   int cw     = gw / 3;
   int bottom = fb->height - (fb->height / 20);
   int ch     = (bottom - y) / 4;
   int wfit   = (cw - (4 * sc)) / (3 * 6); /* widest label "DEL" fits width */
   int hfit   = (ch - (4 * sc)) / 7;
   int ksc    = wfit < hfit ? wfit : hfit;
   if (ksc < sc)
      ksc = sc;
   static const char *keys[12] = {"7", "8", "9", "4", "5", "6",
                                  "1", "2", "3", "0", "<", "OK"};
   static const int acts[12]   = {107, 108, 109, 104, 105, 106,
                                  101, 102, 103, 100, 110, 111};
   for (int r = 0; r < 4; r++)
      for (int col = 0; col < 3; col++) {
         int idx = (r * 3) + col;
         pad_key(fb, h, gm + (col * cw), y + (r * ch), cw - (2 * sc),
                 ch - (2 * sc), ksc, keys[idx], acts[idx]);
      }
}

/* Pairing candidate picker: scanned sensors strongest-first; a tap pairs one
 * (ACT_MENU 200+index), the X cancels (199). */
static void render_devlist(struct ANativeWindow_Buffer *fb,
                           const struct screen *m, struct hits *h)
{
   uint32_t *px = fb->bits;
   int sc       = fb->width / (UI_COLS * 6);
   if (sc < 1)
      sc = 1;
   int tsc = 2 * sc;
   int lh  = 16 * sc;
   int x   = 4 * sc;
   int rx  = fb->width - (4 * sc);
   int y   = (fb->height / 20) + (8 * sc);

   draw_str(px, fb, x, y, tsc, "SELECT SENSOR", 0xFFFFFFFF);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", 0xFFFFFFFF);
   add_hit(h, 0, y - (3 * sc), fb->width, 8 * tsc, ACT_MENU, 199);
   y += 2 * lh;

   if (m->ndev <= 0) {
      draw_str(px, fb, x, y, sc, "Searching for sensors...", 0xFF888888);
      return;
   }
   draw_str(px, fb, x, y, sc, "Nearest first -- tap yours:", 0xFF888888);
   y += 2 * lh;

   /* selection sort by RSSI, strongest first (the model owns index -> device)
    */
   int order[16];
   int n = m->ndev < 16 ? m->ndev : 16;
   for (int i = 0; i < n; i++)
      order[i] = i;
   for (int i = 0; i < n; i++)
      for (int j = i + 1; j < n; j++)
         if (m->devs[order[j]].rssi > m->devs[order[i]].rssi) {
            int t    = order[i];
            order[i] = order[j];
            order[j] = t;
         }
   for (int kk = 0; kk < n; kk++) {
      int i = order[kk];
      char rs[12];
      (void)snprintf(rs, sizeof rs, "%d dBm", m->devs[i].rssi);
      menu_row(fb, h, y, sc, lh, m->devs[i].name, rs, 0xFFFFFFFF, 200 + i);
      y += lh;
   }
}

/* First-run permission rationale + a CONTINUE button (records
 * ACT_GATE_CONTINUE). All static copy; the model is unused beyond the
 * framebuffer size. */
static void render_gate(struct ANativeWindow_Buffer *fb, struct hits *h)
{
   uint32_t *px = fb->bits;
   int sc       = fb->width / (UI_COLS * 6);
   if (sc < 1)
      sc = 1;
   static const char *lines[] = {
       "STEALO reads your CGM",
       "sensor over Bluetooth and",
       "warns you of highs and lows.",
       "",
       "IT ASKS FOR:",
       "",
       "BLUETOOTH  find + connect",
       "           to the sensor",
       "NOTIFY     alert you to",
       "           highs and lows",
       "BATTERY    keep reading in",
       "           the background",
       "",
       "Your glucose data never",
       "leaves this phone.",
   };
   int tsc = 2 * sc;
   int lh  = 12 * sc;
   int x   = 6 * sc;
   int y   = fb->height / 12;
   draw_str(px, fb, x, y, tsc, "PERMISSIONS", 0xFFFFFFFF);
   y += 3 * lh;
   for (int i = 0; i < (int)(sizeof lines / sizeof lines[0]); i++) {
      draw_str(px, fb, x, y, sc, lines[i], 0xFFCCCCCC);
      y += lh;
   }
   y += 2 * lh;
   const char *lbl = "CONTINUE";
   int bsc         = 2 * sc;
   int lw          = str_len(lbl) * 6 * bsc;
   int gh          = 7 * bsc;
   int padx        = 6 * bsc;
   int pady        = 5 * bsc; /* roomy box around the label */
   int bw          = lw + (2 * padx);
   int bh          = gh + (2 * pady);
   int bx          = (fb->width - bw) / 2;
   draw_frame(px, fb, bx, y, bw, bh, 0xFF33FF88);
   draw_str(px, fb, bx + padx, y + pady, bsc, lbl, 0xFF33FF88);
   add_hit(h, bx, y, bw, bh, ACT_GATE_CONTINUE, 0);

   /* disclaimer, dim, at the foot of the screen */
   static const char *disc[] = {
       "Not a medical device, and",
       "not affiliated with Dexcom.",
       "For awareness only, not for",
       "treatment or hypoglycemia",
       "decisions.",
   };
   y += bh + (3 * lh);
   for (int i = 0; i < (int)(sizeof disc / sizeof disc[0]); i++) {
      draw_str(px, fb, x, y, sc, disc[i], 0xFF777777);
      y += lh;
   }
}

void ui_render(struct ANativeWindow_Buffer *fb, const struct screen *m,
               struct hits *h)
{
   h->n = 0;
   clear_fb(fb, 0xFF181818);
   switch (m->scr) {
      case SCR_SETTINGS: render_settings(fb, m, h); break;
      case SCR_KEYPAD: render_keypad(fb, m, h); break;
      case SCR_DEVLIST: render_devlist(fb, m, h); break;
      case SCR_GATE: render_gate(fb, h); break;
      case SCR_MAIN: render_main(fb, m, h); break;
   }
}

struct action ui_hit(const struct hits *h, int x, int y)
{
   /* last box wins, matching draw order (later-drawn overlays are on top) */
   for (int i = h->n - 1; i >= 0; i--) {
      int bx = h->box[i].x;
      int by = h->box[i].y;
      if (x >= bx && x < bx + h->box[i].w && y >= by && y < by + h->box[i].h)
         return (struct action){h->box[i].kind, h->box[i].arg};
   }
   return (struct action){ACT_NONE, 0};
}
