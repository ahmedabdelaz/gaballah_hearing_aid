/*
 * equalizer.cpp
 * ============================================================
 * 10-band parametric IIR equalizer using second-order peak
 * (bell) filters derived from the Audio EQ Cookbook.
 *
 * Why IIR instead of FIR?
 *   IIR: O(5) multiplies per sample per band → ~50 ops/sample total.
 *   A linear-phase FIR with equivalent frequency resolution would
 *   need hundreds of taps and add 5–20 ms of latency. IIR gives us
 *   near-zero additional latency with excellent selectivity.
 *
 * Thread safety strategy:
 *   - setBandGain() stores the new dB value atomically.
 *   - process() checks each band's "applied" value; if different,
 *     it recomputes the filter coefficients inline.
 *   This avoids any mutex in the RT path.
 * ============================================================
 */
#include "equalizer.h"
#include <cstring>

// ── Constructor ───────────────────────────────────────────────────────────────
Equalizer::Equalizer(int sampleRate) : mSampleRate(sampleRate) {
    for (int i = 0; i < NUM_BANDS; i++) {
        mGainDb[i].store(0.0f, std::memory_order_relaxed);
        mAppliedGain[i] = 1e9f;  // sentinel → force recompute on first process()
    }
}

// ── API: set a band gain from the Java thread ─────────────────────────────────
void Equalizer::setBandGain(int band, float gainDb) {
    if (band < 0 || band >= NUM_BANDS) return;
    // Clamp to safe range: -20 dB to +40 dB
    if (gainDb < -20.f) gainDb = -20.f;
    if (gainDb >  40.f) gainDb =  40.f;
    mGainDb[band].store(gainDb, std::memory_order_release);
}

// ── Internal: recompute IIR coefficients for band i ──────────────────────────
/*
 * Peak (bell) filter from Audio EQ Cookbook:
 *   b0 = 1 + alpha*A
 *   b1 = -2*cos(w0)
 *   b2 = 1 - alpha*A
 *   a0 = 1 + alpha/A
 *   a1 = -2*cos(w0)
 *   a2 = 1 - alpha/A
 * where:
 *   w0    = 2*pi*f0/Fs
 *   A     = 10^(dBgain/40)
 *   alpha = sin(w0)/(2*Q)
 */
BiQuad Equalizer::computePeakFilter(float freq, float q, float gainDb, int fs) {
    BiQuad bq;
    float w0    = 2.0f * (float)M_PI * freq / (float)fs;
    float A     = powf(10.0f, gainDb / 40.0f);
    float alpha = sinf(w0) / (2.0f * q);
    float cosw0 = cosf(w0);

    float a0 = 1.0f + alpha / A;
    bq.b0 =  (1.0f + alpha * A) / a0;
    bq.b1 =  (-2.0f * cosw0)    / a0;
    bq.b2 =  (1.0f - alpha * A) / a0;
    bq.a1 =  (-2.0f * cosw0)    / a0;
    bq.a2 =  (1.0f - alpha / A) / a0;
    bq.w1 = 0.f;
    bq.w2 = 0.f;
    return bq;
}

void Equalizer::recomputeCoeffs(int i) {
    float gainDb = mGainDb[i].load(std::memory_order_acquire);
    mAppliedGain[i] = gainDb;
    mFilters[i] = computePeakFilter(BAND_FREQS[i], BAND_Q[i], gainDb, mSampleRate);
    // Keep delay registers (w1, w2) at zero on coefficient change to avoid clicks
}

// ── RT: process audio buffer (called from audio callback) ────────────────────
void Equalizer::process(float* buffer, int numFrames) {
    // 1. Check for pending coefficient updates (no mutex — atomic load)
    for (int i = 0; i < NUM_BANDS; i++) {
        float newGain = mGainDb[i].load(std::memory_order_acquire);
        if (newGain != mAppliedGain[i]) {
            recomputeCoeffs(i);
        }
    }

    // 2. Apply all 10 filters in cascade (series connection)
    //    For each sample, run through all 10 biquad filters.
    //    This is more cache-friendly than filtering band-by-band.
    for (int n = 0; n < numFrames; n++) {
        float s = buffer[n];
        for (int i = 0; i < NUM_BANDS; i++) {
            s = mFilters[i].process(s);
        }
        buffer[n] = s;
    }
}
