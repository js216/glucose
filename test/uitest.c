// SPDX-License-Identifier: GPL-3.0
// uitest.c --- Offline UI harness: render the pure core to PPM + check hit-test
// Copyright 2026 Jakob Kastelic

/* The UI is a pure function of a `struct screen`, so it runs with no phone: fill
 * a model, call ui_render into a plain framebuffer, dump a PPM (into tmp/uitest/,
 * never the source tree), and assert that ui_hit maps a tap to the right action.
 * As each screen is ported this harness grows a case per screen. Built and run
 * by `make uitest`. */
#include "ui.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define W 720
#define H 360

static uint32_t *g_px;
static struct ANativeWindow_Buffer g_buf;

static void write_ppm(const char *path)
{
   FILE *f = fopen(path, "wb");
   if (!f) {
      perror(path);
      exit(1);
   }
   fprintf(f, "P6\n%d %d\n255\n", W, H);
   for (int i = 0; i < W * H; i++) {
      uint32_t c         = g_px[i];
      unsigned char b[3] = {(unsigned char)(c >> 16), (unsigned char)(c >> 8),
                            (unsigned char)c};
      fwrite(b, 1, 3, f);
   }
   fclose(f);
}

static long lit_pixels(uint32_t bg)
{
   long n = 0;
   for (int i = 0; i < W * H; i++)
      if (g_px[i] != bg)
         n++;
   return n;
}

