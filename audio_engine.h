#ifndef AUDIO_ENGINE_H
#define AUDIO_ENGINE_H

// Shared between main.cpp (MSVC) and audio_engine.cu (nvcc).
// Written by the UI thread, read (as a snapshot) by the audio thread.
struct EngineSettings {
    // Module toggles
    bool enableEQ         = true;    // 3-band linear-phase FIR EQ
    bool enableNoiseGate  = true;    // "voice isolation" (gate)
    bool enableSaturation = false;   // tanh soft saturation
    bool enableReverb     = false;   // direct convolution reverb

    // App behaviour
    bool startWithWindows = false;
    bool minimizeToTray   = true;

    // Parameters
    float lowGainDb  = 0.0f;         // below 250 Hz
    float midGainDb  = 0.0f;         // 250 Hz - 4 kHz
    float highGainDb = 0.0f;         // above 4 kHz
    float saturationDrive = 1.5f;
    float gateThresholdDb = -50.0f;
    float reverbMix  = 0.25f;        // 0..1
    float outputGainDb = 0.0f;

    // Telemetry (written by audio thread)
    float livePeakLevel = 0.0f;
};

extern "C" {
    bool verifyNvidiaGPU(char* gpuNameOut, int maxLen);

    bool gpuEngineInit();            // allocs device memory, uploads filters
    void gpuEngineShutdown();
    int  gpuHistoryLength();         // samples of history the caller must supply

    // hostIn  : gpuHistoryLength() + blockSize floats  [history | new block]
    // hostOut : blockSize floats
    // gateStart/gateEnd: gate gain at start/end of block (ramped, no clicks)
    bool gpuProcessBlock(const float* hostIn, float* hostOut, int blockSize,
                         const EngineSettings* settings, float gateStart, float gateEnd);
}

#endif // AUDIO_ENGINE_H
