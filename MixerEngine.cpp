#include "MixerEngine.h"

//==============================================================================
// InputChannel
//==============================================================================
InputChannel::InputChannel (juce::String channelName) : name (std::move (channelName))
{
    ring.clear();
    monoScratch.calloc (monoScratchSize);
}

juce::String InputChannel::setNoiseMode (int m)
{
    return reducer.setMode ((NoiseReducer::Mode) juce::jlimit (0, 2, m), deviceRate.load());
}

InputChannel::~InputChannel()
{
    closeDevice();
}

juce::String InputChannel::openDevice (const juce::String& inputDeviceName)
{
    closeDevice();

    if (inputDeviceName.isEmpty())
        return {};

    if (! managerInitialised)
    {
        auto err = manager.initialise (2, 0, nullptr, false);
        if (err.isNotEmpty())
            return err;
        managerInitialised = true;
    }

    auto setup = manager.getAudioDeviceSetup();
    setup.inputDeviceName = inputDeviceName;
    setup.outputDeviceName = {};
    setup.useDefaultInputChannels = true;
    setup.useDefaultOutputChannels = false;
    setup.outputChannels.clear();

    auto err = manager.setAudioDeviceSetup (setup, true);
    if (err.isNotEmpty())
        return err;

    deviceName = inputDeviceName;
    manager.addAudioCallback (this);
    return {};
}

void InputChannel::closeDevice()
{
    if (managerInitialised)
    {
        manager.removeAudioCallback (this);
        manager.closeAudioDevice();
    }
    deviceName = {};
    peak.store (0.0f);
}

void InputChannel::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    fifo.reset();
    ring.clear();
    if (device != nullptr)
        deviceRate.store (device->getCurrentSampleRate());
    reducer.prepare (deviceRate.load());
}

void InputChannel::pushToFifo (const float* l, const float* r, int n) noexcept
{
    int s1, z1, s2, z2;
    fifo.prepareToWrite (n, s1, z1, s2, z2);   // when full, the excess is dropped

    if (z1 > 0) { ring.copyFrom (0, s1, l, z1);      ring.copyFrom (1, s1, r, z1); }
    if (z2 > 0) { ring.copyFrom (0, s2, l + z1, z2); ring.copyFrom (1, s2, r + z1, z2); }

    fifo.finishedWrite (z1 + z2);
}

void InputChannel::audioDeviceIOCallbackWithContext (const float* const* in, int numIn,
                                                     float* const* out, int numOut, int numSamples,
                                                     const juce::AudioIODeviceCallbackContext&)
{
    for (int c = 0; c < numOut; ++c)
        if (out[c] != nullptr)
            juce::FloatVectorOperations::clear (out[c], numSamples);

    if (numIn <= 0)
        return;

    if (reducer.getMode() == NoiseReducer::off)
    {
        pushToFifo (in[0], in[juce::jmin (1, numIn - 1)], numSamples);
        return;
    }

    // Noise reduction runs on the mono mix of the capture device.
    for (int done = 0; done < numSamples;)
    {
        const int chunk = juce::jmin (numSamples - done, monoScratchSize);
        const float* a = in[0] + done;
        const float* b = in[juce::jmin (1, numIn - 1)] + done;

        for (int i = 0; i < chunk; ++i)
            monoScratch[i] = 0.5f * (a[i] + b[i]);

        reducer.process (monoScratch.get(), chunk);
        pushToFifo (monoScratch.get(), monoScratch.get(), chunk);
        done += chunk;
    }
}

void InputChannel::mixInto (float* l1, float* r1, float* l2, float* r2, int numSamples) noexcept
{
    // Clock drift between two devices makes the FIFO grow. Keep latency bounded.
    const int ready = fifo.getNumReady();
    if (ready > 6000)
    {
        const int drop = ready - 1500;
        int s1, z1, s2, z2;
        fifo.prepareToRead (drop, s1, z1, s2, z2);
        fifo.finishedRead (z1 + z2);
    }

    const int toRead = juce::jmin (numSamples, fifo.getNumReady());
    if (toRead <= 0)
        return;

    const bool sendMain = toMain.load();
    const bool sendStream = toStream.load() && l2 != nullptr && r2 != nullptr;

    const float target = muted.load() ? 0.0f : gain.load();
    const float step = (target - smoothedGain) / (float) toRead;
    float localPeak = 0.0f;
    float g = smoothedGain;

    int s1, z1, s2, z2;
    fifo.prepareToRead (toRead, s1, z1, s2, z2);

    auto process = [&] (int start, int size, int outOffset)
    {
        const float* rl = ring.getReadPointer (0, start);
        const float* rr = ring.getReadPointer (1, start);
        for (int i = 0; i < size; ++i)
        {
            g += step;
            const float l = rl[i] * g;
            const float r = rr[i] * g;
            if (sendMain)   { l1[outOffset + i] += l; r1[outOffset + i] += r; }
            if (sendStream) { l2[outOffset + i] += l; r2[outOffset + i] += r; }
            localPeak = juce::jmax (localPeak, std::abs (l), std::abs (r));
        }
    };

    process (s1, z1, 0);
    process (s2, z2, z1);
    fifo.finishedRead (z1 + z2);

    smoothedGain = target;
    if (localPeak > peak.load())
        peak.store (localPeak);
}

