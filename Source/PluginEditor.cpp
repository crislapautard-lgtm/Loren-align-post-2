#include "PluginEditor.h"

#include <algorithm>
#include <cmath>

namespace
{
    const juce::Colour kBg        { 0xff2b2b2b };
    const juce::Colour kPanel     { 0xff0b0b0b };
    const juce::Colour kWave      { 0xff333333 };
    const juce::Colour kPink      { 0xffff3b7f };
    const juce::Colour kCyan      { 0xff3ee6f2 };
    const juce::Colour kWhite     { 0xfff2f2f2 };

    juce::Font mkFont (float h, int style = juce::Font::plain)
    {
        return juce::Font (h, style);
    }

    // minimo / maximo del canal 0 en [s0, s1)
    bool minMax (const juce::AudioBuffer<float>* b, int64_t s0, int64_t s1, int maxSamples, float& mn, float& mx)
    {
        mn = mx = 0.0f;
        if (b == nullptr || b->getNumSamples() == 0)
            return false;

        s0 = std::max<int64_t> (0, s0);
        s1 = std::min<int64_t> (b->getNumSamples(), s1);
        if (s1 <= s0)
            return false;

        const float* d = b->getReadPointer (0);
        const int64_t step = std::max<int64_t> (1, (s1 - s0) / maxSamples);
        mn = mx = d[s0];
        for (int64_t i = s0; i < s1; i += step)
        {
            mn = std::min (mn, d[i]);
            mx = std::max (mx, d[i]);
        }
        return true;
    }

    juce::String formatTime (double sec)
    {
        sec = std::max (0.0, sec);
        const int m = (int) (sec / 60.0);
        const double s = sec - m * 60.0;
        return juce::String::formatted ("%02d:%06.3f", m, s);
    }
}

//==============================================================================
LorenLookAndFeel::LorenLookAndFeel()
{
    setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff161616));
    setColour (juce::ComboBox::textColourId, kPink);
    setColour (juce::ComboBox::outlineColourId, juce::Colour (0x00000000));
    setColour (juce::ComboBox::arrowColourId, kPink);
    setColour (juce::PopupMenu::backgroundColourId, juce::Colour (0xff1c1c1c));
    setColour (juce::PopupMenu::textColourId, kWhite);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, kPink.darker (0.4f));
    setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::white);
    setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1c1c1c));
    setColour (juce::TextButton::buttonOnColourId, kPink.darker (0.5f));
    setColour (juce::TextButton::textColourOffId, juce::Colour (0xffe8a0b8));
    setColour (juce::TextButton::textColourOnId, juce::Colours::white);
    setColour (juce::ToggleButton::textColourId, juce::Colour (0xffe8a0b8));
    setColour (juce::ToggleButton::tickColourId, kPink);
    setColour (juce::ToggleButton::tickDisabledColourId, juce::Colour (0xff555555));
}

//==============================================================================
LorenAlignEditor::LorenAlignEditor (LorenAlignProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    setLookAndFeel (&laf);

    auto setupBox = [this] (juce::ComboBox& box, const juce::String& id)
    {
        if (auto* c = dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter (id)))
            box.addItemList (c->choices, 1);
        box.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (box);
    };

    setupBox (timeBox, "time");
    setupBox (phaseBox, "phase");
    setupBox (rangeBox, "range");

    timeAtt  = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, "time", timeBox);
    phaseAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, "phase", phaseBox);
    rangeAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, "range", rangeBox);

    for (auto* b : { &scanButton, &viewButton, &zoomOutButton, &zoomInButton, &playButton, &exportButton, &clearButton })
        addAndMakeVisible (*b);

    addAndMakeVisible (syncButton);
    syncAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, "sync", syncButton);

    scanButton.setTooltip ("Vuelve a escanear las voces y a medir la alineacion");
    scanButton.onClick = [this] { infoOverride = {}; proc.rescan(); };

    viewButton.onClick = [this]
    {
        if (auto* prm = proc.apvts.getParameter ("view"))
        {
            const bool toSource = prm->getValue() < 0.5f;
            prm->beginChangeGesture();
            prm->setValueNotifyingHost (toSource ? 1.0f : 0.0f);
            prm->endChangeGesture();
        }
        repaint();
    };

    zoomInButton.onClick  = [this] { zoomBy (0.5, viewStart + viewLen * 0.5); };
    zoomOutButton.onClick = [this] { zoomBy (2.0, viewStart + viewLen * 0.5); };

    playButton.onClick = [this] { proc.setPlaying (! proc.isPlaying()); };
    exportButton.onClick = [this] { exportClicked(); };
    clearButton.onClick = [this] { infoOverride = {}; proc.clearAll(); };

    proc.addChangeListener (this);

    setResizable (true, true);
    setResizeLimits (660, 430, 1700, 1000);
    setSize (800, 540);

    updateSnapshot();
    resetView();
    startTimerHz (30);
}

