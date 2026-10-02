#include "audio_engine.h"
#include <cuda_runtime.h>
#include <device_launch_parameters.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define SAMPLE_RATE 48000.0
#define FIR_TAPS    511          // linear-phase FIR length
#define FIR_DELAY   255          // (FIR_TAPS-1)/2  -> ~5.3 ms latency
#define HISTORY     4800         // samples of past audio kept (>= REVERB_LEN)
#define REVERB_LEN  4800         // 100 ms impulse response
#define MAX_BLOCK   2048

struct DspParams {
    int   eq, sat, rev;
    float gLow, gMid, gHigh;
    float drive, satNorm, mix;
    float gate0, gate1, outGain;
};

__constant__ float c_lowLP[FIR_TAPS];   // low-pass @ 250 Hz
__constant__ float c_midLP[FIR_TAPS];   // low-pass @ 4 kHz

static float*       d_in  = nullptr;
static float*       d_out = nullptr;
static float*       d_ir  = nullptr;
static cudaStream_t g_stream = nullptr;

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

// ---------------------------------------------------------------------------
// One thread per output sample. Each thread reads its own window of past input,
// so there is no sequential dependency -> perfectly parallel (FIR, not IIR).
// ---------------------------------------------------------------------------
__global__ void DspKernel(const float* in, const float* ir, float* out, int n, DspParams p) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;

    const float* x = in + HISTORY + i;      // x[-k] = k samples ago
    float dry = x[-FIR_DELAY];              // dry path delayed to match the FIRs
    float y = dry;

    if (p.eq) {
        float lp1 = 0.f, lp2 = 0.f;
        for (int k = 0; k < FIR_TAPS; ++k) {
            float v = x[-k];
            lp1 += c_lowLP[k] * v;
            lp2 += c_midLP[k] * v;
        }
        // low = lp1, mid = lp2-lp1, high = dry-lp2  (sum == dry when gains are 1)
        y = p.gLow * lp1 + p.gMid * (lp2 - lp1) + p.gHigh * (dry - lp2);
    }

    // noise gate gain, linearly ramped across the block
    float t = ((float)i + 0.5f) / (float)n;
    y *= p.gate0 + (p.gate1 - p.gate0) * t;

    if (p.rev) {
        float wet = 0.f;
        for (int k = 0; k < REVERB_LEN; ++k) wet += ir[k] * x[-k];
        y += p.mix * wet;
    }

    if (p.sat) y = tanhf(y * p.drive) * p.satNorm;

    out[i] = y * p.outGain;
}

static void designLowpass(float* h, double fc) {
    double sum = 0.0, x = 2.0 * fc / SAMPLE_RATE, PI = 3.14159265358979323846;
    for (int k = 0; k < FIR_TAPS; ++k) {
        double m = (double)(k - FIR_DELAY);
        double s = (m == 0.0) ? x : sin(PI * x * m) / (PI * m);
        double w = 0.54 - 0.46 * cos(2.0 * PI * k / (FIR_TAPS - 1));   // Hamming
        h[k] = (float)(s * w);
        sum += h[k];
    }
    for (int k = 0; k < FIR_TAPS; ++k) h[k] = (float)(h[k] / sum);     // unity DC gain
}

extern "C" bool gpuEngineInit() {
    if (d_in) return true;

    float lp1[FIR_TAPS], lp2[FIR_TAPS];
    designLowpass(lp1, 250.0);
    designLowpass(lp2, 4000.0);
    if (cudaMemcpyToSymbol(c_lowLP, lp1, sizeof(lp1)) != cudaSuccess) return false;
    if (cudaMemcpyToSymbol(c_midLP, lp2, sizeof(lp2)) != cudaSuccess) return false;

    // Synthetic room impulse response: decaying noise, unit energy.
    static float ir[REVERB_LEN];
    unsigned int seed = 12345u;
    double energy = 0.0;
    for (int i = 0; i < REVERB_LEN; ++i) {
        seed = seed * 1664525u + 1013904223u;
        float r = ((seed >> 8) & 0xFFFF) / 32768.0f - 1.0f;
        ir[i] = r * expf(-(float)i / (float)SAMPLE_RATE / 0.03f);
        energy += (double)ir[i] * ir[i];
    }
    float norm = (float)(1.0 / sqrt(energy));
    for (int i = 0; i < REVERB_LEN; ++i) ir[i] *= norm;

    if (cudaMalloc((void**)&d_in,  (HISTORY + MAX_BLOCK) * sizeof(float)) != cudaSuccess) { d_in = nullptr; return false; }
    if (cudaMalloc((void**)&d_out, MAX_BLOCK * sizeof(float)) != cudaSuccess) return false;
    if (cudaMalloc((void**)&d_ir,  REVERB_LEN * sizeof(float)) != cudaSuccess) return false;
    cudaMemset(d_in, 0, (HISTORY + MAX_BLOCK) * sizeof(float));
    cudaMemcpy(d_ir, ir, sizeof(ir), cudaMemcpyHostToDevice);
    return cudaStreamCreate(&g_stream) == cudaSuccess;
}

extern "C" void gpuEngineShutdown() {
    if (g_stream) { cudaStreamDestroy(g_stream); g_stream = nullptr; }
    if (d_in)  { cudaFree(d_in);  d_in  = nullptr; }
    if (d_out) { cudaFree(d_out); d_out = nullptr; }
    if (d_ir)  { cudaFree(d_ir);  d_ir  = nullptr; }
}

extern "C" int gpuHistoryLength() { return HISTORY; }

static float dbToLin(float db) { return powf(10.0f, db / 20.0f); }

extern "C" bool gpuProcessBlock(const float* hostIn, float* hostOut, int n,
                                const EngineSettings* s, float gate0, float gate1) {
    if (!d_in || n <= 0 || n > MAX_BLOCK) return false;

    DspParams p;
    p.eq  = s->enableEQ ? 1 : 0;
    p.sat = s->enableSaturation ? 1 : 0;
    p.rev = s->enableReverb ? 1 : 0;
    p.gLow  = dbToLin(s->lowGainDb);
    p.gMid  = dbToLin(s->midGainDb);
    p.gHigh = dbToLin(s->highGainDb);
    p.drive   = s->saturationDrive;
    p.satNorm = 1.0f / tanhf(p.drive);
    p.mix     = s->reverbMix;
    p.gate0 = gate0;
    p.gate1 = gate1;
    p.outGain = dbToLin(s->outputGainDb);

    cudaMemcpyAsync(d_in, hostIn, (HISTORY + n) * sizeof(float), cudaMemcpyHostToDevice, g_stream);
    int threads = 256, blocks = (n + threads - 1) / threads;
    DspKernel<<<blocks, threads, 0, g_stream>>>(d_in, d_ir, d_out, n, p);
    cudaMemcpyAsync(hostOut, d_out, n * sizeof(float), cudaMemcpyDeviceToHost, g_stream);
    return cudaStreamSynchronize(g_stream) == cudaSuccess;
}
