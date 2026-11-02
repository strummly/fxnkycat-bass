#include "PluginProcessor.h"
#include "PluginEditor.h"

using APVTS = juce::AudioProcessorValueTreeState;

namespace
{
    std::unique_ptr<juce::AudioParameterFloat> fparam (const char* id, const char* name,
                                                       float lo, float hi, float def,
                                                       const char* unit, float step = 0.1f)
    {
        return std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { id, 1 }, name, juce::NormalisableRange<float> (lo, hi, step), def,
            juce::AudioParameterFloatAttributes().withLabel (unit));
    }

    std::unique_ptr<juce::AudioParameterBool> bparam (const char* id, const char* name, bool def)
    {
        return std::make_unique<juce::AudioParameterBool> (juce::ParameterID { id, 1 }, name, def);
    }

    std::unique_ptr<juce::AudioParameterChoice> cparam (const char* id, const char* name,
                                                        juce::StringArray choices, int def)
    {
        return std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { id, 1 }, name, choices, def);
    }
}

// Note lengths offered for the delay, and their length in quarter-note beats.
// "Bar" lengths are in bars of the project's time signature (handled in processBlock).
static const char* const kDivNames[] = { "2 bar", "1 bar", "1/2d", "1/2", "1/2t", "1/4d", "1/4", "1/4t",
                                         "1/8d", "1/8", "1/8t", "1/16d", "1/16", "1/16t" };
static const double kDivBeats[] = { -2.0, -1.0, 3.0, 2.0, 4.0 / 3.0, 1.5, 1.0, 2.0 / 3.0,
                                    0.75, 0.5, 1.0 / 3.0, 0.375, 0.25, 1.0 / 6.0 };   // negative = bars

juce::StringArray BassChainProcessor::divisionNames()
{
    juce::StringArray a;
    for (auto* n : kDivNames) a.add (n);
    return a;
}

//==============================================================================
// External plugin insert
juce::StringArray BassChainProcessor::insertPositionNames()
{
    // By stage name. "After Harmonics" / "After Vintage EQ" follow those stages whichever
    // order HARMONIC/EQ SWAP puts them in. No "after Filters": that is just after the plugin.
    return { "After Compressor", "After Harmonics", "After Vintage EQ", "After Limiter", "After Stereo" };
}

BassChainProcessor::~BassChainProcessor()
{
    cancelPendingUpdate();
    const juce::SpinLock::ScopedLockType sl (insertLock);
    insert.reset();
}

void BassChainProcessor::releaseResources()
{
    const juce::SpinLock::ScopedLockType sl (insertLock);
    if (insert != nullptr) insert->releaseResources();
}

void BassChainProcessor::prepareInsert (juce::AudioPluginInstance& inst)
{
    // Stereo in and out on the main buses; any side-chain buses are switched off.
    inst.disableNonMainBuses();
    auto lay = inst.getBusesLayout();
    if (lay.inputBuses.size()  > 0) lay.inputBuses.getReference (0)  = juce::AudioChannelSet::stereo();
    if (lay.outputBuses.size() > 0) lay.outputBuses.getReference (0) = juce::AudioChannelSet::stereo();
    inst.setBusesLayout (lay);   // keeps its own layout if it refuses stereo
    inst.setRateAndBufferSizeDetails (currentRate, kChunk);
    inst.prepareToPlay (currentRate, kChunk);
}

