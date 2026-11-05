#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include "MixerEngine.h"

//==============================================================================
class LevelMeter : public juce::Component
{
public:
    // peak is linear gain, 0..1+
    void push (float peak)
    {
        shown = juce::jmax (peak, shown * 0.88f);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (juce::Colour (0xff11151a));
        g.fillRoundedRectangle (r, 3.0f);

        const float db = juce::Decibels::gainToDecibels (shown, -60.0f);
        const float p = juce::jlimit (0.0f, 1.0f, juce::jmap (db, -60.0f, 0.0f, 0.0f, 1.0f));
        auto bar = r.reduced (2.0f);
        bar = bar.removeFromBottom (bar.getHeight() * p);

        juce::ColourGradient grad (juce::Colour (0xffff4d4d), 0.0f, r.getY(),
                                   juce::Colour (0xff19c3a6), 0.0f, r.getBottom(), false);
        grad.addColour (0.25, juce::Colour (0xffffc83d));
        g.setGradientFill (grad);
        g.fillRoundedRectangle (bar, 2.0f);
    }

private:
    float shown = 0.0f;
};

//==============================================================================
// Describes what a strip controls. Used for input channels and the master strip.
struct StripBinding
{
    juce::String title;
    bool nameEditable = false;
    bool allowNoDevice = false;     // adds a "(none)" entry
    bool removable = false;
    bool hasNoise = false;          // show the noise reduction selector
    int noiseMode = 0;
    std::function<void (int)> onNoiseMode;
    std::atomic<float>* gain = nullptr;
    std::atomic<bool>*  muted = nullptr;
    std::atomic<float>* peak = nullptr;
    std::atomic<bool>* toMain = nullptr;     // routing buttons (channels only)
    std::atomic<bool>* toStream = nullptr;
    juce::StringArray devices;
    juce::String selectedDevice;
    std::function<void (const juce::String&)> onDeviceChosen;   // "" = none
    std::function<void (const juce::String&)> onRenamed;
    std::function<void()> onRemove;
};

class ChannelStrip : public juce::Component
{
public:
    explicit ChannelStrip (StripBinding b) : binding (std::move (b))
    {
        addAndMakeVisible (name);
        name.setText (binding.title, juce::dontSendNotification);
        name.setJustificationType (juce::Justification::centred);
        name.setFont (juce::FontOptions (15.0f, juce::Font::bold));
        name.setEditable (binding.nameEditable);
        name.onTextChange = [this]
        {
            if (binding.onRenamed) binding.onRenamed (name.getText());
        };

        addAndMakeVisible (device);
        int id = 1;
        if (binding.allowNoDevice)
            device.addItem ("(none)", id++);
        for (auto& d : binding.devices)
            device.addItem (d, id++);

        if (binding.selectedDevice.isNotEmpty() && binding.devices.contains (binding.selectedDevice))
            device.setText (binding.selectedDevice, juce::dontSendNotification);
        else if (binding.allowNoDevice)
            device.setSelectedId (1, juce::dontSendNotification);

        device.onChange = [this]
        {
            auto text = device.getText();
            if (binding.onDeviceChosen)
                binding.onDeviceChosen (text == "(none)" ? juce::String() : text);
        };

        if (binding.hasNoise)
        {
            addAndMakeVisible (noise);
            noise.addItem ("Noise: Off", 1);
            noise.addItem ("Noise: Gate", 2);
            noise.addItem ("Noise: AI", 3);
            noise.setSelectedId (binding.noiseMode + 1, juce::dontSendNotification);
            noise.onChange = [this]
            {
                if (binding.onNoiseMode)
                    binding.onNoiseMode (noise.getSelectedId() - 1);
            };
        }

        addAndMakeVisible (fader);
        fader.setSliderStyle (juce::Slider::LinearVertical);
        fader.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 18);
        fader.setRange (-60.0, 6.0, 0.1);
        fader.setDoubleClickReturnValue (true, 0.0);
        fader.setTextValueSuffix (" dB");
        fader.textFromValueFunction = [] (double v) { return v <= -59.9 ? juce::String ("-inf") : juce::String (v, 1); };
        fader.setValue (binding.gain != nullptr
                            ? (double) juce::Decibels::gainToDecibels (binding.gain->load(), -60.0f)
                            : 0.0,
                        juce::dontSendNotification);
        fader.onValueChange = [this]
        {
            if (binding.gain != nullptr)
                binding.gain->store (juce::Decibels::decibelsToGain ((float) fader.getValue(), -59.9f));
        };

        addAndMakeVisible (meter);

