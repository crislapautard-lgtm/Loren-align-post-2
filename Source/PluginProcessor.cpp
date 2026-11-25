#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <algorithm>
#include <cmath>

//==============================================================================
struct LorenAlignProcessor::Worker : public juce::Thread
{
    Worker (LorenAlignProcessor& o, bool r) : juce::Thread ("Loren Align Worker"), owner (o), reanalyse (r) {}
    void run() override { owner.runJob (*this, reanalyse); }

    LorenAlignProcessor& owner;
    bool reanalyse;
};

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout LorenAlignProcessor::createLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;

    p.push_back (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "time", 1 }, "Time",
                                                               juce::StringArray { "Dynamic", "Static" }, 0));
    p.push_back (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "phase", 1 }, "Phase",
                                                               juce::StringArray { "Spectral", "Polarity", "Off" }, 0));
    p.push_back (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "range", 1 }, "Range",
                                                               juce::StringArray { "250 ms", "500 ms", "1 s", "2 s" }, 1));
    p.push_back (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "view", 1 }, "View",
                                                               juce::StringArray { "Aligned", "Source" }, 0));
    p.push_back (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "sync", 1 }, "Host Sync", false));

    return { p.begin(), p.end() };
}

//==============================================================================
LorenAlignProcessor::LorenAlignProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", createLayout())
{
    formatManager.registerBasicFormats();

    timeParam  = apvts.getRawParameterValue ("time");
    phaseParam = apvts.getRawParameterValue ("phase");
    rangeParam = apvts.getRawParameterValue ("range");
    viewParam  = apvts.getRawParameterValue ("view");
    syncParam  = apvts.getRawParameterValue ("sync");

    apvts.addParameterListener ("time", this);
    apvts.addParameterListener ("phase", this);
    apvts.addParameterListener ("range", this);
}

LorenAlignProcessor::~LorenAlignProcessor()
{
    apvts.removeParameterListener ("time", this);
    apvts.removeParameterListener ("phase", this);
    apvts.removeParameterListener ("range", this);

    cancelPendingUpdate();

    if (worker != nullptr)
        worker->stopThread (20000);
}

//==============================================================================
bool LorenAlignProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

void LorenAlignProcessor::prepareToPlay (double sampleRate, int)
{
    if (sampleRate > 0.0 && std::abs (sampleRate - workingSampleRate.load()) > 1.0)
    {
        workingSampleRate.store (sampleRate);

        bool hasFiles;
        {
            juce::SpinLock::ScopedLockType l (bufLock);
            hasFiles = (refBuf != nullptr || srcBuf != nullptr);
        }

        if (hasFiles)
        {
            needReload.store (true);
            triggerAsyncUpdate();
        }
    }
}

//==============================================================================
void LorenAlignProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const bool sync = syncParam->load() > 0.5f;
    bool active = playing.load();
    int64_t pos = playPosition.load();

    if (sync)
    {
        active = false;
        if (auto* ph = getPlayHead())
            if (auto info = ph->getPosition())
                if (info->getIsPlaying())
                    if (auto t = info->getTimeInSamples())
                    {
                        pos = *t;
                        active = true;
                    }
    }

    if (! active)
        return; // sin reproduccion: el audio de entrada pasa tal cual

    const int n = buffer.getNumSamples();
    buffer.clear();

    juce::SpinLock::ScopedTryLockType lock (bufLock);
    if (! lock.isLocked())
        return;

    const bool wantAligned = viewParam->load() < 0.5f;
    const juce::AudioBuffer<float>* b = (wantAligned && alignedBuf != nullptr) ? alignedBuf.get() : srcBuf.get();

    if (b == nullptr || b->getNumSamples() == 0 || pos < 0)
    {
        if (! sync && b == nullptr)
            playing.store (false);
        return;
    }

    const int64_t avail = (int64_t) b->getNumSamples() - pos;
    if (avail <= 0)
    {
        if (! sync)
            playing.store (false);
        return;
    }

    const int cnt = (int) std::min<int64_t> (n, avail);
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    {
        const int s = std::min (ch, b->getNumChannels() - 1);
        buffer.copyFrom (ch, 0, *b, s, (int) pos, cnt);
    }

    playPosition.store (pos + cnt);
    if (! sync && cnt < n)
        playing.store (false);
}