bool BassChainProcessor::loadInsert (const juce::File& chosen, juce::String& error)
{
    // A Windows VST3 bundle is a folder; if the file inside it was picked, use the bundle.
    auto file = chosen;
    for (auto f = chosen; f.getParentDirectory() != f; f = f.getParentDirectory())
        if (f.getFileName().equalsIgnoreCase ("Contents") && f.getParentDirectory().hasFileExtension ("vst3"))
        { file = f.getParentDirectory(); break; }

    juce::OwnedArray<juce::PluginDescription> types;
    for (auto* fmt : formatManager.getFormats())
        if (fmt->fileMightContainThisPluginType (file.getFullPathName()))
            fmt->findAllTypesForFile (types, file.getFullPathName());

    if (types.isEmpty()) { error = "No VST3 plugin was found in:\n" + file.getFullPathName(); return false; }
    if (types[0]->name.containsIgnoreCase ("fxnkycat bass")) { error = "fxnkycat bass can't be loaded inside itself."; return false; }

    auto inst = formatManager.createPluginInstance (*types[0], currentRate, kChunk, error);
    if (inst == nullptr) { if (error.isEmpty()) error = "The plugin could not be opened."; return false; }
    if (inst->getTotalNumOutputChannels() == 0) { error = types[0]->name + " has no audio output."; return false; }

    prepareInsert (*inst);

    if (onInsertAboutToChange) onInsertAboutToChange();   // the editor closes the old plugin's window
    std::unique_ptr<juce::AudioPluginInstance> old;
    {
        const juce::SpinLock::ScopedLockType sl (insertLock);
        old = std::move (insert);
        insert = std::move (inst);
    }
    if (old != nullptr) old->releaseResources();
    old.reset();   // destroyed here, on the message thread

    insertFile = file;
    updateLatency();   // the DAW compensates the plugin's delay (and the limiter look-ahead)
    if (onInsertChanged) onInsertChanged();
    return true;
}

void BassChainProcessor::removeInsert()
{
    if (insert == nullptr) return;
    if (onInsertAboutToChange) onInsertAboutToChange();
    std::unique_ptr<juce::AudioPluginInstance> old;
    {
        const juce::SpinLock::ScopedLockType sl (insertLock);
        old = std::move (insert);
    }
    old->releaseResources();
    old.reset();
    insertFile = juce::File();
    updateLatency();
    if (onInsertChanged) onInsertChanged();
}

juce::StringArray BassChainProcessor::findInstalledVst3s()
{
    juce::StringArray found;
    for (auto* fmt : formatManager.getFormats())
        found.addArray (fmt->searchPathsForPlugins (fmt->getDefaultLocationsToSearch(), true, false));
    for (int i = found.size(); --i >= 0;)
        if (found[i].containsIgnoreCase ("fxnkycat bass")) found.remove (i);   // not ourselves
    found.sort (true);
    return found;
}

void BassChainProcessor::runInsert (float* const* ch, int nch, int len)
{
    // If the message thread is swapping plugins right now, skip this chunk rather than wait.
    const juce::SpinLock::ScopedTryLockType tl (insertLock);
    if (! tl.isLocked() || insert == nullptr) return;

    const int pluginCh = juce::jlimit (1, insertBuf.getNumChannels(),
                                       juce::jmax (insert->getTotalNumInputChannels(), insert->getTotalNumOutputChannels()));
    for (int c = 0; c < pluginCh; ++c)
    {
        if (c < nch)                     std::copy (ch[c], ch[c] + len, insertBuf.getWritePointer (c));
        else if (c == 1 && nch == 1)     std::copy (ch[0], ch[0] + len, insertBuf.getWritePointer (c));   // mono -> both sides
        else                             insertBuf.clear (c, 0, len);
    }

    juce::AudioBuffer<float> view (insertBuf.getArrayOfWritePointers(), pluginCh, len);
    insertMidi.clear();
    if (insertOn->load() > 0.5f) insert->processBlock (view, insertMidi);
    else                         insert->processBlockBypassed (view, insertMidi);

    const int outCh = juce::jmax (1, insert->getTotalNumOutputChannels());
    for (int c = 0; c < nch; ++c)
    {
        const float* src = view.getReadPointer (juce::jmin (c, outCh - 1));   // mono output -> both sides
        std::copy (src, src + len, ch[c]);
    }
}

void BassChainProcessor::handleAsyncUpdate()
{
    std::unique_ptr<juce::XmlElement> xml;
    {
        const juce::ScopedLock sl (pendingLock);
        xml = std::move (pendingInsert);
    }
    if (xml == nullptr) return;

    const juce::File file (xml->getStringAttribute ("file"));
    if (xml->getStringAttribute ("file").isEmpty()) { removeInsert(); return; }

    juce::String error;
    if (! loadInsert (file, error)) return;   // plugin missing on this computer: leave the slot empty

    juce::MemoryBlock mb;
    if (mb.fromBase64Encoding (xml->getStringAttribute ("state")) && mb.getSize() > 0)
        insert->setStateInformation (mb.getData(), (int) mb.getSize());
    updateLatency();
}

