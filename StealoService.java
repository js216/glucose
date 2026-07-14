/* Foreground service: keeps the process alive (and BLE-exempt from Doze) so the
 * sensor connection and reading collection continue when the app is not in the
 * foreground — or has been swiped away entirely, like the official app.
 *
 * It runs no logic itself. All BLE state is static in Ble and lives in this same
 * process; the GATT callbacks arrive on binder threads and keep driving the
 * reconnect/parse path regardless of the Activity's lifecycle. This service
 * exists only to hold foreground priority (with an ongoing notification) so the
 * OS does not kill the process. Started by the activity at launch. */
package com.jk.stealo;

import android.app.AlarmManager;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.net.Uri;
import android.os.Build;
import android.os.IBinder;
import android.os.PowerManager;
import android.os.SystemClock;
import android.provider.Settings;
import android.util.Log;

public final class StealoService extends Service {
    private static final String CH = "stealo";
    private static final String ACTION_WAKE = "com.jk.stealo.WAKE";
    private static final long WAKE_INTERVAL_MS = 5 * 60 * 1000L;   /* ~one sensor cycle */

    /* called by native (via Ble.startService) at activity create */
    public static void start(Context ctx) {
        try {
            Context app = ctx.getApplicationContext();
            app.startForegroundService(new Intent(app, StealoService.class));
        } catch (Throwable t) { Log.i("stealo", "startService: " + t); }
    }

    /* Ask the user to exempt us from battery optimisation. Without this, Doze can
     * still throttle/kill even a foreground-service process over a long idle
     * night. Shows the system dialog once; a no-op if already exempt. Called from
     * the activity (an Activity context) at launch. */
    public static void requestNoBatteryOpt(Context ctx) {
        try {
            PowerManager pm = ctx.getSystemService(PowerManager.class);
            String pkg = ctx.getPackageName();
            if (pm != null && pm.isIgnoringBatteryOptimizations(pkg)) return;
            Intent i = new Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS,
                                  Uri.parse("package:" + pkg));
            i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            ctx.startActivity(i);
        } catch (Throwable t) { Log.i("stealo", "batteryOpt: " + t); }
    }

    private Notification build() {
        NotificationManager nm = getSystemService(NotificationManager.class);
        nm.createNotificationChannel(
            new NotificationChannel(CH, "Stealo", NotificationManager.IMPORTANCE_LOW));
        /* tapping the notification brings the app back to the foreground */
        Intent open = new Intent(this, android.app.NativeActivity.class);
        open.setAction(Intent.ACTION_MAIN);
        open.addCategory(Intent.CATEGORY_LAUNCHER);
        open.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        PendingIntent pi = PendingIntent.getActivity(this, 0, open,
            PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        return new Notification.Builder(this, CH)
            .setContentTitle("Stealo")
            .setContentText("Reading glucose")
            .setSmallIcon(android.R.drawable.ic_dialog_info)
            .setContentIntent(pi)
            .setOngoing(true)
            .build();
    }

    /* Re-arm a periodic wake. Stelo disconnects after every reading, so between
     * cycles reconnection depends on our process getting CPU; in deep Doze the
     * process is frozen and queued BLE callbacks (the auto-reconnect) don't get
     * delivered. This alarm briefly unfreezes us each cycle so those callbacks
     * flush and the EXISTING reconnect path runs — it does not touch BLE itself.
     * setAndAllowWhileIdle needs no special permission and fires during Doze. */
    private void scheduleWake() {
        try {
            AlarmManager am = getSystemService(AlarmManager.class);
            if (am == null) return;
            Intent i = new Intent(this, StealoService.class).setAction(ACTION_WAKE);
            PendingIntent pi = PendingIntent.getForegroundService(this, 1, i,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
            am.setAndAllowWhileIdle(AlarmManager.ELAPSED_REALTIME_WAKEUP,
                SystemClock.elapsedRealtime() + WAKE_INTERVAL_MS, pi);
        } catch (Throwable t) { Log.i("stealo", "scheduleWake: " + t); }
    }

    @Override public int onStartCommand(Intent i, int flags, int startId) {
        try {
            if (Build.VERSION.SDK_INT >= 29)
                startForeground(1, build(), ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE);
            else
                startForeground(1, build());
        } catch (Throwable t) { Log.i("stealo", "startForeground: " + t); }
        scheduleWake();          /* re-arm each time, including on each wake tick */
        return START_STICKY;     /* restart if the system kills us */
    }

    /* When the user swipes the task away, keep running: re-arm the service so the
     * process (and thus the live BLE connection) is not torn down. */
    @Override public void onTaskRemoved(Intent rootIntent) {
        try {
            Intent restart = new Intent(getApplicationContext(), StealoService.class);
            getApplicationContext().startForegroundService(restart);
        } catch (Throwable t) { Log.i("stealo", "onTaskRemoved: " + t); }
    }

    @Override public IBinder onBind(Intent i) { return null; }
}
