// SPDX-License-Identifier: GPL-3.0
// stealo.h --- main.c <-> dexble.c interface declarations
// Copyright 2026 Jakob Kastelic

/* Interface between main.c (UI + JNI registration) and dexble.c (BLE
 * transport). Declaring both directions here gives every translation unit a
 * prototype, so the definitions are checked against it (-Wmissing-prototypes).
 */
#ifndef STEALO_H
#define STEALO_H

#include <jni.h>
#include <stddef.h>

/* The NDK entry point (defined in main.c). Declared here so it has a visible
 * prototype and is understood to be externally linked (the Android runtime
 * resolves it by name), not a candidate for internal linkage. */
struct ANativeActivity;
void ANativeActivity_onCreate(struct ANativeActivity *activity, void *saved,
                              size_t saved_size);

/* UI callbacks -- implemented in main.c, called from the BLE/driver layer. */
void stealo_status(const char *s);
void stealo_glucose(int mg_dl, int trend, int age_s);
void stealo_backfill(int mg_dl, int trend, int age_s);
void stealo_rssi(int rssi);
void stealo_devinfo(const char *uuid, const char *val);

/* BLE transport -- implemented in dexble.c, called from main.c. */
void dexble_init(const char *data_dir);
int dexble_register(JNIEnv *env, jclass ble, jobject ctx);
void dexble_set_alarm(JNIEnv *env, jclass alarm_cls);
void dexble_pair(JNIEnv *env, const char *mac, const char *code);
void dexble_reconnect(JNIEnv *env); /* stall watchdog: force a fresh connect */
void dexble_request_devinfo(void);
void dexble_alarm(int kind, int sound,
                  int vibrate); /* 0 low, 1 high, 2 stale */
void dexble_alarm_silence(void);

#endif
