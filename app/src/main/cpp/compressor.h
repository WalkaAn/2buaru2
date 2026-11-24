#pragma once
// DSP inti: compressor (RMS detector, soft knee ala "OverEasy") + limiter (peak),
// plus rantai stereo (opsi link L/R). Semua gain dihitung dalam dB, dihaluskan
// dengan attack/release satu kutub.

#include <algorithm>
#include <cmath>

#include "effects.h"

inline float dbToLin(float db) { return std::exp(db * 0.11512925f); }  // ln(10)/20

struct CompParams {
    float thresholdDb = -20.f;
    float ratio = 4.f;
    float attackMs = 10.f;
    float releaseMs = 200.f;
    float kneeDb = 6.f;
    float makeupDb = 0.f;
};

class Compressor {
public:
    float gr = 0.f;  // gain reduction saat ini dalam dB (<= 0)

    void reset() {
        ms = 0.f;
        gr = 0.f;
    }

    void prepare(float sampleRate, const CompParams& p) {
        atk = std::exp(-1.f / (std::max(p.attackMs, 0.05f) * 0.001f * sampleRate));
        rel = std::exp(-1.f / (std::max(p.releaseMs, 1.f) * 0.001f * sampleRate));
        rmsCoef = std::exp(-1.f / (0.005f * sampleRate));  // jendela RMS ~5 ms
    }

    // Mengembalikan gain linear (sudah termasuk makeup) untuk sampel x.
    float process(float x, const CompParams& p) {
        ms = rmsCoef * ms + (1.f - rmsCoef) * x * x;
        const float levelDb = 10.f * std::log10(ms + 1e-12f);
        const float over = levelDb - p.thresholdDb;
        const float slope = 1.f / std::max(p.ratio, 1.f) - 1.f;  // <= 0
        const float knee = p.kneeDb;

        float target;  // perubahan gain yang diinginkan (dB, <= 0)
        if (knee < 0.1f) {
            target = over > 0.f ? slope * over : 0.f;  // hard knee
        } else {
            const float half = 0.5f * knee;
            if (over <= -half) {
                target = 0.f;
            } else if (over >= half) {
                target = slope * over;
            } else {
                const float t = over + half;
                target = slope * t * t / (2.f * knee);  // soft knee
            }
        }

        gr = (target < gr) ? atk * gr + (1.f - atk) * target
                           : rel * gr + (1.f - rel) * target;
        return dbToLin(gr + p.makeupDb);
    }

private:
    float ms = 0.f, atk = 0.f, rel = 0.f, rmsCoef = 0.f;
};

class Limiter {
public:
    float gr = 0.f;  // dB (<= 0)

    void reset() { gr = 0.f; }

    void prepare(float sampleRate, float ceilingDb) {
        ceilDb = ceilingDb;
        ceilLin = dbToLin(ceilingDb);
        atk = std::exp(-1.f / (0.0005f * sampleRate));  // 0.5 ms
        rel = std::exp(-1.f / (0.080f * sampleRate));   // 80 ms
    }

    // Memperbarui detektor dan mengembalikan gain linear (tanpa clip).
    float gain(float x) {
        const float a = std::fabs(x) + 1e-9f;
        const float db = 20.f * std::log10(a);
        const float target = db > ceilDb ? ceilDb - db : 0.f;
        gr = (target < gr) ? atk * gr + (1.f - atk) * target
                           : rel * gr + (1.f - rel) * target;
        return dbToLin(gr);
    }

    // Pengaman terakhir: tanpa lookahead, puncak sangat cepat bisa lolos sesaat,
    // jadi dipotong keras di ceiling.
    float clip(float y) const { return std::max(-ceilLin, std::min(ceilLin, y)); }

    float process(float x) { return clip(x * gain(x)); }

private:
    float ceilDb = -1.f, ceilLin = 0.891f, atk = 0.f, rel = 0.f;
};

struct ProcParams {
    CompParams comp;
    float ceilDb = -1.f;
    bool limOn = true;
    bool link = true;     // L/R berbagi gain reduction (ambil yang terbesar)
    float outGain = 1.f;  // linear
    bool revOn = false;   // reverb di antara compressor dan limiter
};

// Rantai compressor -> limiter -> output gain untuk satu frame (mono atau stereo).
class StereoChain {
public:
    void reset() {
        cL.reset();
        cR.reset();
        lL.reset();
        lR.reset();
    }

    void prepare(float sampleRate, const ProcParams& p) {
        cL.prepare(sampleRate, p.comp);
        cR.prepare(sampleRate, p.comp);
        lL.prepare(sampleRate, p.ceilDb);
        lR.prepare(sampleRate, p.ceilDb);
    }

    // stereo=false: hanya l yang dipakai, yr dibuat sama dengan yl.
    // reduction: gain reduction total (compressor + limiter) dalam dB, positif.
    void process(float l, float r, bool stereo, const ProcParams& p,
                 float& yl, float& yr, float& reduction) {
        float gLeft = cL.process(l, p.comp);
        float gRight = 1.f;
        float red = -cL.gr;
        if (stereo) {
            gRight = cR.process(r, p.comp);
            red = std::max(red, -cR.gr);
            if (p.link) {
                const float g = std::min(gLeft, gRight);
                gLeft = gRight = g;
            }
        }
        yl = l * gLeft;
        yr = stereo ? r * gRight : yl;

        if (stereo && rev != nullptr && p.revOn) rev->process(yl, yr);

        if (p.limOn) {
            float limLeft = lL.gain(yl);
            float limRight = limLeft;
            float limRed = -lL.gr;
            if (stereo) {
                limRight = lR.gain(yr);
                limRed = std::max(limRed, -lR.gr);
                if (p.link) {
                    const float g = std::min(limLeft, limRight);
                    limLeft = limRight = g;
                }
            }
            yl = lL.clip(yl * limLeft);
            yr = stereo ? lR.clip(yr * limRight) : yl;
            red += limRed;
        }

        yl *= p.outGain;
        yr = stereo ? yr * p.outGain : yl;
        reduction = red;
    }

    Reverb* rev = nullptr;  // opsional; dipakai hanya di jalur stereo

private:
    Compressor cL, cR;
    Limiter lL, lR;
};
