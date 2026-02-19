#include <juce_gui_basics/juce_gui_basics.h>
#include "MainComponent.h"
#include <cstdio>

class SamplerApplication : public juce::JUCEApplication
{
public:
    SamplerApplication() 
    {
        printf("DEBUG: SamplerApplication constructor\n");
        fflush(stdout);
    }

    const juce::String getApplicationName() override { return "My Sampler"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }

    void initialise(const juce::String&) override
    {
        printf("DEBUG: initialise() started\n");
        fflush(stdout);
        mainWindow.reset(new MainWindow(getApplicationName()));
        printf("DEBUG: initialise() completed\n");
        fflush(stdout);
    }

    void shutdown() override
    {
        printf("DEBUG: shutdown()\n");
        fflush(stdout);
        mainWindow = nullptr;
    }

private:
    class MainWindow : public juce::DocumentWindow
    {
    public:
        MainWindow(juce::String name)
            : DocumentWindow(name,
                           juce::Desktop::getInstance().getDefaultLookAndFeel()
                               .findColour(juce::ResizableWindow::backgroundColourId),
                           juce::DocumentWindow::allButtons)
        {
            printf("DEBUG: MainWindow constructor started\n");
            fflush(stdout);
            
            setUsingNativeTitleBar(true);
            printf("DEBUG: setUsingNativeTitleBar completed\n");
            fflush(stdout);
            
            printf("DEBUG: Creating MainComponent...\n");
            fflush(stdout);
            auto* mainComp = new MainComponent();
            printf("DEBUG: MainComponent created, setting content...\n");
            fflush(stdout);
            
            setContentOwned(mainComp, true);
            printf("DEBUG: setContentOwned completed\n");
            fflush(stdout);
            
            setResizable(true, true);
            centreWithSize(getWidth(), getHeight());
            printf("DEBUG: centreWithSize completed\n");
            fflush(stdout);
            
            setVisible(true);
            printf("DEBUG: MainWindow constructor completed\n");
            fflush(stdout);
        }

        void closeButtonPressed() override
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainWindow)
    };

    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION(SamplerApplication)