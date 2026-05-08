package com.hearingaid.app;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.os.Binder;
import android.os.Build;
import android.os.IBinder;
import android.util.Log;

import androidx.core.app.NotificationCompat;

/**
 * AudioProcessingService.java
 * ============================================================
 * A foreground Service that owns the NativeAudioEngine.
 *
 * Why a foreground service?
 *   Background processes can be killed by the OS at any time.
 *   As a foreground service with a visible notification, Android
 *   grants us elevated priority — the audio engine keeps running
 *   even when the screen is off or another app is in the foreground.
 *
 * Lifecycle:
 *   MainActivity.start() → startForegroundService() → onStartCommand()
 *     → engine.startEngine() → audio runs indefinitely
 *   MainActivity.stop() → stopService() → onDestroy()
 *     → engine.stopEngine()
 * ============================================================
 */
public class AudioProcessingService extends Service {

    private static final String TAG        = "HearingAidService";
    private static final String CHANNEL_ID = "hearing_aid_channel";
    private static final int    NOTIF_ID   = 1001;

    // The JNI bridge — created once, shared with MainActivity via the Binder
    private NativeAudioEngine mEngine;

    // ── Binder — lets MainActivity call engine methods directly ─────────────
    public class LocalBinder extends Binder {
        public NativeAudioEngine getEngine() { return mEngine; }
        public AudioProcessingService getService() { return AudioProcessingService.this; }
    }

    private final IBinder mBinder = new LocalBinder();

    // ── Service lifecycle ─────────────────────────────────────────────────────

    @Override
    public void onCreate() {
        super.onCreate();
        Log.i(TAG, "AudioProcessingService.onCreate()");
        mEngine = new NativeAudioEngine();
        createNotificationChannel();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        Log.i(TAG, "Starting foreground audio service...");

        // Start as foreground — shows a persistent notification
        startForeground(NOTIF_ID, buildNotification());

        // Launch the C++ audio engine
        boolean ok = mEngine.startEngine();
        if (!ok) {
            Log.e(TAG, "Failed to start native audio engine!");
            stopSelf();
        } else {
            Log.i(TAG, "Native audio engine started. Latency ≈ "
                    + mEngine.getLatencyMs() + " ms");
        }

        // STICKY: if the service is killed, Android restarts it with the last intent
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        Log.i(TAG, "AudioProcessingService.onDestroy() — stopping engine");
        if (mEngine != null) {
            mEngine.stopEngine();
        }
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return mBinder;
    }

    // ── Notification helpers ──────────────────────────────────────────────────

    private void createNotificationChannel() {
        NotificationChannel channel = new NotificationChannel(
                CHANNEL_ID,
                "Hearing Aid Active",
                NotificationManager.IMPORTANCE_LOW);   // LOW = no sound
        channel.setDescription("Real-time audio processing is running");
        channel.setShowBadge(false);
        NotificationManager nm = getSystemService(NotificationManager.class);
        if (nm != null) nm.createNotificationChannel(channel);
    }

    private Notification buildNotification() {
        // Tap notification → bring MainActivity to foreground
        PendingIntent pi = PendingIntent.getActivity(
                this, 0,
                new Intent(this, MainActivity.class),
                PendingIntent.FLAG_IMMUTABLE);

        return new NotificationCompat.Builder(this, CHANNEL_ID)
                .setContentTitle("Hearing Aid Active")
                .setContentText("Real-time audio processing is running")
                .setSmallIcon(android.R.drawable.ic_btn_speak_now)
                .setContentIntent(pi)
                .setOngoing(true)            // Cannot be swiped away
                .setPriority(NotificationCompat.PRIORITY_LOW)
                .build();
    }
}
