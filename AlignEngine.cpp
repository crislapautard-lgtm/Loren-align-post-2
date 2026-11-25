#include "AlignEngine.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace lap
{
namespace
{
    constexpr double kPi = 3.14159265358979323846;

    // Algunas implementaciones de FFT escalan la inversa y otras no: lo medimos una vez.
    float fftRoundTripGain()
    {
        static const float gain = []
        {
            juce::dsp::FFT f (6);
            std::vector<float> d (128, 0.0f);
            d[3] = 1.0f;
            f.performRealOnlyForwardTransform (d.data(), true);
            f.performRealOnlyInverseTransform (d.data());
            return std::abs (d[3]) > 1.0e-9f ? d[3] : 1.0f;
        }();
        return gain;
    }

    template <typename T>
    T median (std::vector<T> v)
    {
        if (v.empty())
            return T {};
        const auto mid = v.begin() + (long) (v.size() / 2);
        std::nth_element (v.begin(), mid, v.end());
        return *mid;
    }

    inline float catmullRom (float y0, float y1, float y2, float y3, float t)
    {
        const float a = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
        const float b = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float c = -0.5f * y0 + 0.5f * y2;
        return ((a * t + b) * t + c) * t + y1;
    }

    inline float readInterpolated (const float* x, int len, double pos)
    {
        const double fl = std::floor (pos);
        const int i = (int) fl;
        const float t = (float) (pos - fl);

        auto at = [&] (int k) -> float { return (k >= 0 && k < len) ? x[k] : 0.0f; };
        return catmullRom (at (i - 1), at (i), at (i + 1), at (i + 2), t);
    }
}

//==============================================================================
std::vector<float> AlignEngine::mixToMono (const juce::AudioBuffer<float>& b)
{
    const int n = b.getNumSamples();
    const int ch = b.getNumChannels();
    std::vector<float> m ((size_t) n, 0.0f);

    if (ch == 0 || n == 0)
        return m;

    for (int c = 0; c < ch; ++c)
    {
        const float* r = b.getReadPointer (c);
        for (int i = 0; i < n; ++i)
            m[(size_t) i] += r[i];
    }

    if (ch > 1)
    {
        const float g = 1.0f / (float) ch;
        for (auto& s : m)
            s *= g;
    }
    return m;
}

//==============================================================================
std::vector<VoiceRegion> AlignEngine::detectVoices (const std::vector<float>& mono, double sr)
{
    std::vector<VoiceRegion> regions;

    const int frame = std::max (64, (int) (sr * 0.02));
    const int numFrames = (int) (mono.size() / (size_t) frame);
    if (numFrames < 10)
        return regions;

    // nivel RMS por trama (dB)
    std::vector<float> db ((size_t) numFrames);
    for (int f = 0; f < numFrames; ++f)
    {
        double acc = 0.0;
        const float* p = mono.data() + (size_t) f * (size_t) frame;
        for (int i = 0; i < frame; ++i)
            acc += (double) p[i] * (double) p[i];
        const double rms = std::sqrt (acc / frame);
        db[(size_t) f] = (float) (20.0 * std::log10 (rms + 1.0e-6));
    }

    auto percentile = [&] (float q)
    {
        std::vector<float> c = db;
        const size_t k = std::min (c.size() - 1, (size_t) (q * (float) c.size()));
        std::nth_element (c.begin(), c.begin() + (long) k, c.end());
        return c[k];
    };

    const float floorDb = percentile (0.10f);
    const float peakDb  = percentile (0.95f);

    std::vector<char> active ((size_t) numFrames, 0);

    if (peakDb - floorDb < 10.0f)
    {
        // sin contraste claro: tratamos todo como material valido
        std::fill (active.begin(), active.end(), (char) 1);
    }
    else
    {
        const float thr = std::max (floorDb + 8.0f, floorDb + 0.35f * (peakDb - floorDb));
        for (int f = 0; f < numFrames; ++f)
            active[(size_t) f] = db[(size_t) f] > thr ? 1 : 0;
    }

    // cerrar huecos cortos (< 250 ms)
    const int gapFrames = (int) (0.25 / 0.02);
    int f = 0;
    while (f < numFrames)
    {
        if (active[(size_t) f] == 0)
        {
            int e = f;
            while (e < numFrames && active[(size_t) e] == 0)
                ++e;
            if (f > 0 && e < numFrames && (e - f) < gapFrames)
                for (int k = f; k < e; ++k)
                    active[(size_t) k] = 1;
            f = e;
        }
        else
            ++f;
    }

    // regiones (descartamos las < 120 ms)
    const int minFrames = (int) (0.12 / 0.02);
    f = 0;
    while (f < numFrames)
    {
        if (active[(size_t) f] != 0)
        {
            int e = f;
            while (e < numFrames && active[(size_t) e] != 0)
                ++e;
            if (e - f >= minFrames)
                regions.push_back ({ (int64_t) f * frame, (int64_t) e * frame });
            f = e;
        }
        else
            ++f;
    }
    return regions;
}

//==============================================================================
juce::dsp::FFT& AlignEngine::getFFT (int order)
{
    auto it = ffts.find (order);
    if (it == ffts.end())
        it = ffts.emplace (order, std::make_unique<juce::dsp::FFT> (order)).first;
    return *it->second;
}

AlignEngine::LagResult AlignEngine::estimateLag (const std::vector<float>& ref, const std::vector<float>& src,
                                                 int64_t refStart, int W, int64_t guess, int M,
                                                 double sr, double loHz, double hiHz)
{
    LagResult res;

    const int need = W + 2 * M;
    int order = 4;
    while ((1 << order) < need)
        ++order;
    const int N = 1 << order;

    auto& fft = getFFT (order);

    std::vector<float> A ((size_t) 2 * (size_t) N, 0.0f), B ((size_t) 2 * (size_t) N, 0.0f);

    const int64_t refSize = (int64_t) ref.size();
    const int64_t srcSize = (int64_t) src.size();

    for (int t = 0; t < W; ++t)
    {
        const int64_t idx = refStart + t;
        if (idx >= 0 && idx < refSize)
        {
            const float w = 0.5f - 0.5f * std::cos (2.0f * (float) kPi * ((float) t + 0.5f) / (float) W);
            A[(size_t) t] = ref[(size_t) idx] * w;
        }
    }

    for (int t = 0; t < need; ++t)
    {
        const int64_t idx = refStart + guess - M + t;
        if (idx >= 0 && idx < srcSize)
            B[(size_t) t] = src[(size_t) idx];
    }

    fft.performRealOnlyForwardTransform (A.data(), true);
    fft.performRealOnlyForwardTransform (B.data(), true);

    const int half = N / 2;
    const int kLo = std::max (1, (int) std::ceil (loHz * N / sr));
    const int kHi = std::min (half - 1, (int) std::floor (hiHz * N / sr));

    for (int k = 0; k <= half; ++k)
    {
        const float ar = A[(size_t) (2 * k)], ai = A[(size_t) (2 * k + 1)];
        const float br = B[(size_t) (2 * k)], bi = B[(size_t) (2 * k + 1)];

        float re = 0.0f, im = 0.0f;

        if (k >= kLo && k <= kHi)
        {
            // conj(A) * B  ->  correlacion r(lag) = sum ref[t] * src[t + lag]
            re = ar * br + ai * bi;
            im = ar * bi - ai * br;
            const float mag = std::sqrt (re * re + im * im) + 1.0e-12f;
            re /= mag;   // PHAT: solo la fase importa
            im /= mag;
        }

        A[(size_t) (2 * k)] = re;
        A[(size_t) (2 * k + 1)] = im;
    }

    // el resto del array ya no se usa, pero lo dejamos limpio
    for (size_t i = (size_t) (2 * half + 2); i < A.size(); ++i)
        A[i] = 0.0f;

    fft.performRealOnlyInverseTransform (A.data());

    const float invGain = 1.0f / fftRoundTripGain();

    // buscar el pico en [0, 2M]
    int kp = 0;
    float best = 0.0f;
    for (int k = 0; k <= 2 * M && k < N; ++k)
    {
        const float v = std::abs (A[(size_t) k]) * invGain;
        if (v > best)
        {
            best = v;
            kp = k;
        }
    }

    double frac = 0.0;
    if (kp > 0 && kp < 2 * M && kp + 1 < N)
    {
        const double y0 = std::abs (A[(size_t) (kp - 1)]);
        const double y1 = std::abs (A[(size_t) kp]);
        const double y2 = std::abs (A[(size_t) (kp + 1)]);
        const double den = y0 - 2.0 * y1 + y2;
        if (std::abs (den) > 1.0e-12)
            frac = juce::jlimit (-0.5, 0.5, 0.5 * (y0 - y2) / den);
    }

    // confianza: pico frente al nivel medio del resto del rango
    double sum = 0.0;
    int cnt = 0;
    for (int k = 0; k <= 2 * M && k < N; ++k)
    {
        if (std::abs (k - kp) <= 3)
            continue;
        sum += (double) std::abs (A[(size_t) k]);
        ++cnt;
    }
    const double mean = cnt > 0 ? sum / cnt : 1.0e-12;

    res.lag = (double) guess + (double) (kp - M) + frac;
    res.sign = A[(size_t) kp] >= 0.0f ? 1.0f : -1.0f;
    res.confidence = (float) ((double) std::abs (A[(size_t) kp]) / (mean + 1.0e-12));
    return res;
}

//==============================================================================
Analysis AlignEngine::analyse (const juce::AudioBuffer<float>& reference,
                               const juce::AudioBuffer<float>& source,
                               double sr, double maxOffsetMs,
                               const Cancel& cancel, const Progress& progress)
{
    Analysis a;
    a.sampleRate = sr;

    const int nRef = reference.getNumSamples();
    const int nSrc = source.getNumSamples();
    if (nRef < (int) (sr * 1.0) || nSrc < (int) (sr * 0.5))
        return a;

    const auto refMono = mixToMono (reference);
    const auto srcMono = mixToMono (source);

    a.voices = detectVoices (refMono, sr);

    if (progress)
        progress (0.02f);

    // --- diezmado para la busqueda gruesa (~12 kHz)
    const int dec = std::max (1, (int) std::floor (sr / 12000.0));
    auto decimate = [dec] (const std::vector<float>& x)
    {
        std::vector<float> y (x.size() / (size_t) dec);
        const float g = 1.0f / (float) dec;
        for (size_t i = 0; i < y.size(); ++i)
        {
            float acc = 0.0f;
            for (int k = 0; k < dec; ++k)
                acc += x[i * (size_t) dec + (size_t) k];
            y[i] = acc * g;
        }
        return y;
    };

    const auto refD = decimate (refMono);
    const auto srcD = decimate (srcMono);
    const double srD = sr / dec;

    const int Wd = std::max (256, (int) (2.0 * srD));
    const int hop = Wd / 2;
    const int Md = std::max (4, (int) (maxOffsetMs * 0.001 * srD));
    const int nD = (int) refD.size();

    // mascara de voz (a la tasa diezmada) con suma acumulada
    std::vector<int> cum ((size_t) nD + 1, 0);
    {
        std::vector<char> mask ((size_t) nD, 0);
        for (const auto& r : a.voices)
        {
            const int64_t s = r.start / dec;
            const int64_t e = std::min<int64_t> (nD, r.end / dec);
            for (int64_t i = s; i < e; ++i)
                mask[(size_t) i] = 1;
        }
        for (int i = 0; i < nD; ++i)
            cum[(size_t) i + 1] = cum[(size_t) i] + (int) mask[(size_t) i];
    }

    std::vector<double> rawCentres, rawLags;
    std::vector<float> rawSigns, rawConf;

    const int totalWin = std::max (1, (nD - Wd) / hop + 1);
    int winIndex = 0;

    for (int s = 0; s + Wd <= nD; s += hop, ++winIndex)
    {
        if (cancel && cancel())
            return a;

        if (progress)
            progress (0.02f + 0.88f * (float) winIndex / (float) totalWin);

        const float voiced = (float) (cum[(size_t) (s + Wd)] - cum[(size_t) s]) / (float) Wd;
        if (voiced < 0.35f)
            continue;

        // 1) busqueda gruesa
        const auto coarse = estimateLag (refD, srcD, s, Wd, 0, Md, srD, 100.0, std::min (4500.0, 0.45 * srD));
        if (coarse.confidence < 3.5f)
            continue;

        // 2) refinamiento a la tasa completa
        const int64_t guess = (int64_t) std::llround (coarse.lag * dec);
        const int M2 = 2 * dec + 8;
        const auto fine = estimateLag (refMono, srcMono, (int64_t) s * dec, Wd * dec, guess, M2,
                                       sr, 100.0, std::min (10000.0, 0.45 * sr));

        double lag = coarse.lag * dec;
        float sign = coarse.sign;
        float conf = coarse.confidence;
        if (fine.confidence >= 3.0f)
        {
            lag = fine.lag;
            sign = fine.sign;
            conf = std::max (conf, fine.confidence);
        }

        rawCentres.push_back ((double) (s + Wd / 2) * dec);
        rawLags.push_back (lag);
        rawSigns.push_back (sign);
        rawConf.push_back (conf);
    }

    if (rawLags.empty())
        return a;

    a.windowsMeasured = (int) rawLags.size();
    a.globalLag = median (rawLags);

    double polSum = 0.0, confSum = 0.0;
    for (size_t i = 0; i < rawLags.size(); ++i)
    {
        polSum += (double) rawSigns[i] * (double) rawConf[i];
        confSum += (double) rawConf[i];
    }
    a.polarity = polSum >= 0.0 ? 1 : -1;
    a.confidence = (float) (confSum / (double) rawLags.size());

    // --- curva dinamica: mediana de 7 + suavizado de 3
    const int n = (int) rawLags.size();
    std::vector<double> med ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        std::vector<double> w;
        for (int k = std::max (0, i - 3); k <= std::min (n - 1, i + 3); ++k)
            w.push_back (rawLags[(size_t) k]);
        med[(size_t) i] = median (w);
    }

    const double outlier = 0.005 * sr; // 5 ms
    std::vector<double> clean ((size_t) n);
    for (int i = 0; i < n; ++i)
        clean[(size_t) i] = std::abs (rawLags[(size_t) i] - med[(size_t) i]) > outlier ? med[(size_t) i]
                                                                                       : rawLags[(size_t) i];

    std::vector<double> smooth ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        const double p = clean[(size_t) std::max (0, i - 1)];
        const double c = clean[(size_t) i];
        const double q = clean[(size_t) std::min (n - 1, i + 1)];
        smooth[(size_t) i] = 0.25 * p + 0.5 * c + 0.25 * q;
    }

    a.centres = rawCentres;
    a.lags = smooth;
    a.valid = true;

    if (progress)
        progress (1.0f);
    return a;
}