void LorenAlignProcessor::setPlaying (bool shouldPlay)
{
    if (shouldPlay)
    {
        int len = 0;
        {
            juce::SpinLock::ScopedLockType l (bufLock);
            if (srcBuf != nullptr)
                len = srcBuf->getNumSamples();
            if (alignedBuf != nullptr)
                len = std::max (len, alignedBuf->getNumSamples());
        }
        if (playPosition.load() >= (int64_t) len)
            playPosition.store (0);
    }
    playing.store (shouldPlay);
}

//==============================================================================
juce::String LorenAlignProcessor::loadFileInto (const juce::File& f, juce::AudioBuffer<float>& dest, bool mono)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (f));
    if (reader == nullptr)
        return "No se pudo abrir el archivo: " + f.getFileName();

    if (reader->lengthInSamples <= 0)
        return "El archivo esta vacio: " + f.getFileName();

    if ((double) reader->lengthInSamples / std::max (1.0, reader->sampleRate) > 3600.0)
        return "El archivo supera los 60 minutos.";

    const int len = (int) reader->lengthInSamples;
    const int chIn = (int) std::min<unsigned int> (reader->numChannels, 2u);

    juce::AudioBuffer<float> tmp (chIn, len + 16);
    tmp.clear();

    if (! reader->read (tmp.getArrayOfWritePointers(), chIn, 0, len))
        return "Error leyendo el audio.";

    int chOut = chIn;
    if (mono && chIn > 1)
    {
        for (int i = 0; i < len; ++i)
            tmp.setSample (0, i, 0.5f * (tmp.getSample (0, i) + tmp.getSample (1, i)));
        chOut = 1;
    }

    const double wsr = workingSampleRate.load();
    if (std::abs (reader->sampleRate - wsr) > 0.5)
    {
        const double ratio = reader->sampleRate / wsr;
        const int outLen = std::max (1, (int) std::floor ((double) len / ratio));
        dest.setSize (chOut, outLen);
        dest.clear();

        for (int c = 0; c < chOut; ++c)
        {
            juce::LagrangeInterpolator li;
            li.process (ratio, tmp.getReadPointer (c), dest.getWritePointer (c), outLen);
        }
    }
    else
    {
        dest.setSize (chOut, len);
        for (int c = 0; c < chOut; ++c)
            dest.copyFrom (c, 0, tmp, c, 0, len);
    }

    return {};
}

void LorenAlignProcessor::leaveStereoMode (bool clearReference)
{
    if (! stereoMode)
        return;

    stereoMode = false;
    stereoFile = juce::File();

    std::shared_ptr<juce::AudioBuffer<float>> oldA, oldB;
    {
        juce::SpinLock::ScopedLockType l (bufLock);
        oldA = std::move (srcBuf);
        srcBuf.reset();
        if (clearReference)
        {
            oldB = std::move (refBuf);
            refBuf.reset();
        }
    }
    srcFile = juce::File();
    if (clearReference)
        refFile = juce::File();
}

juce::String LorenAlignProcessor::loadRef (const juce::File& f)
{
    leaveStereoMode (false);

    auto buf = std::make_shared<juce::AudioBuffer<float>>();
    auto err = loadFileInto (f, *buf, true);
    if (err.isNotEmpty())
        return err;

    std::shared_ptr<juce::AudioBuffer<float>> old;
    {
        juce::SpinLock::ScopedLockType l (bufLock);
        old = std::move (refBuf);
        refBuf = buf;
    }
    refFile = f;
    return {};
}

