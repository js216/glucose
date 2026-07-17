// SPDX-License-Identifier: GPL-3.0
// plot.c --- Glucose plot pixel rendering
// Copyright 2026 Jakob Kastelic

/* plot.c -- see plot.h. Pure pixel rendering; no dependencies beyond stdint. */
#include "plot.h"
#include <stdint.h>

/* Set one pixel, clipped to the framebuffer. */
static void put(uint32_t *fb, int stride, int fbw, int fbh, int x, int y,
                uint32_t c)
{
   if ((unsigned)x < (unsigned)fbw && (unsigned)y < (unsigned)fbh)
      fb[(y * stride) + x] = c;
}

/* Filled square dot, half-width r, centred on (cx,cy). */
static void dot(uint32_t *fb, int stride, int fbw, int fbh, int cx, int cy,
                int r, uint32_t c)
{
   for (int dy = -r; dy <= r; dy++)
      for (int dx = -r; dx <= r; dx++)
         put(fb, stride, fbw, fbh, cx + dx, cy + dy, c);
}

/* Top of the vertical scale in mg/dL; runtime-adjustable (PLOT MAX setting). */
static int g_glu_max = PLOT_GLU_MAX;

void plot_set_max(int mgdl)
{
   if (mgdl >= 100 && mgdl <= 400)
      g_glu_max = mgdl;
}

/* Map a glucose value to a pixel row inside the frame (clamped to the scale).
 */
static int glu_to_y(int glu, int y, int h)
{
   if (glu < PLOT_GLU_MIN)
      glu = PLOT_GLU_MIN;
   if (glu > g_glu_max)
      glu = g_glu_max;
   return y + h - 2 -
          (int)((long)(h - 3) * (glu - PLOT_GLU_MIN) /
                (g_glu_max - PLOT_GLU_MIN));
}

/* X pixel for a reading `dt` seconds before now (newest at the right edge). */
static int t_to_x(long dt, int x, int w, long span)
{
   if (dt < 0)
      dt = 0;
   return x + w - 2 - (int)((long)(w - 3) * dt / span);
}

void plot_render(uint32_t *fb, int stride, int fbw, int fbh, int x, int y,
                 int w, int h, const struct plot_pt *pts, int npts, long now,
                 int hours, int radius, uint32_t (*color)(int glu), int hi_idx,
                 uint32_t hi_color)
{
   const uint32_t frame = 0xFF555555; /* 50/max reference lines + sides   */
   const uint32_t band  = 0xFF262626; /* very slight dark-gray shade 70-180 */
   const uint32_t vgrid = 0xFF2E2E2E; /* faint vertical gridlines         */
   const uint32_t vtick = 0xFF666666; /* brighter x-tick at the bottom    */
   long span            = (long)hours * 3600;
   if (radius < 1)
      radius = 1;
   if (span <= 0 || w < 4 || h < 4)
      return;

   int y50   = glu_to_y(50, y, h);
   int y_top = glu_to_y(g_glu_max, y, h);

   /* faint shade behind the 70-180 in-range band */
   int y_hi = glu_to_y(180, y, h);
   int y_lo = glu_to_y(70, y, h);
   for (int j = y_hi; j <= y_lo; j++)
      for (int i = 1; i < w - 1; i++)
         put(fb, stride, fbw, fbh, x + i, j, band);

   /* vertical gridlines + bottom x-ticks: hourly for hour-scale windows
    * (3 lines for 3H ... 24 for 24H), daily once the span exceeds a day
    * (3 lines for 3D, 7 for 7D) so multi-day plots aren't a picket fence */
   long gstep = (span <= 24L * 3600) ? 3600 : 24L * 3600;
   for (long ts = gstep; ts <= span; ts += gstep) {
      int gx = t_to_x(ts, x, w, span);
      for (int j = y_top + 1; j < y50; j++)
         put(fb, stride, fbw, fbh, gx, j, vgrid);
      for (int j = y50 - (2 * radius); j <= y50; j++)
         put(fb, stride, fbw, fbh, gx, j, vtick);
   }

   /* gray reference lines at the 50 and max bounds, plus vertical sides */
   for (int i = 0; i < w; i++) {
      put(fb, stride, fbw, fbh, x + i, y50, frame);
      put(fb, stride, fbw, fbh, x + i, y_top, frame);
   }
   for (int j = y_top; j <= y50; j++) {
      put(fb, stride, fbw, fbh, x, j, frame);
      put(fb, stride, fbw, fbh, x + w - 1, j, frame);
   }

   /* one dot per in-window reading; the highlighted one drawn last, on top */
   int hx = -1;
   int hy = -1;
   for (int i = 0; i < npts; i++) {
      long dt = now - pts[i].t;
      if (dt < 0)
         dt = 0;
      if (dt > span)
         continue;
      int px = t_to_x(dt, x, w, span);
      int py = glu_to_y(pts[i].glu, y, h);
      if (i == hi_idx) {
         hx = px;
         hy = py;
         continue;
      }
      dot(fb, stride, fbw, fbh, px, py, radius, color(pts[i].glu));
   }
   if (hx >= 0) {
      /* white vertical marker; only the dot itself is highlighted in colour */
      for (int j = y_top + 1; j < y50; j++)
         put(fb, stride, fbw, fbh, hx, j, 0xFFFFFFFF);
      dot(fb, stride, fbw, fbh, hx, hy, radius + 2, hi_color);
   }
}

int plot_point_xy(int x, int y, int w, int h, struct plot_pt p, long now,
                  int hours, int *ox, int *oy)
{
   long span = (long)hours * 3600;
   if (span <= 0)
      return 0;
   long dt = now - p.t;
   if (dt < 0)
      dt = 0;
   if (dt > span)
      return 0;
   *ox = t_to_x(dt, x, w, span);
   *oy = glu_to_y(p.glu, y, h);
   return 1;
}

int plot_hit(int x, int y, int w, int h, const struct plot_pt *pts, int npts,
             long now, int hours, int tx, int ty)
{
   /* Select purely by time (horizontal position) so dragging steps smoothly
    * through consecutive points; the finger's vertical position is ignored. */
   (void)y;
   (void)h;
   (void)ty;
   long span = (long)hours * 3600;
   if (span <= 0)
      return -1;
   int best   = -1;
   long bestd = 0;
   for (int i = 0; i < npts; i++) {
      long dt = now - pts[i].t;
      if (dt < 0)
         dt = 0;
      if (dt > span)
         continue;
      long ddx = t_to_x(dt, x, w, span) - tx;
      if (ddx < 0)
         ddx = -ddx;
      if (best < 0 || ddx < bestd) {
         best  = i;
         bestd = ddx;
      }
   }
   return best;
}
