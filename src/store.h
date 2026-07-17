// SPDX-License-Identifier: GPL-3.0
// store.h --- Reading history + append-only persistent log (data model)
// Copyright 2026 Jakob Kastelic

#ifndef STEALO_STORE_H
#define STEALO_STORE_H

#define NHIST 2100 /* master reading history (~7d at 5-min spacing) */

/* One reading in the display history. */
struct reading {
   int glu, trend;
   long t; /* epoch seconds */
};

/* Reading data model, owned by store.c and read by the UI. History is kept
 * newest-first and deduped; g_cur_* is the latest reading snapshot. */
extern struct reading g_hist[NHIST];
extern int g_nhist;
extern int g_cur_glu, g_cur_trend;
extern long g_cur_time; /* epoch seconds of the latest reading */
extern int g_cur_rssi, g_cur_rssi_ok;
extern int g_stored;           /* total rows in the log */
extern char g_store_path[256]; /* path to the readings CSV */

/* Insert a reading (out-of-order safe for backfill); returns 1 if genuinely
 * new, 0 if it deduped within 150 s or was older than everything kept. */
int hist_insert(long t, int glu, int trend);
/* Append one CSV row "epoch,glucose,trend10,rssi,recv_lag". */
void store_append(long t, int glu, int trend, int rssi, int has_rssi);
/* Load the tail of the CSV into g_hist (most-recent NHIST rows) + g_cur_*. */
void store_load(void);
/* Count the rows currently in the log (one pass). */
int store_count(void);

#endif