juce::String LorenAlignProcessor::loadSrc (const juce::File& f)
{
    leaveStereoMode (true);

    auto buf = std::make_shared<juce::AudioBuffer<float>>();
    auto err = loadFileInto (f, *buf, false);
    if (err.isNotEmpty())
        return err;

    std::shared_ptr<juce::AudioBuffer<float>> old;
    {
        juce::SpinLock::ScopedLockType l (bufLock);
        old = std::move (srcBuf);
        srcBuf = buf;
    }
    srcFile = f;
    return {};
}

juce::String LorenAlignProcessor::loadStereo (const juce::File& f)
{
    juce::AudioBuffer<float> both;
    auto err = loadFileInto (f, both, false);
    if (err.isNotEmpty())
        return err;

    if (both.getNumChannels() < 2)
        return "El archivo es mono: arrastra dos archivos (referencia y fuente) o un estereo (L = referencia, R = fuente).";

    auto r = std::make_shared<juce::AudioBuffer<float>> (1, both.getNumSamples());
    auto s = std::make_shared<juce::AudioBuffer<float>> (1, both.getNumSamples());
    r->copyFrom (0, 0, both, 0, 0, both.getNumSamples());
    s->copyFrom (0, 0, both, 1, 0, both.getNumSamples());

    std::shared_ptr<juce::AudioBuffer<float>> oldR, oldS;
    {
        juce::SpinLock::ScopedLockType l (bufLock);
        oldR = std::move (refBuf);
        oldS = std::move (srcBuf);
        refBuf = r;
        srcBuf = s;
    }

    stereoMode = true;
    stereoFile = f;
    refFile = juce::File();
    srcFile = juce::File();
    return {};
}

void LorenAlignProcessor::afterLoad()
{
    std::shared_ptr<juce::AudioBuffer<float>> oldAligned;
    {
        juce::SpinLock::ScopedLockType l (bufLock);
        oldAligned = std::move (alignedBuf);
        alignedBuf.reset();
        analysis = lap::Analysis();
    }

    playPosition.store (0);

    bool both;
    {
        juce::SpinLock::ScopedLockType l (bufLock);
        both = (refBuf != nullptr && srcBuf != nullptr);
    }

    if (both)
        startJob (true);
    else
    {
        if (worker != nullptr)
        {
            worker->stopThread (20000);
            worker.reset();
        }
        status.store ((int) Status::Idle);
        sendChangeMessage();
    }
}

void LorenAlignProcessor::reloadAll()
{
    const auto r = refFile, s = srcFile, st = stereoFile;

    if (st.existsAsFile())
        loadStereo (st);
    else
    {
        if (r.existsAsFile())
            loadRef (r);
        if (s.existsAsFile())
            loadSrc (s);
    }

    afterLoad();
}

juce::String LorenAlignProcessor::setReferenceFile (const juce::File& f)
{
    auto err = loadRef (f);
    if (err.isNotEmpty())
    {
        setError (err);
        return err;
    }
    afterLoad();
    return {};
}

juce::String LorenAlignProcessor::setSourceFile (const juce::File& f)
{
    auto err = loadSrc (f);
    if (err.isNotEmpty())
    {
        setError (err);
        return err;
    }
    afterLoad();
    return {};
}

juce::String LorenAlignProcessor::handleDrop (const juce::Array<juce::File>& files, bool toReference)
{
    juce::Array<juce::File> list;
    for (auto& f : files)
        if (f.existsAsFile())
            list.add (f);

    if (list.isEmpty())
        return "Arrastra archivos de audio (WAV, AIFF, FLAC, MP3...).";

    juce::String err;

    if (list.size() >= 2)
    {
        err = loadRef (list[0]);
        if (err.isEmpty())
            err = loadSrc (list[1]);
    }
    else
    {
        const auto f = list[0];

        bool empty;
        {
            juce::SpinLock::ScopedLockType l (bufLock);
            empty = (refBuf == nullptr && srcBuf == nullptr);
        }

        unsigned int channels = 1;
        if (empty)
            if (std::unique_ptr<juce::AudioFormatReader> r { formatManager.createReaderFor (f) })
                channels = r->numChannels;

        if (empty && channels >= 2)
            err = loadStereo (f);
        else
            err = toReference ? loadRef (f) : loadSrc (f);
    }

    if (err.isNotEmpty())
    {
        setError (err);
        return err;
    }

    afterLoad();
    return {};
}

