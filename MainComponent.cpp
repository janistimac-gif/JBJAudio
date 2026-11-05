#include "MainComponent.h"

MainComponent::MainComponent()
{
    addAndMakeVisible (title);
    title.setText ("JBJAudio", juce::dontSendNotification);
    title.setFont (juce::FontOptions (22.0f, juce::Font::bold));
    title.setColour (juce::Label::textColourId, juce::Colour (0xff19c3a6));

    addAndMakeVisible (status);
    status.setJustificationType (juce::Justification::centredRight);
    status.setFont (juce::FontOptions (13.0f));

    addAndMakeVisible (reverbLabel);
    reverbLabel.setText ("Reverb", juce::dontSendNotification);
    reverbLabel.setFont (juce::FontOptions (13.0f));
    addAndMakeVisible (reverbSlider);
    reverbSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    reverbSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    reverbSlider.setRange (0.0, 1.0, 0.01);
    reverbSlider.setDoubleClickReturnValue (true, 0.0);
    reverbSlider.onValueChange = [this] { engine.reverbAmount.store ((float) reverbSlider.getValue()); };

    addAndMakeVisible (addButton);
    addButton.setButtonText ("+ Add channel");
    addButton.onClick = [this] { addNewChannel(); };

    addAndMakeVisible (refreshButton);
    refreshButton.setButtonText ("Refresh devices");
    refreshButton.onClick = [this] { rebuildStrips(); setStatus ("Device list refreshed", false); };

    addAndMakeVisible (viewport);
    viewport.setViewedComponent (&stripContainer, false);
    viewport.setScrollBarsShown (false, true);

    addAndMakeVisible (padLabel);
    padLabel.setText ("SoundPad  (click = play/stop, right click = assign, keys 1-8)", juce::dontSendNotification);
    padLabel.setFont (juce::FontOptions (13.0f));
    padLabel.setColour (juce::Label::textColourId, juce::Colour (0xff9ba2a8));

    addAndMakeVisible (padVolume);
    padVolume.setSliderStyle (juce::Slider::LinearHorizontal);
    padVolume.setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 20);
    padVolume.setRange (-60.0, 6.0, 0.1);
    padVolume.setDoubleClickReturnValue (true, 0.0);
    padVolume.setTextValueSuffix (" dB");
    padVolume.onValueChange = [this]
    {
        engine.padGain.store (juce::Decibels::decibelsToGain ((float) padVolume.getValue(), -59.9f));
    };

    for (int i = 0; i < MixerEngine::numPads; ++i)
    {
        auto& b = padButtons[i];
        addAndMakeVisible (b);
        b.setClickingTogglesState (false);
        b.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff19c3a6));
        b.onClick = [this, i]
        {
            if (engine.getPadFile (i) == juce::File())
                padRightClicked (i);
            else
                engine.togglePad (i);
        };
        b.onRightClick = [this, i] { padRightClicked (i); };
    }
    setWantsKeyboardFocus (true);

    juce::StringArray errors;
    if (! engine.loadState (MixerEngine::getDefaultStateFile(), errors))
        for (auto* n : { "System", "Chat", "Game", "Music" })
            engine.addChannel (n);

    reverbSlider.setValue (engine.reverbAmount.load(), juce::dontSendNotification);
    padVolume.setValue (juce::Decibels::gainToDecibels (engine.padGain.load(), -60.0f), juce::dontSendNotification);
    refreshPadLabels();
    rebuildStrips();

    if (errors.isEmpty())
        setStatus ("Ready", false);
    else
        setStatus (errors.joinIntoString ("; "), true);

    setSize (960, 540);
    startTimerHz (30);
}

MainComponent::~MainComponent()
{
    stopTimer();
    engine.saveState (MixerEngine::getDefaultStateFile());
    strips.clear();
    masterStrip.reset();
}

void MainComponent::setStatus (const juce::String& text, bool error)
{
    status.setText (text, juce::dontSendNotification);
    status.setColour (juce::Label::textColourId, error ? juce::Colour (0xffff7b7b) : juce::Colour (0xff9ba2a8));
}

void MainComponent::addNewChannel()
{
    engine.addChannel ("Channel " + juce::String (engine.getNumChannels() + 1));
    rebuildStrips();
    engine.saveState (MixerEngine::getDefaultStateFile());
}