//==============================================================================
juce::AudioBuffer<float> AlignEngine::render (const juce::AudioBuffer<float>& reference,
                                              const juce::AudioBuffer<float>& source,
                                              const Analysis& a,
                                              TimeMode timeMode, PhaseMode phaseMode,
                                              const Cancel& cancel, const Progress& progress)
{
    const int nOut = reference.getNumSamples();
    const int nCh = std::max (1, source.getNumChannels());
    const int nSrc = source.getNumSamples();

    juce::AudioBuffer<float> out (nCh, nOut);
    out.clear();

    if (! a.valid || nSrc == 0)
    {
        const int n = std::min (nOut, nSrc);
        for (int c = 0; c < source.getNumChannels(); ++c)
            out.copyFrom (c, 0, source, c, 0, n);
        return out;
    }

    const float pol = (phaseMode == PhaseMode::Off) ? 1.0f : (float) a.polarity;
    const bool dynamic = (timeMode == TimeMode::Dynamic) && a.centres.size() > 1;

    for (int c = 0; c < source.getNumChannels(); ++c)
    {
        const float* in = source.getReadPointer (c);
        float* dst = out.getWritePointer (c);
        size_t j = 0;

        for (int i = 0; i < nOut; ++i)
        {
            if ((i & 0xFFFF) == 0)
            {
                if (cancel && cancel())
                    return out;
                if (progress)
                    progress (0.5f * ((float) c + (float) i / (float) nOut) / (float) source.getNumChannels());
            }

            double lag = a.globalLag;
            if (dynamic)
            {
                const double pos = (double) i;
                if (pos <= a.centres.front())
                    lag = a.lags.front();
                else if (pos >= a.centres.back())
                    lag = a.lags.back();
                else
                {
                    while (j + 1 < a.centres.size() && a.centres[j + 1] <= pos)
                        ++j;
                    const double t0 = a.centres[j], t1 = a.centres[j + 1];
                    const double u = (pos - t0) / std::max (1.0, t1 - t0);
                    lag = a.lags[j] + u * (a.lags[j + 1] - a.lags[j]);
                }
            }

            // aligned[n] = source[n + lag]
            dst[i] = pol * readInterpolated (in, nSrc, (double) i + lag);
        }
    }

    if (phaseMode == PhaseMode::Spectral)
    {
        const auto refMono = mixToMono (reference);
        spectralPhasePass (refMono, out, a.sampleRate, cancel, progress);
    }

    if (progress)
        progress (1.0f);
    return out;
}

