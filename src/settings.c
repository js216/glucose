// SPDX-License-Identifier: GPL-3.0
// settings.c --- Persisted config: alarms, display prefs, device info, code
// Copyright 2026 Jakob Kastelic

/* Small config files, one concern each: device-info strings, alarm thresholds,
 * display/settings-menu prefs, and the pairing code. The UI (main.c) owns when
 * to save/load; this module owns the state and the on-disk format. */
#include "settings.h"
#include "dexlibc.h"
#include "plot.h"
#include "util.h"
#include <stdio.h> /* snprintf */

char g_model[24], g_fw[24], g_mfr[24];
int g_alarm_low = 110, g_alarm_high = 300;
int g_sound_on = 1, g_vib_on = 1;
int g_orient;
int g_units;
int g_disc;
int g_plot_max      = PLOT_GLU_MAX;
char g_code_str[16] = "9973"; /* Stelo applicator default (rebuild to change) */
char g_info_path[256], g_alarm_path[256], g_settings_path[256],
    g_code_path[256];

void info_save(void)
{
   int fd = open(g_info_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
   if (fd < 0)
      return;
   char b[96];
   int n = snprintf(b, sizeof b, "%s\n%s\n%s\n", g_model, g_fw, g_mfr);
   n     = clampn(n, sizeof b);
   if (write(fd, b, n) != n) {
   }
   close(fd);
}

void info_load(void)
{
   int fd = open(g_info_path, O_RDONLY, 0);
   if (fd < 0)
      return;
   char b[96];
   int n = (int)read(fd, b, sizeof b - 1);
   close(fd);
   if (n <= 0)
      return;
   b[n]         = 0;
   char *p      = b;
   char *dst[3] = {g_model, g_fw, g_mfr};
   for (int i = 0; i < 3 && p; i++) {
      char *nl = p;
      while (*nl && *nl != '\n')
         nl++;
      int len = (int)(nl - p);
      if (len > 22)
         len = 22;
      for (int j = 0; j < len; j++)
         dst[i][j] = p[j];
      dst[i][len] = 0;
      p           = *nl ? nl + 1 : 0;
   }
}

void alarm_save(void)
{
   int fd = open(g_alarm_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
   if (fd < 0)
      return;
   char b[32];
   int n = snprintf(b, sizeof b, "%d %d\n", g_alarm_low, g_alarm_high);
   n     = clampn(n, sizeof b);
   if (write(fd, b, n) != n) {
   }
   close(fd);
}

void alarm_load(void)
{
   int fd = open(g_alarm_path, O_RDONLY, 0);
   if (fd < 0)
      return;
   char b[32];
   int n = (int)read(fd, b, sizeof b - 1);
   close(fd);
   if (n <= 0)
      return;
   b[n]    = 0;
   int lo  = 0;
   int hi  = 0;
   char *q = b;
   while (*q >= '0' && *q <= '9')
      lo = (lo * 10) + (*q++ - '0');
   while (*q == ' ')
      q++;
   while (*q >= '0' && *q <= '9')
      hi = (hi * 10) + (*q++ - '0');
   if (lo > 0)
      g_alarm_low = lo;
   if (hi > 0)
      g_alarm_high = hi;
}

void settings_save(void)
{
   int fd = open(g_settings_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
   if (fd < 0)
      return;
   char b[64];
   int n = snprintf(b, sizeof b, "%d %d %d %d %d %d\n", g_sound_on, g_vib_on,
                    g_orient, g_units, g_disc, g_plot_max);
   n     = clampn(n, sizeof b);
   if (write(fd, b, n) != n) {
   }
   close(fd);
}

void settings_load(void)
{
   int fd = open(g_settings_path, O_RDONLY, 0);
   if (fd < 0)
      return;
   char b[64];
   int n = (int)read(fd, b, sizeof b - 1);
   close(fd);
   if (n <= 0)
      return;
   b[n]     = 0;
   int v[6] = {g_sound_on, g_vib_on, g_orient, g_units, g_disc, g_plot_max};
   char *q  = b;
   for (int i = 0; i < 6; i++) {
      while (*q == ' ')
         q++;
      if (*q < '0' || *q > '9')
         break;
      int x = 0;
      while (*q >= '0' && *q <= '9')
         x = (x * 10) + (*q++ - '0');
      v[i] = x;
   }
   g_sound_on = v[0];
   g_vib_on   = v[1];
   g_orient   = (int)((unsigned)v[2] & 3U);
   g_units    = v[3] ? 1 : 0;
   g_disc     = (v[4] >= 0 && v[4] < 4) ? v[4] : 0;
   g_plot_max = (v[5] >= 100 && v[5] <= 400) ? v[5] : PLOT_GLU_MAX;
   plot_set_max(g_plot_max);
}

void code_save(void)
{
   int fd = open(g_code_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
   if (fd < 0)
      return;
   int n = 0;
   while (g_code_str[n])
      n++;
   if (write(fd, g_code_str, n) != n) {
   }
   close(fd);
}

void code_load(void)
{
   int fd = open(g_code_path, O_RDONLY, 0);
   if (fd < 0)
      return;
   char b[16];
   int n = (int)read(fd, b, sizeof b - 1);
   close(fd);
   if (n <= 0)
      return;
   int k = 0;
   for (int i = 0; i < n && k < (int)sizeof g_code_str - 1; i++)
      if (b[i] >= '0' && b[i] <= '9')
         g_code_str[k++] = b[i];
   g_code_str[k] = 0;
}
