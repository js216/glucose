// SPDX-License-Identifier: GPL-3.0
// sensors.h --- Permanent sensor registry: provenance + per-sensor preferences
// Copyright 2026 Jakob Kastelic

/* Every datapoint must name its origin exactly, forever -- decades after the
 * sensor itself is landfill. That splits into two very different kinds of
 * state, so they live in two different files:
 *
 *   sensors.csv  IMMUTABLE provenance, append-only, never rewritten. One row
 *                per minted id: what the device was, which firmware, which
 *                session. A reading's source_id resolves through this table
 *                and the answer is the same in ten years as it is today.
 *   slots.csv    MUTABLE presentation state, rewritten freely: the user's
 *                label, plot marker, colour, and which sensor owns the big
 *                number. Losing this file costs preferences, never data.
 *
 * The id is a monotonically increasing integer and is NEVER reused, not even
 * after a sensor is forgotten -- readings.csv references it permanently, so
 * recycling an id would silently reattribute old data to a different physical
 * device. Forgetting a sensor drops its slot; the provenance row stays.
 */
#ifndef PANCRA_SENSORS_H
#define PANCRA_SENSORS_H

#define MAX_SLOTS 5 /* user-visible sensors; the UI shrinks this to fit */
/* In-memory tail of sensors.csv. Only sensors with points in the plot window
 * need resolving, so a bounded cache of recent provenance rows is enough. */
#define MAX_SENSOR_RECS 64

/* What protocol a sensor speaks. The type -- not the kind -- decides which
 * driver runs, because Stelo and G7 share a GATT layout but differ in policy.
 */
enum sensor_type {
   SENSOR_NONE = 0,
   SENSOR_STELO,    /* Dexcom Stelo, advertises DX01 */
   SENSOR_G7,       /* Dexcom G7, advertises DXCM, rotating RPA */
   SENSOR_ONETOUCH, /* LifeScan OneTouch BLE meter */
   SENSOR_NTYPES
};

/* How the data behaves, which decides plotting and whether it can be primary.
 */
enum sensor_kind {
   KIND_CGM = 0, /* continuous, 5-min, trend arrow, drawn as a line */
   KIND_BGM      /* sparse fingersticks, drawn as discrete markers */
};

/* MARK_HIDE is not a plot.c shape: it means "do not draw this device's points"
 * and is handled in ui.c by skipping them. Keep it last before MARK_N so the
 * drawable shapes stay 0..MARK_TRIANGLE. */
/* Marker shapes. Values 0..MARK_HIDE are FROZEN (persisted in slots.csv); new
 * variants are appended so old files keep their meaning. Every shape has a
 * filled and an empty version except CROSS. MARK_HIDE means "do not draw". */
enum {
   MARK_DOT = 0,    /* small filled dot */
   MARK_CROSS,      /* X (no fill variant) */
   MARK_SQUARE,     /* empty square */
   MARK_TRIANGLE,   /* empty triangle */
   MARK_HIDE,       /* not drawn */
   MARK_SQUARE_F,   /* filled square */
   MARK_TRIANGLE_F, /* filled triangle */
   MARK_CIRCLE,     /* empty circle */
   MARK_CIRCLE_F,   /* filled circle */
   MARK_N
};

/* Immutable provenance for one minted id. */
struct sensor_rec {
   long activation; /* session start, epoch seconds (0 if unknown).
                     * Recorded, but NOT part of the id reuse key. */
   long paired;     /* when this id was minted */
   int id;
   int type;
   char identity[24]; /* MAC for Stelo/meter; BOND identity addr for G7 */
   char serial[24];   /* empty when the device does not expose one */
   char model[24];    /* DIS model string, e.g. SW11163 */
   char fw[24];
};

/* Mutable per-sensor preferences, keyed by id. */
struct sensor_slot {
   int id;
   int marker;  /* MARK_* */
   int color;   /* index into ui_sensor_colors[] */
   int primary; /* owns the big number; CGM only, at most one */
   int size;    /* marker size 1..MARK_SIZE_MAX; 0 = unset -> default */
   char label[20];
};

#define MARK_SIZE_DEF 2
#define MARK_SIZE_MAX 5

extern struct sensor_rec g_srec[MAX_SENSOR_RECS];
extern int g_nsrec;
extern struct sensor_slot g_slot[MAX_SLOTS];
extern int g_nslot;
extern char g_sensors_path[256], g_slots_path[256];

/* Registry lock, for callers holding it across a multi-step read of g_slot.
 * Recursive. Lock order is driver -> reg -> hist; reg is a leaf. */
void sensors_lock(void);
void sensors_unlock(void);

void sensors_load(void); /* both files; safe on a fresh install */
void slots_save(void);   /* rewrite slots.csv from g_slot */

/* Resolve a reading's source_id to its provenance, or 0 if it has aged out of
 * the cache (or predates the registry entirely). */
const struct sensor_rec *sensor_rec_by_id(int id);
/* The slot for an id, or 0 if the sensor has been forgotten. */
struct sensor_slot *sensor_slot_by_id(int id);
/* Derived, never stored: the kind follows from the type. */
int sensor_kind(int type);
const char *sensor_type_name(int type);
/* Nominal wear time in seconds, so the UI can show when a session ends.
 * 0 for a meter, which has no session at all. */
long sensor_session_len(int type);

/* Find a live slot by address alone, regardless of type. Use this to recognise
 * an already-registered device: keying on type as well lets a stale UI
 * selection re-register one physical sensor under a second, wrong type. */
int sensor_slot_by_mac(const char *identity);

/* Fill `out` (capacity `max`) with the ids of slots whose marker is MARK_HIDE,
 * returning the count. One locked pass, so the plot's scrub path can flag
 * hidden points without a per-point registry lock. */
int sensor_hidden_ids(int *out, int max);

/* Mint an id for a newly paired sensor and append its provenance row. Reuses
 * the existing id when every identifying field is unchanged; mints a new one
 * the moment any of them differs, so an id maps to exactly one (device,
 * firmware, session) tuple for all time. Returns the id, or -1 on failure. */
int sensor_mint(int type, const char *identity, const char *serial,
                const char *model, const char *fw, long activation);

/* Give a freshly minted sensor a slot (label defaults to type + MAC tail).
 * Returns the slot index, or -1 when all MAX_SLOTS are taken. */
int sensor_claim_slot(int id, int type, const char *identity);
/* Repoint the slot holding `old_id` at `new_id` and persist, atomically under
 * the registry lock. Returns 1 if a slot was found and updated, 0 otherwise.
 * Use this instead of assigning through a sensor_slot_by_id() pointer: that
 * pointer is an index into an array sensor_forget_slot() shifts. */
int sensor_rebind_slot(int old_id, int new_id);
/* Drop the slot (provenance is untouched, so old readings stay attributed). */
void sensor_forget_slot(int idx);
/* Make `idx` the primary; clears any other primary. No-op for a BGM. */
void sensor_set_primary(int idx);
/* Index of the primary slot, or -1. Unlocked -- for main-thread UI use. */
int sensor_primary_slot(void);
/* The primary sensor's id, or -1, resolved under the registry lock. Use this
 * (not sensor_primary_slot) from any path that also takes hist_lock: resolve
 * BEFORE taking hist_lock, so the reg->hist order is preserved. */
int sensor_primary_id(void);

#endif
