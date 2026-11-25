#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace lap
{
struct VoiceRegion
{
    int64_t start = 0; // muestras (de la referencia)
    int64_t end = 0;
};

enum class TimeMode  { Dynamic = 0, Static = 1 };
enum class PhaseMode { Spectral = 0, Polarity = 1, Off = 2 };

// Resultado del escaneo
struct Analysis
{
    bool valid = false;
    double sampleRate = 48000.0;

    std::vector<VoiceRegion> voices;   // regiones con voz detectadas en la referencia
    std::vector<double> centres;       // centro de cada ventana medida (muestras)
    std::vector<double> lags;          // retraso medido por ventana (muestras); >0 = la fuente llega tarde
    double globalLag = 0.0;            // retraso global (mediana)
    int polarity = 1;                  // +1 = en fase, -1 = polaridad invertida
    float confidence = 0.0f;           // relacion pico/ruido de la correlacion (media)
    int windowsMeasured = 0;
};

class AlignEngine
{
public:
    using Cancel   = std::function<bool()>;
    using Progress = std::function<void (float)>;

    static std::vector<float> mixToMono (const juce::AudioBuffer<float>& b);

    // Escanea el audio y detecta las regiones con voz
    static std::vector<VoiceRegion> detectVoices (const std::vector<float>& mono, double sampleRate);

    // Mide el retraso entre referencia y fuente (GCC-PHAT, grueso + fino)
    Analysis analyse (const juce::AudioBuffer<float>& reference,
                      const juce::AudioBuffer<float>& source,
                      double sampleRate,
                      double maxOffsetMs,
                      const Cancel& cancel,
                      const Progress& progress);

    // Genera la fuente alineada (misma linea de tiempo que la referencia)
    juce::AudioBuffer<float> render (const juce::AudioBuffer<float>& reference,
                                     const juce::AudioBuffer<float>& source,
                                     const Analysis& analysis,
                                     TimeMode timeMode,
                                     PhaseMode phaseMode,
                                     const Cancel& cancel,
                                     const Progress& progress);

private:
    struct LagResult
    {
        double lag = 0.0;
        float sign = 1.0f;
        float confidence = 0.0f;
    };

    LagResult estimateLag (const std::vector<float>& ref, const std::vector<float>& src,
                           int64_t refStart, int windowLen, int64_t guess, int searchRange,
                           double sampleRate, double loHz, double hiHz);

    void spectralPhasePass (const std::vector<float>& refMono,
                            juce::AudioBuffer<float>& aligned, double sampleRate,
                            const Cancel& cancel, const Progress& progress);

    juce::dsp::FFT& getFFT (int order);

    std::map<int, std::unique_ptr<juce::dsp::FFT>> ffts;
};
} // namespace lap
