/*
 * equalizer.h
 * 10-band parametric IIR equalizer.
 * Each band is a second-order peak/shelf filter (BiQuad).
 * Thread-safe: band gains are updated via atomic stores;
 * coefficients are recomputed lock-free with a "dirty" flag.
 */
#pragma once
#include <atomic>
#include <cmath>
#include <array>

// Number of EQ bands
static constexpr int NUM_BANDS = 10;

// Centre frequencies for each band (Hz) — typical audiogram frequencies
static constexpr float BAND_FREQS[NUM_BANDS] = {
    125.0f, 250.0f, 500.0f, 1000.0f, 2000.0f,
    3000.0f, 4000.0f, 6000.0f, 8000.0f, 12000.0f
};

// Q factor for each peak filter (bandwidth)
static constexpr float BAND_Q[NUM_BANDS] = {
    1.4f, 1.4f, 1.4f, 1.4f, 1.4f,
    1.4f, 1.4f, 1.4f, 1.4f, 1.4f
};

// BiQuad filter state (2 delay samples per channel)
struct BiQuad {
    // Coefficients: y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2]
    //                           - a1*y[n-1] - a2*y[n-2]
    float b0 = 1.f, b1 = 0.f, b2 = 0.f;
    float a1 = 0.f, a2 = 0.f;
    // Delay registers (direct form II transposed)
    float w1 = 0.f, w2 = 0.f;

    // Process a single sample inline — called 48000× per second
    inline float process(float x) {
        float y = b0 * x + w1;
        w1 = b1 * x - a1 * y + w2;
        w2 = b2 * x - a2 * y;
        return y;
    }
};

class Equalizer {
public:
    explicit Equalizer(int sampleRate);

    // Called from Java thread — atomic update, recomputes coeffs on next process()
    void setBandGain(int band, float gainDb);

    // Called from RT audio thread — processes a mono float buffer in-place
    void process(float* buffer, int numFrames);

private:
    int mSampleRate;
    std::array<std::atomic<float>, NUM_BANDS> mGainDb;
    std::array<float, NUM_BANDS>              mAppliedGain;
    std::array<BiQuad, NUM_BANDS>             mFilters;

    // Recompute coefficients for band i when gain changes
    void recomputeCoeffs(int i);

    // Compute peak filter coefficients (Audio EQ Cookbook, R. Bristow-Johnson)
    static BiQuad computePeakFilter(float freq, float q, float gainDb, int fs);
};