void MainComponent::rebuildStrips()
{
    strips.clear();
    masterStrip.reset();
    streamStrip.reset();

    const auto inputs = engine.getInputDeviceNames();
    const auto outputs = engine.getOutputDeviceNames();

    for (int i = 0; i < engine.getNumChannels(); ++i)
    {
        auto* ch = engine.getChannel (i);

        StripBinding b;
        b.title = ch->name;
        b.nameEditable = true;
        b.allowNoDevice = true;
        b.removable = true;
        b.hasNoise = true;
        b.noiseMode = ch->getNoiseMode();
        b.gain = &ch->gain;
        b.muted = &ch->muted;
        b.toMain = &ch->toMain;
        b.toStream = &ch->toStream;
        b.peak = &ch->peak;
        b.devices = inputs;
        b.selectedDevice = ch->getDeviceName();
        b.onNoiseMode = [this, ch] (int mode)
        {
            auto err = ch->setNoiseMode (mode);
            if (err.isEmpty())
            {
                setStatus (ch->name + ": noise reduction " + juce::StringArray { "off", "gate", "AI" }[mode], false);
                return;
            }
            setStatus (err, true);
            // revert the selector to the mode that is actually active
            juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MainComponent> (this)]
                                             { if (safe != nullptr) safe->rebuildStrips(); });
        };
        b.onRenamed = [ch] (const juce::String& n) { ch->name = n; };
        b.onDeviceChosen = [this, ch] (const juce::String& dev)
        {
            auto err = ch->openDevice (dev);
            if (err.isEmpty())
                setStatus (dev.isEmpty() ? ch->name + ": no input" : ch->name + " <- " + dev, false);
            else
                setStatus (ch->name + ": " + err, true);
        };
        b.onRemove = [this, ch]
        {
            engine.removeChannel (ch);
            rebuildStrips();
            engine.saveState (MixerEngine::getDefaultStateFile());
        };

        auto* strip = strips.add (new ChannelStrip (std::move (b)));
        stripContainer.addAndMakeVisible (strip);
    }

    StripBinding m;
    m.title = "Headphones";
    m.gain = &engine.masterGain;
    m.muted = &engine.masterMuted;
    m.peak = &engine.masterPeak;
    m.devices = outputs;
    m.selectedDevice = engine.getOutputDeviceName();
    m.onDeviceChosen = [this] (const juce::String& dev)
    {
        auto err = engine.setOutputDevice (dev);
        setStatus (err.isEmpty() ? "Output: " + dev : "Output: " + err, err.isNotEmpty());
    };
    masterStrip = std::make_unique<ChannelStrip> (std::move (m));
    addAndMakeVisible (*masterStrip);

    StripBinding sb;
    sb.title = "Stream";
    sb.allowNoDevice = true;
    sb.gain = &engine.streamGain;
    sb.muted = &engine.streamMuted;
    sb.peak = &engine.streamPeak;
    sb.devices = outputs;
    sb.selectedDevice = engine.getStreamDeviceName();
    sb.onDeviceChosen = [this] (const juce::String& dev)
    {
        auto err = engine.setStreamDevice (dev);
        setStatus (err.isEmpty() ? (dev.isEmpty() ? juce::String ("Stream: off") : "Stream: " + dev) : "Stream: " + err,
                   err.isNotEmpty());
    };
    streamStrip = std::make_unique<ChannelStrip> (std::move (sb));
    addAndMakeVisible (*streamStrip);

    resized();
}

void MainComponent::refreshPadLabels()
{
    for (int i = 0; i < MixerEngine::numPads; ++i)
    {
        auto f = engine.getPadFile (i);
        padButtons[i].setButtonText (juce::String (i + 1) + ": "
                                     + (f == juce::File() ? juce::String ("empty") : f.getFileNameWithoutExtension()));
    }
}

void MainComponent::padRightClicked (int index)
{
    juce::PopupMenu menu;
    menu.addItem (1, "Assign sound...");
    menu.addItem (2, "Clear", engine.getPadFile (index) != juce::File());
    menu.showMenuAsync (juce::PopupMenu::Options(), [this, index] (int result)
    {
        if (result == 2)
        {
            engine.clearPad (index);
            refreshPadLabels();
        }
        else if (result == 1)
        {
            chooser = std::make_unique<juce::FileChooser> ("Select a sound", juce::File(), "*.wav;*.mp3;*.flac;*.aiff;*.ogg");
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                  [this, index] (const juce::FileChooser& fc)
                                  {
                                      auto f = fc.getResult();
                                      if (f == juce::File())
                                          return;
                                      auto err = engine.loadPad (index, f);
                                      setStatus (err.isEmpty() ? "Pad " + juce::String (index + 1) + ": " + f.getFileName() : err,
                                                 err.isNotEmpty());
                                      refreshPadLabels();
                                  });
        }
    });
}

