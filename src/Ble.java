// SPDX-License-Identifier: GPL-3.0
// Ble.java --- BLE GATT pipe (scan/connect/read/write/subscribe)
// Copyright 2026 Jakob Kastelic

/* Dumb pipe to the Android BLE APIs: exposes primitives, interprets nothing.
 * All protocol meaning lives on the C side (dexble.c). Java only:
 *   - scans and reports advertisements,
 *   - connects / discovers / subscribes / writes on request,
 *   - serialises GATT operations (Android allows one in flight at a time),
 *   - forwards connection events, notifications and write-acks to native.
 */
package com.jk.stealo;

import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothProfile;
import android.bluetooth.le.BluetoothLeScanner;
import android.bluetooth.le.ScanCallback;
import android.bluetooth.le.ScanResult;
import android.bluetooth.le.ScanSettings;
import android.app.Activity;
import android.content.Context;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.util.Log;

import java.util.ArrayDeque;
import java.util.UUID;

public final class Ble {
    private static final String TAG = "stealo";
    private static final UUID CCCD =
        UUID.fromString("00002902-0000-1000-8000-00805f9b34fb");

    public static int ping() { return 42; }

    /* ---- settings-menu helpers (ctx is the NativeActivity, i.e. an Activity) ---- */
    /* mode: 0 portrait, 1 landscape, 2 gravity (sensor always), 3 system (sensor
     * only if the OS auto-rotate setting allows it) */
    public static void setOrientation(Context ctx, int mode) {
        try {
            if (!(ctx instanceof Activity)) return;
            int o;
            switch (mode) {
                case 1:  o = ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE;   break;
                case 2:  o = ActivityInfo.SCREEN_ORIENTATION_FULL_SENSOR; break;
                case 3:  o = ActivityInfo.SCREEN_ORIENTATION_USER;        break;
                default: o = ActivityInfo.SCREEN_ORIENTATION_PORTRAIT;    break;
            }
            ((Activity) ctx).setRequestedOrientation(o);
        } catch (Throwable t) { Log.i(TAG, "orient: " + t); }
    }
    public static boolean permGranted(Context ctx, String perm) {
        try { return ctx.checkSelfPermission(perm) == PackageManager.PERMISSION_GRANTED; }
        catch (Throwable t) { return false; }
    }
    public static void requestPerm(Context ctx, String perm) {
        try {
            if (ctx instanceof Activity)
                ((Activity) ctx).requestPermissions(new String[]{ perm }, 0);
        } catch (Throwable t) { Log.i(TAG, "reqperm: " + t); }
    }
    /* app details page — the only place the user can REVOKE an already-granted one */
    public static void openAppSettings(Context ctx) {
        try {
            android.content.Intent i = new android.content.Intent(
                android.provider.Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                android.net.Uri.parse("package:" + ctx.getPackageName()));
            i.addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK);
            ctx.startActivity(i);
        } catch (Throwable t) { Log.i(TAG, "appsettings: " + t); }
    }

    /* ---- background-running controls a CGM needs alive ---- */
    /* battery-optimisation exemption: readable + requestable (revoke via settings) */
    public static boolean isBatteryUnrestricted(Context ctx) {
        try {
            android.os.PowerManager pm = ctx.getSystemService(android.os.PowerManager.class);
            return pm != null && pm.isIgnoringBatteryOptimizations(ctx.getPackageName());
        } catch (Throwable t) { return false; }
    }
    public static void requestBatteryOpt(Context ctx) {
        try {
            android.content.Intent i = new android.content.Intent(
                android.provider.Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS,
                android.net.Uri.parse("package:" + ctx.getPackageName()));
            i.addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK);
            ctx.startActivity(i);
        } catch (Throwable t) { Log.i(TAG, "reqbatt: " + t); }
    }
    /* app standby bucket: readable (own app), NOT settable without system privilege */
    public static int standbyBucket(Context ctx) {
        try {
            android.app.usage.UsageStatsManager u = (android.app.usage.UsageStatsManager)
                ctx.getSystemService(Context.USAGE_STATS_SERVICE);
            return u != null ? u.getAppStandbyBucket() : -1;
        } catch (Throwable t) { return -1; }
    }
    /* background-execution restriction: readable, changed only in app settings */
    public static boolean isBgRestricted(Context ctx) {
        try {
            android.app.ActivityManager am = (android.app.ActivityManager)
                ctx.getSystemService(Context.ACTIVITY_SERVICE);
            return am != null && am.isBackgroundRestricted();
        } catch (Throwable t) { return false; }
    }

    /* start the foreground service so BLE keeps running in the background, and
     * ask to be exempted from battery optimisation so Doze can't kill us */
    public static void startService(Context ctx) {
        StealoService.start(ctx);
        StealoService.requestNoBatteryOpt(ctx);
    }

    /* Push the live glucose + a 3H plot bitmap into the ongoing notification
     * (shown on the lock screen / shade). Called from native each reading. */
    public static void showGlucose(Context ctx, String title, String text,
                                   int[] px, int w, int h) {
        StealoService.showGlucose(ctx, title, text, px, w, h);
    }

    /* ---- Java -> C callbacks (bound via RegisterNatives in dexble.c) ---- */
    static native void onAdvert(String name, String mac, int rssi);
    static native void onConnected();
    static native void onDisconnected(int status);
    static native void onNotify(String uuid, byte[] data);
    static native void onWritten(String uuid, int status);
    static native void onRssi(int rssi);
    static native void onRead(String uuid, byte[] data);

    private static BluetoothLeScanner scanner;
    private static ScanCallback scanCb;
    private static BluetoothGatt gatt;

    /* GATT operations must be serialised; queue Runnables, run one at a time. */
    private static final ArrayDeque<Runnable> ops = new ArrayDeque<>();
    private static boolean busy;

    private static synchronized void enqueue(Runnable r) { ops.add(r); pump(); }
    private static synchronized void pump() {
        if (busy || ops.isEmpty() || gatt == null) return;
        busy = true;
        Runnable r = ops.poll();
        try { r.run(); } catch (Throwable t) { Log.i(TAG, "op: " + t); done(); }
    }
    private static synchronized void done() { busy = false; pump(); }

    /* MAC of the bonded Stelo (name starts "DX01"), or "" if none is bonded.
     * Bonded-device names are reliable, unlike the advertised local name (often
     * absent), so this resolves our sensor's address deterministically. Used to
     * re-lock after an update that cleared files/stelo.mac but kept the key, so
     * reconnect never has to guess from adverts. Needs BLUETOOTH_CONNECT (held).
     * Deliberately ignores the G7 ("DXCM") so it is never selected. */
    public static String bondedStelo(Context ctx) {
        try {
            BluetoothManager bm =
                (BluetoothManager) ctx.getSystemService(Context.BLUETOOTH_SERVICE);
            BluetoothAdapter ad = (bm == null) ? null : bm.getAdapter();
            if (ad == null) return "";
            for (BluetoothDevice d : ad.getBondedDevices()) {
                String nm = d.getName();
                if (nm != null && nm.startsWith("DX01")) return d.getAddress();
            }
        } catch (Throwable t) { Log.i(TAG, "bondedStelo: " + t); }
        return "";
    }

    /* ---- scanning (unchanged pipe) ---- */
    public static String scan(Context ctx) {
        try {
            BluetoothManager bm =
                (BluetoothManager) ctx.getSystemService(Context.BLUETOOTH_SERVICE);
            BluetoothAdapter ad = (bm == null) ? null : bm.getAdapter();
            if (ad == null)      return "NO BLUETOOTH";
            if (!ad.isEnabled()) return "BLUETOOTH OFF";
            scanner = ad.getBluetoothLeScanner();
            if (scanner == null) return "NO LE SCANNER";
            scanCb = new ScanCallback() {
                @Override public void onScanResult(int type, ScanResult r) {
                    String name = (r.getScanRecord() == null)
                                ? null : r.getScanRecord().getDeviceName();
                    /* The advertised local name is frequently absent in Dexcom
                     * adverts; fall back to the device's cached name (reliable
                     * for a device we've seen/bonded) so the Stelo/G7 filter has
                     * something to match. Needs BLUETOOTH_CONNECT (held). */
                    if (name == null || name.isEmpty()) {
                        try { name = r.getDevice().getName(); }
                        catch (Throwable t) { /* no CONNECT perm yet */ }
                    }
                    onAdvert(name == null ? "" : name,
                             r.getDevice().getAddress(), r.getRssi());
                }
                @Override public void onScanFailed(int err) { Log.i(TAG, "scan failed: " + err); }
            };
            scanner.startScan(null,
                new ScanSettings.Builder()
                    .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build(), scanCb);
            return null;
        } catch (Throwable t) { return t.getClass().getSimpleName(); }
    }

    public static void stop() {
        try { if (scanner != null && scanCb != null) scanner.stopScan(scanCb); }
        catch (Throwable t) { Log.i(TAG, "stop: " + t); }
        scanCb = null;
    }

    /* ---- connect / GATT ---- */
    public static String connect(Context ctx, String mac) {
        try {
            BluetoothManager bm =
                (BluetoothManager) ctx.getSystemService(Context.BLUETOOTH_SERVICE);
            BluetoothAdapter ad = (bm == null) ? null : bm.getAdapter();
            if (ad == null) return "NO BLUETOOTH";
            BluetoothDevice dev = ad.getRemoteDevice(mac);
            /* Close any client still open from a previous attempt before making a
             * new one — otherwise re-issuing connect() (e.g. the stall watchdog)
             * leaks a GATT client interface, which strands the link. Clean up
             * after ourselves so a fresh connect always starts from a clean slate. */
            synchronized (Ble.class) {
                if (gatt != null) {
                    try { gatt.disconnect(); gatt.close(); } catch (Throwable t) { /* ignore */ }
                    gatt = null;
                }
                ops.clear(); busy = false;
            }
            /* autoConnect=true: connect when the sensor next advertises rather than
             * failing immediately (status 62). Robust for periodic CGM advertising
             * and gentle on the sensor battery (passive wait, no connect storms). */
            gatt = dev.connectGatt(ctx.getApplicationContext(), true, cb, BluetoothDevice.TRANSPORT_LE);
            return gatt == null ? "CONNECT NULL" : null;
        } catch (Throwable t) { return t.getClass().getSimpleName(); }
    }

    public static void disconnect() {
        try { if (gatt != null) { gatt.disconnect(); gatt.close(); } }
        catch (Throwable t) { Log.i(TAG, "disc: " + t); }
        synchronized (Ble.class) { gatt = null; ops.clear(); busy = false; }
    }

    private static BluetoothGattCharacteristic find(String uuid) {
        if (gatt == null) return null;
        UUID u = UUID.fromString(uuid);
        for (BluetoothGattService s : gatt.getServices()) {
            BluetoothGattCharacteristic c = s.getCharacteristic(u);
            if (c != null) return c;
        }
        return null;
    }

    /* Enable notifications (indicate=false) or indications (indicate=true). */
    public static void subscribe(final String uuid, final boolean indicate) {
        enqueue(new Runnable() { public void run() {
            Log.i(TAG, "op subscribe " + uuid + (indicate ? " IND" : " NOT"));
            BluetoothGattCharacteristic c = find(uuid);
            if (c == null) { Log.i(TAG, "  char not found"); onWritten(uuid, -1); done(); return; }
            gatt.setCharacteristicNotification(c, true);
            BluetoothGattDescriptor d = c.getDescriptor(CCCD);
            if (d == null) { Log.i(TAG, "  no CCCD"); onWritten(uuid, -1); done(); return; }
            d.setValue(indicate ? BluetoothGattDescriptor.ENABLE_INDICATION_VALUE
                                : BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE);
            /* If the stack rejects the write (returns false), onDescriptorWrite
             * never fires — so, like write(), advance the queue ourselves instead
             * of stalling forever (the CCCD is often already enabled on a bonded
             * reconnect, so notifications still flow). */
            if (!gatt.writeDescriptor(d)) {
                Log.i(TAG, "  writeDescriptor false"); onWritten(uuid, -1); done();
            }
        }});
    }

    /* Read the live connection RSSI; result arrives in onReadRemoteRssi.
     * This only reads packets already being received — nothing is sent to the
     * sensor, so it costs the sensor no battery. */
    public static void readRssi() {
        enqueue(new Runnable() { public void run() {
            if (gatt == null || !gatt.readRemoteRssi()) done();
        }});
    }

    /* Read a characteristic (e.g. Device Information Service strings); the value
     * arrives in onCharacteristicRead and is forwarded to native via onRead. */
    public static void read(final String uuid) {
        enqueue(new Runnable() { public void run() {
            BluetoothGattCharacteristic c = find(uuid);
            if (c == null) { Log.i(TAG, "op read " + uuid + " -> char not found"); onRead(uuid, new byte[0]); done(); return; }
            Log.i(TAG, "op read " + uuid);
            if (!gatt.readCharacteristic(c)) { Log.i(TAG, "  readCharacteristic false"); onRead(uuid, new byte[0]); done(); }
        }});
    }

    /* Write a characteristic; noResponse selects WRITE_TYPE_NO_RESPONSE. */
    public static void write(final String uuid, final byte[] data, final boolean noResponse) {
        enqueue(new Runnable() { public void run() {
            Log.i(TAG, "op write " + uuid + " len=" + data.length + (noResponse ? " NR" : " REQ"));
            BluetoothGattCharacteristic c = find(uuid);
            if (c == null) { Log.i(TAG, "  char not found"); onWritten(uuid, -1); done(); return; }
            c.setWriteType(noResponse ? BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
                                      : BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
            c.setValue(data);
            if (!gatt.writeCharacteristic(c)) { Log.i(TAG, "  writeCharacteristic false"); onWritten(uuid, -1); done(); }
        }});
    }

    private static final BluetoothGattCallback cb = new BluetoothGattCallback() {
        @Override public void onConnectionStateChange(BluetoothGatt g, int status, int newState) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                g.requestMtu(185);           /* enough for a 20-byte-chunked round, +headroom */
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                try { g.close(); } catch (Throwable t) { /* ignore */ }
                synchronized (Ble.class) { gatt = null; ops.clear(); busy = false; }
                onDisconnected(status);
            }
        }
        @Override public void onMtuChanged(BluetoothGatt g, int mtu, int status) {
            g.discoverServices();
        }
        @Override public void onServicesDiscovered(BluetoothGatt g, int status) {
            onConnected();
        }
        @Override public void onCharacteristicChanged(BluetoothGatt g,
                BluetoothGattCharacteristic c) {
            onNotify(c.getUuid().toString(), c.getValue());
        }
        @Override public void onCharacteristicWrite(BluetoothGatt g,
                BluetoothGattCharacteristic c, int status) {
            onWritten(c.getUuid().toString(), status);
            done();
        }
        @Override public void onDescriptorWrite(BluetoothGatt g,
                BluetoothGattDescriptor d, int status) {
            onWritten(d.getCharacteristic().getUuid().toString(), status);
            done();
        }
        @Override public void onReadRemoteRssi(BluetoothGatt g, int rssi, int status) {
            if (status == BluetoothGatt.GATT_SUCCESS) onRssi(rssi);
            done();
        }
        @Override public void onCharacteristicRead(BluetoothGatt g,
                BluetoothGattCharacteristic c, int status) {
            byte[] v = (status == BluetoothGatt.GATT_SUCCESS) ? c.getValue() : null;
            Log.i(TAG, "onRead " + c.getUuid() + " status=" + status + " len=" + (v == null ? -1 : v.length));
            onRead(c.getUuid().toString(), v == null ? new byte[0] : v);
            done();
        }
    };
}
