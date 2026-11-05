#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <memory>
#include <vector>

//==============================================================================
// Runtime binding to the DeepFilterNet C API (open source, libDF "capi").
// The library is NOT linked at build time: it is loaded on demand from
//   <folder of JBJAudio.exe>  or  %APPDATA%\JBJAudio
// Names tried: deepfilter.dll, libdf.dll, libdf.so
// Model (tar.gz, e.g. DeepFilterNet3_onnx.tar.gz) is searched in the same folders.
class DeepFilterLib
{
public:
    static DeepFilterLib& get();

    // Empty string = available, otherwise the reason it is not.
    juce::String load();

    struct State;   // opaque DFState
    State* create (float attenLimDb) const;
    void destroy (State* s) const;
    int frameLength (State* s) const;
    void setAttenLim (State* s, float db) const;
    void setPostFilterBeta (State* s, float beta) const;
    float processFrame (State* s, float* in, float* out) const;

private:
    DeepFilterLib() = default;

    juce::DynamicLibrary lib;
    bool loaded = false;
    juce::String error;
    juce::File modelFile;

    using CreateFn   = State* (*)(const char*, float, const char*);
    using FreeFn     = void (*)(State*);
    using FrameLenFn = size_t (*)(State*);
    using SetFloatFn = void (*)(State*, float);
    using ProcessFn  = float (*)(State*, float*, float*);

    CreateFn fCreate = nullptr;
    FreeFn fFree = nullptr;
    FrameLenFn fFrameLen = nullptr;
    SetFloatFn fAtten = nullptr, fBeta = nullptr;
    ProcessFn fProcess = nullptr;
};

//==============================================================================
// Mono noise reduction for one channel. Streaming: any block size in, same size out.
class NoiseReducer
{
public:
    enum Mode { off = 0, gate = 1, ai = 2 };

    NoiseReducer() = default;
    ~NoiseReducer();

    // Message thread. Returns "" on success, otherwise a reason (mode stays unchanged).
    juce::String setMode (Mode newMode, double sampleRate);
    Mode getMode() const noexcept { return mode.load(); }

    // Audio thread (capture callback).
    void prepare (double sampleRate);
    void process (float* mono, int numSamples) noexcept;

private:
    void processGate (float* x, int n) noexcept;
    void processAI (float* x, int n) noexcept;

    std::atomic<Mode> mode { off };
    juce::SpinLock lock;
    double sr = 48000.0;

    // gate
    float env = 0.0f, gateGain = 1.0f;
    float thresholdDb = -48.0f, floorGain = 0.03f;

    // AI
    DeepFilterLib::State* state = nullptr;
    int frameLen = 0;
    std::vector<float> inFrame, outFrame, queue;
    int inPos = 0, qRead = 0, qCount = 0;
};
