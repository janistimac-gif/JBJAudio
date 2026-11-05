#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include "MainComponent.h"

namespace
{
    const juce::String runKey = "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run\\JBJAudio";

    bool autostartSupported()
    {
       #if JUCE_WINDOWS
        return true;
       #else
        return false;
       #endif
    }

    bool isAutostartEnabled()
    {
       #if JUCE_WINDOWS
        return juce::WindowsRegistry::valueExists (runKey);
       #else
        return false;
       #endif
    }

    void setAutostart (bool enable)
    {
       #if JUCE_WINDOWS
        if (enable)
            juce::WindowsRegistry::setValue (runKey, "\"" + juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName() + "\" --tray");
        else
            juce::WindowsRegistry::deleteValue (runKey);
       #else
        juce::ignoreUnused (enable);
       #endif
    }
}

//==============================================================================
class TrayIcon : public juce::SystemTrayIconComponent
{
public:
    TrayIcon (std::function<void()> showWindow, std::function<void()> quitApp)
        : onShow (std::move (showWindow)), onQuit (std::move (quitApp))
    {
        juce::Image img (juce::Image::ARGB, 64, 64, true);
        {
            juce::Graphics g (img);
            g.setColour (juce::Colour (0xff19c3a6));
            g.fillEllipse (2.0f, 2.0f, 60.0f, 60.0f);
            g.setColour (juce::Colours::white);
            g.setFont (juce::FontOptions (38.0f, juce::Font::bold));
            g.drawText ("J", img.getBounds(), juce::Justification::centred);
        }
        setIconImage (img, img);
        setIconTooltip ("JBJAudio");
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! e.mods.isPopupMenu())
        {
            onShow();
            return;
        }

        juce::PopupMenu m;
        m.addItem (1, "Show JBJAudio");
        if (autostartSupported())
            m.addItem (2, "Start with Windows", true, isAutostartEnabled());
        m.addSeparator();
        m.addItem (3, "Quit");

        m.showMenuAsync (juce::PopupMenu::Options(), [this] (int r)
        {
            if (r == 1) onShow();
            else if (r == 2) setAutostart (! isAutostartEnabled());
            else if (r == 3) onQuit();
        });
    }

private:
    std::function<void()> onShow, onQuit;
};

//==============================================================================
class JBJAudioApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return "JBJAudio"; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override          { return false; }

    void initialise (const juce::String& commandLine) override
    {
        mainWindow = std::make_unique<MainWindow> (getApplicationName(), [this] { showWindow(); });
        tray = std::make_unique<TrayIcon> ([this] { showWindow(); }, [this] { quit(); });

        if (commandLine.contains ("--tray"))
            mainWindow->setVisible (false);   // started with Windows: stay in the tray
    }

    void shutdown() override
    {
        tray.reset();
        mainWindow.reset();
    }

    void systemRequestedQuit() override { quit(); }

    void anotherInstanceStarted (const juce::String&) override { showWindow(); }

    void showWindow()
    {
        if (mainWindow != nullptr)
        {
            mainWindow->setVisible (true);
            mainWindow->setMinimised (false);
            mainWindow->toFront (true);
        }
    }

    class MainWindow : public juce::DocumentWindow
    {
    public:
        MainWindow (juce::String name, std::function<void()>)
            : DocumentWindow (name, juce::Colour (0xff12161b), DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent(), true);
            setResizable (true, true);
            setResizeLimits (760, 420, 4000, 1200);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        // Close = hide to the tray (the mixer keeps running). Quit from the tray menu.
        void closeButtonPressed() override { setVisible (false); }

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

private:
    std::unique_ptr<MainWindow> mainWindow;
    std::unique_ptr<TrayIcon> tray;
};

START_JUCE_APPLICATION (JBJAudioApplication)