LorenAlignEditor::~LorenAlignEditor()
{
    stopTimer();
    proc.removeChangeListener (this);
    setLookAndFeel (nullptr);
}

//==============================================================================
double LorenAlignEditor::sampleRate() const { return std::max (1.0, proc.getWorkingSampleRate()); }

double LorenAlignEditor::totalSamples() const
{
    int n = 0;
    if (snap.reference != nullptr) n = std::max (n, snap.reference->getNumSamples());
    if (snap.source != nullptr)    n = std::max (n, snap.source->getNumSamples());
    if (snap.aligned != nullptr)   n = std::max (n, snap.aligned->getNumSamples());
    return (double) n;
}

double LorenAlignEditor::sampleAtX (float x) const
{
    return viewStart + ((double) x - waveArea.getX()) / std::max (1, waveArea.getWidth()) * viewLen;
}

float LorenAlignEditor::xAtSample (double s) const
{
    return (float) waveArea.getX() + (float) ((s - viewStart) / std::max (1.0, viewLen) * waveArea.getWidth());
}

void LorenAlignEditor::clampView()
{
    const double total = std::max (1.0, totalSamples());
    viewLen = juce::jlimit (std::min (total, sampleRate() * 0.05), total, viewLen);
    viewStart = juce::jlimit (0.0, std::max (0.0, total - viewLen), viewStart);
}

void LorenAlignEditor::resetView()
{
    const double total = std::max (1.0, totalSamples());
    viewStart = 0.0;
    viewLen = std::min (total, sampleRate() * 12.0);
    clampView();
}

void LorenAlignEditor::zoomBy (double factor, double anchor)
{
    const double rel = (anchor - viewStart) / std::max (1.0, viewLen);
    viewLen *= factor;
    clampView();
    viewStart = anchor - rel * viewLen;
    clampView();
    repaint();
}

void LorenAlignEditor::setViewCentre (double sample)
{
    viewStart = sample - viewLen * 0.5;
    clampView();
    repaint();
}

//==============================================================================
void LorenAlignEditor::updateSnapshot()
{
    snap = proc.getSnapshot();

    const void* r = snap.reference.get();
    const void* s = snap.source.get();
    const bool filesChanged = (r != lastRef || s != lastSrc);
    lastRef = r;
    lastSrc = s;

    // normalizacion visual
    float pk = 1.0e-3f;
    for (auto* b : { snap.reference.get(), snap.source.get() })
    {
        if (b == nullptr || b->getNumSamples() == 0)
            continue;
        const float* d = b->getReadPointer (0);
        const int step = std::max (1, b->getNumSamples() / 200000);
        for (int i = 0; i < b->getNumSamples(); i += step)
            pk = std::max (pk, std::abs (d[i]));
    }
    peak = pk;

    rebuildOverview();

    if (filesChanged)
        resetView();
    else
        clampView();
}

void LorenAlignEditor::rebuildOverview()
{
    const int w = std::max (1, overviewRect.getWidth());
    ovRef.assign ((size_t) w, { 0.0f, 0.0f });
    ovSrc.assign ((size_t) w, { 0.0f, 0.0f });

    const double total = std::max (1.0, totalSamples());
    for (int x = 0; x < w; ++x)
    {
        const int64_t s0 = (int64_t) (total * x / w);
        const int64_t s1 = (int64_t) (total * (x + 1) / w);
        float mn, mx;
        if (minMax (snap.reference.get(), s0, s1, 2048, mn, mx))
            ovRef[(size_t) x] = { mn, mx };
        if (minMax (snap.source.get(), s0, s1, 2048, mn, mx))
            ovSrc[(size_t) x] = { mn, mx };
    }
}

void LorenAlignEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    updateSnapshot();
    repaint();
}