//==============================================================================
// Ajuste de fase por bandas: rota la fase de la fuente hacia la de la referencia,
// solo donde ambas son coherentes (evita que se "cuele" la referencia entera).
void AlignEngine::spectralPhasePass (const std::vector<float>& refMono,
                                     juce::AudioBuffer<float>& aligned, double sr,
                                     const Cancel& cancel, const Progress& progress)
{
    constexpr int order = 11;
    constexpr int N = 1 << order;
    constexpr int hop = N / 4;
    constexpr int nb = N / 2 + 1;

    const int len = aligned.getNumSamples();
    const int nCh = aligned.getNumChannels();
    if (len < N || nCh == 0)
        return;

    auto& fft = getFFT (order);
    const float invGain = 1.0f / fftRoundTripGain();

    std::vector<float> window ((size_t) N);
    for (int i = 0; i < N; ++i)
        window[(size_t) i] = std::sqrt (0.5f - 0.5f * std::cos (2.0f * (float) kPi * (float) i / (float) N));

    const auto alMono = mixToMono (aligned);

    juce::AudioBuffer<float> result (nCh, len);
    result.clear();

    std::vector<std::complex<float>> P ((size_t) nb, { 0.0f, 0.0f });
    std::vector<float> Prr ((size_t) nb, 0.0f), Pss ((size_t) nb, 0.0f);
    std::vector<std::complex<float>> Ps ((size_t) nb);
    std::vector<float> Prrs ((size_t) nb), Psss ((size_t) nb);
    std::vector<std::complex<float>> rot ((size_t) nb);

    std::vector<float> fr ((size_t) 2 * N), fs ((size_t) 2 * N), fc ((size_t) 2 * N);

    const int kLo = (int) std::ceil (60.0 * N / sr);
    const int kHi = std::min (nb - 2, (int) std::floor (10000.0 * N / sr));
    const float alpha = 0.8f;

    const int64_t firstStart = -(int64_t) N + hop;
    int frameCount = 0;
    const int totalFrames = (int) ((len - firstStart) / hop) + 1;

    for (int64_t start = firstStart; start < len; start += hop, ++frameCount)
    {
        if (cancel && cancel())
            return;

        if ((frameCount & 63) == 0 && progress)
            progress (0.5f + 0.5f * (float) frameCount / (float) totalFrames);

        std::fill (fr.begin(), fr.end(), 0.0f);
        std::fill (fs.begin(), fs.end(), 0.0f);

        for (int i = 0; i < N; ++i)
        {
            const int64_t idx = start + i;
            if (idx >= 0 && idx < len)
            {
                const float w = window[(size_t) i];
                if ((size_t) idx < refMono.size())
                    fr[(size_t) i] = refMono[(size_t) idx] * w;
                fs[(size_t) i] = alMono[(size_t) idx] * w;
            }
        }

        fft.performRealOnlyForwardTransform (fr.data(), true);
        fft.performRealOnlyForwardTransform (fs.data(), true);

        for (int k = 0; k < nb; ++k)
        {
            const std::complex<float> R (fr[(size_t) (2 * k)], fr[(size_t) (2 * k + 1)]);
            const std::complex<float> S (fs[(size_t) (2 * k)], fs[(size_t) (2 * k + 1)]);
            const auto X = R * std::conj (S);
            P[(size_t) k] = alpha * P[(size_t) k] + (1.0f - alpha) * X;
            Prr[(size_t) k] = alpha * Prr[(size_t) k] + (1.0f - alpha) * std::norm (R);
            Pss[(size_t) k] = alpha * Pss[(size_t) k] + (1.0f - alpha) * std::norm (S);
        }

        for (int k = 0; k < nb; ++k)
        {
            const int k0 = std::max (0, k - 1), k1 = std::min (nb - 1, k + 1);
            Ps[(size_t) k] = 0.25f * P[(size_t) k0] + 0.5f * P[(size_t) k] + 0.25f * P[(size_t) k1];
            Prrs[(size_t) k] = 0.25f * Prr[(size_t) k0] + 0.5f * Prr[(size_t) k] + 0.25f * Prr[(size_t) k1];
            Psss[(size_t) k] = 0.25f * Pss[(size_t) k0] + 0.5f * Pss[(size_t) k] + 0.25f * Pss[(size_t) k1];

            if (k >= kLo && k <= kHi)
            {
                const float coh = juce::jlimit (0.0f, 1.0f,
                                                std::abs (Ps[(size_t) k]) / (std::sqrt (Prrs[(size_t) k] * Psss[(size_t) k]) + 1.0e-12f));
                const float theta = juce::jlimit (-1.0f, 1.0f, std::arg (Ps[(size_t) k]) * coh * coh);
                rot[(size_t) k] = std::polar (1.0f, theta);
            }
            else
                rot[(size_t) k] = { 1.0f, 0.0f };
        }

        for (int c = 0; c < nCh; ++c)
        {
            const float* x = aligned.getReadPointer (c);
            std::fill (fc.begin(), fc.end(), 0.0f);

            for (int i = 0; i < N; ++i)
            {
                const int64_t idx = start + i;
                if (idx >= 0 && idx < len)
                    fc[(size_t) i] = x[idx] * window[(size_t) i];
            }

            fft.performRealOnlyForwardTransform (fc.data(), true);

            for (int k = 0; k < nb; ++k)
            {
                const std::complex<float> v (fc[(size_t) (2 * k)], fc[(size_t) (2 * k + 1)]);
                const auto y = v * rot[(size_t) k];
                fc[(size_t) (2 * k)] = y.real();
                fc[(size_t) (2 * k + 1)] = (k == 0 || k == nb - 1) ? 0.0f : y.imag();
            }

            fft.performRealOnlyInverseTransform (fc.data());

            float* o = result.getWritePointer (c);
            for (int i = 0; i < N; ++i)
            {
                const int64_t idx = start + i;
                if (idx >= 0 && idx < len)
                    o[idx] += fc[(size_t) i] * invGain * window[(size_t) i] * 0.5f; // sum(hann) a 75% = 2
            }
        }
    }

    for (int c = 0; c < nCh; ++c)
        aligned.copyFrom (c, 0, result, c, 0, len);
}
} // namespace lap