void LorenAlignProcessor::clearAll()
{
    if (worker != nullptr)
    {
        worker->stopThread (20000);
        worker.reset();
    }

    playing.store (false);
    playPosition.store (0);

    std::shared_ptr<juce::AudioBuffer<float>> a, b, c;
    {
        juce::SpinLock::ScopedLockType l (bufLock);
        a = std::move (refBuf);
        b = std::move (srcBuf);
        c = std::move (alignedBuf);
        refBuf.reset();
        srcBuf.reset();
        alignedBuf.reset();
        analysis = lap::Analysis();
    }

    refFile = juce::File();
    srcFile = juce::File();
    stereoFile = juce::File();
    stereoMode = false;

    status.store ((int) Status::Idle);
    sendChangeMessage();
}

void LorenAlignProcessor::rescan()
{
    bool both;
    {
        juce::SpinLock::ScopedLockType l (bufLock);
        both = (refBuf != nullptr && srcBuf != nullptr);
    }
    if (both)
        startJob (true);
}

//==============================================================================
void LorenAlignProcessor::parameterChanged (const juce::String& id, float)
{
    if (id == "range")
        needReanalyse.store (true);
    else
        needRerender.store (true);

    triggerAsyncUpdate();
}

void LorenAlignProcessor::handleAsyncUpdate()
{
    if (needReload.exchange (false))
    {
        needReanalyse.store (false);
        needRerender.store (false);
        reloadAll();
        return;
    }

    const bool re = needReanalyse.exchange (false);
    const bool rr = needRerender.exchange (false);
    if (re || rr)
        startJob (re);
}

//==============================================================================
void LorenAlignProcessor::startJob (bool reanalyse)
{
    if (worker != nullptr)
    {
        worker->stopThread (20000);
        worker.reset();
    }

    bool both;
    {
        juce::SpinLock::ScopedLockType l (bufLock);
        both = (refBuf != nullptr && srcBuf != nullptr);
    }
    if (! both)
        return;

    progress.store (0.0f);
    status.store ((int) (reanalyse ? Status::Analysing : Status::Rendering));

    worker = std::make_unique<Worker> (*this, reanalyse);
    worker->startThread();
    sendChangeMessage();
}

void LorenAlignProcessor::runJob (Worker& w, bool reanalyse)
{
    auto cancel = [&w] { return w.threadShouldExit(); };

    std::shared_ptr<juce::AudioBuffer<float>> ref, src;
    lap::Analysis a;
    {
        juce::SpinLock::ScopedLockType l (bufLock);
        ref = refBuf;
        src = srcBuf;
        a = analysis;
    }

    if (ref == nullptr || src == nullptr)
        return;

    const double sr = workingSampleRate.load();
    lap::AlignEngine engine;

    const bool needAnalysis = reanalyse || ! a.valid;

    if (needAnalysis)
    {
        status.store ((int) Status::Analysing);

        static const double ranges[] = { 250.0, 500.0, 1000.0, 2000.0 };
        const int ri = juce::jlimit (0, 3, (int) std::lround (rangeParam->load()));

        a = engine.analyse (*ref, *src, sr, ranges[ri], cancel, [this] (float p) { progress.store (0.6f * p); });

        if (w.threadShouldExit())
            return;

        {
            juce::SpinLock::ScopedLockType l (bufLock);
            analysis = a;
        }
        sendChangeMessage(); // la interfaz ya puede mostrar las voces detectadas
    }

    if (! a.valid)
    {
        {
            juce::SpinLock::ScopedLockType l (bufLock);
            alignedBuf.reset();
        }
        setError ("No pude medir la alineacion. Necesito al menos ~2 s de voz que aparezca en AMBOS audios.");
        return;
    }

    status.store ((int) Status::Rendering);

    const auto tm = timeParam->load() < 0.5f ? lap::TimeMode::Dynamic : lap::TimeMode::Static;
    const int pi = juce::jlimit (0, 2, (int) std::lround (phaseParam->load()));
    const auto pm = (lap::PhaseMode) pi;

    const float base = needAnalysis ? 0.6f : 0.0f;
    auto out = engine.render (*ref, *src, a, tm, pm, cancel,
                              [this, base] (float p) { progress.store (base + (1.0f - base) * p); });

    if (w.threadShouldExit())
        return;

    auto sp = std::make_shared<juce::AudioBuffer<float>> (std::move (out));

    std::shared_ptr<juce::AudioBuffer<float>> old;
    {
        juce::SpinLock::ScopedLockType l (bufLock);
        old = std::move (alignedBuf);
        alignedBuf = sp;
    }

    progress.store (1.0f);
    status.store ((int) Status::Ready);
    sendChangeMessage();
}