//==============================================================================
// StreamOutput
//==============================================================================
StreamOutput::StreamOutput() { ring.clear(); }
StreamOutput::~StreamOutput() { close(); }

juce::String StreamOutput::open (const juce::String& outputDeviceName)
{
    close();
    if (outputDeviceName.isEmpty())
        return {};

    if (! managerInitialised)
    {
        auto err = manager.initialise (0, 2, nullptr, false);
        if (err.isNotEmpty())
            return err;
        managerInitialised = true;
    }

    auto setup = manager.getAudioDeviceSetup();
    setup.outputDeviceName = outputDeviceName;
    setup.inputDeviceName = {};
    setup.useDefaultInputChannels = false;
    setup.useDefaultOutputChannels = true;

    auto err = manager.setAudioDeviceSetup (setup, true);
    if (err.isNotEmpty())
        return err;

    deviceName = outputDeviceName;
    manager.addAudioCallback (this);
    active.store (true);
    return {};
}

void StreamOutput::close()
{
    active.store (false);
    if (managerInitialised)
    {
        manager.removeAudioCallback (this);
        manager.closeAudioDevice();
    }
    deviceName = {};
}

void StreamOutput::audioDeviceAboutToStart (juce::AudioIODevice*)
{
    fifo.reset();
    ring.clear();
}

void StreamOutput::push (const float* l, const float* r, int n) noexcept
{
    int s1, z1, s2, z2;
    fifo.prepareToWrite (n, s1, z1, s2, z2);
    if (z1 > 0) { ring.copyFrom (0, s1, l, z1);      ring.copyFrom (1, s1, r, z1); }
    if (z2 > 0) { ring.copyFrom (0, s2, l + z1, z2); ring.copyFrom (1, s2, r + z1, z2); }
    fifo.finishedWrite (z1 + z2);
}

void StreamOutput::audioDeviceIOCallbackWithContext (const float* const*, int, float* const* out, int numOut,
                                                     int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    for (int c = 0; c < numOut; ++c)
        if (out[c] != nullptr)
            juce::FloatVectorOperations::clear (out[c], numSamples);

    const int ready = fifo.getNumReady();
    if (ready > 6000)
    {
        int s1, z1, s2, z2;
        fifo.prepareToRead (ready - 1500, s1, z1, s2, z2);
        fifo.finishedRead (z1 + z2);
    }

    const int toRead = juce::jmin (numSamples, fifo.getNumReady());
    if (toRead <= 0 || numOut <= 0)
        return;

    int s1, z1, s2, z2;
    fifo.prepareToRead (toRead, s1, z1, s2, z2);

    auto copy = [&] (int start, int size, int offset)
    {
        for (int c = 0; c < numOut && c < 2; ++c)
            if (out[c] != nullptr)
                juce::FloatVectorOperations::copy (out[c] + offset, ring.getReadPointer (c, start), size);
    };
    copy (s1, z1, 0);
    copy (s2, z2, z1);
    fifo.finishedRead (z1 + z2);
}

//==============================================================================
// MixerEngine
//==============================================================================
MixerEngine::MixerEngine()
{
    scratchL.calloc (scratchSize);
    scratchR.calloc (scratchSize);
    scratchL2.calloc (scratchSize);
    scratchR2.calloc (scratchSize);
    formats.registerBasicFormats();   // wav, aiff, flac, ogg, mp3 (JUCE_USE_MP3AUDIOFORMAT)

    outputManager.initialise (0, 2, nullptr, true);
    outputManager.addAudioCallback (this);
}

MixerEngine::~MixerEngine()
{
    outputManager.removeAudioCallback (this);
    outputManager.closeAudioDevice();
    stream.close();
    channels.clear();
}

void MixerEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    if (device != nullptr)
        outputRate.store (device->getCurrentSampleRate());
    reverb.setSampleRate (outputRate.load());
    reverb.reset();
    reverbStream.setSampleRate (outputRate.load());
    reverbStream.reset();
    lastReverb = -1.0f;
}