APVTS::ParameterLayout BassChainProcessor::createLayout()
{
    APVTS::ParameterLayout l;

    // Input high-pass (before stage 1). Lowest setting (0 Hz) = Off.
    {
        juce::NormalisableRange<float> r (0.0f, 500.0f, 1.0f);
        r.setSkewForCentre (100.0f);
        l.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { "inHpf", 1 }, "Input High Pass", r, 0.0f,
            juce::AudioParameterFloatAttributes()
                .withLabel ("Hz")
                .withStringFromValueFunction ([] (float v, int) { return v <= kInHpfOffHz ? juce::String ("Off") : juce::String (juce::roundToInt (v)) + " Hz"; })
                .withValueFromStringFunction ([] (const juce::String& s) { return s.trim().equalsIgnoreCase ("off") ? 0.0f : s.getFloatValue(); })));
    }

    // Input low-pass (before stage 1, right of the high-pass). Highest setting (20 kHz) = Off.
    {
        juce::NormalisableRange<float> r (500.0f, 20000.0f, 10.0f);
        r.setSkewForCentre (3000.0f);
        l.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { "inLpf", 1 }, "Input Low Pass", r, 20000.0f,
            juce::AudioParameterFloatAttributes()
                .withLabel ("Hz")
                .withStringFromValueFunction ([] (float v, int)
                {
                    if (v >= kInLpfOffHz) return juce::String ("Off");
                    return v >= 1000.0f ? juce::String (v / 1000.0f, 1) + " kHz" : juce::String (juce::roundToInt (v)) + " Hz";
                })
                .withValueFromStringFunction ([] (const juce::String& s)
                {
                    const auto t = s.trim();
                    if (t.equalsIgnoreCase ("off")) return 20000.0f;
                    return t.containsIgnoreCase ("k") ? t.getFloatValue() * 1000.0f : t.getFloatValue();
                })));
    }

    // Output auto gain: keeps the output about as loud as the input
    l.add (bparam ("autoGain", "Auto Gain", false));
    l.add (bparam ("bypass", "Bypass", false));   // whole plugin; also the DAW's own bypass
    // External plugin insert: on/off and which stage it comes after
    l.add (bparam ("insertOn", "Insert On", false));   // switched on when a plugin is added
    l.add (cparam ("insertPos", "Insert After", insertPositionNames(), 0));   // default: after the Compressor

    // Global timing note value. Tempo itself comes from the project (host).
    l.add (cparam ("division", "Timing", divisionNames(), 6));   // default 1/4

    // 1. Opto compressor (LA-2A style): peak reduction, 3:1 / 4:1, 50 ms attack, tempo release, gain matched
    l.add (bparam ("compOn", "Compressor On", false));
    l.add (fparam ("compPeak", "Peak Reduction", 0.0f, 100.0f, 0.0f, "", 1.0f));
    l.add (cparam ("compRatio", "Compressor Ratio", { "2:1", "3:1", "4:1" }, 1));   // default 3:1

    // 2. Harmonics: sub octave, tape saturation and mid overdrive, processed in parallel and summed
    l.add (bparam ("colorOn", "Harmonics On", false));
    l.add (fparam ("subBass", "Sub Bass", 0.0f, 100.0f, 0.0f, "%", 1.0f));
    l.add (fparam ("tapeAmount", "Tape", 0.0f, 100.0f, 0.0f, "%", 1.0f));
    l.add (fparam ("odMix", "Overdrive", 0.0f, 100.0f, 0.0f, "%", 1.0f));

    // 3. Vintage EQ (Pultec EQP-1A style)
    l.add (bparam ("pultecOn", "Pultec On", false));
    l.add (cparam ("pLowFreq", "Low Frequency", { "20 Hz", "30 Hz", "60 Hz", "100 Hz" }, 2));
    l.add (fparam ("pLowBoost", "Low Boost", 0.0f, 10.0f, 0.0f, "", 0.1f));
    l.add (fparam ("pLowAtten", "Low Atten", 0.0f, 10.0f, 0.0f, "", 0.1f));
    l.add (cparam ("pHighFreq", "High Frequency",
                   { "3 kHz", "4 kHz", "5 kHz", "8 kHz", "10 kHz", "12 kHz", "16 kHz" }, 4));
    l.add (fparam ("pHighBoost", "High Boost", 0.0f, 10.0f, 0.0f, "", 0.1f));
    l.add (fparam ("pBandwidth", "Bandwidth", 0.0f, 10.0f, 5.0f, "", 0.1f));
    l.add (cparam ("pHighAttFreq", "Atten Select", { "5 kHz", "10 kHz", "20 kHz" }, 1));
    l.add (fparam ("pHighAtten", "High Atten", 0.0f, 10.0f, 0.0f, "", 0.1f));

    // Order of stages 2 and 3: false = Harmonics then Vintage EQ (default), true = EQ first
    l.add (bparam ("swapHarm", "EQ Before Harmonics", false));

    // 4. Limiter: threshold knob; release follows the tempo
    l.add (bparam ("limOn", "Limiter On", false));
    l.add (fparam ("limThresh", "Limiter Threshold", -24.0f, 0.0f, 0.0f, "dB"));

    // 5. Stereo: mono and centred below the chosen frequency
    l.add (bparam ("stereoOn", "Stereo On", false));
    l.add (cparam ("monoFreq", "Mono Below", { "200 Hz", "250 Hz", "300 Hz" }, 1));   // default 250 Hz

    // 6. Filters: high / low pass (log-scaled knobs)
    auto freqParam = [] (const char* id, const char* name, float lo, float hi, float centre, float def)
    {
        juce::NormalisableRange<float> r (lo, hi, 1.0f);
        r.setSkewForCentre (centre);
        return std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { id, 1 }, name, r, def,
                                                            juce::AudioParameterFloatAttributes().withLabel ("Hz"));
    };
    l.add (bparam ("filterOn", "Filter On", false));
    l.add (freqParam ("hpFreq", "High Pass", 0.0f, 1000.0f, 100.0f, 0.0f));          // 0 Hz = no high-pass
    l.add (freqParam ("lpFreq", "Low Pass", 500.0f, 20000.0f, 3162.0f, 20000.0f));

    return l;
}

