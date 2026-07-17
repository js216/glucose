// SPDX-License-Identifier: GPL-3.0
// store.c --- Reading history + append-only persistent log (data model)
// Copyright 2026 Jakob Kastelic

/* The reading data model: an in-memory newest-first history (deduped, out-of-
 * order safe for backfill) plus an append-only CSV log. The UI reads this via
 * store.h; only store_append / hist_insert mutate g_hist, always under the
 * caller's hist_lock (see main.c) so a main-thread draw sees consistent data.
 */
#include "store.h"
#include "dexlibc.h"
#include "util.h"
#include <stdio.h> /* snprintf, SEEK_SET / SEEK_END */

struct reading g_hist[NHIST];
int g_nhist;
int g_cur_glu = -1, g_cur_trend;
long g_cur_time;
int g_cur_rssi, g_cur_rssi_ok;
int g_stored;
char g_store_path[256];

int hist_insert(long t, int glu, int trend)
{
   for (int i = 0; i < g_nhist; i++) {
      long d = t - g_hist[i].t;
      if (d < 0)
         d = -d;
      if (d < 150) { /* same 5-min sample */
         g_hist[i].glu   = glu;
         g_hist[i].trend = trend;
         return 0;
      }
   }
   if (g_nhist == NHIST && t <= g_hist[NHIST - 1].t)
      return 0; /* older than all kept */
   if (g_nhist < NHIST)
      g_nhist++;
   int i = g_nhist - 1;
   while (i > 0 && g_hist[i - 1].t < t) {
      g_hist[i] = g_hist[i - 1];
      i--;
   }
   g_hist[i].t     = t;
   g_hist[i].glu   = glu;
   g_hist[i].trend = trend;
   return 1;
}

void store_append(long t, int glu, int trend, int rssi, int has_rssi)
{
   int fd = open(g_store_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
   if (fd < 0)
      return;
   long lag = realtime_s() - t; /* arrival delay from the datapoint timestamp */
   char line[64];
   int n = has_rssi ? snprintf(line, sizeof line, "%ld,%d,%d,%d,%ld\n", t, glu,
                               trend, rssi, lag)
                    : snprintf(line, sizeof line, "%ld,%d,%d,,%ld\n", t, glu,
                               trend, lag);
   n     = clampn(n, sizeof line);
   if (write(fd, line, n) != n) { /* ignore */
   }
   close(fd);
   g_stored++;
}

int store_count(void)
{
   int fd = open(g_store_path, O_RDONLY, 0);
   if (fd < 0)
      return 0;
   char b[4096];
   long n = 0;
   int c  = 0;
   while ((n = read(fd, b, sizeof b)) > 0)
      for (long i = 0; i < n; i++)
         if (b[i] == '\n')
            c++;
   close(fd);
   return c;
}

void store_load(void)
{
   int fd = open(g_store_path, O_RDONLY, 0);
   if (fd < 0)
      return;
   long sz = lseek(fd, 0, SEEK_END);
   /* keep ~7 days on screen (2016 rows x ~25 B); read a generous 64 KB tail */
   long off = sz > 65536 ? sz - 65536 : 0;
   lseek(fd, off, SEEK_SET);
   static char buf[65537];
   long n = read(fd, buf, 65536);
   close(fd);
   if (n <= 0)
      return;
   buf[n]        = 0;
   char *p       = buf;
   long best_t   = 0;
   int best_rssi = 0;
   int best_ok   = 0; /* rssi of the newest row */
   if (off > 0) {
      while (*p && *p != '\n')
         p++;
      if (*p == '\n')
         p++;
   } /* skip partial line */
   while (*p) {
      char *e = p;
      while (*e && *e != '\n')
         e++;
      long t   = 0;
      long glu = 0;
      long tr  = 0;
      int neg  = 0;
      char *q  = p;
      while (q < e && *q >= '0' && *q <= '9')
         t = (t * 10) + (*q++ - '0');
      if (q < e && *q == ',')
         q++;
      while (q < e && *q >= '0' && *q <= '9')
         glu = (glu * 10) + (*q++ - '0');
      if (q < e && *q == ',')
         q++;
      if (q < e && *q == '-') {
         neg = 1;
         q++;
      }
      while (q < e && *q >= '0' && *q <= '9')
         tr = (tr * 10) + (*q++ - '0');
      if (neg)
         tr = -tr;
      int rneg    = 0;
      int rssi    = 0;
      int rssi_ok = 0; /* trailing rssi field: "-72" or empty */
      if (q < e && *q == ',') {
         q++;
         if (q < e && *q == '-') {
            rneg = 1;
            q++;
         }
         while (q < e && *q >= '0' && *q <= '9') {
            rssi    = (rssi * 10) + (*q++ - '0');
            rssi_ok = 1;
         }
         if (rneg)
            rssi = -rssi;
      }
      if (t > 0) {
         hist_insert(t, (int)glu, (int)tr);
         if (t >= best_t) {
            best_t    = t;
            best_rssi = rssi;
            best_ok   = rssi_ok;
         }
      }
      p = (*e == '\n') ? e + 1 : e;
   }
   if (g_nhist > 0) {
      g_cur_glu   = g_hist[0].glu;
      g_cur_trend = g_hist[0].trend;
      g_cur_time  = g_hist[0].t;
   }
   if (best_ok) {
      g_cur_rssi    = best_rssi;
      g_cur_rssi_ok = 1;
   }
}
