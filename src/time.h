// SPDX-License-Identifier: GPL-3.0
// time.h --- Freestanding <time.h> shim
// Copyright 2026 Jakob Kastelic

/* Minimal freestanding <time.h> shim (see string.h for the rationale). */
#ifndef STEALO_TIME_H
#define STEALO_TIME_H

struct timespec {
   long tv_sec, tv_nsec;
};

int clock_gettime(int clk, struct timespec *ts);

#endif