BassChainProcessor::BassChainProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createLayout())
{
    compOn   = p ("compOn");   colorOn = p ("colorOn"); pultecOn = p ("pultecOn");
    limOn    = p ("limOn");    stereoOn = p ("stereoOn"); filterOn = p ("filterOn");

    swapHarm = p ("swapHarm");  inHpf = p ("inHpf"); inLpf = p ("inLpf");
    autoGain = p ("autoGain"); division = p ("division");
    bypassParam = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter ("bypass"));
    insertOn    = p ("insertOn");    insertPos   = p ("insertPos");

    formatManager.addDefaultFormats();   // VST3 (the only hosting format enabled in the build)
    insertBuf.setSize (16, kChunk);

    compPeak    = p ("compPeak");    compRatio   = p ("compRatio");
    tapeAmount  = p ("tapeAmount");  subBass     = p ("subBass");     odMix     = p ("odMix");

    pLowFreq   = p ("pLowFreq");   pLowBoost  = p ("pLowBoost");  pLowAtten    = p ("pLowAtten");
    pHighFreq  = p ("pHighFreq");  pHighBoost = p ("pHighBoost"); pBandwidth   = p ("pBandwidth");
    pHighAttFreq = p ("pHighAttFreq"); pHighAtten = p ("pHighAtten");

    limThresh = p ("limThresh");
    monoFreq  = p ("monoFreq");
    hpFreq    = p ("hpFreq");    lpFreq = p ("lpFreq");
}

bool BassChainProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return out == layouts.getMainInputChannelSet();
}

void BassChainProcessor::updateLatency()
{
    setLatencySamples (limiter.latency() + (insert != nullptr ? insert->getLatencySamples() : 0));
}

void BassChainProcessor::prepareToPlay (double sr, int samplesPerBlock)
{
    comp.prepare (sr);
    inputHpf.prepare (sr);
    inputLpf.prepare (sr);
    currentRate = sr;
    dryBuf.setSize (vc::kMaxCh, juce::jmax (samplesPerBlock, 512));   // bypass crossfade (no allocation later)
    wasBypassed = bypassParam != nullptr && bypassParam->get();
    agIn = agOut = 0.0f; agGainDb = 0.0f;
    tape.prepare (sr);
    subOctave.prepare (sr);
    fundDrive.prepare (sr);
    lastSubAmt = lastOdAmt = 0.0f;
    pultec.prepare (sr);
    limiter.prepare (sr);
    monoBelow.prepare (sr);
    passFilters.prepare (sr);
    {
        const juce::SpinLock::ScopedLockType sl (insertLock);   // re-prepare the hosted plugin at the new rate
        if (insert != nullptr) prepareInsert (*insert);
    }
    updateLatency();
}

void BassChainProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numIn = getTotalNumInputChannels();
    for (int c = numIn; c < getTotalNumOutputChannels(); ++c)
        buffer.clear (c, 0, buffer.getNumSamples());

    const int nch = juce::jmin (numIn, vc::kMaxCh);
    const int n   = buffer.getNumSamples();
    if (nch <= 0 || n <= 0) return;

    // Bypass: pass the input straight through (the analyzer shows it as both before and after).
    // Switching on or off crossfades over one block so it never clicks.
    const bool bypassNow = bypassParam != nullptr && bypassParam->get();
    if (bypassNow && wasBypassed)
    {
        const float* const* rd = buffer.getArrayOfReadPointers();
        preTap.push (rd, nch, n);
        postTap.push (rd, nch, n);
        return;
    }
    const bool bypassFade = bypassNow != wasBypassed;
    if (bypassFade)
    {
        dryBuf.setSize (nch, n, false, false, true);
        for (int c = 0; c < nch; ++c) dryBuf.copyFrom (c, 0, buffer, c, 0, n);
    }

    auto on = [] (std::atomic<float>* a) { return a->load() > 0.5f; };

    // Tempo and time signature from the project; 120 BPM in 4/4 if the host doesn't say.
    double hostTempo = 120.0, beatsPerBar = 4.0;
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
        {
            if (auto b = pos->getBpm(); b.hasValue() && *b > 1.0)
                hostTempo = *b;
            if (auto ts = pos->getTimeSignature(); ts.hasValue() && ts->denominator > 0)
                beatsPerBar = ts->numerator * 4.0 / ts->denominator;
        }
    hostBpm.store ((float) hostTempo);

    const double quarter = 60.0 / hostTempo;   // one 1/4 note in seconds

    // Global Timing note value (default 1/4)
    const int    divIdx   = juce::jlimit (0, (int) std::size (kDivBeats) - 1, (int) division->load());
    const double divBeats = kDivBeats[divIdx] < 0 ? -kDivBeats[divIdx] * beatsPerBar : kDivBeats[divIdx];
    const double unit     = divBeats * quarter;

    comp.setTiming (unit);   // compressor release: fast part 1/2 note, slow part 4 notes
    limiter.setRelease (unit * 0.25);   // limiter release: a quarter of the Timing note (1/16 note at the default 1/4)

    float stageAct[kNumStages] {}, stageAct2[kNumStages] {}, stageAct3[kNumStages] {};

    // Change in dB (relative to the input) -> 0-1, same scale as the static stages.
    auto changeAct = [] (float changeDb) { return juce::jlimit (0.0f, 1.0f, juce::jmap (changeDb, -40.0f, -10.0f, 0.0f, 1.0f)); };

    // Dynamic stages report how many dB they are moving the level; 6 dB = fully lit.
    auto dynAct = [] (float& meter) { const float v = juce::jlimit (0.0f, 1.0f, meter / 6.0f); meter = 0; return v; };

    // Static stages: measure the effect itself (what the stage adds or changes in the
    // sound), with any plain volume change taken out, so the bar shows the effect level.
    auto measureStatic = [&] (float* const* ch, int nch, int len, auto&& fn)
    {
        for (int c = 0; c < nch; ++c)
            std::copy (ch[c], ch[c] + len, stageIn[(size_t) c].begin());
        fn();
        double eIn = 0, eOut = 0, cross = 0;
        for (int c = 0; c < nch; ++c)
            for (int i = 0; i < len; ++i)
            {
                const float x = stageIn[(size_t) c][(size_t) i], y = ch[c][i];
                eIn += x * x; eOut += y * y; cross += x * y;
            }
        return changeAct (vc::effectDb (eIn, eOut, cross, len * nch));
    };

    const int insAfter = juce::jlimit (0, 4, (int) insertPos->load());   // external plugin insert position

    // Process in chunks so per-stage measurement needs no allocation.
    for (int start = 0; start < n; start += kChunk)
    {
        const int len = juce::jmin (kChunk, n - start);
        float* ch[vc::kMaxCh] = { buffer.getWritePointer (0) + start,
                                  nch > 1 ? buffer.getWritePointer (1) + start : nullptr };

        preTap.push (ch, nch, len);   // "before" for the analyzer (raw input)

        auto meanSquare = [&]
        {
            double e = 0;
            for (int c = 0; c < nch; ++c)
                for (int i = 0; i < len; ++i) e += ch[c][i] * ch[c][i];
            return (float) (e / (double) (len * nch));
        };
        const float inMs = meanSquare();   // for auto gain

        // Input high-pass and low-pass, ahead of stage 1
        if (const float hz = inHpf->load(); hz > kInHpfOffHz)
            inputHpf.process (ch, nch, len, hz);
        if (const float hz = inLpf->load(); hz < kInLpfOffHz)
            inputLpf.process (ch, nch, len, hz);

        // External plugin insert: runs right after the stage chosen in "Insert After".
        auto insertAfter = [&] (int pos) { if (insAfter == pos) runInsert (ch, nch, len); };

        // 1. Opto compressor (meter shows gain reduction)
        if (on (compOn))
        {
            comp.process (ch, nch, len, compPeak->load(), 2.0f + (float) juce::jlimit (0, 2, (int) compRatio->load()));
            stageAct[0] = juce::jmax (stageAct[0], dynAct (comp.meter));
        }
        insertAfter (0);

        // 2/3. Harmonics: Sub Bass, Tape and Overdrive each process the stage's input on their own,
        // then are added together:  out = Tape(in) + sub x SubOctave(in) + overdrive x FundDrive(in).
        // Two bars: left = tape (how much it changes the sound), right = sub + overdrive layers added.
        auto runHarmonics = [&]
        {
            if (! on (colorOn)) return;

            // Each layer gets its own copy of the input.
            for (int c = 0; c < nch; ++c)
            {
                std::copy (ch[c], ch[c] + len, subBuf[(size_t) c].begin());
                std::copy (ch[c], ch[c] + len, odBuf[(size_t) c].begin());
            }
            float* sub[vc::kMaxCh] = { subBuf[0].data(), subBuf[1].data() };
            float* od[vc::kMaxCh]  = { odBuf[0].data(),  odBuf[1].data() };
            // Both layers' filters follow the Stereo stage's Mono Below frequency (even when that
            // stage is off): the sub's low-pass and the saturation's high-pass. The saturation's
            // low-pass follows its own knob.
            const float monoHz = vc::MonoBelow::kFreqs[juce::jlimit (0, 2, (int) monoFreq->load())];
            subOctave.process (sub, nch, len, monoHz);   // -> sub octave layer only
            fundDrive.process (od, nch, len, odMix->load() * 0.01f, monoHz);   // -> distorted fundamental only

            // Main path: tape (at 0 this is the untouched input). Middle meter.
            stageAct3[1] = juce::jmax (stageAct3[1],
                measureStatic (ch, nch, len, [&] { tape.process (ch, nch, len, tapeAmount->load(), false); }));

            // Sum, with the knobs glided across the chunk so turning them doesn't click
            const float s1 = juce::jlimit (0.0f, 1.0f, subBass->load() * 0.01f);
            const float o1 = juce::jlimit (0.0f, 1.0f, odMix->load() * 0.01f);
            const float s0 = lastSubAmt, o0 = lastOdAmt;
            lastSubAmt = s1; lastOdAmt = o1;
            double eIn = 0, eSub = 0, eOd = 0;
            for (int c = 0; c < nch; ++c)
                for (int i = 0; i < len; ++i)
                {
                    const float t = (float) (i + 1) / (float) len;
                    const float subAdd = (s0 + (s1 - s0) * t) * sub[c][i];
                    const float odAdd  = (o0 + (o1 - o0) * t) * od[c][i];
                    eIn  += (double) stageIn[(size_t) c][(size_t) i] * stageIn[(size_t) c][(size_t) i];
                    eSub += (double) subAdd * subAdd;
                    eOd  += (double) odAdd * odAdd;
                    ch[c][i] += subAdd + odAdd;
                }
            // How loud each added layer is, relative to the input
            auto layerDb = [&] (double e) { return eIn / (double) (len * nch) < 1.0e-7 ? -200.0f
                                                   : 10.0f * std::log10 ((float) (e / eIn) + 1.0e-12f); };
            stageAct2[1] = juce::jmax (stageAct2[1], changeAct (layerDb (eSub)));   // left meter: sub bass
            stageAct[1]  = juce::jmax (stageAct[1],  changeAct (layerDb (eOd)));    // right meter: overdrive
        };

        // 2/3. Vintage EQ
        auto runPultec = [&]
        {
            if (! on (pultecOn)) return;
            pultec.update ((int) pLowFreq->load(), pLowBoost->load(), pLowAtten->load(),
                           (int) pHighFreq->load(), pHighBoost->load(), pBandwidth->load(),
                           (int) pHighAttFreq->load(), pHighAtten->load());
            pultec.process (ch, nch, len);
            // Two bars: left = low boost, right = high boost
            float lowDb, highDb;
            pultec.takeMeters (lowDb, highDb, len * nch);
            stageAct2[2] = juce::jmax (stageAct2[2], changeAct (lowDb));
            stageAct[2]  = juce::jmax (stageAct[2],  changeAct (highDb));
        };

        // Order: Harmonics then Vintage EQ (default), or the EQ first when swapped
        if (on (swapHarm)) { runPultec();    insertAfter (2); runHarmonics(); insertAfter (1); }
        else               { runHarmonics(); insertAfter (1); runPultec();    insertAfter (2); }

        // 4. Limiter (always runs so its look-ahead delay is constant; only limits when on).
        // Meter shows gain reduction.
        limiter.process (ch, nch, len, limThresh->load(), on (limOn));
        stageAct[3] = juce::jmax (stageAct[3], dynAct (limiter.meter));
        insertAfter (3);

        // 5. Stereo: mono and centred below 200 / 250 / 300 Hz
        if (on (stereoOn))
        {
            monoBelow.process (ch, nch, len, (int) monoFreq->load());
            stageAct[4] = juce::jmax (stageAct[4], changeAct (monoBelow.takeMeter (len * nch)));
        }
        insertAfter (4);

        // 6. Filters: high / low pass
        if (on (filterOn))
            stageAct[5] = juce::jmax (stageAct[5],
                measureStatic (ch, nch, len, [&] { passFilters.process (ch, nch, len, hpFreq->load(), lpFreq->load()); }));

        // Auto gain: follow input and output loudness (~1 s average) and trim the
        // output so it matches the input, within +/-18 dB. Holds during silence.
        {
            const float outMs = meanSquare();
            const float c = (float) std::exp (-(double) len / (1.0 * currentRate));
            agIn  = c * agIn  + (1.0f - c) * inMs;
            agOut = c * agOut + (1.0f - c) * outMs;

            float target = agGainDb;
            if (! on (autoGain))
                target = 0.0f;
            else if (agIn > 1.0e-7f && agOut > 1.0e-9f)   // ignore near-silence
                target = juce::jlimit (-18.0f, 18.0f, 10.0f * std::log10 (agIn / agOut));

            const float gc = (float) std::exp (-(double) len / (0.3 * currentRate));
            const float newDb = gc * agGainDb + (1.0f - gc) * target;
            const float g0 = juce::Decibels::decibelsToGain (agGainDb), g1 = juce::Decibels::decibelsToGain (newDb);
            agGainDb = newDb;
            if (std::abs (g1 - 1.0f) > 1.0e-5f || std::abs (g0 - 1.0f) > 1.0e-5f)
                for (int cc = 0; cc < nch; ++cc)
                    for (int i = 0; i < len; ++i)
                        ch[cc][i] *= g0 + (g1 - g0) * (float) (i + 1) / (float) len;
        }

        postTap.push (ch, nch, len);  // "after" for the analyzer
    }

    // Bypass just switched: crossfade processed <-> dry across this block.
    if (bypassFade)
    {
        for (int c = 0; c < nch; ++c)
        {
            float* out = buffer.getWritePointer (c);
            const float* dry = dryBuf.getReadPointer (c);
            for (int i = 0; i < n; ++i)
            {
                const float t = (float) (i + 1) / (float) n;
                const float wDry = bypassNow ? t : 1.0f - t;
                out[i] = out[i] * (1.0f - wDry) + dry[i] * wDry;
            }
        }
    }
    wasBypassed = bypassNow;

    // Publish to the display: keep the peak until the editor reads it.
    for (int s = 0; s < kNumStages; ++s)
    {
        float prev = activity[s].load();
        while (stageAct[s] > prev && ! activity[s].compare_exchange_weak (prev, stageAct[s])) {}
        float prev2 = activity2[s].load();
        while (stageAct2[s] > prev2 && ! activity2[s].compare_exchange_weak (prev2, stageAct2[s])) {}
        float prev3 = activity3[s].load();
        while (stageAct3[s] > prev3 && ! activity3[s].compare_exchange_weak (prev3, stageAct3[s])) {}
    }
}

