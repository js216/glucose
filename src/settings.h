// SPDX-License-Identifier: GPL-3.0
// settings.h --- Persisted config: alarms, display prefs, device info, code
// Copyright 2026 Jakob Kastelic

#ifndef STEALO_SETTINGS_H
#define STEALO_SETTINGS_H

/* Config state owned by settings.c and persisted to small files. The UI reads
 * these for display and writes them from the settings menu, then calls the
 * matching *_save(). Paths are built once at startup by the app. */
extern char g_model[24], g_fw[24],
    g_mfr[24];                        /* DIS: model / fw / manufacturer */
extern int g_alarm_low, g_alarm_high; /* thresholds, mg/dL */
extern int g_sound_on, g_vib_on;      /* alarm sound / vibration */
extern int g_orient;        /* 0 portrait 1 landscape 2 gravity 3 system */
extern int g_units;         /* 0 mg/dL, 1 mmol/L */
extern int g_disc;          /* stale-data alarm: index into disc_min */
extern int g_plot_max;      /* plot vertical-scale top, mg/dL */
extern char g_code_str[16]; /* runtime pairing code (PAIR NEW SENSOR) */
extern char g_info_path[256], g_alarm_path[256], g_settings_path[256],
    g_code_path[256];

void info_save(void); /* device-info strings "model\nfw\nmfr\n" */
void info_load(void);
void alarm_save(void); /* "low high\n" */
void alarm_load(void);
void settings_save(void); /* "sound vib orient units disc plot_max\n" */
void settings_load(void); /* also applies plot_set_max() */
void code_save(void);     /* pairing code digits */
void code_load(void);

#endif
