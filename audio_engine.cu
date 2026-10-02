#include "audio_engine.h"
#include <device_launch_parameters.h>
#include <stdio.h>
#include <math.h>

extern "C" bool verifyNvidiaGPU(char* gpuNameOut, int maxLen) {
    int deviceCount = 0;
    cudaError_t err = cudaGetDeviceCount(&deviceCount);
    if (err != cudaSuccess || deviceCount == 0) return false;

    cudaDeviceProp prop;
    cudaGetDeviceProperties(&prop, 0);

    if (prop.major == 0 && prop.minor == 0) {
        snprintf(gpuNameOut, maxLen, "Unknown Non-NVIDIA GPU");
        return false;
    }

    snprintf(gpuNameOut, maxLen, "%s (Compute %d.%d)", prop.name, prop.major, prop.minor);
    return true;
}

__global__ void UnifiedAudioDSPKernel(float* buffer, unsigned int count, EngineSettings settings) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= count) return;

    float sample = buffer[idx];

    if (settings.enableParametricEQ) {
        sample *= settings.lowGain;
    }

    if (settings.enableSoftSaturation) {
        sample = tanhf(sample * settings.saturationDrive);
    }

    if (settings.enableVoiceIsolation) {
        if (fabsf(sample) < 0.02f) sample = 0.0f;
    }

    buffer[idx] = sample;
}

__global__ void Int16ToFloat(const short* in, float* out, int count) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < count) out[i] = (float)in[i] / 32768.0f;
}

__global__ void FloatToInt16(const float* in, short* out, int count) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < count) {
        float s = in[i];
        if (s > 1.0f) s = 1.0f;
        if (s < -1.0f) s = -1.0f;
        out[i] = (short)(s * 32767.0f);
    }
}

extern "C" void runCudaAudioPipelineAsync(const short* d_inPCM, short* d_outPCM, unsigned int samples, EngineSettings* settings, cudaStream_t stream) {
    static float* d_floatBuf = nullptr;
    if (!d_floatBuf) {
        cudaMalloc((void**)&d_floatBuf, 4096 * sizeof(float));
    }

    int threads = 256;
    int blocks = (samples + threads - 1) / threads;

    Int16ToFloat<<<blocks, threads, 0, stream>>>(d_inPCM, d_floatBuf, samples);
    UnifiedAudioDSPKernel<<<blocks, threads, 0, stream>>>(d_floatBuf, samples, *settings);
    FloatToInt16<<<blocks, threads, 0, stream>>>(d_floatBuf, d_outPCM, samples);
}