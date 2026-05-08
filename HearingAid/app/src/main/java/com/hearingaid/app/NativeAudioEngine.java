package com.hearingaid.app;

/**
 * NativeAudioEngine.java
 * ============================================================
 * JNI bridge between Java and the C++ audio engine.
 *
 * All native methods are declared here as 'native'.
 * The actual implementations live in audio_engine.cpp.
 * The method names MUST follow the JNI naming convention:
 *   Java_<package_underscored>_<className>_<methodName>
 * ============================================================
 */
public class NativeAudioEngine {

    // Load the compiled shared library at class-load time
    static {
        System.loadLibrary("hearingaid");  // loads libhearingaid.so
    }

    // ── Engine lifecycle ──────────────────────────────────────────────────

    /**
     * Initialise and start the AAudio streams.
     * Must be called before any other method.
     * @return true if both streams opened and started successfully
     */
    public native boolean startEngine();

    /**
     * Stop processing and release all audio resources.
     * Call when the app goes to background or is destroyed.
     */
    public native void stopEngine();

    // ── Equalizer control ─────────────────────────────────────────────────

    /**
     * Set gain for a single EQ band.
     * @param bandIndex  0–9 (125 Hz … 12 kHz)
     * @param gainDb     gain in dB, range [-20, +40]
     */
    public native void setEqBand(int bandIndex, float gainDb);

    /**
     * Set all 10 EQ bands in one JNI call — preferred for bulk updates
     * (e.g. when the user releases their finger from the graph).
     * @param gains  float[10] array, index 0 = 125 Hz … 9 = 12 kHz
     */
    public native void setAllEqBands(float[] gains);

    // ── Global gain ───────────────────────────────────────────────────────

    /**
     * Adjust the overall output level.
     * @param gainDb  e.g. +10 dB to make everything louder
     */
    public native void setMasterGain(float gainDb);

    // ── Noise gate ────────────────────────────────────────────────────────

    /**
     * @param thresholdDb  RMS level below which the gate attenuates (e.g. -50 dB)
     */
    public native void setNoiseGateThreshold(float thresholdDb);

    // ── Compressor ────────────────────────────────────────────────────────

    /**
     * @param threshDb   compression starts above this level (e.g. -30 dB)
     * @param ratio      compression ratio (e.g. 3.0 = 3:1)
     * @param attackMs   attack time in ms
     * @param releaseMs  release time in ms
     */
    public native void setCompressor(float threshDb, float ratio,
                                     float attackMs, float releaseMs);

    // ── Utility ───────────────────────────────────────────────────────────

    /**
     * Pass true to route mic → speaker unprocessed (diagnostic mode).
     */
    public native void setBypass(boolean bypass);

    /**
     * @return estimated round-trip latency in milliseconds
     */
    public native float getLatencyMs();
}
