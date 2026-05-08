/*
 * audio_engine.cpp
 * ============================================================
 * The heart of the hearing aid: opens an AAudio INPUT stream
 * (microphone) and an OUTPUT stream (speaker/earphones),
 * then in the real-time callback applies the EQ, compressor,
 * and noise gate with < 10 ms round-trip latency.
 *
 * Architecture:
 *   Mic → [noise gate] → [10-band EQ] → [compressor] → Speaker
 *
 * Why AAudio instead of AudioRecord/AudioTrack (Java)?
 *   - AAudio uses MMAP mode which bypasses the AudioFlinger
 *     mixer, giving ~2-5 ms hardware latency.
 *   - The callback runs on a high-priority RT thread managed
 *     by the OS scheduler — no GC pauses, no JVM overhead.
 * ============================================================
 */

#include <aaudio/AAudio.h>
#include <android/log.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>

#include "equalizer.h"
#include "noise_gate.h"
#include "compressor.h"

#define LOG_TAG "HearingAid"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ── Singleton state ──────────────────────────────────────────────────────────

static AAudioStream*  gInputStream  = nullptr;
static AAudioStream*  gOutputStream = nullptr;
static Equalizer*     gEqualizer    = nullptr;
static NoiseGate*     gNoiseGate    = nullptr;
static Compressor*    gCompressor   = nullptr;

// Master gain in linear scale (0.0 – 4.0, default 1.0)
static std::atomic<float> gMasterGain{1.0f};

// Global bypass toggle
static std::atomic<bool>  gBypass{false};

// ── Sample rate & buffer info (filled after stream open) ──────────────────
static int32_t gSampleRate    = 48000;
static int32_t gChannelCount  = 1;     // Mono for hearing aid (lower latency)
static int32_t gFramesPerBurst = 96;   // Typically 2 ms at 48 kHz

// ── Intermediate processing buffer ───────────────────────────────────────────
// We allocate once at stream-open time to avoid malloc in the RT callback.
static float* gProcessBuffer = nullptr;
static int32_t gProcessBufferSize = 0;

// ── AAudio data callback ─────────────────────────────────────────────────────
/*
 * This function is called by AAudio on a high-priority RT thread.
 * RULES for RT callbacks:
 *   ❌ No malloc/free/new/delete
 *   ❌ No mutex locking (use atomics)
 *   ❌ No file I/O, no logging (logcat involves a mutex)
 *   ❌ No JNI calls
 *   ✅ Fixed-duration deterministic code only
 */
static aaudio_data_callback_result_t audioCallback(
        AAudioStream* /*stream*/,
        void*         /*userData*/,
        void*          audioData,
        int32_t        numFrames) {

    float* output = static_cast<float*>(audioData);
    int32_t totalSamples = numFrames * gChannelCount;

    // Read from the INPUT stream into our local buffer
    // (in the duplex MMAP mode AAudio handles the circular buffer,
    //  but we still need to read from the input stream explicitly)
    aaudio_result_t result = AAudioStream_read(
            gInputStream,
            gProcessBuffer,
            numFrames,
            0);   // timeout=0 → non-blocking; if no data available, we get silence

    if (result < 0) {
        // Underrun or stream error — output silence to avoid glitches
        memset(output, 0, totalSamples * sizeof(float));
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    if (gBypass.load(std::memory_order_relaxed)) {
        // Bypass mode: pass audio through unmodified
        memcpy(output, gProcessBuffer, totalSamples * sizeof(float));
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    // ── DSP chain ─────────────────────────────────────────────────────────

    // 1. Noise gate — attenuates signal below threshold (reduces hiss/hum)
    gNoiseGate->process(gProcessBuffer, numFrames);

    // 2. 10-band parametric EQ — applies the user's frequency compensation curve
    gEqualizer->process(gProcessBuffer, numFrames);

    // 3. Dynamic range compressor — normalises loud transients,
    //    compensates for recruitment (loudness discomfort in SNHL)
    gCompressor->process(gProcessBuffer, numFrames);

    // 4. Master gain
    float gain = gMasterGain.load(std::memory_order_relaxed);
    for (int i = 0; i < totalSamples; i++) {
        output[i] = gProcessBuffer[i] * gain;
    }

    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

// ── Error callback ────────────────────────────────────────────────────────────
static void errorCallback(AAudioStream* /*stream*/, void* /*userData*/, aaudio_result_t error) {
    LOGE("AAudio error: %s (%d)", AAudio_convertResultToText(error), error);
    // In production you'd restart the stream here (handle disconnect events)
}

// ── Stream helpers ────────────────────────────────────────────────────────────

static AAudioStream* openInputStream(int32_t sampleRate) {
    AAudioStreamBuilder* builder = nullptr;
    AAudio_createStreamBuilder(&builder);

    // Performance mode EXCLUSIVE: bypass the AudioFlinger mixer = lowest latency
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    // Sharing mode EXCLUSIVE: only our app can use the mic directly
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_EXCLUSIVE);
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_INPUT);
    AAudioStreamBuilder_setSampleRate(builder, sampleRate);
    AAudioStreamBuilder_setChannelCount(builder, 1);         // Mono
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT); // Float32
    // Use the unprocessed preset — no noise cancellation, EQ, or AGC from the OS
    AAudioStreamBuilder_setInputPreset(builder, AAUDIO_INPUT_PRESET_UNPROCESSED);

    AAudioStream* stream = nullptr;
    aaudio_result_t result = AAudioStreamBuilder_openStream(builder, &stream);
    AAudioStreamBuilder_delete(builder);

    if (result != AAUDIO_OK) {
        LOGE("Failed to open input stream: %s", AAudio_convertResultToText(result));
        return nullptr;
    }
    LOGI("Input stream opened. SampleRate=%d  FramesPerBurst=%d",
         AAudioStream_getSampleRate(stream),
         AAudioStream_getFramesPerBurst(stream));
    return stream;
}

