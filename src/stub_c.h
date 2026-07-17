// SPDX-License-Identifier: GPL-3.0
// stub_c.h --- libc stub declarations
// Copyright 2026 Jakob Kastelic

/* Declarations for the libc symbols defined in stub_c.c. These are the ABI the
 * stub libc.so exports; the phone's real bionic binds them at runtime.
 * Declaring them here (rather than leaving the definitions with implicit
 * external linkage) documents that they are intentionally exported
 * cross-module. The C-standard functions come from the freestanding shims; the
 * POSIX symbols below have no standard header and are declared here. */
#ifndef STUB_C_H
#define STUB_C_H

#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int open(const char *p, int f, ...);
long read(int fd, void *b, size_t n);
long write(int fd, const void *b, size_t n);
int close(int fd);
int unlink(const char *p);
long lseek(int fd, long off, int w);
int timerfd_create(int c, int f);
int timerfd_settime(int fd, int f, const void *n, void *o);

#endif
