// SPDX-License-Identifier: GPL-3.0
// stats.h --- Rolling glucose stats (time-in-range / average)
// Copyright 2026 Jakob Kastelic

#ifndef STEALO_STATS_H
#define STEALO_STATS_H

/* O(1) per reading via hourly buckets; O(days*24) to read a rolling window. */
void stat_add(long t, int glu);
/* time-in-range (%) and average over the rolling last `days`; returns 0
 * (=>"--") until the data reaches back far enough to cover the whole window. */
int stat_window(int days, int *tir, int *avg);
/* seed the buckets from the tail of the readings CSV at `readings_path`. */
void stat_load(const char *readings_path);

#endif
