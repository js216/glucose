// SPDX-License-Identifier: GPL-3.0
// plot.h --- Glucose plot rendering (API)
// Copyright 2026 Jakob Kastelic

/* plot.h -- a crude fixed-scale glucose time plot.
 *
 * The module is deliberately self-contained: plot_render() only writes RGBA
 * pixels into a caller-supplied framebuffer rectangle and knows nothing about
 * the rest of the app -- no globals, no fonts, no Android types. The caller
 * owns layout, the colour palette (passed as a function), and interaction. */
#ifndef PLOT_H
#define PLOT_H
#include <stdint.h>

/* One reading: epoch-second timestamp and glucose in mg/dL. */
struct plot_pt {
   long t;
   int glu;
};

/* Vertical scale runs PLOT_GLU_MIN..(runtime max, default PLOT_GLU_MAX) mg/dL.
 */
#define PLOT_GLU_MIN 50
#define PLOT_GLU_MAX 300

/* Set the top of the vertical scale in mg/dL (clamped to 100..400). */
void plot_set_max(int mgdl);

/* Render the readings in `pts` (any order, newest-first is fine) whose
 * timestamps fall within the last `hours` before `now` into the framebuffer
 * rectangle (x,y)-(x+w,y+h). Draws a frame and 100/200 gridlines, then one
 * dot per in-window reading. `color(glu)` yields each dot's colour, so the
 * palette stays with the caller. `fb` is RGBA_8888 with `stride` pixels per
 * row and total size `fbw`x`fbh`; writes are clipped to those bounds. */
/* As above; the point at index `hi_idx` (if in-window) is drawn larger in
 * `hi_color` -- pass hi_idx < 0 for no highlight. */
void plot_render(uint32_t *fb, int stride, int fbw, int fbh, int x, int y,
                 int w, int h, const struct plot_pt *pts, int npts, long now,
                 int hours, int radius, uint32_t (*color)(int glu), int hi_idx,
                 uint32_t hi_color);

/* Return the index into pts of the in-window point nearest to pixel (tx,ty),
 * using the same mapping as plot_render, or -1 if there are no in-window
 * points. Lets the caller resolve a touch to a datapoint. */
int plot_hit(int x, int y, int w, int h, const struct plot_pt *pts, int npts,
             long now, int hours, int tx, int ty);

/* Pixel centre of point `p` under the same mapping as plot_render. Returns 1
 * and fills *ox,*oy if p is in-window; returns 0 otherwise. */
int plot_point_xy(int x, int y, int w, int h, struct plot_pt p, long now,
                  int hours, int *ox, int *oy);

#endif
