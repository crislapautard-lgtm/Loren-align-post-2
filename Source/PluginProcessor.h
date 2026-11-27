#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>

#include <atomic>
#include <memory>

#include "AlignEngine.h"

class LorenAlignProcessor : public juce::AudioProcessor,
                            public juce::ChangeBroadcaster,
                            private juce::AsyncUpdater,
                            private juce::AudioProcessorValueTreeState::Listener
{
public:
    enum class Status { Idle = 0, Analysing, Rendering, Ready, Error };

    LorenAlignProcessor();
    ~LorenAlignProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Loren Align Post 2"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==============================================================================
    // Archivos
    juce::String handleDrop (const juce::Array<juce::File>& files, bool toReference);
    juce::String setReferenceFile (const juce::File& f);
    juce::String setSourceFile (const juce::File& f);
    void clearAll();
    void rescan();                       // vuelve a medir
    juce::String exportAligned (const juce::File& destination);

    // Datos para la interfaz (se llama desde el hilo de mensajes)
    struct Snapshot
    {
        std::shared_ptr<juce::AudioBuffer<float>> reference, source, aligned;
        lap::Analysis analysis;
        juce::String referenceName, sourceName;
    };
    Snapshot getSnapshot() const;

    Status getStatus() const noexcept { return (Status) status.load(); }
    float getProgress() const noexcept { return progress.load(); }
    juce::String getErrorText() const;
    double getWorkingSampleRate() const noexcept { return workingSampleRate.load(); }

    // Reproduccion de prueba
    void setPlaying (bool shouldPlay);
    bool isPlaying() const noexcept { return playing.load(); }
    int64_t getPlayPosition() const noexcept { return playPosition.load(); }
    void setPlayPosition (int64_t p) noexcept { playPosition.store (juce::jmax ((int64_t) 0, p)); }

    juce::AudioProcessorValueTreeState apvts;
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

private:
    struct Worker;

    void parameterChanged (const juce::String& id, float newValue) override;
    void handleAsyncUpdate() override;

    juce::String loadFileInto (const juce::File& f, juce::AudioBuffer<float>& dest, bool mono);
    juce::String loadRef (const juce::File& f);
    juce::String loadSrc (const juce::File& f);
    juce::String loadStereo (const juce::File& f);
    void leaveStereoMode (bool clearReference);
    void afterLoad();
    void reloadAll();
    void startJob (bool reanalyse);
    void runJob (Worker& w, bool reanalyse);
    void setError (const juce::String& message);

    juce::AudioFormatManager formatManager;

    mutable juce::SpinLock bufLock;
    std::shared_ptr<juce::AudioBuffer<float>> refBuf, srcBuf, alignedBuf;
    lap::Analysis analysis;
    juce::File refFile, srcFile;
    juce::File stereoFile;
    bool stereoMode = false;

    std::unique_ptr<Worker> worker;

    std::atomic<int> status { (int) Status::Idle };
    std::atomic<float> progress { 0.0f };
    std::atomic<double> workingSampleRate { 48000.0 };
    mutable juce::CriticalSection textLock;
    juce::String errorText;

    std::atomic<bool> playing { false };
    std::atomic<int64_t> playPosition { 0 };

    std::atomic<bool> needReanalyse { false };
    std::atomic<bool> needRerender { false };
    std::atomic<bool> needReload { false };

    std::atomic<float>* timeParam = nullptr;
    std::atomic<float>* phaseParam = nullptr;
    std::atomic<float>* rangeParam = nullptr;
    std::atomic<float>* viewParam = nullptr;
    std::atomic<float>* syncParam = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LorenAlignProcessor)
};
