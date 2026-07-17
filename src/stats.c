// SPDX-License-Identifier: GPL-3.0
// stats.c --- Rolling glucose stats (time-in-range / average)
// Copyright 2026 Jakob Kastelic

/* Rolling stats via hourly buckets: O(1) per reading, O(days*24) to read a
 * window, ~35 KB for 90 days at 1-hour resolution. A TRUE rolling "last N days"
 * window (not calendar-aligned), so every column lines up with the plot. The
 * bucket ring is private to this module; the UI only sees stat_window(). */
#include "stats.h"
#include "dexlibc.h"
#include "util.h"
#include <stdio.h> /* SEEK_SET / SEEK_END */
#include <stdlib.h>

#define STAT_HOURS 2200 /* 1h buckets, ~91.6 days (~35 KB) */

struct hourbucket {
   int hour, count, in_range, sum;
};
static struct hourbucket
    g_hours[STAT_HOURS];   /* ring keyed by hour % STAT_HOURS */
static long g_stat_oldest; /* oldest reading time fed in */

void stat_add(long t, int glu)
{
   int hour = (int)(t / 3600);
   int idx  = hour % STAT_HOURS;
   if (g_hours[idx].hour != hour) { /* recycle a slot >= STAT_HOURS old */
      g_hours[idx].hour     = hour;
      g_hours[idx].count    = 0;
      g_hours[idx].in_range = 0;
      g_hours[idx].sum      = 0;
   }
   g_hours[idx].count++;
   g_hours[idx].sum += glu;
   if (glu >= 70 && glu <= 180)
      g_hours[idx].in_range++;
   if (!g_stat_oldest || t < g_stat_oldest)
      g_stat_oldest = t;
}

int stat_window(int days, int *tir, int *avg)
{
   long now = realtime_s();
   if (!g_stat_oldest || now - g_stat_oldest < ((long)days * 86400) - 3600)
      return 0;
   int nowh  = (int)(now / 3600);
   int hours = days * 24;
   long cnt  = 0;
   long inr  = 0;
   long sum  = 0;
   for (int h = 0; h < hours; h++) {
      int hour = nowh - h;
      int idx  = hour % STAT_HOURS;
      if (g_hours[idx].hour == hour) {
         cnt += g_hours[idx].count;
         inr += g_hours[idx].in_range;
         sum += g_hours[idx].sum;
      }
   }
   if (cnt == 0)
      return 0;
   *tir = (int)(100 * inr / cnt);
   *avg = (int)(sum / cnt);
   return 1;
}

/* Advance p past the current line (to the char after the next '\n', or to the
 * terminating NUL). Pure pointer walk (local copy; store.c has its own). */
static char *skip_line(char *p)
{
   while (*p && *p != '\n')
      p++;
   if (*p == '\n')
      p++;
   return p;
}

/* Seed the stats from the last ~90 days of the log -- a bounded tail read, so
 * it stays O(90 days) regardless of how large the file has grown. */
void stat_load(const char *readings_path)
{
   int fd = open(readings_path, O_RDONLY, 0);
   if (fd < 0)
      return;
   long sz   = lseek(fd, 0, SEEK_END);
   long want = 1024L * 1024; /* ~90 days of rows */
   long off  = sz > want ? sz - want : 0;
   lseek(fd, off, SEEK_SET);
   char *buf = malloc((unsigned long)want + 1);
   if (!buf) {
      close(fd);
      return;
   }
   long n = read(fd, buf, want);
   close(fd);
   if (n <= 0) {
      free(buf);
      return;
   }
   buf[n]  = 0;
   char *p = buf;
   if (off > 0)
      p = skip_line(p);
   while (*p) {
      long t  = 0;
      int glu = 0;
      char *q = p;
      while (*q >= '0' && *q <= '9')
         t = (t * 10) + (*q++ - '0');
      if (*q == ',')
         q++;
      while (*q >= '0' && *q <= '9')
         glu = (glu * 10) + (*q++ - '0');
      if (t > 0 && glu > 0)
         stat_add(t, glu);
      p = skip_line(p);
   }
   free(buf);
}