juce::String MixerEngine::loadPad (int index, const juce::File& file)
{
    if (index < 0 || index >= numPads)
        return "Invalid pad";

    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr)
        return "Unsupported or unreadable file: " + file.getFileName();

    const double rate = outputRate.load();
    if (reader->lengthInSamples <= 0 || (double) reader->lengthInSamples / reader->sampleRate > 600.0)
        return "Sound is empty or longer than 10 minutes";

    const int srcLen = (int) reader->lengthInSamples;
    juce::AudioBuffer<float> src (2, srcLen);
    reader->read (&src, 0, srcLen, 0, true, true);
    if (reader->numChannels < 2)
        src.copyFrom (1, 0, src, 0, 0, srcLen);

    auto clip = std::make_unique<Clip>();
    const double ratio = reader->sampleRate / rate;
    const int dstLen = (int) std::floor ((double) srcLen / ratio);
    clip->data.setSize (2, juce::jmax (1, dstLen));

    for (int ch = 0; ch < 2; ++ch)
    {
        const float* in = src.getReadPointer (ch);
        float* out = clip->data.getWritePointer (ch);
        for (int i = 0; i < clip->data.getNumSamples(); ++i)     // linear resampling
        {
            const double x = i * ratio;
            const int i0 = (int) x;
            const int i1 = juce::jmin (i0 + 1, srcLen - 1);
            const float f = (float) (x - i0);
            out[i] = in[i0] + f * (in[i1] - in[i0]);
        }
    }

    std::unique_ptr<Clip> old;
    {
        const juce::SpinLock::ScopedLockType sl (padLock);
        pads[index].playing.store (false);
        pads[index].pos.store (0);
        old = std::move (pads[index].clip);
        pads[index].clip = std::move (clip);
        pads[index].file = file;
    }
    return {};
}

void MixerEngine::clearPad (int index)
{
    if (index < 0 || index >= numPads)
        return;

    std::unique_ptr<Clip> old;
    const juce::SpinLock::ScopedLockType sl (padLock);
    pads[index].playing.store (false);
    old = std::move (pads[index].clip);
    pads[index].file = juce::File();
}

juce::File MixerEngine::getPadFile (int index) const
{
    return (index >= 0 && index < numPads) ? pads[index].file : juce::File();
}

void MixerEngine::togglePad (int index)
{
    if (index < 0 || index >= numPads)
        return;

    auto& p = pads[index];
    if (p.playing.load())
    {
        p.playing.store (false);
    }
    else if (p.clip != nullptr)
    {
        p.pos.store (0);
        p.playing.store (true);
    }
}

bool MixerEngine::isPadPlaying (int index) const noexcept
{
    return index >= 0 && index < numPads && pads[index].playing.load();
}

juce::String MixerEngine::setOutputDevice (const juce::String& outputDeviceName)
{
    auto setup = outputManager.getAudioDeviceSetup();
    setup.outputDeviceName = outputDeviceName;
    setup.inputDeviceName = {};
    setup.useDefaultInputChannels = false;
    setup.useDefaultOutputChannels = true;
    return outputManager.setAudioDeviceSetup (setup, true);
}

juce::String MixerEngine::setStreamDevice (const juce::String& outputDeviceName)
{
    return stream.open (outputDeviceName);
}

juce::String MixerEngine::getOutputDeviceName() const
{
    return outputManager.getAudioDeviceSetup().outputDeviceName;
}

InputChannel* MixerEngine::addChannel (const juce::String& name)
{
    auto channel = std::make_unique<InputChannel> (name);
    auto* raw = channel.get();
    const juce::SpinLock::ScopedLockType sl (channelLock);
    channels.push_back (std::move (channel));
    return raw;
}

void MixerEngine::removeChannel (InputChannel* channel)
{
    std::unique_ptr<InputChannel> removed;
    {
        const juce::SpinLock::ScopedLockType sl (channelLock);
        for (auto it = channels.begin(); it != channels.end(); ++it)
        {
            if (it->get() == channel)
            {
                removed = std::move (*it);
                channels.erase (it);
                break;
            }
        }
    }
    removed.reset();   // closes the capture device outside the lock
}

juce::StringArray MixerEngine::getInputDeviceNames()
{
    if (auto* type = outputManager.getCurrentDeviceTypeObject())
    {
        type->scanForDevices();
        return type->getDeviceNames (true);
    }
    return {};
}

juce::StringArray MixerEngine::getOutputDeviceNames()
{
    if (auto* type = outputManager.getCurrentDeviceTypeObject())
    {
        type->scanForDevices();
        return type->getDeviceNames (false);
    }
    return {};
}

