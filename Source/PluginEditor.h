#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <utility>
#include <vector>

#include "PluginProcessor.h"

class LorenLookAndFeel : public juce::LookAndFeel_V4
{
public:
    LorenLookAndFeel();
};

class LorenAlignEditor : public juce::AudioProcessorEditor,
                         public juce::FileDragAndDropTarget,
                         private juce::ChangeListener,
                         private juce::Timer
{
public:
    explicit LorenAlignEditor (LorenAlignProcessor&);
    ~LorenAlignEditor() override;

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray& files, int x, int y) override;
    void fileDragMove (const juce::StringArray& files, int x, int y) override;
    void fileDragExit (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;

    void updateSnapshot();
    void rebuildOverview();
    void clampView();
    void resetView();
    void zoomBy (double factor, double anchorSample);
    void setViewCentre (double sample);
    void exportClicked();

    double totalSamples() const;
    double sampleRate() const;
    double sampleAtX (float x) const;
    float xAtSample (double s) const;
    juce::String buildInfoText (juce::Colour& colour) const;

    LorenAlignProcessor& proc;
    LorenLookAndFeel laf;

    // controles
    juce::ComboBox timeBox, phaseBox, rangeBox;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> timeAtt, phaseAtt, rangeAtt;

    juce::TextButton scanButton { "SCAN" }, viewButton { "VIEW: ALIGNED" }, zoomOutButton { "-" }, zoomInButton { "+" },
        playButton { "PLAY" }, exportButton { "EXPORT WAV" }, clearButton { "CLEAR" };
    juce::ToggleButton syncButton { "SYNC DAW" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> syncAtt;

    std::unique_ptr<juce::FileChooser> chooser;

    // datos
    LorenAlignProcessor::Snapshot snap;
    const void* lastRef = nullptr;
    const void* lastSrc = nullptr;
    float peak = 1.0f;
    std::vector<std::pair<float, float>> ovRef, ovSrc;

    // vista (en muestras)
    double viewStart = 0.0;
    double viewLen = 1.0;

    // zonas
    juce::Rectangle<int> topBar, overviewRect, scrollRect, waveRect, waveArea, bottomBar, dropArea;

    // interaccion
    bool draggingOverview = false, draggingWave = false, waveMoved = false;
    double dragStartViewStart = 0.0;
    int dragLane = 0; // 0 = nada, 1 = referencia, 2 = fuente

    juce::String infoOverride;
    juce::Colour infoOverrideColour { juce::Colours::white };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LorenAlignEditor)
};
