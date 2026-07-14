/* Portable SHA-256. */
#ifndef DEX_SHA256_H
#define DEX_SHA256_H
#include <stdint.h>
#include <stddef.h>
void sha256(const uint8_t *data, size_t len, uint8_t out[32]);
#endif