void LorenAlignEditor::timerCallback()
{
    const bool viewIsSource = proc.apvts.getRawParameterValue ("view")->load() > 0.5f;
    viewButton.setButtonText (viewIsSource ? "VIEW: SOURCE" : "VIEW: ALIGNED");
    playButton.setButtonText (proc.isPlaying() ? "STOP" : "PLAY");
    playButton.setToggleState (proc.isPlaying(), juce::dontSendNotification);

    const auto st = proc.getStatus();
    const bool busy = (st == LorenAlignProcessor::Status::Analysing || st == LorenAlignProcessor::Status::Rendering);
    const bool syncOn = proc.apvts.getRawParameterValue ("sync")->load() > 0.5f;

    if (proc.isPlaying() || syncOn)
    {
        const double pos = (double) proc.getPlayPosition();
        if (pos > viewStart + viewLen || pos < viewStart)
        {
            viewStart = pos;
            clampView();
        }
    }

    if (busy || proc.isPlaying() || syncOn)
        repaint();
}

//==============================================================================
void LorenAlignEditor::resized()
{
    const int w = getWidth(), h = getHeight();
    const int m = 12;

    topBar = { 0, 0, w, 48 };
    overviewRect = { m, 56, w - 2 * m, 76 };
    scrollRect = { m, 136, w - 2 * m, 10 };
    bottomBar = { 0, h - 48, w, 48 };
    waveRect = { m, 154, w - 2 * m, h - 154 - 54 };
    waveArea = waveRect.withTrimmedTop (18);
    dropArea = overviewRect.getUnion (waveRect);

    // combos arriba a la derecha
    const int cbH = 24;
    const int cy = 12;
    int x = w - m;
    auto place = [&] (juce::ComboBox& box, int width, int labelW)
    {
        x -= width;
        box.setBounds (x, cy, width, cbH);
        x -= labelW + 14;
    };
    place (rangeBox, 84, 52);
    place (phaseBox, 104, 52);
    place (timeBox, 96, 44);

    // botones abajo
    const int by = h - 40, bh = 28;
    int bx = m;
    auto put = [&] (juce::Component& c, int width)
    {
        c.setBounds (bx, by, width, bh);
        bx += width + 6;
    };
    put (scanButton, 62);
    put (viewButton, 118);
    put (zoomOutButton, 30);
    put (zoomInButton, 30);
    put (playButton, 62);
    put (exportButton, 100);
    put (clearButton, 58);
    syncButton.setBounds (bx + 4, by, 100, bh);

    rebuildOverview();
    clampView();
}

//==============================================================================
juce::String LorenAlignEditor::buildInfoText (juce::Colour& colour) const
{
    colour = juce::Colour (0xffc8c8c8);

    if (infoOverride.isNotEmpty())
    {
        colour = infoOverrideColour;
        return infoOverride;
    }

    switch (proc.getStatus())
    {
        case LorenAlignProcessor::Status::Analysing:
            colour = kCyan;
            return "ESCANEANDO VOCES Y MIDIENDO...  " + juce::String ((int) (proc.getProgress() * 100.0f)) + "%";
        case LorenAlignProcessor::Status::Rendering:
            colour = kCyan;
            return "ALINEANDO...  " + juce::String ((int) (proc.getProgress() * 100.0f)) + "%";
        case LorenAlignProcessor::Status::Error:
            colour = juce::Colour (0xffff7070);
            return proc.getErrorText();
        default:
            break;
    }

    const auto& a = snap.analysis;
    if (a.valid)
    {
        const double sr = std::max (1.0, a.sampleRate);
        double lo = a.lags.empty() ? a.globalLag : a.lags.front(), hi = lo;
        for (double l : a.lags)
        {
            lo = std::min (lo, l);
            hi = std::max (hi, l);
        }

        const juce::String conf = a.confidence > 8.0f ? "ALTA" : (a.confidence > 4.5f ? "MEDIA" : "BAJA");
        return juce::String::formatted ("OFFSET %+.2f ms   |   DERIVA %.2f ms   |   POLARIDAD %s   |   VOCES %d   |   CONFIANZA %s",
                                        a.globalLag / sr * 1000.0, (hi - lo) / sr * 1000.0,
                                        a.polarity > 0 ? "OK" : "INVERTIDA", (int) a.voices.size(), conf.toRawUTF8());
    }

    if (snap.reference == nullptr && snap.source == nullptr)
        return "ARRASTRA TU AUDIO AQUI PARA ESCANEARLO";
    if (snap.reference == nullptr)
        return "FALTA LA REFERENCIA (zona rosa, arriba)";
    if (snap.source == nullptr)
        return "FALTA LA FUENTE A ALINEAR (zona cian, abajo)";
    return {};
}

