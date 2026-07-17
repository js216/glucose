// SPDX-License-Identifier: GPL-3.0
// sha256.h --- Portable SHA-256 (API)
// Copyright 2026 Jakob Kastelic

/* Portable SHA-256. */
#ifndef DEX_SHA256_H
#define DEX_SHA256_H
#include <stddef.h>
#include <stdint.h>
void sha256(const uint8_t *data, size_t len, uint8_t out[32]);
#endif
