package com.example.dbxcomp

/** Jembatan JNI ke mesin audio native (Oboe). */
object AudioEngine {
    // ID parameter, harus sama dengan enum ParamId di audio_engine.cpp
    const val P_THRESH = 0
    const val P_RATIO = 1
    const val P_ATTACK = 2
    const val P_RELEASE = 3
    const val P_KNEE = 4
    const val P_MAKEUP = 5
    const val P_CEIL = 6
    const val P_LIM_ON = 7
    const val P_BYPASS = 8
    const val P_IN_GAIN = 9
    const val P_OUT_GAIN = 10
    const val P_LINK = 11
    const val P_GATE_L = 12
    const val P_GATE_R = 13
    const val P_IN_GAIN_INT = 14
    const val P_MONITOR = 15
    const val P_MIC_DYN = 16
    const val P_HPF_FREQ = 17
    const val P_GATE_THRESH = 18
    const val P_GATE_REL = 19
    const val P_REV_ON = 20
    const val P_REV_MIX = 21
    const val P_REV_SIZE = 22
    const val P_REV_DAMP = 23
    const val P_REV_PRE = 24
    const val P_REV_WIDTH = 25
    const val P_REV_LOWCUT = 26
    const val P_COUNT = 27

    init {
        System.loadLibrary("dbxcomp")
    }

    /**
     * [stereo] = minta input mic 2 channel; kalau perangkat menolak, otomatis turun ke mono.
     * [internal] = sumber suara internal HP (stereo asli, data didorong lewat [nativePushCapture]);
     * [rate] = laju sampel tangkap internal (output Oboe disamakan).
     */
    external fun nativeStart(
        inputDeviceId: Int, outputDeviceId: Int, stereo: Boolean, internal: Boolean, rate: Int
    ): Boolean

    /** Dari thread tangkap: [count] float stereo interleaved (L, R, L, R, ...). */
    external fun nativePushCapture(data: FloatArray, count: Int)

    /** Mode rekam A/B (hanya sumber mono). Panggil sebelum rekaman dimulai. */
    external fun nativeSetRecordAB(ab: Boolean)
    external fun nativeStop()
    external fun nativeSetParam(id: Int, value: Float)

    /** Mengisi [out] dengan: input peak dB, output peak dB, gain reduction dB (positif). */
    external fun nativeGetMeters(out: FloatArray)

    /** Aktif/nonaktifkan penangkapan audio (pasangan asli+hasil) ke ring buffer. */
    external fun nativeSetRecording(on: Boolean)

    /** Mengambil data rekaman terkumpul ke [out]; mengembalikan jumlah float (genap). */
    external fun nativeReadRecorded(out: FloatArray): Int

    external fun nativeGetSampleRate(): Int

    /** Jumlah channel input yang benar-benar diberikan perangkat (1 atau 2). */
    external fun nativeGetInputChannels(): Int
}