int main(void)
{
   g_px  = malloc((size_t)W * H * 4);
   g_buf = (struct ANativeWindow_Buffer){
       .width = W, .height = H, .stride = W, .format = 1, .bits = g_px};

   /* newest-first history spanning a few hours so the plot has points */
   struct ui_point hist[4] = {
       {900, 148}, {800, 143}, {400, 155}, {100, 132}};
   struct ui_stat s = {.have = 1, .tir = 82, .avg = 149};
   struct screen m  = {
        .scr           = SCR_MAIN,
        .now           = 1000,
        .glu           = 148,
        .trend         = 2,
        .t             = 900,
        .rssi          = -72,
        .rssi_ok       = 1,
        .hist          = hist,
        .nhist         = 4,
        .scrub         = -1,
        .plot_hours    = 24,
        .plot_max      = 300,
        .bonded        = 1,
        .have_reading  = 1,
        .predicted     = 152,
        .sequence      = 41,
        .session_seconds = 3L * 86400,
        .stored        = 812,
        .units         = 0,
        .alarm_low     = 100,
        .alarm_high    = 300,
        .status        = "CONNECTED",
        .stat          = {s, s, s, s, s},
   };

   int fail = 0;

   /* --- portrait main screen --- */
   struct hits h;
   ui_render(&g_buf, &m, &h);
   write_ppm("tmp/uitest/main.ppm");
   long lit = lit_pixels(0xFF181818);
   printf("uitest: main.ppm %dx%d, %ld lit pixels, %d hit targets\n", W, H, lit,
          h.n);
   if (lit < 500) {
      printf("  FAIL: main screen rendered almost nothing\n");
      fail = 1;
   }
   /* a tap on the big number (top band) must open the settings menu */
   if (ui_hit(&h, W / 4, H / 12).kind != ACT_OPEN_SETTINGS) {
      printf("  FAIL: big-number tap should be ACT_OPEN_SETTINGS\n");
      fail = 1;
   }
   /* the tab row, the plot, and the alarm buttons must each be reachable */
   int saw_tab = 0, saw_scrub = 0, saw_low = 0, saw_high = 0;
   for (int i = 0; i < h.n; i++) {
      int k = h.box[i].kind;
      saw_tab |= (k == ACT_PLOT_TAB);
      saw_scrub |= (k == ACT_SCRUB);
      saw_low |= (k == ACT_ALARM_LOW);
      saw_high |= (k == ACT_ALARM_HIGH);
   }
   if (!(saw_tab && saw_scrub && saw_low && saw_high)) {
      printf("  FAIL: missing targets tab=%d scrub=%d low=%d high=%d\n", saw_tab,
             saw_scrub, saw_low, saw_high);
      fail = 1;
   }
   /* a tap in dead space (far right edge, top) must do nothing */
   if (ui_hit(&h, W - 2, 1).kind != ACT_NONE) {
      printf("  FAIL: empty-area tap should be ACT_NONE\n");
      fail = 1;
   }

   /* --- no-reading screen (scan status + sensor list) --- */
   struct ui_dev devs[2] = {{"DX01AB", "F8:DA:11:22:33:44", -61},
                            {"DX01CD", "C1:22:33:44:55:66", -80}};
   struct screen scan    = {.scr       = SCR_MAIN,
                            .glu       = -1,
                            .status    = "SCANNING",
                            .adv_total = 137,
                            .devs      = devs,
                            .ndev      = 2};
   ui_render(&g_buf, &scan, &h);
   write_ppm("tmp/uitest/scan.ppm");
   if (lit_pixels(0xFF181818) < 200) {
      printf("  FAIL: no-reading screen rendered almost nothing\n");
      fail = 1;
   }

   /* --- settings screen (rows carry menu_action codes via ACT_MENU) --- */
   struct screen set = m;
   set.scr           = SCR_SETTINGS;
   set.plot_max      = 300;
   set.sound_on      = 1;
   set.perm[0] = set.perm[1] = set.perm[2] = 1;
   set.batt_ok       = 1;
   set.standby_bucket = 10;
   set.code          = "1234";
   set.mac           = "F8:DA:11:22:33:44";
   set.model         = "SW11163";
   set.fw            = "1.6.5.15";
   ui_render(&g_buf, &set, &h);
   write_ppm("tmp/uitest/settings.ppm");
   if (lit_pixels(0xFF181818) < 500) {
      printf("  FAIL: settings screen rendered almost nothing\n");
      fail = 1;
   }
   /* the title band is the close target (menu_action 99) */
   struct action ca = ui_hit(&h, W / 2, H / 6);
   if (ca.kind != ACT_MENU) {
      printf("  FAIL: settings title tap should be ACT_MENU (got %d)\n",
             ca.kind);
      fail = 1;
   }
   /* at least one actionable row must be recorded */
   int saw_menu = 0;
   for (int i = 0; i < h.n; i++)
      saw_menu |= (h.box[i].kind == ACT_MENU);
   if (!saw_menu) {
      printf("  FAIL: settings recorded no ACT_MENU targets\n");
      fail = 1;
   }

   /* --- pairing keypad (digits carry menu_action codes) --- */
   struct screen kp = {.scr = SCR_KEYPAD, .kp_mode = 0, .entry = "12"};
   ui_render(&g_buf, &kp, &h);
   write_ppm("tmp/uitest/keypad.ppm");
   int keys = 0;
   for (int i = 0; i < h.n; i++)
      keys += (h.box[i].kind == ACT_MENU);
   if (keys < 12) { /* 12 keys + close band */
      printf("  FAIL: keypad recorded %d ACT_MENU targets, want >=12\n", keys);
      fail = 1;
   }

   /* --- device list (a pick per scanned sensor) --- */
   struct screen dl = {
       .scr = SCR_DEVLIST, .devs = devs, .ndev = 2};
   ui_render(&g_buf, &dl, &h);
   write_ppm("tmp/uitest/devlist.ppm");
   int picks = 0;
   for (int i = 0; i < h.n; i++)
      picks += (h.box[i].kind == ACT_MENU && h.box[i].arg >= 200);
   if (picks != 2) {
      printf("  FAIL: device list recorded %d picks, want 2\n", picks);
      fail = 1;
   }

   /* --- first-run gate screen (CONTINUE button) --- */
   struct screen gate = {.scr = SCR_GATE};
   ui_render(&g_buf, &gate, &h);
   write_ppm("tmp/uitest/gate.ppm");
   int saw_cont = 0;
   for (int i = 0; i < h.n; i++)
      saw_cont |= (h.box[i].kind == ACT_GATE_CONTINUE);
   if (!saw_cont) {
      printf("  FAIL: gate recorded no ACT_GATE_CONTINUE target\n");
      fail = 1;
   }

   printf("uitest: %s\n", fail ? "FAIL" : "OK");
   return fail;
}