//==============================================================================
void LorenAlignEditor::paint (juce::Graphics& g)
{
    g.fillAll (kBg);

    // ---------- barra superior
    {
        const float cx = 18.0f, cy = 24.0f, r = 9.0f;
        g.setColour (kPink);
        g.drawEllipse (cx, cy - r, r * 2, r * 2, 3.0f);
        g.setColour (kCyan);
        g.drawEllipse (cx + 11.0f, cy - r, r * 2, r * 2, 3.0f);

        auto f = mkFont (22.0f, juce::Font::bold);
        f.setExtraKerningFactor (0.10f);
        g.setFont (f);
        g.setColour (kWhite);
        g.drawText ("LOREN ALIGN POST", 58, 8, 300, 30, juce::Justification::centredLeft, false);

        const int tw = f.getStringWidth ("LOREN ALIGN POST");
        g.setColour (kPink);
        g.drawText ("2", 58 + tw + 6, 8, 30, 30, juce::Justification::centredLeft, false);

        g.setFont (mkFont (11.0f, juce::Font::bold));
        g.setColour (kPink);
        auto lbl = [&] (const juce::ComboBox& b, const juce::String& t)
        {
            g.drawText (t, b.getX() - 54, b.getY(), 50, b.getHeight(), juce::Justification::centredRight, false);
        };
        lbl (timeBox, "TIME");
        lbl (phaseBox, "PHASE");
        lbl (rangeBox, "RANGE");
    }

    // ---------- vista general (overview)
    {
        g.setColour (kPanel);
        g.fillRect (overviewRect);

        const auto laneH = overviewRect.getHeight() / 2;
        const auto refLane = overviewRect.withHeight (laneH);
        const auto srcLane = overviewRect.withTrimmedTop (laneH);

        g.setColour (juce::Colour (0xff1e1e1e));
        g.drawHorizontalLine (srcLane.getY(), (float) overviewRect.getX(), (float) overviewRect.getRight());

        const double total = std::max (1.0, totalSamples());

        // regiones con voz (escaneo)
        g.setColour (juce::Colours::white.withAlpha (0.10f));
        for (const auto& v : snap.analysis.voices)
        {
            const float x0 = (float) overviewRect.getX() + (float) (v.start / total) * overviewRect.getWidth();
            const float x1 = (float) overviewRect.getX() + (float) (v.end / total) * overviewRect.getWidth();
            g.fillRect (x0, (float) refLane.getY() + 1.0f, std::max (1.0f, x1 - x0), (float) laneH - 2.0f);
        }

        auto drawLane = [&] (const std::vector<std::pair<float, float>>& data, juce::Rectangle<int> lane, juce::Colour c)
        {
            g.setColour (c);
            const float mid = (float) lane.getCentreY();
            const float amp = (float) lane.getHeight() * 0.46f;
            for (size_t x = 0; x < data.size(); ++x)
            {
                const float top = mid - data[x].second / peak * amp;
                const float bot = mid - data[x].first / peak * amp;
                g.fillRect ((float) overviewRect.getX() + (float) x, top, 1.0f, std::max (1.0f, bot - top));
            }
        };

        if (snap.reference != nullptr)
            drawLane (ovRef, refLane, kPink);
        else
        {
            g.setColour (kPink.withAlpha (0.55f));
            g.setFont (mkFont (11.0f, juce::Font::bold));
            g.drawText ("REFERENCIA: arrastra aqui el audio limpio (boom / microfono principal)", refLane,
                        juce::Justification::centred, false);
        }

        if (snap.source != nullptr)
            drawLane (ovSrc, srcLane, kCyan);
        else
        {
            g.setColour (kCyan.withAlpha (0.55f));
            g.setFont (mkFont (11.0f, juce::Font::bold));
            g.drawText ("FUENTE: arrastra aqui el audio a alinear (lavalier / segundo microfono)", srcLane,
                        juce::Justification::centred, false);
        }

        // ventana visible
        if (totalSamples() > 0.0)
        {
            const float x0 = (float) overviewRect.getX() + (float) (viewStart / total) * overviewRect.getWidth();
            const float x1 = (float) overviewRect.getX() + (float) ((viewStart + viewLen) / total) * overviewRect.getWidth();
            g.setColour (juce::Colours::white.withAlpha (0.95f));
            g.drawRect (juce::Rectangle<float> (x0, (float) overviewRect.getY(), std::max (3.0f, x1 - x0),
                                                (float) overviewRect.getHeight()), 2.0f);
        }
    }

    // ---------- barra de desplazamiento
    {
        g.setColour (kPanel);
        g.fillRect (scrollRect);
        const double total = std::max (1.0, totalSamples());
        const float x0 = (float) scrollRect.getX() + (float) (viewStart / total) * scrollRect.getWidth();
        const float x1 = (float) scrollRect.getX() + (float) ((viewStart + viewLen) / total) * scrollRect.getWidth();
        g.setColour (juce::Colour (0xffa0a0a0));
        g.fillRect (x0, (float) scrollRect.getY(), std::max (6.0f, x1 - x0), (float) scrollRect.getHeight());
    }

    // ---------- forma de onda principal
    {
        g.setColour (kWave);
        g.fillRect (waveRect);

        // regla de tiempo
        const double sr = sampleRate();
        const double secPerPx = (viewLen / sr) / std::max (1, waveArea.getWidth());
        static const double steps[] = { 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 30.0, 60.0, 300.0 };
        double step = steps[std::size (steps) - 1];
        for (double s : steps)
            if (s / secPerPx >= 90.0)
            {
                step = s;
                break;
            }

        g.setFont (mkFont (10.0f));
        const double t0 = viewStart / sr, t1 = (viewStart + viewLen) / sr;
        for (double t = std::ceil (t0 / step) * step; t <= t1; t += step)
        {
            const float x = xAtSample (t * sr);
            g.setColour (juce::Colour (0xff777777));
            g.drawVerticalLine ((int) x, (float) waveRect.getY(), (float) waveRect.getY() + 14.0f);
            g.setColour (juce::Colour (0xffaaaaaa));
            g.drawText (formatTime (t), (int) x + 3, waveRect.getY(), 80, 14, juce::Justification::centredLeft, false);
        }

        // linea central
        g.setColour (juce::Colours::white.withAlpha (0.08f));
        g.drawHorizontalLine (waveArea.getCentreY(), (float) waveArea.getX(), (float) waveArea.getRight());

        const bool viewIsSource = proc.apvts.getRawParameterValue ("view")->load() > 0.5f;
        const juce::AudioBuffer<float>* second = (! viewIsSource && snap.aligned != nullptr) ? snap.aligned.get()
                                                                                              : snap.source.get();

        if (snap.reference != nullptr || second != nullptr)
        {
            const float mid = (float) waveArea.getCentreY();
            const float amp = (float) waveArea.getHeight() * 0.47f;
            auto yOf = [&] (float v) { return mid - v / peak * amp; };

            for (int px = 0; px < waveArea.getWidth(); ++px)
            {
                const int64_t s0 = (int64_t) (viewStart + viewLen * px / waveArea.getWidth());
                const int64_t s1 = (int64_t) (viewStart + viewLen * (px + 1) / waveArea.getWidth()) + 1;
                const float x = (float) (waveArea.getX() + px);

                float rmn = 0, rmx = 0, smn = 0, smx = 0;
                const bool hasR = minMax (snap.reference.get(), s0, s1, 256, rmn, rmx);
                const bool hasS = minMax (second, s0, s1, 256, smn, smx);

                if (hasR)
                {
                    g.setColour (kPink);
                    g.fillRect (x, yOf (rmx), 1.0f, std::max (1.0f, yOf (rmn) - yOf (rmx)));
                }
                if (hasS)
                {
                    g.setColour (kCyan);
                    g.fillRect (x, yOf (smx), 1.0f, std::max (1.0f, yOf (smn) - yOf (smx)));
                }
                if (hasR && hasS)
                {
                    const float lo = std::max (rmn, smn), hi = std::min (rmx, smx);
                    if (hi > lo)
                    {
                        g.setColour (kWhite);
                        g.fillRect (x, yOf (hi), 1.0f, std::max (1.0f, yOf (lo) - yOf (hi)));
                    }
                }
            }

            // leyenda
            g.setFont (mkFont (11.0f, juce::Font::bold));
            auto legend = [&] (int y, juce::Colour c, const juce::String& t)
            {
                g.setColour (c);
                g.fillRect (waveArea.getX() + 8, waveArea.getY() + y + 3, 8, 8);
                g.setColour (c.withAlpha (0.9f));
                g.drawText (t, waveArea.getX() + 22, waveArea.getY() + y, 420, 14, juce::Justification::centredLeft, false);
            };
            legend (4, kPink, "REF  " + snap.referenceName);
            legend (20, kCyan, juce::String (viewIsSource ? "SRC  " : "ALIGNED  ") + snap.sourceName);

            // cabezal
            const double pos = (double) proc.getPlayPosition();
            if (pos >= viewStart && pos <= viewStart + viewLen)
            {
                g.setColour (juce::Colours::white);
                g.drawVerticalLine ((int) xAtSample (pos), (float) waveArea.getY(), (float) waveArea.getBottom());
            }
        }
        else
        {
            g.setColour (juce::Colours::white.withAlpha (0.55f));
            g.setFont (mkFont (18.0f, juce::Font::bold));
            g.drawText ("ARRASTRA TU AUDIO AQUI", waveArea.withTrimmedBottom (waveArea.getHeight() / 2),
                        juce::Justification::centredBottom, false);
            g.setFont (mkFont (12.0f));
            g.setColour (juce::Colours::white.withAlpha (0.45f));
            g.drawFittedText ("Mitad de arriba (rosa) = referencia.   Mitad de abajo (cian) = audio a alinear.\n"
                              "Suelta 2 archivos a la vez (1.o = referencia, 2.o = fuente) o 1 archivo estereo (L = ref, R = fuente).",
                              waveArea.withTrimmedTop (waveArea.getHeight() / 2).reduced (30, 4), juce::Justification::centredTop, 3);
        }
    }

    // ---------- estado
    {
        juce::Colour c;
        const auto text = buildInfoText (c);
        g.setColour (c);
        g.setFont (mkFont (11.0f, juce::Font::bold));
        const int x = syncButton.getRight() + 8;
        g.drawText (text, x, bottomBar.getY(), std::max (10, getWidth() - x - 150), bottomBar.getHeight(),
                    juce::Justification::centredLeft, true);
    }
}