void MixerEngine::audioDeviceIOCallbackWithContext (const float* const*, int,
                                                    float* const* out, int numOut, int numSamples,
                                                    const juce::AudioIODeviceCallbackContext&)
{
    for (int c = 0; c < numOut; ++c)
        if (out[c] != nullptr)
            juce::FloatVectorOperations::clear (out[c], numSamples);

    const juce::SpinLock::ScopedTryLockType sl (channelLock);
    if (! sl.isLocked())
        return;

    const bool streamOn = stream.isActive();
    const float n1 = (float) juce::jmax (1, numSamples);

    const float targetM = masterMuted.load() ? 0.0f : masterGain.load();
    const float stepM = (targetM - smoothedMaster) / n1;
    float gM = smoothedMaster;
    float peakM = 0.0f;

    const float targetS = streamMuted.load() ? 0.0f : streamGain.load();
    const float stepS = (targetS - smoothedStream) / n1;
    float gS = smoothedStream;
    float peakS = 0.0f;

    const float rv = reverbAmount.load();
    if (rv != lastReverb)
    {
        juce::Reverb::Parameters rp;
        rp.roomSize = 0.55f;
        rp.damping = 0.5f;
        rp.wetLevel = 0.45f * rv;
        rp.dryLevel = 1.0f;
        rp.width = 1.0f;
        reverb.setParameters (rp);
        reverbStream.setParameters (rp);
        lastReverb = rv;
    }

    for (int done = 0; done < numSamples;)
    {
        const int chunk = juce::jmin (numSamples - done, scratchSize);
        juce::FloatVectorOperations::clear (scratchL.get(), chunk);
        juce::FloatVectorOperations::clear (scratchR.get(), chunk);
        juce::FloatVectorOperations::clear (scratchL2.get(), chunk);
        juce::FloatVectorOperations::clear (scratchR2.get(), chunk);

        for (auto& ch : channels)
            ch->mixInto (scratchL.get(), scratchR.get(),
                         streamOn ? scratchL2.get() : nullptr, streamOn ? scratchR2.get() : nullptr, chunk);

        {
            const juce::SpinLock::ScopedTryLockType pl (padLock);
            if (pl.isLocked())
            {
                const float pg = padGain.load();
                for (auto& pad : pads)
                {
                    if (! pad.playing.load() || pad.clip == nullptr)
                        continue;

                    const int len = pad.clip->data.getNumSamples();
                    const float* cl = pad.clip->data.getReadPointer (0);
                    const float* cr = pad.clip->data.getReadPointer (1);
                    int p = pad.pos.load();
                    int i = 0;
                    for (; i < chunk && p < len; ++i, ++p)
                    {
                        scratchL[i] += cl[p] * pg;
                        scratchR[i] += cr[p] * pg;
                        if (streamOn)
                        {
                            scratchL2[i] += cl[p] * pg;
                            scratchR2[i] += cr[p] * pg;
                        }
                    }
                    pad.pos.store (p);
                    if (p >= len)
                        pad.playing.store (false);
                }
            }
        }

        if (rv > 0.001f)
        {
            reverb.processStereo (scratchL.get(), scratchR.get(), chunk);
            if (streamOn)
                reverbStream.processStereo (scratchL2.get(), scratchR2.get(), chunk);
        }

        for (int i = 0; i < chunk; ++i)
        {
            gM += stepM;
            const float l = juce::jlimit (-1.0f, 1.0f, scratchL[i] * gM);
            const float r = juce::jlimit (-1.0f, 1.0f, scratchR[i] * gM);
            peakM = juce::jmax (peakM, std::abs (l), std::abs (r));

            if (numOut >= 2)
            {
                if (out[0] != nullptr) out[0][done + i] = l;
                if (out[1] != nullptr) out[1][done + i] = r;
            }
            else if (numOut == 1 && out[0] != nullptr)
            {
                out[0][done + i] = 0.5f * (l + r);
            }
        }

        if (streamOn)
        {
            for (int i = 0; i < chunk; ++i)
            {
                gS += stepS;
                scratchL2[i] = juce::jlimit (-1.0f, 1.0f, scratchL2[i] * gS);
                scratchR2[i] = juce::jlimit (-1.0f, 1.0f, scratchR2[i] * gS);
                peakS = juce::jmax (peakS, std::abs (scratchL2[i]), std::abs (scratchR2[i]));
            }
            stream.push (scratchL2.get(), scratchR2.get(), chunk);
        }

        done += chunk;
    }

    smoothedMaster = targetM;
    smoothedStream = targetS;
    if (peakM > masterPeak.load()) masterPeak.store (peakM);
    if (peakS > streamPeak.load()) streamPeak.store (peakS);
}