        if (binding.toMain != nullptr && binding.toStream != nullptr)
        {
            for (auto* bt : { &sendMain, &sendStream })
            {
                addAndMakeVisible (*bt);
                bt->setClickingTogglesState (true);
                bt->setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff19c3a6));
                bt->setColour (juce::TextButton::textColourOnId, juce::Colour (0xff12161b));
            }
            sendMain.setButtonText ("HP");
            sendMain.setTooltip ("Send to Headphones");
            sendStream.setButtonText ("ST");
            sendStream.setTooltip ("Send to Stream");
            sendMain.setToggleState (binding.toMain->load(), juce::dontSendNotification);
            sendStream.setToggleState (binding.toStream->load(), juce::dontSendNotification);
            sendMain.onClick = [this] { binding.toMain->store (sendMain.getToggleState()); };
            sendStream.onClick = [this] { binding.toStream->store (sendStream.getToggleState()); };
        }

        addAndMakeVisible (mute);
        mute.setButtonText ("Mute");
        mute.setClickingTogglesState (true);
        mute.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffe5484d));
        mute.setToggleState (binding.muted != nullptr && binding.muted->load(), juce::dontSendNotification);
        mute.onClick = [this]
        {
            if (binding.muted != nullptr)
                binding.muted->store (mute.getToggleState());
        };

        if (binding.removable)
        {
            addAndMakeVisible (remove);
            remove.setButtonText ("Remove");
            remove.onClick = [this]
            {
                if (binding.onRemove)
                    juce::MessageManager::callAsync ([cb = binding.onRemove] { cb(); });
            };
        }
    }

    void setNoiseSelection (int mode) { noise.setSelectedId (mode + 1, juce::dontSendNotification); }

    // Re-read mute/fader after they were changed from outside (hotkeys).
    void syncFromBinding()
    {
        if (binding.muted != nullptr)
            mute.setToggleState (binding.muted->load(), juce::dontSendNotification);
        if (binding.gain != nullptr)
            fader.setValue (juce::Decibels::gainToDecibels (binding.gain->load(), -60.0f), juce::dontSendNotification);
    }

    void updateMeter()
    {
        if (binding.peak != nullptr)
            meter.push (binding.peak->exchange (0.0f));
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (2.0f);
        g.setColour (juce::Colour (0xff1c222a));
        g.fillRoundedRectangle (r, 8.0f);
        g.setColour (juce::Colour (0xff2b343f));
        g.drawRoundedRectangle (r, 8.0f, 1.0f);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8);
        name.setBounds (r.removeFromTop (24));
        r.removeFromTop (4);
        device.setBounds (r.removeFromTop (26));
        r.removeFromTop (4);
        if (binding.hasNoise)
        {
            noise.setBounds (r.removeFromTop (26));
            r.removeFromTop (4);
        }
        r.removeFromTop (2);

        if (binding.removable)
        {
            remove.setBounds (r.removeFromBottom (24));
            r.removeFromBottom (4);
        }
        mute.setBounds (r.removeFromBottom (28));
        r.removeFromBottom (4);
        if (binding.toMain != nullptr)
        {
            auto sends = r.removeFromBottom (24);
            sendMain.setBounds (sends.removeFromLeft (sends.getWidth() / 2).reduced (1, 0));
            sendStream.setBounds (sends.reduced (1, 0));
            r.removeFromBottom (4);
        }

        meter.setBounds (r.removeFromLeft (14).withTrimmedBottom (22));
        r.removeFromLeft (6);
        fader.setBounds (r);
    }

private:
    StripBinding binding;
    juce::Label name;
    juce::ComboBox device, noise;
    juce::Slider fader;
    LevelMeter meter;
    juce::TextButton mute, remove, sendMain, sendStream;
};

//==============================================================================
// Left click = play/stop, right click = assign / clear sound.
class PadButton : public juce::TextButton
{
public:
    std::function<void()> onRightClick;

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
        {
            if (onRightClick) onRightClick();
            return;
        }
        juce::TextButton::mouseDown (e);
    }
};

//==============================================================================
class MainComponent : public juce::Component, private juce::Timer
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    void timerCallback() override;
    void rebuildStrips();
    void addNewChannel();
    void setStatus (const juce::String& text, bool error);
    void padRightClicked (int index);
    void pollGlobalHotkeys();
    void refreshPadLabels();

    MixerEngine engine;

    juce::Label title, status, reverbLabel;
    juce::Slider reverbSlider;
    bool hotkeyPrev[16] = {};
    juce::TextButton addButton, refreshButton;
    PadButton padButtons[MixerEngine::numPads];
    juce::Slider padVolume;
    juce::Label padLabel;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::Viewport viewport;
    juce::Component stripContainer;
    juce::OwnedArray<ChannelStrip> strips;
    std::unique_ptr<ChannelStrip> masterStrip, streamStrip;

    static constexpr int stripWidth = 130;
    static constexpr int stripGap = 8;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
