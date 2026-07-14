/* Audible + vibrating glucose alarm. Kept separate from the BLE pipe: native
 * code (main.c) decides WHEN to alarm (edge-triggered on the transition into an
 * out-of-range reading) and calls trigger()/silence() via dexble.c.
 *
 * The alert loops (sound + vibration) until silenced, so a single missed beep
 * can't be the difference — silence() stops everything at once. We play the
 * sound ourselves (looping MediaPlayer, USAGE_ALARM) rather than via the
 * notification channel, because channel sounds play once and can't be stopped
 * on demand; the notification is kept silent and used only for the heads-up
 * banner when the app isn't in front. The tone is the device's Default alarm
 * sound (Settings > Sound > Default alarm sound). */
package com.jk.stealo;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.media.AudioAttributes;
import android.media.MediaPlayer;
import android.media.RingtoneManager;
import android.net.Uri;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.util.Log;

public final class Alarm {
    private static final String CH = "stealo-alarm";
    private static final int NID = 2;              /* distinct from the service's id 1 */
    private static MediaPlayer player;

    private static void ensureChannel(NotificationManager nm) {
        if (nm.getNotificationChannel(CH) != null) return;
        NotificationChannel c = new NotificationChannel(CH, "Glucose alarm",
            NotificationManager.IMPORTANCE_HIGH);
        c.setSound(null, null);        /* we loop the sound ourselves */
        c.enableVibration(false);      /* we loop the vibration ourselves */
        nm.createNotificationChannel(c);
    }

    public static void trigger(Context ctx, boolean high) {
        try {
            Context app = ctx.getApplicationContext();
            NotificationManager nm = app.getSystemService(NotificationManager.class);
            ensureChannel(nm);

            Intent open = new Intent(app, android.app.NativeActivity.class);
            open.setAction(Intent.ACTION_MAIN);
            open.addCategory(Intent.CATEGORY_LAUNCHER);
            open.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP);
            PendingIntent pi = PendingIntent.getActivity(app, 0, open,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
            Notification n = new Notification.Builder(app, CH)
                .setContentTitle(high ? "Glucose HIGH" : "Glucose LOW")
                .setContentText("Open the app to silence")
                .setSmallIcon(android.R.drawable.stat_sys_warning)
                .setCategory(Notification.CATEGORY_ALARM)
                .setContentIntent(pi)
                .setOngoing(true)
                .build();
            nm.notify(NID, n);

            Vibrator v = app.getSystemService(Vibrator.class);
            if (v != null && v.hasVibrator())      /* 600ms on / 400ms off, repeating */
                v.vibrate(VibrationEffect.createWaveform(new long[]{0, 600, 400}, 0));

            stopSound();
            Uri uri = RingtoneManager.getActualDefaultRingtoneUri(app, RingtoneManager.TYPE_ALARM);
            if (uri == null) uri = RingtoneManager.getDefaultUri(RingtoneManager.TYPE_ALARM);
            if (uri != null) {
                player = new MediaPlayer();
                player.setAudioAttributes(new AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_ALARM)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION).build());
                player.setDataSource(app, uri);
                player.setLooping(true);
                player.prepare();
                player.start();
            }
        } catch (Throwable t) { Log.i("stealo", "alarm trigger: " + t); }
    }

    public static void silence(Context ctx) {
        try {
            Context app = ctx.getApplicationContext();
            Vibrator v = app.getSystemService(Vibrator.class);
            if (v != null) v.cancel();
            stopSound();
            NotificationManager nm = app.getSystemService(NotificationManager.class);
            if (nm != null) nm.cancel(NID);
        } catch (Throwable t) { Log.i("stealo", "alarm silence: " + t); }
    }

    private static void stopSound() {
        try { if (player != null) { player.stop(); player.release(); player = null; } }
        catch (Throwable t) { player = null; }
    }
}
