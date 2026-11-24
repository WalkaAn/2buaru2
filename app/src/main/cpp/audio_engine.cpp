// Mesin audio latensi rendah: sumber (mic/line-in mono atau stereo, ATAU suara internal HP
// stereo dari AudioPlaybackCapture) -> gate L/R -> compressor -> limiter -> output stereo.
// Memakai Oboe (AAudio di Android 8.1+, OpenSL ES di bawahnya).

#include <android/log.h>
#include <jni.h>
#include <oboe/Oboe.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <vector>

#include "compressor.h"
#include "effects.h"
#include "frame_ring.h"
#include "ring_buffer.h"

#define LOG_TAG "DbxComp"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

enum ParamId {
    P_THRESH = 0,
    P_RATIO,
    P_ATTACK,
    P_RELEASE,
    P_KNEE,
    P_MAKEUP,
    P_CEIL,
    P_LIM_ON,
    P_BYPASS,
    P_IN_GAIN,
    P_OUT_GAIN,
    P_LINK,
    P_GATE_L,
    P_GATE_R,
    P_IN_GAIN_INT,  // input gain khusus suara internal (terpisah dari mic)
    P_MONITOR,      // 1 = output terdengar, 0 = senyap (meter dan rekam tetap jalan)
    P_MIC_DYN,      // mode mic dinamis: high-pass + noise gate (hanya sumber mic)
    P_HPF_FREQ,     // Hz
    P_GATE_THRESH,  // dBFS
    P_GATE_REL,     // ms
    P_REV_ON,
    P_REV_MIX,      // persen
    P_REV_SIZE,     // persen
    P_REV_DAMP,     // persen
    P_REV_PRE,      // ms
    P_REV_WIDTH,    // persen
    P_REV_LOWCUT,   // Hz
    P_COUNT
};

