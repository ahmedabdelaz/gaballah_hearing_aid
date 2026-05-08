package com.hearingaid.app;

import android.Manifest;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.os.Bundle;
import android.os.IBinder;
import android.util.Log;
import android.view.View;
import android.widget.Button;
import android.widget.SeekBar;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

import androidx.annotation.NonNull;
import androidx.appcompat.app.AppCompatActivity;
import androidx.core.app.ActivityCompat;
import androidx.core.content.ContextCompat;

import com.github.mikephil.charting.charts.LineChart;
import com.github.mikephil.charting.components.XAxis;
import com.github.mikephil.charting.components.YAxis;
import com.github.mikephil.charting.data.Entry;
import com.github.mikephil.charting.data.LineData;
import com.github.mikephil.charting.data.LineDataSet;
import com.github.mikephil.charting.listener.ChartTouchListener;
import com.github.mikephil.charting.listener.OnChartGestureListener;
import android.view.MotionEvent;

import java.util.ArrayList;
import java.util.Arrays;

/**
 * MainActivity.java
 * ============================================================
 * The main UI screen of the hearing aid app.
 *
 * Features:
 *   • Runtime RECORD_AUDIO permission request
 *   • Start / Stop button that binds to AudioProcessingService
 *   • Interactive 10-band EQ graph (MPAndroidChart LineChart)
 *     — drag points up/down to set each band's gain
 *   • Master Gain SeekBar
 *   • Noise Gate threshold SeekBar
 *   • Compressor ratio SeekBar
 *   • Bypass toggle switch
 *   • Latency display (updates every second)
 *   • Persistent settings via SharedPreferences
 * ============================================================
 */
public class MainActivity extends AppCompatActivity {

    private static final String TAG             = "HearingAidMain";
    private static final int    PERM_REQUEST    = 100;
    private static final String PREFS_NAME      = "HearingAidPrefs";

    // Band frequencies for X-axis labels
    private static final String[] FREQ_LABELS = {
        "125", "250", "500", "1k", "2k", "3k", "4k", "6k", "8k", "12k"
    };

    // ── EQ state ─────────────────────────────────────────────────────────────
    // Stored in dB. Default = flat (0 dB on every band)
    private final float[] mEqGains = new float[10];

    // ── Service connection ────────────────────────────────────────────────────
    private AudioProcessingService.LocalBinder mServiceBinder = null;
    private boolean mServiceBound = false;

    private final ServiceConnection mServiceConnection = new ServiceConnection() {
        @Override
        public void onServiceConnected(ComponentName name, IBinder service) {
            mServiceBinder = (AudioProcessingService.LocalBinder) service;
            mServiceBound  = true;
            Log.i(TAG, "Service connected");
            applyAllSettings();
            startLatencyUpdater();
        }
        @Override
        public void onServiceDisconnected(ComponentName name) {
            mServiceBound  = false;
            mServiceBinder = null;
        }
    };

    // ── UI references ─────────────────────────────────────────────────────────
    private Button   btnStartStop;
    private Switch   switchBypass;
    private SeekBar  seekMasterGain, seekNoiseGate, seekCompRatio, seekCompThresh;
    private TextView tvMasterGain, tvNoiseGate, tvCompRatio, tvCompThresh, tvLatency;
    private LineChart eqChart;

    // Latency update runnable
    private final Runnable latencyUpdater = new Runnable() {
        @Override public void run() {
            if (mServiceBound && mServiceBinder != null) {
                float ms = mServiceBinder.getEngine().getLatencyMs();
                tvLatency.setText(String.format("Latency: %.1f ms", ms));
            }
            tvLatency.postDelayed(this, 1000);
        }
    };

    private boolean mIsRunning = false;

    // ── Activity lifecycle ────────────────────────────────────────────────────

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        initViews();
        loadPreferences();
        setupEqChart();
        setupControls();