bool MainComponent::keyPressed (const juce::KeyPress& key)
{
    const auto c = key.getTextCharacter();
    if (c >= '1' && c <= '8')
    {
        engine.togglePad (c - '1');
        return true;
    }
    return false;
}

// Global hotkeys (work while JBJAudio is in the background):
//   Ctrl+Alt+M mute master, Ctrl+Alt+Up/Down master volume, Ctrl+Alt+1..8 SoundPad
void MainComponent::pollGlobalHotkeys()
{
    const auto mods = juce::ModifierKeys::getCurrentModifiersRealtime();
    const bool armed = mods.isCtrlDown() && mods.isAltDown();

    auto pressed = [&] (int slot, int keyCode)
    {
        const bool down = armed && juce::KeyPress::isKeyCurrentlyDown (keyCode);
        const bool edge = down && ! hotkeyPrev[slot];
        hotkeyPrev[slot] = down;
        return edge;
    };

    bool masterChanged = false;

    if (pressed (0, 'M'))
    {
        // mute both buses, or unmute both when everything is already muted
        const bool mute = ! (engine.masterMuted.load() && engine.streamMuted.load());
        engine.masterMuted.store (mute);
        engine.streamMuted.store (mute);
        masterChanged = true;
    }

    const bool upEdge = pressed (1, juce::KeyPress::upKey);
    const bool downEdge = pressed (2, juce::KeyPress::downKey);
    if (upEdge || downEdge)
    {
        const float db = juce::Decibels::gainToDecibels (engine.masterGain.load(), -60.0f) + (upEdge ? 2.0f : -2.0f);
        engine.masterGain.store (juce::Decibels::decibelsToGain (juce::jlimit (-60.0f, 6.0f, db), -59.9f));
        masterChanged = true;
    }

    for (int i = 0; i < MixerEngine::numPads; ++i)
        if (pressed (3 + i, '1' + i))
            engine.togglePad (i);

    if (masterChanged && masterStrip != nullptr && streamStrip != nullptr)
    {
        masterStrip->syncFromBinding();
        streamStrip->syncFromBinding();
        setStatus (engine.masterMuted.load() ? "Master muted" : "Master unmuted", false);
    }
}

void MainComponent::timerCallback()
{
    pollGlobalHotkeys();
    for (int i = 0; i < MixerEngine::numPads; ++i)
        padButtons[i].setToggleState (engine.isPadPlaying (i), juce::dontSendNotification);

    for (auto* s : strips)
        s->updateMeter();
    if (masterStrip != nullptr)
        masterStrip->updateMeter();
    if (streamStrip != nullptr)
        streamStrip->updateMeter();
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff12161b));
}

void MainComponent::resized()
{
    auto r = getLocalBounds().reduced (12);

    auto top = r.removeFromTop (34);
    title.setBounds (top.removeFromLeft (140));
    addButton.setBounds (top.removeFromLeft (120).reduced (0, 3));
    top.removeFromLeft (8);
    refreshButton.setBounds (top.removeFromLeft (130).reduced (0, 3));
    top.removeFromLeft (12);
    reverbLabel.setBounds (top.removeFromLeft (50));
    reverbSlider.setBounds (top.removeFromLeft (120));
    top.removeFromLeft (8);
    status.setBounds (top);

    r.removeFromTop (8);

    auto padArea = r.removeFromBottom (92);
    r.removeFromBottom (8);
    {
        auto head = padArea.removeFromTop (24);
        padVolume.setBounds (head.removeFromRight (240));
        padLabel.setBounds (head);
        padArea.removeFromTop (4);
        const int w = padArea.getWidth() / MixerEngine::numPads;
        for (int i = 0; i < MixerEngine::numPads; ++i)
            padButtons[i].setBounds (padArea.removeFromLeft (w).reduced (3, 0));
    }

    if (streamStrip != nullptr)
    {
        streamStrip->setBounds (r.removeFromRight (stripWidth));
        r.removeFromRight (stripGap);
    }
    if (masterStrip != nullptr)
    {
        masterStrip->setBounds (r.removeFromRight (stripWidth));
        r.removeFromRight (stripGap);
    }

    viewport.setBounds (r);

    const int n = strips.size();
    const int contentW = juce::jmax (r.getWidth(), n * (stripWidth + stripGap));
    const int h = r.getHeight() - viewport.getScrollBarThickness();
    stripContainer.setSize (contentW, h);

    for (int i = 0; i < n; ++i)
        strips[i]->setBounds (i * (stripWidth + stripGap), 0, stripWidth, h);
}