void LorenAlignProcessor::setError (const juce::String& message)
{
    {
        const juce::ScopedLock sl (textLock);
        errorText = message;
    }
    status.store ((int) Status::Error);
    sendChangeMessage();
}

juce::String LorenAlignProcessor::getErrorText() const
{
    const juce::ScopedLock sl (textLock);
    return errorText;
}

LorenAlignProcessor::Snapshot LorenAlignProcessor::getSnapshot() const
{
    Snapshot s;
    juce::SpinLock::ScopedLockType l (bufLock);
    s.reference = refBuf;
    s.source = srcBuf;
    s.aligned = alignedBuf;
    s.analysis = analysis;

    if (stereoMode)
    {
        s.referenceName = stereoFile.getFileName() + "  (L)";
        s.sourceName = stereoFile.getFileName() + "  (R)";
    }
    else
    {
        s.referenceName = refFile.getFileName();
        s.sourceName = srcFile.getFileName();
    }
    return s;
}

//==============================================================================
juce::String LorenAlignProcessor::exportAligned (const juce::File& destination)
{
    std::shared_ptr<juce::AudioBuffer<float>> buf;
    {
        juce::SpinLock::ScopedLockType l (bufLock);
        buf = alignedBuf;
    }

    if (buf == nullptr || buf->getNumSamples() == 0)
        return "Todavia no hay audio alineado para exportar.";

    juce::WavAudioFormat wav;
    destination.deleteFile();

    auto* stream = new juce::FileOutputStream (destination);
    if (stream->failedToOpen())
    {
        delete stream;
        return "No pude crear el archivo: " + destination.getFullPathName();
    }

    std::unique_ptr<juce::AudioFormatWriter> writer (
        wav.createWriterFor (stream, workingSampleRate.load(), (unsigned int) buf->getNumChannels(), 24,
                             juce::StringPairArray(), 0));

    if (writer == nullptr)
        return "No pude iniciar la escritura del WAV.";

    writer->writeFromAudioSampleBuffer (*buf, 0, buf->getNumSamples());
    writer.reset(); // cierra y vacia el archivo
    return {};
}

//==============================================================================
void LorenAlignProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty ("refPath", refFile.getFullPathName(), nullptr);
    state.setProperty ("srcPath", srcFile.getFullPathName(), nullptr);
    state.setProperty ("stereoPath", stereoFile.getFullPathName(), nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void LorenAlignProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    auto vt = juce::ValueTree::fromXml (*xml);

    refFile = juce::File (vt.getProperty ("refPath", "").toString());
    srcFile = juce::File (vt.getProperty ("srcPath", "").toString());
    stereoFile = juce::File (vt.getProperty ("stereoPath", "").toString());

    apvts.replaceState (vt);

    if (refFile.existsAsFile() || srcFile.existsAsFile() || stereoFile.existsAsFile())
    {
        needReload.store (true);
        triggerAsyncUpdate();
    }
}

//==============================================================================
juce::AudioProcessorEditor* LorenAlignProcessor::createEditor()
{
    return new LorenAlignEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new LorenAlignProcessor();
}