//==============================================================================
// Presets
juce::File BassChainProcessor::presetFolder()
{
    auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
   #if JUCE_MAC
    dir = dir.getChildFile ("Application Support");
   #endif
    return dir.getChildFile ("fxnkycat bass").getChildFile ("Presets");
}

juce::File BassChainProcessor::presetFile (int slot)
{
    return presetFolder().getChildFile ("Preset " + juce::String (slot + 1).paddedLeft ('0', 2) + ".xml");
}

juce::String BassChainProcessor::presetName (int slot) const
{
    const auto f = presetFile (slot);
    if (! f.existsAsFile()) return {};
    if (auto xml = juce::XmlDocument::parse (f))
        return xml->getStringAttribute ("presetName", "Preset " + juce::String (slot + 1));
    return {};
}

bool BassChainProcessor::savePreset (int slot, const juce::String& name)
{
    if (slot < 0 || slot >= kNumPresets) return false;
    auto xml = apvts.copyState().createXml();
    if (xml == nullptr) return false;
    xml->setAttribute ("presetName", name.trim().isEmpty() ? "Preset " + juce::String (slot + 1) : name.trim());
    presetFolder().createDirectory();
    return xml->writeTo (presetFile (slot));
}

bool BassChainProcessor::loadPreset (int slot)
{
    if (slot < 0 || slot >= kNumPresets) return false;
    auto xml = juce::XmlDocument::parse (presetFile (slot));
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType())) return false;
    xml->removeAttribute ("presetName");

    // Presets never change the bypass button: keep whatever it is set to now.
    const bool keepBypass = bypassParam != nullptr && bypassParam->get();
    apvts.replaceState (juce::ValueTree::fromXml (*xml));
    if (bypassParam != nullptr && bypassParam->get() != keepBypass)
        bypassParam->setValueNotifyingHost (keepBypass ? 1.0f : 0.0f);
    return true;
}

