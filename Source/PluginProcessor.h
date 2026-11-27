#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include "GateDSP.h"

// Identificadores de los parámetros (los mismos del C1 Gate de Waves + Ratio para el modo Expander)
namespace IDs
{
    inline constexpr const char* type           = "type";
    inline constexpr const char* floorDb        = "floor";
    inline constexpr const char* thresholdOpen  = "gateOpen";
    inline constexpr const char* thresholdClose = "gateClose";
    inline constexpr const char* attack         = "attack";
    inline constexpr const char* release        = "release";
    inline constexpr const char* hold           = "hold";
    inline constexpr const char* ratio          = "ratio";
    inline constexpr const char* output         = "output";
    inline constexpr const char* bypass         = "bypass";
}

class GateProcessor : public juce::AudioProcessor
{
public:
    GateProcessor();
    ~GateProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "La Pauta Gate C1"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorParameter* getBypassParameter() const override
    {
        return apvts.getParameter (IDs::bypass);
    }

    juce::AudioProcessorValueTreeState apvts;

    // Medidores: los escribe el audio y los lee la interfaz (valores lineales)
    std::atomic<float> inPeak  { 0.0f };
    std::atomic<float> outPeak { 0.0f };
    std::atomic<float> minGain { 1.0f };
    std::atomic<bool>  gateIsOpen { false };

    // Ancho guardado de la ventana (el alto sale de la relación de aspecto)
    int uiWidth = 760;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    std::atomic<float>* pType    = nullptr;
    std::atomic<float>* pFloor   = nullptr;
    std::atomic<float>* pOpen    = nullptr;
    std::atomic<float>* pClose   = nullptr;
    std::atomic<float>* pAttack  = nullptr;
    std::atomic<float>* pRelease = nullptr;
    std::atomic<float>* pHold    = nullptr;
    std::atomic<float>* pRatio   = nullptr;
    std::atomic<float>* pOutput  = nullptr;
    std::atomic<float>* pBypass  = nullptr;

    GateDSP dsp;
    LookaheadDelay delays[2];   // retardo de 5 ms (lookahead) por canal

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GateProcessor)
};
