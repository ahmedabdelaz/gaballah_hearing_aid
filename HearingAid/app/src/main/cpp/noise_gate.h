/*
 * noise_gate.h / noise_gate.cpp (combined for brevity)
 * ============================================================
 * A simple noise gate (squelch) that attenuates audio below
 * a configurable RMS threshold.
 *
 * In a hearing aid context this reduces the background hiss
 * that occurs when the microphone gain is very high.
 *
 * Parameters:
 *   threshold  – RMS level (dB) below which the gate closes
 *   attack     – time (ms) for gate to open  (default 1 ms)
 *   hold       – time (ms) gate stays open after signal drops
 *   release    – time (ms) for gate to close (default 20 ms)
 * ============================================================
 */
#pragma once
#include <atomic>
#include <cmath>

class NoiseGate {
public:
    explicit NoiseGate(int sampleRate)
        : mSampleRate(sampleRate),
          mThresholdDb(-50.0f),
          mThresholdLin(dbToLin(-50.0f)) {
        setAttack(1.0f);
        setHold(10.0f);
        setRelease(20.0f);
    }

    void setThreshold(float db) {
        mThresholdDb = db;
        mThresholdLin.store(dbToLin(db), std::memory_order_relaxed);
    }
    void setAttack (float ms) { mAttackCoeff  = expCoeff(ms); }
    void setHold   (float ms) { mHoldSamples  = (int)(ms * mSampleRate / 1000.f); }
    void setRelease(float ms) { mReleaseCoeff = expCoeff(ms); }

    // In-place processing — called from RT audio thread
    void process(float* buf, int frames) {
        float thresh  = mThresholdLin.load(std::memory_order_relaxed);
        float attack  = mAttackCoeff;
        float release = mReleaseCoeff;

        for (int n = 0; n < frames; n++) {
            float x = buf[n];
            // Estimate RMS via leaky integrator on squared signal
            mRms = 0.995f * mRms + 0.005f * x * x;
            float rmsLin = sqrtf(mRms);

            if (rmsLin >= thresh) {
                // Signal is above threshold → open gate
                mHoldCounter = mHoldSamples;
                mGain += attack * (1.0f - mGain);   // smooth open
            } else {
                if (mHoldCounter > 0) {
                    mHoldCounter--;
                    // hold phase — keep gain where it is
                } else {
                    // Signal dropped — close gate smoothly
                    mGain += release * (0.0f - mGain);
                }
            }
            buf[n] = x * mGain;
        }
    }

private:
    int   mSampleRate;
    float mThresholdDb;
    std::atomic<float> mThresholdLin;

    float mAttackCoeff  = 0.f;
    float mReleaseCoeff = 0.f;
    int   mHoldSamples  = 0;
    int   mHoldCounter  = 0;

    float mRms  = 0.f;    // running RMS estimate
    float mGain = 0.f;    // current gate gain [0..1]

    static float dbToLin(float db)  { return powf(10.f, db / 20.f); }
    // One-pole RC coefficient for a given time constant
    float expCoeff(float ms) {
        return 1.0f - expf(-1.0f / (ms * 0.001f * mSampleRate));
    }
};