void BassChainProcessor::resetToDefaults()
{
    for (auto* prm : getParameters())
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (prm))
            if (rp != bypassParam)   // presets never touch bypass
            {
                rp->beginChangeGesture();
                rp->setValueNotifyingHost (rp->getDefaultValue());
                rp->endChangeGesture();
            }
}

juce::AudioProcessorEditor* BassChainProcessor::createEditor()
{
    return new BassChainEditor (*this);
}

void BassChainProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
    {
        // The external plugin insert is saved with the session: its file and its own settings.
        // (Not locked: the plugin is only ever swapped on the message thread, and locking here
        // would make the audio thread skip the insert while the session saves.)
        if (insert != nullptr)
        {
            auto* ins = xml->createNewChildElement ("INSERT");
            ins->setAttribute ("file", insertFile.getFullPathName());
            juce::MemoryBlock mb;
            insert->getStateInformation (mb);
            ins->setAttribute ("state", mb.toBase64Encoding());
        }
        copyXmlToBinary (*xml, destData);
    }
}

void BassChainProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
        {
            // Take the insert out first (it isn't a parameter), then restore it on the message thread.
            std::unique_ptr<juce::XmlElement> ins;
            if (auto* child = xml->getChildByName ("INSERT"))
            {
                ins = std::make_unique<juce::XmlElement> (*child);
                xml->removeChildElement (child, true);
            }
            else
                ins = std::make_unique<juce::XmlElement> ("INSERT");   // no insert in this session: empty the slot

            apvts.replaceState (juce::ValueTree::fromXml (*xml));
            {
                const juce::ScopedLock sl (pendingLock);
                pendingInsert = std::move (ins);
            }
            triggerAsyncUpdate();
        }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new BassChainProcessor();
}