class Engine : public oboe::AudioStreamDataCallback,
               public oboe::AudioStreamErrorCallback,
               public std::enable_shared_from_this<Engine> {
public:
    Engine() {
        const float defaults[P_COUNT] = {-20.f, 4.f, 10.f, 200.f, 6.f, 0.f,
                                         -1.f,  1.f, 0.f,  0.f,   0.f, 1.f,
                                         1.f,   1.f,  0.f,  1.f,
                                         0.f,   100.f, -55.f, 200.f,
                                         0.f,   20.f,  60.f,  50.f,
                                         15.f,  100.f, 200.f};
        for (int i = 0; i < P_COUNT; ++i) params[i].store(defaults[i]);
        chain.rev = &rev;
    }

    void setParam(int id, float v) {
        if (id >= 0 && id < P_COUNT) params[id].store(v, std::memory_order_relaxed);
    }

    // Mode rekam A/B (kiri = asli, kanan = hasil); hanya berlaku untuk sumber mono.
    void setRecordAB(bool ab) { recAB.store(ab, std::memory_order_relaxed); }

    // Dari thread tangkap audio internal: frame stereo interleaved.
    void pushCapture(const float* data, size_t frames) { capRing.write(data, frames); }

    void setRecording(bool on) {
        if (on) ring.clear();  // thread audio belum menulis karena flag masih false
        recording.store(on, std::memory_order_relaxed);
    }

    int readRecorded(float* out, int maxFloats) {
        return static_cast<int>(ring.read(out, static_cast<size_t>(std::max(0, maxFloats))));
    }

    int getSampleRate() const { return static_cast<int>(sampleRate); }

    // Jumlah channel input yang benar-benar diberikan perangkat (1 atau 2).
    int getInputChannels() const { return inChannels.load(std::memory_order_relaxed); }

    void getMeters(float* out3) {
        out3[0] = meterIn.exchange(-100.f);
        out3[1] = meterOut.exchange(-100.f);
        out3[2] = meterGr.exchange(0.f);
    }

    bool start(int inId, int outId, bool stereo, bool internal, int rate) {
        std::lock_guard<std::mutex> lock(mtx);
        inputDeviceId = inId;
        outputDeviceId = outId;
        wantStereo = stereo;
        internalSrc.store(internal);
        captureRate = rate;
        running = true;
        if (!openStreams()) {
            running = false;
            return false;
        }
        return true;
    }

    void stop() {
        std::lock_guard<std::mutex> lock(mtx);
        running = false;
        closeStreams();
    }

    oboe::DataCallbackResult onAudioReady(oboe::AudioStream* stream, void* audioData,
                                          int32_t numFrames) override {
        float* out = static_cast<float*>(audioData);
        const int outCh = stream->getChannelCount();
        const bool internal = internalSrc.load(std::memory_order_relaxed);
        const int inCh = internal ? 2 : std::max(1, inChannels.load(std::memory_order_relaxed));
        const bool srcStereo = inCh >= 2;

        // Ambil input tanpa blocking. Kekurangan data diisi nol.
        int32_t got = 0;
        const int32_t frames = std::min<int32_t>(numFrames, (int32_t)(inBuf.size() / inCh));
        if (internal) {
            // Audio internal datang dari thread tangkap lewat ring buffer. Tunggu terisi
            // ~30 ms dulu supaya tidak putus-putus; kalau menumpuk >120 ms, kejar.
            size_t avail = capRing.available();
            const size_t maxF = static_cast<size_t>(sampleRate * 0.12f);
            const size_t tgtF = static_cast<size_t>(sampleRate * 0.03f);
            if (avail > maxF) {
                capRing.skip(avail - tgtF);
                avail = tgtF;
            }
            if (!primed && avail >= tgtF) primed = true;
            if (primed && frames > 0) {
                if (avail < static_cast<size_t>(frames)) primed = false;
                got = static_cast<int32_t>(capRing.read(inBuf.data(), static_cast<size_t>(frames)));
            }
        } else {
            oboe::AudioStream* in = inStream.get();
            if (in != nullptr && frames > 0) {
                auto res = in->read(inBuf.data(), frames, 0);
                if (res.error() == oboe::Result::OK) got = res.value();
            }
        }

        ProcParams pp;
        pp.comp.thresholdDb = params[P_THRESH].load(std::memory_order_relaxed);
        pp.comp.ratio = params[P_RATIO].load(std::memory_order_relaxed);
        pp.comp.attackMs = params[P_ATTACK].load(std::memory_order_relaxed);
        pp.comp.releaseMs = params[P_RELEASE].load(std::memory_order_relaxed);
        pp.comp.kneeDb = params[P_KNEE].load(std::memory_order_relaxed);
        pp.comp.makeupDb = params[P_MAKEUP].load(std::memory_order_relaxed);
        pp.ceilDb = params[P_CEIL].load(std::memory_order_relaxed);
        pp.limOn = params[P_LIM_ON].load(std::memory_order_relaxed) > 0.5f;
        pp.link = params[P_LINK].load(std::memory_order_relaxed) > 0.5f;
        pp.outGain = dbToLin(params[P_OUT_GAIN].load(std::memory_order_relaxed));
        const bool bypass = params[P_BYPASS].load(std::memory_order_relaxed) > 0.5f;
        const float inG = dbToLin(
            params[internal ? P_IN_GAIN_INT : P_IN_GAIN].load(std::memory_order_relaxed));
        const float mon = params[P_MONITOR].load(std::memory_order_relaxed) > 0.5f ? 1.f : 0.f;

        const bool revOn = params[P_REV_ON].load(std::memory_order_relaxed) > 0.5f;
        pp.revOn = revOn;
        rev.setParams(params[P_REV_SIZE].load(std::memory_order_relaxed),
                      params[P_REV_DAMP].load(std::memory_order_relaxed),
                      params[P_REV_PRE].load(std::memory_order_relaxed),
                      params[P_REV_WIDTH].load(std::memory_order_relaxed),
                      params[P_REV_LOWCUT].load(std::memory_order_relaxed),
                      params[P_REV_MIX].load(std::memory_order_relaxed));
        // Mode mic dinamis hanya untuk sumber mic (bukan suara internal).
        const bool dynOn = !internal && params[P_MIC_DYN].load(std::memory_order_relaxed) > 0.5f;
        hpL.setup(sampleRate, params[P_HPF_FREQ].load(std::memory_order_relaxed));
        hpR.setup(sampleRate, params[P_HPF_FREQ].load(std::memory_order_relaxed));
        gate.prepare(sampleRate, params[P_GATE_REL].load(std::memory_order_relaxed));
        const float gateThr = dbToLin(params[P_GATE_THRESH].load(std::memory_order_relaxed));

        chain.prepare(sampleRate, pp);

        float inPeak = 0.f, outPeak = 0.f, maxReduction = 0.f;
        const bool rec = recording.load(std::memory_order_relaxed);
        // Rekam A/B hanya untuk sumber mono; saat itu gate L/R diabaikan.
        const bool ab = rec && recAB.load(std::memory_order_relaxed) && !srcStereo;
        const bool gateL = ab || params[P_GATE_L].load(std::memory_order_relaxed) > 0.5f;
        const bool gateR = ab || params[P_GATE_R].load(std::memory_order_relaxed) > 0.5f;
        // Jalur stereo dipakai kalau sumber stereo, atau sumber mono tapi satu sisi dimatikan.
        // Reverb selalu butuh jalur stereo (ekornya kiri-kanan berbeda).
        const bool stereo = srcStereo || !(gateL && gateR) || revOn;

        for (int32_t i = 0; i < numFrames; ++i) {
            const bool have = i < got;
            const float rawL = have ? inBuf[i * inCh] : 0.f;
            const float rawR = (have && srcStereo) ? inBuf[i * inCh + 1] : rawL;
            float l = gateL ? rawL * inG : 0.f;
            float r = gateR ? rawR * inG : 0.f;
            if (dynOn && !bypass) {
                // High-pass lalu noise gate (terhubung L/R) sebelum compressor.
                l = hpL.process(l);
                if (stereo) r = hpR.process(r);
                const float lvl = stereo ? std::max(std::fabs(l), std::fabs(r)) : std::fabs(l);
                const float g = gate.process(lvl, gateThr);
                l *= g;
                r *= g;
            }

            float yl, yr;
            if (bypass) {
                yl = l;
                yr = stereo ? r : l;
            } else {
                float red = 0.f;
                chain.process(l, r, stereo, pp, yl, yr, red);
                maxReduction = std::max(maxReduction, red);
            }
            yl = std::max(-1.f, std::min(1.f, yl));  // jangan pernah clip digital di output
            yr = std::max(-1.f, std::min(1.f, yr));

            // Rekaman: normal -> (kiri, kanan) hasil; A/B (mono) -> (asli, hasil).
            if (rec) {
                if (ab) ring.push2(l, yl);
                else ring.push2(yl, yr);
            }

            inPeak = std::max(inPeak, std::max(std::fabs(l), stereo ? std::fabs(r) : 0.f));
            outPeak = std::max(outPeak, std::max(std::fabs(yl), std::fabs(yr)));
            for (int c = 0; c < outCh; ++c) out[i * outCh + c] = ((c == 0) ? yl : yr) * mon;
        }

        publishMax(meterIn, 20.f * std::log10(inPeak + 1e-6f));
        publishMax(meterOut, 20.f * std::log10(outPeak + 1e-6f));
        publishMax(meterGr, maxReduction);
        return oboe::DataCallbackResult::Continue;
    }

    // Dipanggil Oboe di thread terpisah kalau perangkat audio terputus (mis. USB dicabut).
    void onErrorAfterClose(oboe::AudioStream*, oboe::Result error) override {
        if (error != oboe::Result::ErrorDisconnected) return;
        std::lock_guard<std::mutex> lock(mtx);
        if (running) openStreams();
    }

private:
    static void publishMax(std::atomic<float>& a, float v) {
        if (v > a.load(std::memory_order_relaxed)) a.store(v, std::memory_order_relaxed);
    }

    bool openInput(int channels) {
        oboe::AudioStreamBuilder ib;
        ib.setDirection(oboe::Direction::Input)
            ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
            ->setSharingMode(oboe::SharingMode::Exclusive)
            ->setFormat(oboe::AudioFormat::Float)
            ->setChannelCount(channels)
            ->setSampleRate(outStream->getSampleRate())
            ->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Medium)
            // Stereo: preset Camcorder (jalur yang dipakai kamera untuk dua mic).
            // Mono: Unprocessed (matikan AGC/noise suppression).
            ->setInputPreset(channels == 2 ? oboe::InputPreset::Camcorder
                                           : oboe::InputPreset::Unprocessed)
            ->setDeviceId(inputDeviceId);
        const oboe::Result r = ib.openStream(inStream);
        if (r != oboe::Result::OK) {
            LOGE("Gagal membuka input %d channel: %s", channels, oboe::convertToText(r));
            inStream.reset();
            return false;
        }
        return true;
    }

    bool openStreams() {
        closeStreams();

        oboe::AudioStreamBuilder ob;
        ob.setDirection(oboe::Direction::Output)
            ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
            ->setSharingMode(oboe::SharingMode::Exclusive)
            ->setFormat(oboe::AudioFormat::Float)
            ->setChannelCount(oboe::ChannelCount::Stereo)
            ->setUsage(oboe::Usage::Media)
            ->setDeviceId(outputDeviceId)
            ->setDataCallback(shared_from_this())
            ->setErrorCallback(shared_from_this());
        if (internalSrc.load() && captureRate > 0) {
            // Samakan laju sampel output dengan laju tangkap internal (resample kalau perlu).
            ob.setSampleRate(captureRate)
                ->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Medium);
        }
        oboe::Result r = ob.openStream(outStream);
        if (r != oboe::Result::OK) {
            LOGE("Gagal membuka output: %s", oboe::convertToText(r));
            outStream.reset();
            return false;
        }
        outStream->setBufferSizeInFrames(outStream->getFramesPerBurst() * 2);
        sampleRate = static_cast<float>(outStream->getSampleRate());
        rev.init(sampleRate);  // alokasi buffer reverb (stream sedang tertutup)
        hpL.reset();
        hpR.reset();
        gate.reset();

        if (internalSrc.load()) {
            // Tanpa stream input Oboe: data datang dari ring buffer tangkap internal.
            inChannels.store(2);
            capRing.clear();
            primed = false;
        } else {
            // Coba stereo dulu kalau diminta; kalau perangkat menolak, turun ke mono.
            bool inOk = false;
            if (wantStereo) inOk = openInput(2);
            if (!inOk) inOk = openInput(1);
            if (!inOk) {
                outStream->close();
                outStream.reset();
                return false;
            }
            inChannels.store(std::max(1, inStream->getChannelCount()));
        }

        inBuf.assign(static_cast<size_t>(std::max<int32_t>(8192, outStream->getFramesPerBurst() * 8)) *
                         static_cast<size_t>(inChannels.load()),
                     0.f);
        chain.reset();

        if (inStream) inStream->requestStart();
        outStream->requestStart();
        return true;
    }

    void closeStreams() {
        // Output ditutup lebih dulu supaya callback berhenti sebelum input dilepas.
        if (outStream) {
            outStream->stop();
            outStream->close();
            outStream.reset();
        }
        if (inStream) {
            inStream->stop();
            inStream->close();
            inStream.reset();
        }
    }

    std::mutex mtx;
    bool running = false;
    bool wantStereo = false;
    int captureRate = 48000;
    std::atomic<bool> internalSrc{false};
    bool primed = false;  // hanya diakses thread audio (dan saat stream tertutup)
    int inputDeviceId = 0;
    int outputDeviceId = 0;
    float sampleRate = 48000.f;
    std::atomic<int> inChannels{1};

    std::shared_ptr<oboe::AudioStream> outStream;
    std::shared_ptr<oboe::AudioStream> inStream;
    std::vector<float> inBuf;

    std::atomic<float> params[P_COUNT];
    std::atomic<float> meterIn{-100.f};
    std::atomic<float> meterOut{-100.f};
    std::atomic<float> meterGr{0.f};

    RingBuffer ring;
    FrameRing capRing;
    std::atomic<bool> recording{false};
    std::atomic<bool> recAB{false};

    StereoChain chain;
    Reverb rev;
    HighPass hpL, hpR;
    Gate gate;
};

