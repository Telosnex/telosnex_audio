package com.telosnex.audio;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.media.AudioDeviceCallback;
import android.media.AudioDeviceInfo;
import android.media.AudioManager;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.SystemClock;
import android.util.Log;

import androidx.annotation.RequiresApi;

import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * Android audio routes for the native engine (ADR D6, D15). The engine calls
 * the static methods through JNI, on its device thread
 * (src/android/android_jni.cc).
 */
final class AudioRoutes {
    private static final String TAG = "telosnex_audio";

    private static volatile AudioManager audioManager;
    private static volatile Context appContext;
    private static volatile boolean loaded;
    private static Handler handler;
    // The engine set MODE_IN_COMMUNICATION. Device thread.
    private static boolean inCommunication;

    private AudioRoutes() {}

    static synchronized void attach(Context context) {
        if (audioManager != null) return;
        appContext = context.getApplicationContext();
        audioManager = (AudioManager) appContext.getSystemService(Context.AUDIO_SERVICE);
        // Callbacks and broadcasts arrive here, not on the main thread: the
        // device thread can wait for them while the main thread waits for
        // the engine.
        HandlerThread thread = new HandlerThread("tsnx_routes");
        thread.start();
        handler = new Handler(thread.getLooper());
        audioManager.registerAudioDeviceCallback(
                new AudioDeviceCallback() {
                    @Override
                    public void onAudioDevicesAdded(AudioDeviceInfo[] added) {
                        changed();
                    }

                    @Override
                    public void onAudioDevicesRemoved(AudioDeviceInfo[] removed) {
                        changed();
                    }
                },
                handler);
        // The library is large, and Dart can load it first. JNI_OnLoad runs
        // either way, with this class loader.
        handler.post(
                () -> {
                    try {
                        System.loadLibrary("telosnex_audio");
                        loaded = true;
                        changed();
                    } catch (UnsatisfiedLinkError e) {
                        Log.w(TAG, "Native library not loaded; Android routes are off", e);
                    }
                });
    }

    private static void changed() {
        if (!loaded) return;
        try {
            nativeDevicesChanged();
        } catch (UnsatisfiedLinkError e) {
            // JNI_OnLoad did not register it: an older native library.
            loaded = false;
        }
    }

    private static native void nativeDevicesChanged();

    /** One record per device: "id\ttype\tsink\tname\taddress\n". */
    static String devices() {
        AudioManager am = audioManager;
        if (am == null) return "";
        StringBuilder sb = new StringBuilder();
        for (AudioDeviceInfo d : am.getDevices(AudioManager.GET_DEVICES_ALL)) {
            sb.append(d.getId())
                    .append('\t')
                    .append(d.getType())
                    .append('\t')
                    .append(d.isSink() ? '1' : '0')
                    .append('\t')
                    .append(clean(d.getProductName()))
                    .append('\t')
                    .append(Build.VERSION.SDK_INT >= 28 ? clean(d.getAddress()) : "")
                    .append('\n');
        }
        return sb.toString();
    }

    private static String clean(CharSequence s) {
        return s == null ? "" : s.toString().replace('\t', ' ').replace('\n', ' ').trim();
    }

    /**
     * With deviceId 0, returns to MODE_NORMAL. Otherwise sets
     * MODE_IN_COMMUNICATION with that communication device (an output:
     * the earpiece, or a Bluetooth headset in a call), and waits up to
     * timeoutMs for Android to use it. On failure, returns to MODE_NORMAL.
     */
    static boolean setCommunication(int deviceId, int type, int timeoutMs) {
        AudioManager am = audioManager;
        if (am == null) return deviceId == 0;
        if (deviceId == 0) {
            leave(am);
            return true;
        }
        am.setMode(AudioManager.MODE_IN_COMMUNICATION);
        inCommunication = true;
        boolean ok =
                Build.VERSION.SDK_INT >= 31
                        ? setDevice(am, deviceId, type, timeoutMs)
                        : setLegacy(am, type, timeoutMs);
        if (!ok) {
            Log.w(TAG, "Communication device " + deviceId + " (type " + type + ") failed");
            leave(am);
        }
        return ok;
    }

    @SuppressWarnings("deprecation")
    private static void leave(AudioManager am) {
        if (!inCommunication) return;
        inCommunication = false;
        if (Build.VERSION.SDK_INT >= 31) {
            am.clearCommunicationDevice();
        } else if (am.isBluetoothScoOn()) {
            am.setBluetoothScoOn(false);
            am.stopBluetoothSco();
        }
        am.setMode(AudioManager.MODE_NORMAL);
    }

    @RequiresApi(31)
    private static boolean setDevice(AudioManager am, int deviceId, int type, int timeoutMs) {
        AudioDeviceInfo target = null;
        for (AudioDeviceInfo d : am.getAvailableCommunicationDevices()) {
            if (d.getId() == deviceId) target = d;
        }
        if (target == null) {
            for (AudioDeviceInfo d : am.getAvailableCommunicationDevices()) {
                if (d.getType() == type && target == null) target = d;
            }
        }
        if (target == null || !am.setCommunicationDevice(target)) return false;
        // A Bluetooth headset takes about a second to open its call link.
        long end = SystemClock.elapsedRealtime() + timeoutMs;
        for (; ; ) {
            AudioDeviceInfo now = am.getCommunicationDevice();
            if (now != null && now.getId() == target.getId()) return true;
            if (SystemClock.elapsedRealtime() >= end) return false;
            SystemClock.sleep(20);
        }
    }

    @SuppressWarnings("deprecation")
    private static boolean setLegacy(AudioManager am, int type, int timeoutMs) {
        if (type == AudioDeviceInfo.TYPE_BUILTIN_EARPIECE) {
            if (am.isBluetoothScoOn()) {
                am.setBluetoothScoOn(false);
                am.stopBluetoothSco();
            }
            am.setSpeakerphoneOn(false);
            return true;
        }
        if (type != AudioDeviceInfo.TYPE_BLUETOOTH_SCO || !am.isBluetoothScoAvailableOffCall()) {
            return false;
        }
        CountDownLatch connected = new CountDownLatch(1);
        BroadcastReceiver receiver =
                new BroadcastReceiver() {
                    @Override
                    public void onReceive(Context context, Intent intent) {
                        int state =
                                intent.getIntExtra(
                                        AudioManager.EXTRA_SCO_AUDIO_STATE,
                                        AudioManager.SCO_AUDIO_STATE_ERROR);
                        if (state == AudioManager.SCO_AUDIO_STATE_CONNECTED) {
                            connected.countDown();
                        }
                    }
                };
        Context context = appContext;
        IntentFilter filter = new IntentFilter(AudioManager.ACTION_SCO_AUDIO_STATE_UPDATED);
        // A system broadcast: no export flag is needed (API 24-30 only).
        context.registerReceiver(receiver, filter, null, handler);
        try {
            am.setSpeakerphoneOn(false);
            am.startBluetoothSco();
            am.setBluetoothScoOn(true);
            return connected.await(timeoutMs, TimeUnit.MILLISECONDS);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return false;
        } finally {
            context.unregisterReceiver(receiver);
        }
    }
}
