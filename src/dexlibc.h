// SPDX-License-Identifier: GPL-3.0
// dexlibc.h --- Minimal libc declarations (host + freestanding)
// Copyright 2026 Jakob Kastelic

/* Minimal libc declarations, so the crypto/driver sources compile both on the
 * host (against glibc) and in the freestanding Android target build (no bionic
 * headers -- the phone's libc binds these at runtime; see stub_c.c). The ABI
 * matches glibc/bionic, so no system headers are needed. The C-standard
 * functions come from the freestanding shims (string.h/stdlib.h/stdio.h); the
 * POSIX file I/O below has no standard header and is declared here directly. */
#ifndef DEXLIBC_H
#define DEXLIBC_H

#include <stddef.h>
#include <stdint.h> /* freestanding: provided by the compiler */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* POSIX file / RNG source */
int open(const char *path, int flags, ...);
long read(int fd, void *buf, size_t n);
long write(int fd, const void *buf, size_t n);
int close(int fd);
int unlink(const char *path);
int rename(const char *from, const char *to);
int ftruncate(int fd, long len);
int sched_yield(void);
long lseek(int fd, long off, int whence);
int gettid(void); /* kernel thread id (tell the main looper from BLE threads) */
#ifndef O_RDONLY
#define O_RDONLY 0U
#endif
#ifndef O_WRONLY
#define O_WRONLY 1U
#define O_CREAT  0100U
#define O_TRUNC  01000U
#endif
#ifndef O_APPEND
#define O_APPEND 02000U
#endif

#endif
