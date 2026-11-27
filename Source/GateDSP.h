#pragma once

// Motor de la puerta de ruido / expansor "La Pauta Gate C1".
// C++ puro (sin JUCE) para poder probarlo por separado.

#include <algorithm>
#include <cmath>
#include <vector>

// Retardo fijo de N muestras. La puerta "escucha" la señal sin retardo y aplica
// la ganancia sobre la señal retardada, así abre ANTES de que llegue el inicio
// de la palabra y no se lo come.
class LookaheadDelay
{
public:
    void prepare (int delaySamples)
    {
        d = std::max (0, delaySamples);
        buf.assign ((size_t) std::max (1, d), 0.0f);
        pos = 0;
    }

    float process (float x) noexcept
    {
        if (d == 0)
            return x;

        const float y = buf[(size_t) pos];
        buf[(size_t) pos] = x;
        if (++pos >= d)
            pos = 0;
        return y;
    }

private:
    std::vector<float> buf;
    int d = 0;
    int pos = 0;
};

struct GateParams
{
    bool  expander  = false;     // false = Gate, true = Expander
    float floorDb   = -80.0f;    // <= -79.9 se interpreta como -Inf
    float openDb    = -50.0f;    // umbral de apertura (Gate Open)
    float closeDb   = -54.0f;    // umbral de cierre (Gate Close)
    float attackMs  = 2.0f;
    float releaseMs = 80.0f;
    float holdMs    = 20.0f;
    float ratio     = 2.0f;      // solo en modo Expander
};

class GateDSP
{
public:
    void prepare (double sampleRate)
    {
        fs = sampleRate;
        detectorCoef = coefFor (20.0f);   // el detector de picos cae en ~20 ms
        reset();
    }

    void reset() noexcept
    {
        env = 0.0f;
        gain = 1.0f;
        open = false;
        holdCounter = 0;
    }

    void setParams (const GateParams& newParams)
    {
        p = newParams;
        p.closeDb = std::min (p.closeDb, p.openDb);   // Close nunca por encima de Open
        p.ratio   = std::max (p.ratio, 1.0f);

        attackCoef  = coefFor (p.attackMs);
        releaseCoef = coefFor (p.releaseMs);
        holdSamples = (int) std::lround (p.holdMs * 0.001 * fs);

        const bool floorInf = p.floorDb <= -79.9f;
        floorLin  = floorInf ? 0.0f : std::pow (10.0f, p.floorDb / 20.0f);
        maxRedDb  = floorInf ? 120.0f : -p.floorDb;
    }

    // 'peak' = pico absoluto de la muestra (canales enlazados).
    // Devuelve la ganancia lineal que hay que aplicar.
    float process (float peak) noexcept
    {
        env = peak > env ? peak : peak + detectorCoef * (env - peak);
        const float lvlDb = env > 1.0e-6f ? 20.0f * std::log10 (env) : -120.0f;

        if (open)
        {
            if (lvlDb < p.closeDb)
            {
                if (holdCounter > 0)
                    --holdCounter;
                else
                    open = false;
            }
            else
            {
                holdCounter = holdSamples;
            }
        }
        else if (lvlDb >= p.openDb)
        {
            open = true;
            holdCounter = holdSamples;
        }

        float target = 1.0f;

        if (! open)
        {
            if (! p.expander)
            {
                target = floorLin;
            }
            else
            {
                float red = std::max (0.0f, p.closeDb - lvlDb) * (p.ratio - 1.0f);
                red = std::min (red, maxRedDb);
                target = std::pow (10.0f, -red / 20.0f);
            }
        }

        const float c = target > gain ? attackCoef : releaseCoef;
        gain = target + c * (gain - target);
        return gain;
    }

    bool isOpen() const noexcept { return open; }

private:
    float coefFor (float ms) const
    {
        const double t = std::max (0.01, (double) ms) * 0.001 * fs;
        return (float) std::exp (-1.0 / t);
    }

    GateParams p;
    double fs = 44100.0;

    float env = 0.0f;
    float gain = 1.0f;
    bool  open = false;
    int   holdCounter = 0;

    float detectorCoef = 0.0f;
    float attackCoef = 0.0f;
    float releaseCoef = 0.0f;
    int   holdSamples = 0;
    float floorLin = 0.0f;
    float maxRedDb = 120.0f;
};
