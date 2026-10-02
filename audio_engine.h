#ifndef AUDIO_ENGINE_H
#define AUDIO_ENGINE_H

#include <cuda_runtime.h>

struct EngineSettings {
    bool enableEngine = true;
    bool startWithWindows = false;
    bool minimizeToTray = true;

    // Module 1: Spectral (cuFFT)
    bool enablePitchShift = false;
    float pitchShiftFactor = 1.0f;
    bool enableDeEsser = true;
    bool enableAdditiveSynth = false;

    // Module 2: Parallel DSP & Acoustic Modeling
    bool enableParametricEQ = true;
    float lowGain = 1.0f, midGain = 1.0f, highGain = 1.0f;
    bool enableSoftSaturation = true;
    float saturationDrive = 1.5f;
    bool enableConvolutionReverb = false;
    bool enableSpatial3DAudio = false;

    // Module 3: Tensor-Core AI
    bool enableVoiceIsolation = true;
    bool enableStemSeparation = false;
    
    // Telemetry
    float livePeakLevel = 0.0f;
};

extern "C" {
    bool verifyNvidiaGPU(char* gpuNameOut, int maxLen);
    void runCudaAudioPipelineAsync(const short* inPCM, short* outPCM, unsigned int samples, EngineSettings* settings, cudaStream_t stream);
}

#endif // AUDIO_ENGINE_H