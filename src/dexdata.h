/* Dexcom G7 / Stelo glucose decoding — public C API. See NOTES.md. */
#ifndef DEXDATA_H
#define DEXDATA_H

#include <stdint.h>
#include <stddef.h>

/* One backfill/EGV record (9 bytes on the wire). */
typedef struct {
    uint32_t timestamp;    /* seconds since session start */
    uint16_t glucose;      /* mg/dL */
    int      display_only; /* high nibble of the glucose field */
    uint8_t  status[3];    /* status / trend, not fully decoded */
} dex_record;

/* Current-EGV control response (opcode 0x4e). */
typedef struct {
    uint8_t  status_raw;
    uint32_t clock;        /* seconds since session start */
    uint16_t sequence;
    uint16_t age;          /* seconds since this reading was taken */
    uint16_t glucose;      /* mg/dL */
    int      display_only;
    uint8_t  state;        /* calibration/session state */
    int8_t   trend;        /* trend*10 mg/dL/min; 127 = unavailable */
    uint16_t predicted;    /* predicted glucose, 10-bit */
} dex_egv;

int dexdata_record(const uint8_t rec[9], dex_record *out);
int dexdata_records(const uint8_t *buf, size_t len, dex_record *out, int max);
int dexdata_egv(const uint8_t *p, size_t len, dex_egv *out);

#endif