//==============================================================================
// Persistence
//==============================================================================
juce::File MixerEngine::getDefaultStateFile()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("JBJAudio")
               .getChildFile ("mixer.xml");
}

bool MixerEngine::saveState (const juce::File& file) const
{
    juce::XmlElement root ("JBJAudioMixer");
    root.setAttribute ("version", 1);

    auto* master = root.createNewChildElement ("Master");
    master->setAttribute ("device", getOutputDeviceName());
    master->setAttribute ("gain", (double) masterGain.load());
    master->setAttribute ("muted", masterMuted.load());

    master->setAttribute ("reverb", (double) reverbAmount.load());
    master->setAttribute ("padGain", (double) padGain.load());
    auto* st = root.createNewChildElement ("Stream");
    st->setAttribute ("device", stream.getDeviceName());
    st->setAttribute ("gain", (double) streamGain.load());
    st->setAttribute ("muted", streamMuted.load());

    for (int i = 0; i < numPads; ++i)
        if (pads[i].file.existsAsFile())
        {
            auto* e = root.createNewChildElement ("Pad");
            e->setAttribute ("index", i);
            e->setAttribute ("file", pads[i].file.getFullPathName());
        }

    for (auto& ch : channels)
    {
        auto* e = root.createNewChildElement ("Channel");
        e->setAttribute ("name", ch->name);
        e->setAttribute ("device", ch->getDeviceName());
        e->setAttribute ("gain", (double) ch->gain.load());
        e->setAttribute ("muted", ch->muted.load());
        e->setAttribute ("noise", ch->getNoiseMode());
        e->setAttribute ("toMain", ch->toMain.load());
        e->setAttribute ("toStream", ch->toStream.load());
    }

    file.getParentDirectory().createDirectory();
    return root.writeTo (file);
}

bool MixerEngine::loadState (const juce::File& file, juce::StringArray& errors)
{
    auto xml = juce::parseXML (file);
    if (xml == nullptr || ! xml->hasTagName ("JBJAudioMixer"))
        return false;

    if (auto* m = xml->getChildByName ("Master"))
    {
        masterGain.store ((float) m->getDoubleAttribute ("gain", 1.0));
        masterMuted.store (m->getBoolAttribute ("muted", false));
        padGain.store ((float) m->getDoubleAttribute ("padGain", 1.0));
        reverbAmount.store ((float) m->getDoubleAttribute ("reverb", 0.0));
        const auto dev = m->getStringAttribute ("device");
        if (dev.isNotEmpty())
        {
            auto err = setOutputDevice (dev);
            if (err.isNotEmpty())
                errors.add ("Output: " + err);
        }
    }

    if (auto* st = xml->getChildByName ("Stream"))
    {
        streamGain.store ((float) st->getDoubleAttribute ("gain", 1.0));
        streamMuted.store (st->getBoolAttribute ("muted", false));
        const auto dev = st->getStringAttribute ("device");
        if (dev.isNotEmpty())
        {
            auto err = setStreamDevice (dev);
            if (err.isNotEmpty())
                errors.add ("Stream: " + err);
        }
    }

    for (auto* e : xml->getChildWithTagNameIterator ("Pad"))
    {
        auto err = loadPad (e->getIntAttribute ("index", -1), juce::File (e->getStringAttribute ("file")));
        if (err.isNotEmpty())
            errors.add ("Pad " + juce::String (e->getIntAttribute ("index", -1) + 1) + ": " + err);
    }

    for (auto* e : xml->getChildWithTagNameIterator ("Channel"))
    {
        auto* ch = addChannel (e->getStringAttribute ("name", "Channel"));
        ch->gain.store ((float) e->getDoubleAttribute ("gain", 1.0));
        ch->muted.store (e->getBoolAttribute ("muted", false));
        ch->toMain.store (e->getBoolAttribute ("toMain", true));
        ch->toStream.store (e->getBoolAttribute ("toStream", true));
        const auto dev = e->getStringAttribute ("device");
        if (dev.isNotEmpty())
        {
            auto err = ch->openDevice (dev);
            if (err.isNotEmpty())
                errors.add (ch->name + ": " + err);
        }

        const int nr = e->getIntAttribute ("noise", 0);
        if (nr != 0)
        {
            auto err = ch->setNoiseMode (nr);
            if (err.isNotEmpty())
            {
                errors.add (ch->name + ": " + err);
                ch->setNoiseMode (nr == 2 ? 1 : 0);   // fall back to the DSP gate
            }
        }
    }

    return true;
}