void LorenAlignEditor::paintOverChildren (juce::Graphics& g)
{
    // marca de agua grande y tenue sobre la onda
    {
        g.setColour (juce::Colours::white.withAlpha (0.10f));
        auto f = mkFont (std::max (24.0f, (float) waveArea.getHeight() * 0.16f), juce::Font::bold | juce::Font::italic);
        g.setFont (f);
        g.drawText ("x la pauta studio", waveArea, juce::Justification::centred, false);
    }

    // marca de agua fija, siempre visible
    {
        auto r = juce::Rectangle<int> (getWidth() - 176, getHeight() - 40, 164, 28);
        g.setColour (juce::Colour (0xcc000000));
        g.fillRoundedRectangle (r.toFloat(), 6.0f);
        g.setColour (kPink);
        g.drawRoundedRectangle (r.toFloat(), 6.0f, 1.2f);
        g.setColour (kWhite);
        g.setFont (mkFont (14.0f, juce::Font::bold | juce::Font::italic));
        g.drawText ("x la pauta studio", r, juce::Justification::centred, false);
    }

    // resaltado al arrastrar archivos
    if (dragLane != 0)
    {
        const auto half = dropArea.getHeight() / 2;
        const auto zone = dragLane == 1 ? dropArea.withHeight (half) : dropArea.withTrimmedTop (half);
        const auto col = dragLane == 1 ? kPink : kCyan;
        g.setColour (col.withAlpha (0.18f));
        g.fillRect (zone);
        g.setColour (col);
        g.drawRect (zone, 3);
        g.setColour (juce::Colours::white);
        g.setFont (mkFont (16.0f, juce::Font::bold));
        g.drawText (dragLane == 1 ? "SOLTAR COMO REFERENCIA" : "SOLTAR COMO FUENTE A ALINEAR", zone,
                    juce::Justification::centred, false);
    }
}

