#pragma once
// Efek tambahan: high-pass (biquad), noise gate, dan reverb gaya Freeverb
// (8 comb + 4 allpass per sisi, kiri-kanan dibuat berbeda supaya melebar).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

inline float flushDenormal(float v) { return std::fabs(v) < 1e-20f ? 0.f : v; }

// High-pass orde-2 (Butterworth), membuang dengung rendah dan suara genggaman mic.
class HighPass {
public:
    void setup(float sr, float freq) {
        if (sr == lastSr && freq == lastFreq) return;
        lastSr = sr;
        lastFreq = freq;
        const float f = std::min(std::max(freq, 10.f), sr * 0.45f);
        const float w0 = 6.2831853f * f / sr;
        const float cw = std::cos(w0);
        const float alpha = std::sin(w0) / (2.f * 0.70710678f);
        const float a0 = 1.f + alpha;
        b0 = (1.f + cw) * 0.5f / a0;
        b1 = -(1.f + cw) / a0;
        b2 = b0;
        a1 = -2.f * cw / a0;
        a2 = (1.f - alpha) / a0;
    }
    void reset() { z1 = z2 = 0.f; }
    float process(float x) {
        const float y = b0 * x + z1;
        z1 = flushDenormal(b1 * x - a1 * y + z2);
        z2 = flushDenormal(b2 * x - a2 * y);
        return y;
    }

private:
    float b0 = 1.f, b1 = 0.f, b2 = 0.f, a1 = 0.f, a2 = 0.f;
    float z1 = 0.f, z2 = 0.f;
    float lastSr = 0.f, lastFreq = 0.f;
};

// Noise gate (terhubung L/R): membuka cepat, menahan 60 ms, menutup sesuai release.
// Saat tertutup sinyal ditekan ~45 dB (bukan senyap total, supaya tidak terdengar patah).
class Gate {
public:
    void prepare(float sr, float releaseMs) {
        atk = std::exp(-1.f / (0.001f * sr));
        rel = std::exp(-1.f / (std::max(releaseMs, 5.f) * 0.001f * sr));
        envRel = std::exp(-1.f / (0.030f * sr));
        holdMax = 0.060f * sr;
    }
    void reset() {
        env = 0.f;
        gain = 1.f;
        hold = 0.f;
        open = true;
    }
    // lvl = level puncak saat ini (linear), thr = ambang buka (linear). Mengembalikan gain.
    float process(float lvl, float thr) {
        env = (lvl > env) ? lvl : envRel * env + (1.f - envRel) * lvl;
        if (env > thr) {
            open = true;
            hold = holdMax;
        } else if (env < thr * 0.7f) {
            if (hold > 0.f) hold -= 1.f;
            else open = false;
        }
        const float target = open ? 1.f : kFloor;
        gain = (target > gain) ? atk * gain + (1.f - atk) * target
                               : rel * gain + (1.f - rel) * target;
        return gain;
    }

private:
    static constexpr float kFloor = 0.0056f;  // ~ -45 dB
    float atk = 0.f, rel = 0.f, envRel = 0.f, holdMax = 0.f;
    float env = 0.f, gain = 1.f, hold = 0.f;
    bool open = true;
};