static std::shared_ptr<Engine> gEngine;

static Engine& engine() {
    if (!gEngine) gEngine = std::make_shared<Engine>();
    return *gEngine;
}

extern "C" {

JNIEXPORT jboolean JNICALL Java_com_example_dbxcomp_AudioEngine_nativeStart(
    JNIEnv*, jobject, jint inputDeviceId, jint outputDeviceId, jboolean stereo,
    jboolean internal, jint rate) {
    return engine().start(inputDeviceId, outputDeviceId, stereo == JNI_TRUE,
                          internal == JNI_TRUE, rate)
               ? JNI_TRUE
               : JNI_FALSE;
}

// Dari thread tangkap Kotlin: [count] float stereo interleaved.
JNIEXPORT void JNICALL Java_com_example_dbxcomp_AudioEngine_nativePushCapture(
    JNIEnv* env, jobject, jfloatArray data, jint count) {
    static float tmp[16384];
    const jint n = std::min<jint>(std::min<jint>(count, env->GetArrayLength(data)), 16384) & ~1;
    if (n <= 0) return;
    env->GetFloatArrayRegion(data, 0, n, tmp);
    engine().pushCapture(tmp, static_cast<size_t>(n / 2));
}

JNIEXPORT void JNICALL Java_com_example_dbxcomp_AudioEngine_nativeSetRecordAB(
    JNIEnv*, jobject, jboolean ab) {
    engine().setRecordAB(ab == JNI_TRUE);
}

JNIEXPORT void JNICALL Java_com_example_dbxcomp_AudioEngine_nativeStop(JNIEnv*, jobject) {
    engine().stop();
}

JNIEXPORT void JNICALL Java_com_example_dbxcomp_AudioEngine_nativeSetParam(
    JNIEnv*, jobject, jint id, jfloat value) {
    engine().setParam(id, value);
}

JNIEXPORT void JNICALL Java_com_example_dbxcomp_AudioEngine_nativeGetMeters(
    JNIEnv* env, jobject, jfloatArray out) {
    jfloat tmp[3];
    engine().getMeters(tmp);
    env->SetFloatArrayRegion(out, 0, 3, tmp);
}

JNIEXPORT void JNICALL Java_com_example_dbxcomp_AudioEngine_nativeSetRecording(
    JNIEnv*, jobject, jboolean on) {
    engine().setRecording(on == JNI_TRUE);
}

JNIEXPORT jint JNICALL Java_com_example_dbxcomp_AudioEngine_nativeReadRecorded(
    JNIEnv* env, jobject, jfloatArray out) {
    const jsize cap = env->GetArrayLength(out);
    jfloat* p = env->GetFloatArrayElements(out, nullptr);
    if (p == nullptr) return 0;
    const int n = engine().readRecorded(p, cap);
    env->ReleaseFloatArrayElements(out, p, 0);
    return n;
}

JNIEXPORT jint JNICALL Java_com_example_dbxcomp_AudioEngine_nativeGetSampleRate(
    JNIEnv*, jobject) {
    return engine().getSampleRate();
}

JNIEXPORT jint JNICALL Java_com_example_dbxcomp_AudioEngine_nativeGetInputChannels(
    JNIEnv*, jobject) {
    return engine().getInputChannels();
}

}  // extern "C"