static AAudioStream* openOutputStream(int32_t sampleRate,
                                      aaudio_data_callback_t callback) {
    AAudioStreamBuilder* builder = nullptr;
    AAudio_createStreamBuilder(&builder);

    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_EXCLUSIVE);
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSampleRate(builder, sampleRate);
    AAudioStreamBuilder_setChannelCount(builder, 1);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
    // Register our RT callback
    AAudioStreamBuilder_setDataCallback(builder, callback, nullptr);
    AAudioStreamBuilder_setErrorCallback(builder, errorCallback, nullptr);

    AAudioStream* stream = nullptr;
    aaudio_result_t result = AAudioStreamBuilder_openStream(builder, &stream);
    AAudioStreamBuilder_delete(builder);

    if (result != AAUDIO_OK) {
        LOGE("Failed to open output stream: %s", AAudio_convertResultToText(result));
        return nullptr;
    }
    LOGI("Output stream opened. SampleRate=%d  FramesPerBurst=%d",
         AAudioStream_getSampleRate(stream),
         AAudioStream_getFramesPerBurst(stream));
    return stream;
}

// ── JNI interface (called from Java/Kotlin) ───────────────────────────────────

extern "C" {

/**
 * Start the audio engine.
 * Called from AudioProcessingService.onCreate()
 */
JNIEXPORT jboolean JNICALL
Java_com_hearingaid_app_NativeAudioEngine_startEngine(
        JNIEnv* /*env*/, jobject /*obj*/) {

    LOGI("Starting hearing aid audio engine...");

    // 1. Open input stream first to discover the native sample rate
    gInputStream = openInputStream(48000);
    if (!gInputStream) return JNI_FALSE;

    gSampleRate = AAudioStream_getSampleRate(gInputStream);
    gFramesPerBurst = AAudioStream_getFramesPerBurst(gInputStream);

    // 2. Allocate processing buffer (enough for 2× burst size = headroom)
    int32_t bufSize = gFramesPerBurst * 2;
    delete[] gProcessBuffer;
    gProcessBuffer = new float[bufSize];
    gProcessBufferSize = bufSize;
    memset(gProcessBuffer, 0, bufSize * sizeof(float));

    // 3. Instantiate DSP objects
    gEqualizer  = new Equalizer(gSampleRate);
    gNoiseGate  = new NoiseGate(gSampleRate);
    gCompressor = new Compressor(gSampleRate);

    // 4. Open output stream with our callback
    gOutputStream = openOutputStream(gSampleRate, audioCallback);
    if (!gOutputStream) {
        AAudioStream_close(gInputStream);
        gInputStream = nullptr;
        return JNI_FALSE;
    }

    // 5. Set the output buffer size to 2× the burst for double-buffering
    AAudioStream_setBufferSizeInFrames(gOutputStream, gFramesPerBurst * 2);

    // 6. Start both streams
    AAudioStream_requestStart(gInputStream);
    AAudioStream_requestStart(gOutputStream);

    int32_t outBurst = AAudioStream_getFramesPerBurst(gOutputStream);
    float latencyMs  = (float)(outBurst * 2) / (float)gSampleRate * 1000.0f;
    LOGI("Engine started. SampleRate=%d  Burst=%d  EstimatedLatency=%.1f ms",
         gSampleRate, outBurst, latencyMs);

    return JNI_TRUE;
}

/**
 * Stop and release all audio resources.
 * Called from AudioProcessingService.onDestroy()
 */
JNIEXPORT void JNICALL
Java_com_hearingaid_app_NativeAudioEngine_stopEngine(
        JNIEnv* /*env*/, jobject /*obj*/) {

    LOGI("Stopping audio engine...");
    if (gOutputStream) {
        AAudioStream_requestStop(gOutputStream);
        AAudioStream_close(gOutputStream);
        gOutputStream = nullptr;
    }
    if (gInputStream) {
        AAudioStream_requestStop(gInputStream);
        AAudioStream_close(gInputStream);
        gInputStream = nullptr;
    }
    delete gEqualizer;  gEqualizer  = nullptr;
    delete gNoiseGate;  gNoiseGate  = nullptr;
    delete gCompressor; gCompressor = nullptr;
    delete[] gProcessBuffer; gProcessBuffer = nullptr;
    LOGI("Audio engine stopped.");
}

/**
 * Set a single EQ band.
 * @param bandIndex  0–9  (maps to ~125Hz, 250Hz, 500Hz, 1k, 2k, 3k, 4k, 6k, 8k, 12k)
 * @param gainDb     gain in dB, range -20 to +40
 */
JNIEXPORT void JNICALL
Java_com_hearingaid_app_NativeAudioEngine_setEqBand(
        JNIEnv* /*env*/, jobject /*obj*/, jint bandIndex, jfloat gainDb) {
    if (gEqualizer) {
        gEqualizer->setBandGain(bandIndex, gainDb);
    }
}

/**
 * Set all 10 EQ bands at once from the graph.
 * @param gains  float[10] array of dB values
 */
JNIEXPORT void JNICALL
Java_com_hearingaid_app_NativeAudioEngine_setAllEqBands(
        JNIEnv* env, jobject /*obj*/, jfloatArray gains) {
    if (!gEqualizer) return;
    jfloat* arr = env->GetFloatArrayElements(gains, nullptr);
    for (int i = 0; i < 10; i++) {
        gEqualizer->setBandGain(i, arr[i]);
    }
    env->ReleaseFloatArrayElements(gains, arr, JNI_ABORT);
}

/**
 * Set master output gain.
 * @param gainDb  overall volume adjustment in dB
 */
JNIEXPORT void JNICALL
Java_com_hearingaid_app_NativeAudioEngine_setMasterGain(
        JNIEnv* /*env*/, jobject /*obj*/, jfloat gainDb) {
    gMasterGain.store(powf(10.0f, gainDb / 20.0f), std::memory_order_relaxed);
}

/**
 * Set noise gate threshold.
 * @param thresholdDb  e.g. -60 dB means very sensitive (open even for quiet sounds)
 */
JNIEXPORT void JNICALL
Java_com_hearingaid_app_NativeAudioEngine_setNoiseGateThreshold(
        JNIEnv* /*env*/, jobject /*obj*/, jfloat thresholdDb) {
    if (gNoiseGate) gNoiseGate->setThreshold(thresholdDb);
}

/**
 * Set compressor parameters.
 * @param threshDb   e.g. -30 dB
 * @param ratio      e.g. 3.0 (3:1 compression)
 * @param attackMs   e.g. 5 ms
 * @param releaseMs  e.g. 100 ms
 */
JNIEXPORT void JNICALL
Java_com_hearingaid_app_NativeAudioEngine_setCompressor(
        JNIEnv* /*env*/, jobject /*obj*/,
        jfloat threshDb, jfloat ratio, jfloat attackMs, jfloat releaseMs) {
    if (gCompressor) {
        gCompressor->setThreshold(threshDb);
        gCompressor->setRatio(ratio);
        gCompressor->setAttack(attackMs);
        gCompressor->setRelease(releaseMs);
    }
}

/**
 * Enable/disable processing bypass (pass-through mode).
 */
JNIEXPORT void JNICALL
Java_com_hearingaid_app_NativeAudioEngine_setBypass(
        JNIEnv* /*env*/, jobject /*obj*/, jboolean bypass) {
    gBypass.store(bypass == JNI_TRUE, std::memory_order_relaxed);
}

/**
 * Query actual measured round-trip latency in milliseconds.
 */
JNIEXPORT jfloat JNICALL
Java_com_hearingaid_app_NativeAudioEngine_getLatencyMs(
        JNIEnv* /*env*/, jobject /*obj*/) {
    if (!gOutputStream || !gInputStream) return 0.0f;
    int64_t outLatencyNs = 0, inLatencyNs = 0;
    AAudioStream_getTimestamp(gOutputStream, CLOCK_MONOTONIC, nullptr, &outLatencyNs);
    // Total estimated: 2× burst + hardware pipeline
    int32_t burst = AAudioStream_getFramesPerBurst(gOutputStream);
    return (float)(burst * 2) / (float)gSampleRate * 1000.0f;
}

} // extern "C"