class Reverb {
public:
    void init(float sampleRate) {
        sr = sampleRate;
        const float k = sr / 44100.f;
        static const int combTune[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
        static const int apTune[4] = {556, 441, 341, 225};
        const int spread = 23;
        for (int i = 0; i < 8; ++i) {
            combL[i].alloc(scaled(combTune[i], k));
            combR[i].alloc(scaled(combTune[i] + spread, k));
        }
        for (int i = 0; i < 4; ++i) {
            apL[i].alloc(scaled(apTune[i], k));
            apR[i].alloc(scaled(apTune[i] + spread, k));
        }
        pre.assign(static_cast<size_t>(sr * 0.12f) + 2, 0.f);
        preIdx = 0;
        lcState = 0.f;
        setParams(60.f, 50.f, 15.f, 100.f, 200.f, 20.f);
    }

    void reset() {
        for (int i = 0; i < 8; ++i) {
            combL[i].clear();
            combR[i].clear();
        }
        for (int i = 0; i < 4; ++i) {
            apL[i].clear();
            apR[i].clear();
        }
        std::fill(pre.begin(), pre.end(), 0.f);
        lcState = 0.f;
    }

    // Semua dalam persen (0-100) kecuali preMs (ms) dan lowcutHz (Hz).
    void setParams(float sizePct, float dampPct, float preMs, float widthPct, float lowcutHz,
                   float mixPct) {
        const float size = std::min(std::max(sizePct, 0.f), 100.f) * 0.01f;
        const float damp = std::min(std::max(dampPct, 0.f), 100.f) * 0.01f;
        const float width = std::min(std::max(widthPct, 0.f), 100.f) * 0.01f;
        feedback = 0.7f + 0.28f * size;
        damp1 = damp * 0.4f;
        damp2 = 1.f - damp1;
        wet1 = width * 0.5f + 0.5f;
        wet2 = (1.f - width) * 0.5f;
        mix = std::min(std::max(mixPct, 0.f), 100.f) * 0.01f;
        const float maxPre = pre.empty() ? 0.f : static_cast<float>(pre.size() - 1);
        preDelay = static_cast<size_t>(std::min(std::max(preMs * 0.001f * sr, 0.f), maxPre));
        const float f = std::min(std::max(lowcutHz, 10.f), sr * 0.4f);
        lcA = 1.f - std::exp(-6.2831853f * f / sr);
    }

    // Menambahkan ekor reverb ke (l, r). Panggil hanya dengan jalur stereo.
    void process(float& l, float& r) {
        if (pre.empty()) return;
        float in = (l + r) * 0.5f;
        lcState += lcA * (in - lcState);  // low-cut: buang bass dari masukan reverb
        in -= lcState;
        pre[preIdx] = in;
        size_t rd = preIdx + pre.size() - preDelay;
        if (rd >= pre.size()) rd -= pre.size();
        const float d = pre[rd];
        if (++preIdx >= pre.size()) preIdx = 0;

        const float x = d * 0.03f;
        float oL = 0.f, oR = 0.f;
        for (int i = 0; i < 8; ++i) {
            oL += combL[i].run(x, feedback, damp1, damp2);
            oR += combR[i].run(x, feedback, damp1, damp2);
        }
        for (int i = 0; i < 4; ++i) {
            oL = apL[i].run(oL);
            oR = apR[i].run(oR);
        }
        l += (oL * wet1 + oR * wet2) * mix;
        r += (oR * wet1 + oL * wet2) * mix;
    }

private:
    static size_t scaled(int tune, float k) {
        return static_cast<size_t>(std::max(1.f, std::floor(static_cast<float>(tune) * k + 0.5f)));
    }

    struct Comb {
        std::vector<float> buf;
        size_t idx = 0;
        float store = 0.f;
        void alloc(size_t n) {
            buf.assign(n, 0.f);
            idx = 0;
            store = 0.f;
        }
        void clear() {
            std::fill(buf.begin(), buf.end(), 0.f);
            idx = 0;
            store = 0.f;
        }
        float run(float in, float fb, float d1, float d2) {
            const float out = buf[idx];
            store = flushDenormal(out * d2 + store * d1);
            buf[idx] = flushDenormal(in + store * fb);
            if (++idx >= buf.size()) idx = 0;
            return out;
        }
    };
    struct Allpass {
        std::vector<float> buf;
        size_t idx = 0;
        void alloc(size_t n) {
            buf.assign(n, 0.f);
            idx = 0;
        }
        void clear() {
            std::fill(buf.begin(), buf.end(), 0.f);
            idx = 0;
        }
        float run(float in) {
            const float b = buf[idx];
            const float out = b - in;
            buf[idx] = flushDenormal(in + b * 0.5f);
            if (++idx >= buf.size()) idx = 0;
            return out;
        }
    };

    Comb combL[8], combR[8];
    Allpass apL[4], apR[4];
    std::vector<float> pre;
    size_t preIdx = 0, preDelay = 0;
    float sr = 48000.f;
    float feedback = 0.84f, damp1 = 0.2f, damp2 = 0.8f;
    float wet1 = 1.f, wet2 = 0.f, mix = 0.2f;
    float lcA = 0.f, lcState = 0.f;
};
