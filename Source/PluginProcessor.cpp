#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    juce::String msText (float v)
    {
        return juce::String (v, v < 10.0f ? 2 : (v < 100.0f ? 1 : 0)) + " ms";
    }

    float numberFromText (const juce::String& t)
    {
        return t.retainCharacters ("-0123456789.").getFloatValue();
    }
}

GateProcessor::GateProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", createLayout())
{
    pType    = apvts.getRawParameterValue (IDs::type);
    pFloor   = apvts.getRawParameterValue (IDs::floorDb);
    pOpen    = apvts.getRawParameterValue (IDs::thresholdOpen);
    pClose   = apvts.getRawParameterValue (IDs::thresholdClose);
    pAttack  = apvts.getRawParameterValue (IDs::attack);
    pRelease = apvts.getRawParameterValue (IDs::release);
    pHold    = apvts.getRawParameterValue (IDs::hold);
    pRatio   = apvts.getRawParameterValue (IDs::ratio);
    pOutput  = apvts.getRawParameterValue (IDs::output);
    pBypass  = apvts.getRawParameterValue (IDs::bypass);
}

juce::AudioProcessorValueTreeState::ParameterLayout GateProcessor::createLayout()
{
    using Attr = juce::AudioParameterFloatAttributes;
    using Range = juce::NormalisableRange<float>;

    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    const auto dbAttr = Attr()
        .withStringFromValueFunction ([] (float v, int) { return juce::String (v, 1) + " dB"; })
        .withValueFromStringFunction (numberFromText);

    const auto floorAttr = Attr()
        .withStringFromValueFunction ([] (float v, int) { return v <= -79.9f ? juce::String ("-Inf") : juce::String (v, 1) + " dB"; })
        .withValueFromStringFunction ([] (const juce::String& t) { return t.containsIgnoreCase ("inf") ? -80.0f : numberFromText (t); });

    const auto msAttr = Attr()
        .withStringFromValueFunction ([] (float v, int) { return msText (v); })
        .withValueFromStringFunction (numberFromText);

    const auto ratioAttr = Attr()
        .withStringFromValueFunction ([] (float v, int) { return juce::String (v, 2) + ":1"; })
        .withValueFromStringFunction (numberFromText);

    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { IDs::type, 1 }, "Type", juce::StringArray { "Gate", "Expander" }, 0));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { IDs::floorDb, 1 }, "Floor", Range (-80.0f, 0.0f, 0.1f), -40.0f, floorAttr));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { IDs::thresholdOpen, 1 }, "Gate Open", Range (-90.0f, 0.0f, 0.1f), -52.0f, dbAttr));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { IDs::thresholdClose, 1 }, "Gate Close", Range (-90.0f, 0.0f, 0.1f), -60.0f, dbAttr));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { IDs::attack, 1 }, "Attack", Range (0.05f, 500.0f, 0.01f, 0.35f), 2.0f, msAttr));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { IDs::release, 1 }, "Release", Range (1.0f, 4000.0f, 0.1f, 0.35f), 150.0f, msAttr));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { IDs::hold, 1 }, "Hold", Range (0.0f, 5000.0f, 0.01f, 0.35f), 100.0f, msAttr));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { IDs::ratio, 1 }, "Ratio", Range (1.0f, 10.0f, 0.01f, 0.6f), 2.0f, ratioAttr));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { IDs::output, 1 }, "Output", Range (-36.0f, 18.0f, 0.1f), 0.0f, dbAttr));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { IDs::bypass, 1 }, "Bypass", false));

    return layout;
}

void GateProcessor::prepareToPlay (double sampleRate, int)
{
    dsp.prepare (sampleRate);

    // Lookahead de 5 ms: la puerta abre antes de que llegue el inicio de la palabra.
    // El plugin avisa su latencia al DAW, que la compensa solo.
    const int lookahead = (int) std::lround (0.005 * sampleRate);
    delays[0].prepare (lookahead);
    delays[1].prepare (lookahead);
    setLatencySamples (lookahead);
}

bool GateProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& in  = layouts.getMainInputChannelSet();
    const auto& out = layouts.getMainOutputChannelSet();

    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;

    return in == out;
}

void GateProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numCh      = juce::jmin (buffer.getNumChannels(), 2);
    const int numSamples = buffer.getNumSamples();

    if (numCh == 0 || numSamples == 0)
        return;

    float* data[2] = { buffer.getWritePointer (0),
                       numCh > 1 ? buffer.getWritePointer (1) : nullptr };

    // Bypass: el audio pasa intacto
    if (pBypass->load() > 0.5f)
    {
        const float pk = buffer.getMagnitude (0, numSamples);
        inPeak.store  (juce::jmax (inPeak.load(),  pk));
        outPeak.store (juce::jmax (outPeak.load(), pk));
        gateIsOpen.store (true);

        // Se retarda igual que en uso normal para que el tiempo no cambie al activar Bypass
        for (int ch = 0; ch < numCh; ++ch)
            for (int i = 0; i < numSamples; ++i)
                data[ch][i] = delays[ch].process (data[ch][i]);

        return;
    }

    GateParams p;
    p.expander  = pType->load() > 0.5f;
    p.floorDb   = pFloor->load();
    p.openDb    = pOpen->load();
    p.closeDb   = pClose->load();
    p.attackMs  = pAttack->load();
    p.releaseMs = pRelease->load();
    p.holdMs    = pHold->load();
    p.ratio     = pRatio->load();
    dsp.setParams (p);

    const float outGain = juce::Decibels::decibelsToGain (pOutput->load());

    float blockIn = 0.0f, blockOut = 0.0f, blockMinGain = 1.0f;

    for (int i = 0; i < numSamples; ++i)
    {
        float peak = std::abs (data[0][i]);
        if (data[1] != nullptr)
            peak = juce::jmax (peak, std::abs (data[1][i]));

        const float gate = dsp.process (peak);   // la puerta mira la señal sin retardo
        const float g = gate * outGain;

        // ...y la ganancia se aplica a la señal retardada (lookahead)
        const float y0 = delays[0].process (data[0][i]) * g;
        data[0][i] = y0;
        float outPk = std::abs (y0);

        if (data[1] != nullptr)
        {
            const float y1 = delays[1].process (data[1][i]) * g;
            data[1][i] = y1;
            outPk = juce::jmax (outPk, std::abs (y1));
        }

        blockIn      = juce::jmax (blockIn, peak);
        blockOut     = juce::jmax (blockOut, outPk);
        blockMinGain = juce::jmin (blockMinGain, gate);
    }

    inPeak.store  (juce::jmax (inPeak.load(),  blockIn));
    outPeak.store (juce::jmax (outPeak.load(), blockOut));
    minGain.store (juce::jmin (minGain.load(), blockMinGain));
    gateIsOpen.store (dsp.isOpen());
}

juce::AudioProcessorEditor* GateProcessor::createEditor()
{
    return new GateEditor (*this);
}

void GateProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    apvts.state.setProperty ("uiWidth", uiWidth, nullptr);

    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void GateProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
    {
        if (xml->hasTagName (apvts.state.getType()))
        {
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
            uiWidth = juce::jlimit (570, 1520, (int) apvts.state.getProperty ("uiWidth", 760));
        }
    }
}

// Punto de entrada del plugin
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new GateProcessor();
}