//==============================================================================
void LorenAlignEditor::mouseDown (const juce::MouseEvent& e)
{
    const auto p = e.getPosition();
    draggingOverview = false;
    draggingWave = false;
    waveMoved = false;

    if (overviewRect.contains (p) || scrollRect.contains (p))
    {
        draggingOverview = true;
        const double total = std::max (1.0, totalSamples());
        setViewCentre ((double) (p.x - overviewRect.getX()) / std::max (1, overviewRect.getWidth()) * total);
    }
    else if (waveArea.contains (p))
    {
        draggingWave = true;
        dragStartViewStart = viewStart;
    }
}

void LorenAlignEditor::mouseDrag (const juce::MouseEvent& e)
{
    if (draggingOverview)
    {
        const double total = std::max (1.0, totalSamples());
        setViewCentre ((double) (e.x - overviewRect.getX()) / std::max (1, overviewRect.getWidth()) * total);
    }
    else if (draggingWave)
    {
        const int dx = e.getDistanceFromDragStartX();
        if (std::abs (dx) > 3)
            waveMoved = true;

        if (waveMoved)
        {
            viewStart = dragStartViewStart - (double) dx * viewLen / std::max (1, waveArea.getWidth());
            clampView();
            repaint();
        }
    }
}

void LorenAlignEditor::mouseUp (const juce::MouseEvent& e)
{
    if (draggingWave && ! waveMoved && waveArea.contains (e.getPosition()))
    {
        proc.setPlayPosition ((int64_t) std::max (0.0, sampleAtX ((float) e.x)));
        repaint();
    }

    draggingOverview = draggingWave = waveMoved = false;
}

