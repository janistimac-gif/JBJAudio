#include "NoiseReducer.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <cmath>

//==============================================================================
// DeepFilterLib
//==============================================================================
DeepFilterLib& DeepFilterLib::get()
{
    static DeepFilterLib instance;
    return instance;
}

juce::String DeepFilterLib::load()
{
    if (loaded)
        return {};

    juce::Array<juce::File> dirs;
    dirs.add (juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory());
    dirs.add (juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("JBJAudio"));

    juce::File libFile;
    for (auto& d : dirs)
        for (auto* n : { "deepfilter.dll", "libdf.dll", "libdf.so" })
            if (d.getChildFile (n).existsAsFile() && ! libFile.existsAsFile())
                libFile = d.getChildFile (n);

    for (auto& d : dirs)
        for (auto& f : d.findChildFiles (juce::File::findFiles, false, "*.tar.gz"))
            if (! modelFile.existsAsFile() && f.getFileName().containsIgnoreCase ("DeepFilterNet"))
                modelFile = f;

    if (! libFile.existsAsFile())
        return "AI noise reduction: deepfilter.dll not found (put it next to JBJAudio.exe)";
    if (! modelFile.existsAsFile())
        return "AI noise reduction: model DeepFilterNet3_onnx.tar.gz not found (put it next to JBJAudio.exe)";
    if (! lib.open (libFile.getFullPathName()))
        return "AI noise reduction: cannot load " + libFile.getFileName();

    fCreate   = (CreateFn)   lib.getFunction ("df_create");
    fFree     = (FreeFn)     lib.getFunction ("df_free");
    fFrameLen = (FrameLenFn) lib.getFunction ("df_get_frame_length");
    fAtten    = (SetFloatFn) lib.getFunction ("df_set_atten_lim");
    fBeta     = (SetFloatFn) lib.getFunction ("df_set_post_filter_beta");
    fProcess  = (ProcessFn)  lib.getFunction ("df_process_frame");

    if (fCreate == nullptr || fFree == nullptr || fFrameLen == nullptr || fProcess == nullptr)
    {
        lib.close();
        return "AI noise reduction: " + libFile.getFileName() + " does not export the DeepFilterNet API";
    }

    loaded = true;
    return {};
}

DeepFilterLib::State* DeepFilterLib::create (float attenLimDb) const
{
    return fCreate (modelFile.getFullPathName().toRawUTF8(), attenLimDb, "error");
}

void DeepFilterLib::destroy (State* s) const                      { if (s != nullptr) fFree (s); }
int DeepFilterLib::frameLength (State* s) const                   { return (int) fFrameLen (s); }
void DeepFilterLib::setAttenLim (State* s, float db) const        { if (fAtten != nullptr) fAtten (s, db); }
void DeepFilterLib::setPostFilterBeta (State* s, float b) const   { if (fBeta != nullptr) fBeta (s, b); }
float DeepFilterLib::processFrame (State* s, float* i, float* o) const { return fProcess (s, i, o); }

//==============================================================================
// NoiseReducer
//==============================================================================
NoiseReducer::~NoiseReducer()
{
    if (state != nullptr)
        DeepFilterLib::get().destroy (state);
}

void NoiseReducer::prepare (double sampleRate)
{
    sr = sampleRate;
    env = 0.0f;
    gateGain = 1.0f;
    if (mode.load() == ai && std::abs (sampleRate - 48000.0) > 1.0)
        mode.store (off);   // DeepFilterNet only runs at 48 kHz
}

juce::String NoiseReducer::setMode (Mode newMode, double sampleRate)
{
    sr = sampleRate;

    if (newMode != ai)
    {
        mode.store (newMode);
        return {};
    }

    if (std::abs (sampleRate - 48000.0) > 1.0)
        return "AI noise reduction needs a 48 kHz device (set it in Windows Sound settings)";

    auto err = DeepFilterLib::get().load();
    if (err.isNotEmpty())
        return err;

    auto& df = DeepFilterLib::get();
    auto* s = df.create (100.0f);       // 100 dB = maximum suppression
    if (s == nullptr)
        return "AI noise reduction: model could not be loaded";

    const int len = df.frameLength (s);
    if (len <= 0 || len > 4096)
    {
        df.destroy (s);
        return "AI noise reduction: unexpected frame length";
    }

    DeepFilterLib::State* old = nullptr;
    {
        const juce::SpinLock::ScopedLockType sl (lock);
        old = state;
        state = s;
        frameLen = len;
        inFrame.assign ((size_t) len, 0.0f);
        outFrame.assign ((size_t) len, 0.0f);
        queue.assign ((size_t) len * 4, 0.0f);
        inPos = 0;
        qRead = 0;
        qCount = len;               // one frame of silence = fixed latency, no underruns
        mode.store (ai);
    }
    df.destroy (old);
    return {};
}

void NoiseReducer::process (float* mono, int n) noexcept
{
    const auto m = mode.load();
    if (m == off)
        return;

    const juce::SpinLock::ScopedTryLockType sl (lock);
    if (! sl.isLocked())
        return;

    if (m == gate)
        processGate (mono, n);
    else
        processAI (mono, n);
}

void NoiseReducer::processGate (float* x, int n) noexcept
{
    const float s = (float) sr;
    const float envAttack  = 1.0f - std::exp (-1.0f / (0.002f * s));
    const float envRelease = 1.0f - std::exp (-1.0f / (0.150f * s));
    const float gainOpen   = 1.0f - std::exp (-1.0f / (0.005f * s));
    const float gainClose  = 1.0f - std::exp (-1.0f / (0.080f * s));
    const float threshold  = juce::Decibels::decibelsToGain (thresholdDb);

    for (int i = 0; i < n; ++i)
    {
        const float a = std::abs (x[i]);
        env += (a > env ? envAttack : envRelease) * (a - env);

        const float target = env > threshold ? 1.0f : floorGain;
        gateGain += (target > gateGain ? gainOpen : gainClose) * (target - gateGain);
        x[i] *= gateGain;
    }
}

void NoiseReducer::processAI (float* x, int n) noexcept
{
    if (state == nullptr || frameLen <= 0)
        return;

    auto& df = DeepFilterLib::get();
    const int qSize = (int) queue.size();

    for (int i = 0; i < n; ++i)
    {
        inFrame[(size_t) inPos++] = x[i];

        if (inPos == frameLen)
        {
            df.processFrame (state, inFrame.data(), outFrame.data());
            inPos = 0;
            int w = (qRead + qCount) % qSize;
            for (int k = 0; k < frameLen; ++k)
            {
                queue[(size_t) w] = outFrame[(size_t) k];
                w = (w + 1) % qSize;
            }
            qCount += frameLen;
        }

        if (qCount > 0)
        {
            x[i] = queue[(size_t) qRead];
            qRead = (qRead + 1) % qSize;
            --qCount;
        }
        else
        {
            x[i] = 0.0f;
        }
    }
}
