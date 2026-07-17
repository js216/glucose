// SPDX-License-Identifier: GPL-3.0
// util.c --- Small dependency-free time/format helpers
// Copyright 2026 Jakob Kastelic

#include "util.h"
#include <time.h>

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

long long now_ms(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (ts.tv_sec * 1000LL) + (ts.tv_nsec / 1000000);
}

long realtime_s(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_REALTIME, &ts);
   return ts.tv_sec;
}

/* snprintf returns the would-be length, which can exceed the buffer on
 * truncation; clamp so write() emits only bytes actually in the buffer. */
int clampn(int n, int cap)
{
   if (n < 0)
      return 0;
   return n >= cap ? cap - 1 : n;
}