        // Check / request mic permission immediately
        if (!hasMicPermission()) {
            ActivityCompat.requestPermissions(this,
                    new String[]{Manifest.permission.RECORD_AUDIO},
                    PERM_REQUEST);
        }
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        tvLatency.removeCallbacks(latencyUpdater);
        if (mServiceBound) {
            unbindService(mServiceConnection);
        }
    }

    // ── Permission handling ───────────────────────────────────────────────────

    @Override
    public void onRequestPermissionsResult(int requestCode,
                                           @NonNull String[] permissions,
                                           @NonNull int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == PERM_REQUEST) {
            boolean granted = grantResults.length > 0
                    && grantResults[0] == PackageManager.PERMISSION_GRANTED;
            if (!granted) {
                Toast.makeText(this,
                    "Microphone permission is required for the hearing aid to work.",
                    Toast.LENGTH_LONG).show();
                btnStartStop.setEnabled(false);
            }
        }
    }

    private boolean hasMicPermission() {
        return ContextCompat.checkSelfPermission(this,
                Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED;
    }

    // ── UI init ───────────────────────────────────────────────────────────────

    private void initViews() {
        btnStartStop    = findViewById(R.id.btn_start_stop);
        switchBypass    = findViewById(R.id.switch_bypass);
        seekMasterGain  = findViewById(R.id.seek_master_gain);
        seekNoiseGate   = findViewById(R.id.seek_noise_gate);
        seekCompRatio   = findViewById(R.id.seek_comp_ratio);
        seekCompThresh  = findViewById(R.id.seek_comp_thresh);
        tvMasterGain    = findViewById(R.id.tv_master_gain);
        tvNoiseGate     = findViewById(R.id.tv_noise_gate);
        tvCompRatio     = findViewById(R.id.tv_comp_ratio);
        tvCompThresh    = findViewById(R.id.tv_comp_thresh);
        tvLatency       = findViewById(R.id.tv_latency);
        eqChart         = findViewById(R.id.chart_eq);
    }

    // ── EQ Chart ─────────────────────────────────────────────────────────────

    private void setupEqChart() {
        // Style the chart to look like a professional audiogram
        eqChart.getDescription().setEnabled(false);
        eqChart.setTouchEnabled(true);
        eqChart.setDragEnabled(false);
        eqChart.setScaleEnabled(false);
        eqChart.setPinchZoom(false);
        eqChart.setDrawGridBackground(true);
        eqChart.setGridBackgroundColor(0xFF1A1A2E);
        eqChart.setBackgroundColor(0xFF1A1A2E);
        eqChart.getLegend().setEnabled(false);
        eqChart.setExtraTopOffset(10f);
        eqChart.setExtraBottomOffset(10f);

        // X axis — frequency labels
        XAxis xAxis = eqChart.getXAxis();
        xAxis.setPosition(XAxis.XAxisPosition.BOTTOM);
        xAxis.setDrawGridLines(true);
        xAxis.setGridColor(0x334444FF);
        xAxis.setTextColor(0xFFAAAAFF);
        xAxis.setValueFormatter((value, axis) -> {
            int idx = (int) value;
            return (idx >= 0 && idx < FREQ_LABELS.length) ? FREQ_LABELS[idx] : "";
        });
        xAxis.setGranularity(1f);

        // Left Y axis — gain in dB (-20 to +40)
        YAxis leftAxis = eqChart.getAxisLeft();
        leftAxis.setAxisMinimum(-20f);
        leftAxis.setAxisMaximum(40f);
        leftAxis.setDrawGridLines(true);
        leftAxis.setGridColor(0x334444FF);
        leftAxis.setTextColor(0xFFAAAAFF);
        leftAxis.setValueFormatter((value, axis) ->
                String.format("%+.0f dB", value));
        leftAxis.addLimitLine(createLimitLine(0f)); // 0 dB reference line

        eqChart.getAxisRight().setEnabled(false);

        refreshChartData();

        // Custom touch listener: allow dragging data points vertically
        eqChart.setOnChartGestureListener(new EqGestureListener());
    }

    /** Called when mEqGains changes — rebuilds the chart dataset */
    private void refreshChartData() {
        ArrayList<Entry> entries = new ArrayList<>();
        for (int i = 0; i < 10; i++) {
            entries.add(new Entry(i, mEqGains[i]));
        }

        LineDataSet dataset = new LineDataSet(entries, "EQ");
        dataset.setColor(0xFF00E5FF);
        dataset.setCircleColor(0xFF00E5FF);
        dataset.setCircleHoleColor(0xFF1A1A2E);
        dataset.setCircleRadius(8f);
        dataset.setLineWidth(2.5f);
        dataset.setDrawValues(true);
        dataset.setValueTextColor(0xFFFFFFFF);
        dataset.setValueTextSize(10f);
        dataset.setValueFormatter((value, entry, dataSetIndex, viewPortHandler) ->
                String.format("%+.0f", value));
        dataset.setMode(LineDataSet.Mode.CUBIC_BEZIER);
        dataset.setDrawFilled(true);
        dataset.setFillColor(0xFF00E5FF);
        dataset.setFillAlpha(30);

        eqChart.setData(new LineData(dataset));
        eqChart.invalidate();
    }

    private com.github.mikephil.charting.components.LimitLine createLimitLine(float value) {
        com.github.mikephil.charting.components.LimitLine ll =
                new com.github.mikephil.charting.components.LimitLine(value, "0 dB");
        ll.setLineColor(0xFF666688);
        ll.setLineWidth(1f);
        ll.setTextColor(0xFF8888AA);
        ll.setTextSize(9f);
        return ll;
    }

    /**
     * Custom gesture listener: converts touch Y position on the chart
     * to a dB gain value and updates the nearest band.
     */
    private class EqGestureListener implements OnChartGestureListener {
        private int  mDragBand = -1;
        private boolean mDragging = false;

        @Override
        public void onChartGestureStart(MotionEvent me,
                ChartTouchListener.ChartGesture lastPerformedGesture) {
            mDragBand = nearestBand(me.getX());
            mDragging = true;
        }

        @Override
        public void onChartGestureEnd(MotionEvent me,
                ChartTouchListener.ChartGesture lastPerformedGesture) {
            mDragging = false;
            // Send all bands in one JNI call when user lifts finger
            if (mServiceBound && mServiceBinder != null) {
                mServiceBinder.getEngine().setAllEqBands(mEqGains);
            }
            savePreferences();
        }

        @Override
        public void onChartLongPressed(MotionEvent me) {}
        @Override
        public void onChartDoubleTapped(MotionEvent me) {
            // Double-tap resets the band to 0 dB
            int band = nearestBand(me.getX());
            mEqGains[band] = 0f;
            refreshChartData();
            sendEqUpdate();
        }
        @Override public void onChartSingleTapped(MotionEvent me) {}
        @Override public void onChartFling(MotionEvent me1, MotionEvent me2,
                float vx, float vy) {}
        @Override public void onChartScale(MotionEvent me, float sX, float sY) {}
        @Override
        public void onChartTranslate(MotionEvent me, float dX, float dY) {
            if (!mDragging || mDragBand < 0) return;
            // Convert touch Y → dB
            float gainDb = eqChart.getAxisLeft().getAxisMaximum()
                    - (me.getY() / eqChart.getHeight())
                      * (eqChart.getAxisLeft().getAxisMaximum()
                         - eqChart.getAxisLeft().getAxisMinimum());
            gainDb = Math.max(-20f, Math.min(40f, gainDb));
            mEqGains[mDragBand] = gainDb;
            refreshChartData();
        }

        private int nearestBand(float touchX) {
            float bandWidth = eqChart.getWidth() / 10f;
            int band = (int)(touchX / bandWidth);
            return Math.max(0, Math.min(9, band));
        }
    }

    // ── Controls setup ────────────────────────────────────────────────────────

    private void setupControls() {
        // Start / Stop button
        btnStartStop.setOnClickListener(v -> {
            if (!mIsRunning) startProcessing();
            else             stopProcessing();
        });

        // Bypass switch
        switchBypass.setOnCheckedChangeListener((btn, checked) -> {
            if (mServiceBound && mServiceBinder != null) {
                mServiceBinder.getEngine().setBypass(checked);
            }
        });

        // Master gain SeekBar (0–60 dB, default 20 = 0 dB offset)
        seekMasterGain.setMax(60);
        seekMasterGain.setProgress(20);
        updateMasterGainLabel(20);
        seekMasterGain.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar s, int p, boolean u) {
                float db = p - 20f; // -20 to +40 dB
                updateMasterGainLabel(p);
                if (mServiceBound && mServiceBinder != null)
                    mServiceBinder.getEngine().setMasterGain(db);
                savePreferences();
            }
            @Override public void onStartTrackingTouch(SeekBar s) {}
            @Override public void onStopTrackingTouch(SeekBar s) {}
        });

        // Noise gate threshold SeekBar (-80 to -20 dB)
        seekNoiseGate.setMax(60);
        seekNoiseGate.setProgress(30); // default = -50 dB
        updateNoiseGateLabel(30);
        seekNoiseGate.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar s, int p, boolean u) {
                float db = -(80 - p);  // -80 to -20 dB
                updateNoiseGateLabel(p);
                if (mServiceBound && mServiceBinder != null)
                    mServiceBinder.getEngine().setNoiseGateThreshold(db);
                savePreferences();
            }
            @Override public void onStartTrackingTouch(SeekBar s) {}
            @Override public void onStopTrackingTouch(SeekBar s) {}
        });

        // Compressor threshold SeekBar (-60 to 0 dB)
        seekCompThresh.setMax(60);
        seekCompThresh.setProgress(30); // default = -30 dB
        updateCompThreshLabel(30);
        seekCompThresh.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar s, int p, boolean u) {
                float db = p - 60f;  // -60 to 0 dB
                updateCompThreshLabel(p);
                sendCompressorUpdate();
                savePreferences();
            }
            @Override public void onStartTrackingTouch(SeekBar s) {}
            @Override public void onStopTrackingTouch(SeekBar s) {}
        });

        // Compressor ratio SeekBar (1:1 to 10:1)
        seekCompRatio.setMax(90);
        seekCompRatio.setProgress(20); // default = 3:1
        updateCompRatioLabel(20);
        seekCompRatio.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar s, int p, boolean u) {
                updateCompRatioLabel(p);
                sendCompressorUpdate();
                savePreferences();
            }
            @Override public void onStartTrackingTouch(SeekBar s) {}
            @Override public void onStopTrackingTouch(SeekBar s) {}
        });
    }

    // ── Service start/stop ────────────────────────────────────────────────────

    private void startProcessing() {
        if (!hasMicPermission()) {
            Toast.makeText(this, "Microphone permission required!", Toast.LENGTH_SHORT).show();
            return;
        }
        Intent intent = new Intent(this, AudioProcessingService.class);
        ContextCompat.startForegroundService(this, intent);
        bindService(intent, mServiceConnection, Context.BIND_AUTO_CREATE);
        mIsRunning = true;
        btnStartStop.setText("■  Stop");
        btnStartStop.setBackgroundColor(0xFFE53935);
    }

    private void stopProcessing() {
        tvLatency.removeCallbacks(latencyUpdater);
        if (mServiceBound) {
            unbindService(mServiceConnection);
            mServiceBound = false;
        }
        stopService(new Intent(this, AudioProcessingService.class));
        mIsRunning = false;
        btnStartStop.setText("▶  Start Hearing Aid");
        btnStartStop.setBackgroundColor(0xFF00897B);
        tvLatency.setText("Latency: — ms");
    }

    // ── Apply all settings to native engine ──────────────────────────────────

    private void applyAllSettings() {
        if (!mServiceBound || mServiceBinder == null) return;
        NativeAudioEngine engine = mServiceBinder.getEngine();
        engine.setAllEqBands(mEqGains);
        float masterDb = seekMasterGain.getProgress() - 20f;
        engine.setMasterGain(masterDb);
        float ngDb = seekNoiseGate.getProgress() - 80f;
        engine.setNoiseGateThreshold(ngDb);
        sendCompressorUpdate();
        engine.setBypass(switchBypass.isChecked());
    }

    private void sendEqUpdate() {
        if (mServiceBound && mServiceBinder != null)
            mServiceBinder.getEngine().setAllEqBands(mEqGains);
        savePreferences();
    }

    private void sendCompressorUpdate() {
        if (!mServiceBound || mServiceBinder == null) return;
        float threshDb = seekCompThresh.getProgress() - 60f;
        float ratio    = 1f + seekCompRatio.getProgress() / 10f;
        mServiceBinder.getEngine().setCompressor(threshDb, ratio, 5f, 100f);
    }

    private void startLatencyUpdater() {
        tvLatency.post(latencyUpdater);
    }

    // ── Persistence ───────────────────────────────────────────────────────────

    private void savePreferences() {
        SharedPreferences.Editor ed = getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit();
        for (int i = 0; i < 10; i++) ed.putFloat("eq_" + i, mEqGains[i]);
        ed.putInt("master_gain", seekMasterGain.getProgress());
        ed.putInt("noise_gate",  seekNoiseGate.getProgress());
        ed.putInt("comp_thresh", seekCompThresh.getProgress());
        ed.putInt("comp_ratio",  seekCompRatio.getProgress());
        ed.apply();
    }

    private void loadPreferences() {
        SharedPreferences p = getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
        for (int i = 0; i < 10; i++) mEqGains[i] = p.getFloat("eq_" + i, 0f);
        seekMasterGain.setProgress(p.getInt("master_gain", 20));
        seekNoiseGate .setProgress(p.getInt("noise_gate",  30));
        seekCompThresh.setProgress(p.getInt("comp_thresh", 30));
        seekCompRatio .setProgress(p.getInt("comp_ratio",  20));
        updateMasterGainLabel(seekMasterGain.getProgress());
        updateNoiseGateLabel(seekNoiseGate.getProgress());
        updateCompThreshLabel(seekCompThresh.getProgress());
        updateCompRatioLabel(seekCompRatio.getProgress());
    }

    // ── Label updaters ────────────────────────────────────────────────────────

    private void updateMasterGainLabel(int p)  {
        tvMasterGain.setText(String.format("Master Gain: %+.0f dB", (float)(p - 20)));
    }
    private void updateNoiseGateLabel(int p)   {
        tvNoiseGate.setText(String.format("Noise Gate: %d dB", p - 80));
    }
    private void updateCompThreshLabel(int p)  {
        tvCompThresh.setText(String.format("Compressor Threshold: %d dB", p - 60));
    }
    private void updateCompRatioLabel(int p)   {
        float ratio = 1f + p / 10f;
        tvCompRatio.setText(String.format("Compression Ratio: %.1f:1", ratio));
    }
}
