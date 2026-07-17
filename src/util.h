// SPDX-License-Identifier: GPL-3.0
// util.h --- Small dependency-free time/format helpers
// Copyright 2026 Jakob Kastelic

#ifndef STEALO_UTIL_H
#define STEALO_UTIL_H

long long now_ms(void); /* CLOCK_MONOTONIC milliseconds */
long realtime_s(void);  /* CLOCK_REALTIME seconds (epoch) */
int clampn(int n,
           int cap); /* clamp a snprintf length to [0, cap-1] for write() */

#endif
