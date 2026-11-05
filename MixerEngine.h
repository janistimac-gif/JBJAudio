#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include "NoiseReducer.h"
#include <atomic>
#include <memory>
#include <vector>

//==============================================================================
// One mixer channel = one capture device (microphone, virtual cable, loopback
// "Stereo Mix" ...). Each channel owns its own AudioDeviceManager, so channels
// may use different devices. Captured audio goes through a lock-free FIFO to the
// master output callback.
class InputChannel : private juce::AudioIODeviceCallback
{
public:
    explicit InputChannel (juce::String channelName);
    ~InputChannel() override;

    // Returns an empty string on success, otherwise an error message.
    juce::String openDevice (const juce::String& inputDeviceName);
    void closeDevice();
    const juce::String& getDeviceName() const noexcept { return deviceName; }

    // 0 = off, 1 = gate (DSP), 2 = AI (DeepFilterNet). "" = ok, otherwise the reason it failed.
    juce::String setNoiseMode (int mode);
    int getNoiseMode() const noexcept { return (int) reducer.getMode(); }

    // Called from the master audio thread: adds this channel into the Headphones bus
    // (l1/r1) and, when l2/r2 are not null, into the Stream bus.
    void mixInto (float* l1, float* r1, float* l2, float* r2, int numSamples) noexcept;

    juce::String name;
    std::atomic<float> gain { 1.0f };
    std::atomic<bool>  muted { false };
    std::atomic<bool>  toMain { true };     // send to the Headphones bus
    std::atomic<bool>  toStream { true };   // send to the Stream bus
    std::atomic<float> peak  { 0.0f };   // post-fader peak, reset by the UI

private:
    void audioDeviceIOCallbackWithContext (const float* const* in, int numIn,
                                           float* const* out, int numOut, int numSamples,
                                           const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override {}

    static constexpr int ringSize = 16384;

    juce::AudioDeviceManager manager;
    bool managerInitialised = false;
    juce::String deviceName;

    void pushToFifo (const float* l, const float* r, int n) noexcept;

    NoiseReducer reducer;
    std::atomic<double> deviceRate { 48000.0 };
    juce::HeapBlock<float> monoScratch;
    static constexpr int monoScratchSize = 2048;

    juce::AbstractFifo fifo { ringSize };
    juce::AudioBuffer<float> ring { 2, ringSize };
    float smoothedGain = 1.0f;   // audio thread only
};

//==============================================================================
// Second output device (e.g. a virtual cable that OBS / Discord record from).
// The master callback pushes the finished Stream mix here; this device pulls it.
class StreamOutput : private juce::AudioIODeviceCallback
{
public:
    StreamOutput();
    ~StreamOutput() override;

    juce::String open (const juce::String& outputDeviceName);   // "" closes
    void close();
    const juce::String& getDeviceName() const noexcept { return deviceName; }
    bool isActive() const noexcept { return active.load(); }
    void push (const float* l, const float* r, int numSamples) noexcept;

private:
    void audioDeviceIOCallbackWithContext (const float* const*, int, float* const* out, int numOut,
                                           int numSamples, const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override {}

    static constexpr int ringSize = 16384;
    juce::AudioDeviceManager manager;
    bool managerInitialised = false;
    juce::String deviceName;
    std::atomic<bool> active { false };
    juce::AbstractFifo fifo { ringSize };
    juce::AudioBuffer<float> ring { 2, ringSize };
};

//==============================================================================
// Master bus: sums every channel and plays it on the chosen output device.
class MixerEngine : private juce::AudioIODeviceCallback
{
public:
    MixerEngine();
    ~MixerEngine() override;

    juce::String setOutputDevice (const juce::String& outputDeviceName);
    juce::String setStreamDevice (const juce::String& outputDeviceName);   // "" = none
    juce::String getStreamDeviceName() const { return stream.getDeviceName(); }
    juce::String getOutputDeviceName() const;

    InputChannel* addChannel (const juce::String& name);
    void removeChannel (InputChannel* channel);
    int getNumChannels() const noexcept { return (int) channels.size(); }
    InputChannel* getChannel (int index) const { return channels[(size_t) index].get(); }

    juce::StringArray getInputDeviceNames();
    juce::StringArray getOutputDeviceNames();

    // ---- SoundPad -------------------------------------------------------------
    static constexpr int numPads = 8;
    juce::String loadPad (int index, const juce::File& file);   // "" = ok
    void clearPad (int index);
    juce::File getPadFile (int index) const;
    void togglePad (int index);                                  // play / stop
    bool isPadPlaying (int index) const noexcept;
    std::atomic<float> padGain { 1.0f };

    bool saveState (const juce::File& file) const;
    // Returns false when there was nothing to load.
    bool loadState (const juce::File& file, juce::StringArray& errors);

    static juce::File getDefaultStateFile();

    std::atomic<float> masterGain { 1.0f };
    std::atomic<bool>  masterMuted { false };
    std::atomic<float> masterPeak { 0.0f };
    std::atomic<float> streamGain { 1.0f };
    std::atomic<bool>  streamMuted { false };
    std::atomic<float> streamPeak { 0.0f };
    std::atomic<float> reverbAmount { 0.0f };   // 0..1 wet level on the whole mix

private:
    void audioDeviceIOCallbackWithContext (const float* const* in, int numIn,
                                           float* const* out, int numOut, int numSamples,
                                           const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override {}

    struct Clip { juce::AudioBuffer<float> data; };
    struct Pad
    {
        std::unique_ptr<Clip> clip;
        juce::File file;
        std::atomic<int> pos { 0 };
        std::atomic<bool> playing { false };
    };
    Pad pads[numPads];
    juce::SpinLock padLock;
    juce::AudioFormatManager formats;
    std::atomic<double> outputRate { 48000.0 };

    static constexpr int scratchSize = 4096;

    juce::AudioDeviceManager outputManager;
    std::vector<std::unique_ptr<InputChannel>> channels;
    juce::SpinLock channelLock;

    StreamOutput stream;
    juce::HeapBlock<float> scratchL, scratchR, scratchL2, scratchR2;
    float smoothedMaster = 1.0f;   // audio thread only
    juce::Reverb reverb, reverbStream;   // audio thread only
    float smoothedStream = 1.0f;
    float lastReverb = -1.0f;
};
