/*
 * compressor.h
 * ============================================================
 * Feed-forward dynamic range compressor.
 *
 * Purpose in a hearing aid:
 *   People with sensorineural hearing loss (SNHL) often suffer
 *   from "recruitment" — the dynamic range of comfortable
 *   hearing is severely narrowed (e.g. 30 dB instead of 120 dB).
 *   A compressor with a soft knee maps the wide dynamic range
 *   of real-world sound into the listener's usable range.
 *
 * Algorithm:
 *   1. Level detection via RMS with attack/release smoothing.
 *   2. Gain computation in log domain:
 *      CS  = min(0, (level - threshold) * (1/ratio - 1))
 *      gain_dB = CS  (soft-knee applied around threshold)
 *   3. Apply gain in linear domain.
 * ============================================================
 */
#pragma once
#include <atomic>
#include <cmath>

class Compressor {
public:
    explicit Compressor(int sampleRate)
        : mSampleRate(sampleRate) {
        setThreshold(-30.f);
        setRatio(3.0f);
        setAttack(5.0f);
        setRelease(100.0f);
        setKnee(6.0f);
        setMakeupGain(6.0f);
    }

    void setThreshold (float db)  { mThreshDb.store(db,   std::memory_order_relaxed); }
    void setRatio     (float r)   { mRatio.store(r,        std::memory_order_relaxed); }
    void setKnee      (float db)  { mKneeDb.store(db,      std::memory_order_relaxed); }
    void setMakeupGain(float db)  { mMakeupLin.store(powf(10.f, db/20.f), std::memory_order_relaxed); }

    void setAttack (float ms) {
        mAttackCoeff.store(expf(-1.f / (ms * 0.001f * mSampleRate)), std::memory_order_relaxed);
    }
    void setRelease(float ms) {
        mReleaseCoeff.store(expf(-1.f / (ms * 0.001f * mSampleRate)), std::memory_order_relaxed);
    }

    // In-place processing — RT thread
    void process(float* buf, int frames) {
        const float thresh  = mThreshDb.load(std::memory_order_relaxed);
        const float ratio   = mRatio.load(std::memory_order_relaxed);
        const float knee    = mKneeDb.load(std::memory_order_relaxed);
        const float makeup  = mMakeupLin.load(std::memory_order_relaxed);
        const float att     = mAttackCoeff.load(std::memory_order_relaxed);
        const float rel     = mReleaseCoeff.load(std::memory_order_relaxed);
        const float halfKnee = knee * 0.5f;

        for (int n = 0; n < frames; n++) {
            float x    = buf[n];
            float xAbs = fabsf(x);

            // ── Level detection (RMS estimate) ───────────────
            mEnv = (xAbs > mEnv) ? (att  * mEnv + (1.f - att)  * xAbs)
                                  : (rel  * mEnv + (1.f - rel)  * xAbs);

            // Convert to dB (floor at -120 dB)
            float levelDb = (mEnv > 1e-6f) ? 20.f * log10f(mEnv) : -120.f;

            // ── Gain computer (soft-knee) ────────────────────
            float gainDb = 0.f;
            float overshoot = levelDb - thresh;

            if (overshoot < -halfKnee) {
                gainDb = 0.f;   // below knee: no compression
            } else if (overshoot <= halfKnee) {
                // Soft-knee zone
                float t = (overshoot + halfKnee) / knee;
                gainDb = (1.f/ratio - 1.f) * t * t * halfKnee;
            } else {
                // Above knee: full compression
                gainDb = overshoot * (1.f/ratio - 1.f);
            }

            // ── Apply gain ───────────────────────────────────
            float gainLin = powf(10.f, gainDb / 20.f) * makeup;
            buf[n] = x * gainLin;
        }
    }

private:
    int mSampleRate;
    std::atomic<float> mThreshDb{-30.f};
    std::atomic<float> mRatio{3.f};
    std::atomic<float> mKneeDb{6.f};
    std::atomic<float> mMakeupLin{1.f};
    std::atomic<float> mAttackCoeff{0.99f};
    std::atomic<float> mReleaseCoeff{0.9f};

    float mEnv = 0.f;   // envelope follower state
};
