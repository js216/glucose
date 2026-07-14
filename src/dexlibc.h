/* Minimal libc declarations, so the crypto/driver sources compile both on the
 * host (against glibc) and in the freestanding Android target build (no bionic
 * headers — the phone's libc binds these at runtime; see stub_c.c). The ABI
 * matches glibc/bionic, so no system headers are needed. */
#ifndef DEXLIBC_H
#define DEXLIBC_H

#include <stdint.h>   /* freestanding: provided by the compiler */
#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int   memcmp(const void *a, const void *b, size_t n);
int   strcmp(const char *a, const char *b);
int   snprintf(char *s, size_t n, const char *fmt, ...);
void *malloc(size_t n);
void *calloc(size_t nmemb, size_t size);
void  free(void *p);

/* file / RNG source */
int   open(const char *path, int flags, ...);
long  read(int fd, void *buf, size_t n);
long  write(int fd, const void *buf, size_t n);
int   close(int fd);
int   unlink(const char *path);
#ifndef O_RDONLY
#define O_RDONLY 0
#endif
#ifndef O_WRONLY
#define O_WRONLY 1
#define O_CREAT  0100
#define O_TRUNC  01000
#endif

#endif