void LorenAlignEditor::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (! (waveArea.contains (e.getPosition()) || overviewRect.contains (e.getPosition())))
        return;

    if (std::abs (w.deltaX) > std::abs (w.deltaY))
    {
        viewStart -= (double) w.deltaX * viewLen * 0.5;
        clampView();
        repaint();
        return;
    }

    const double anchor = waveArea.contains (e.getPosition()) ? sampleAtX ((float) e.x) : viewStart + viewLen * 0.5;
    zoomBy (std::pow (0.5, (double) w.deltaY * 3.0), anchor);
}

//==============================================================================
bool LorenAlignEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    return files.size() > 0;
}

void LorenAlignEditor::fileDragEnter (const juce::StringArray& files, int x, int y) { fileDragMove (files, x, y); }

void LorenAlignEditor::fileDragMove (const juce::StringArray& files, int, int y)
{
    const int lane = files.size() >= 2 ? 0 : (y < dropArea.getCentreY() ? 1 : 2);
    if (lane != dragLane)
    {
        dragLane = lane;
        repaint();
    }
}

void LorenAlignEditor::fileDragExit (const juce::StringArray&)
{
    dragLane = 0;
    repaint();
}

void LorenAlignEditor::filesDropped (const juce::StringArray& files, int, int y)
{
    dragLane = 0;

    juce::Array<juce::File> list;
    for (auto& f : files)
        list.add (juce::File (f));

    const bool toRef = y < dropArea.getCentreY();
    infoOverride = {};

    const auto err = proc.handleDrop (list, toRef);
    if (err.isNotEmpty())
    {
        infoOverride = err;
        infoOverrideColour = juce::Colour (0xffff7070);
    }

    repaint();
}

//==============================================================================
void LorenAlignEditor::exportClicked()
{
    if (snap.aligned == nullptr)
    {
        infoOverride = "TODAVIA NO HAY AUDIO ALINEADO: arrastra la referencia y la fuente primero.";
        infoOverrideColour = juce::Colour (0xffff7070);
        repaint();
        return;
    }

    auto base = juce::File (snap.sourceName).getFileNameWithoutExtension();
    if (base.isEmpty())
        base = "audio";

    auto def = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile (base + "_aligned.wav");

    chooser = std::make_unique<juce::FileChooser> ("Exportar fuente alineada (WAV 24 bit)", def, "*.wav");

    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [safe = juce::Component::SafePointer<LorenAlignEditor> (this)] (const juce::FileChooser& fc)
                          {
                              if (safe == nullptr)
                                  return;

                              const auto f = fc.getResult();
                              if (f == juce::File())
                                  return;

                              const auto err = safe->proc.exportAligned (f.withFileExtension ("wav"));
                              if (err.isEmpty())
                              {
                                  safe->infoOverride = "EXPORTADO: " + f.withFileExtension ("wav").getFileName();
                                  safe->infoOverrideColour = juce::Colour (0xff7dff9b);
                              }
                              else
                              {
                                  safe->infoOverride = err;
                                  safe->infoOverrideColour = juce::Colour (0xffff7070);
                              }
                              safe->repaint();
                          });
}
