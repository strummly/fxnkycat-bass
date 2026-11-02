#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <atomic>
#include <iterator>
#include "DSP.h"

// Lock-free hand-off of mono audio from the audio thread to the analyzer display.
struct AnalyzerTap
{
    static constexpr int kSize = 32768;

    juce::AbstractFifo fifo { kSize };
    std::vector<float> buf = std::vector<float> ((size_t) kSize, 0.0f);

    void push (const float* const* ch, int nch, int n)
    {
        int i = 0;
        auto w = fifo.write (n);   // drops samples if the display isn't keeping up
        w.forEach ([&] (int idx)
        {
            float s = 0;
            for (int c = 0; c < nch; ++c) s += ch[c][i];
            buf[(size_t) idx] = s / (float) nch;
            ++i;
        });
    }

    int pull (float* dest, int maxN)
    {
        int i = 0;
        auto r = fifo.read (juce::jmin (maxN, fifo.getNumReady()));
        r.forEach ([&] (int idx) { dest[i++] = buf[(size_t) idx]; });
        return i;
    }
};

class BassChainProcessor : public juce::AudioProcessor, private juce::AsyncUpdater
{
public:
    AnalyzerTap preTap, postTap;   // input and output of the whole chain

    // ---- External plugin insert (one VST3, after the stage chosen with "insertPos") ----
    // All of these are for the message thread only.
    bool loadInsert (const juce::File& file, juce::String& error);   // replaces any loaded plugin
    void removeInsert();
    juce::AudioPluginInstance* getInsert() const { return insert.get(); }
    juce::String insertName() const { return insert != nullptr ? insert->getName() : juce::String(); }
    juce::StringArray findInstalledVst3s();                          // paths of VST3s in the standard folders
    std::function<void()> onInsertAboutToChange, onInsertChanged;    // set by the editor (close / refresh its window)
    static juce::StringArray insertPositionNames();

    // How hard each stage is working right now, 0-1 (peak since the editor last read it).
    // Order: compressor, harmonics, vintage EQ, limiter, stereo, filters.
    static constexpr int kNumStages = 6;
    std::atomic<float> activity[kNumStages] {};
    // Second (left-hand) meter for stages with split meters, 0-1:
    // vintage EQ = low boost, harmonics = sub bass (and activity3 = tape, activity = overdrive).
    std::atomic<float> activity2[kNumStages] {};
    std::atomic<float> activity3[kNumStages] {};   // third (middle) meter: harmonics = tape

    BassChainProcessor();
    ~BassChainProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "fxnkycat bass"; }
    bool acceptsMidi() const override  { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    // The top-bar BYPASS button is also the DAW's own bypass for this plugin.
    juce::AudioProcessorParameter* getBypassParameter() const override { return bypassParam; }

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    static juce::StringArray divisionNames();

    // User presets: 9 slots saved as XML files shared by every project/DAW.
    static constexpr int kNumPresets = 9;
    static juce::File presetFolder();
    static juce::File presetFile (int slot);          // slot 0-8
    juce::String presetName (int slot) const;         // empty if the slot is unused
    bool savePreset (int slot, const juce::String& name);
    bool loadPreset (int slot);
    void resetToDefaults();                           // "-init-": every setting back to default (bypass untouched)

    std::atomic<float> hostBpm { 120.0f };   // project tempo, for display
    static constexpr float kInHpfOffHz = 0.5f;      // input high-pass at/below this (0 Hz) = Off
    static constexpr float kInLpfOffHz = 19990.0f;  // input low-pass at/above this = Off
    juce::AudioProcessorValueTreeState apvts;

private:
    std::atomic<float>* p (const char* id) { return apvts.getRawParameterValue (id); }

    // External plugin insert
    void handleAsyncUpdate() override;                        // restores the insert saved with a session
    void prepareInsert (juce::AudioPluginInstance&);
    void runInsert (float* const* ch, int nch, int len);     // audio thread
    juce::AudioPluginFormatManager formatManager;
    std::unique_ptr<juce::AudioPluginInstance> insert;
    juce::SpinLock insertLock;                               // guards 'insert' between the threads
    juce::AudioBuffer<float> insertBuf;                      // scratch for the hosted plugin (16 ch x kChunk)
    juce::MidiBuffer insertMidi;
    juce::File insertFile;
    std::unique_ptr<juce::XmlElement> pendingInsert;         // from setStateInformation, loaded on the message thread
    juce::CriticalSection pendingLock;
    std::atomic<float> *insertOn = nullptr, *insertPos = nullptr;

    // Stage on/off
    std::atomic<float> *compOn, *colorOn, *pultecOn, *limOn, *stereoOn, *filterOn;
    // Top bar
    std::atomic<float> *swapHarm, *inHpf, *inLpf, *autoGain, *division;
    // 1 Opto compressor
    std::atomic<float> *compPeak, *compRatio;
    // 2 Harmonics: sub bass, tape, overdrive (parallel)
    std::atomic<float> *subBass, *tapeAmount, *odMix;
    // 3 Vintage EQ (Pultec)
    std::atomic<float> *pLowFreq, *pLowBoost, *pLowAtten, *pHighFreq, *pHighBoost,
                       *pBandwidth, *pHighAttFreq, *pHighAtten;
    // 4 Limiter
    std::atomic<float> *limThresh;
    // 5 Stereo
    std::atomic<float> *monoFreq;
    // 6 Filters
    std::atomic<float> *hpFreq, *lpFreq;

    // Bypass
    juce::AudioParameterBool* bypassParam = nullptr;
    bool wasBypassed = false;
    juce::AudioBuffer<float> dryBuf;   // copy of the input for the bypass crossfade

    // Auto gain state (audio thread only)
    double currentRate = 44100.0;
    float  agIn = 0.0f, agOut = 0.0f, agGainDb = 0.0f;

    static constexpr int kChunk = 512;
    std::array<std::array<float, kChunk>, vc::kMaxCh> stageIn {}, subBuf {}, odBuf {};   // scratch (no allocation)
    float lastSubAmt = 0.0f, lastOdAmt = 0.0f;   // Sub Bass / Overdrive knobs last chunk (for gliding)

    vc::OptoComp       comp;
    vc::InputHighPass  inputHpf;
    vc::InputLowPass   inputLpf;
    vc::TapeSaturation tape;
    vc::SubOctave      subOctave;
    vc::FundDrive      fundDrive;
    vc::PultecEQ       pultec;
    vc::Limiter        limiter;
    vc::MonoBelow      monoBelow;
    vc::PassFilters    passFilters;

    void updateLatency();   // limiter look-ahead + the hosted plugin's own delay

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BassChainProcessor)
};
