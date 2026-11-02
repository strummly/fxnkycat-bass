#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>
#include "PluginProcessor.h"

namespace ui
{
    // Spectrum of the plugin's input (before) and output (after), overlaid.
    class SpectrumAnalyzer : public juce::Component, private juce::Timer
    {
    public:
        explicit SpectrumAnalyzer (BassChainProcessor& p);
        ~SpectrumAnalyzer() override { stopTimer(); }
        void paint (juce::Graphics&) override;
        void setWatermark (juce::Image img, juce::String name)
        {
            watermark = std::move (img); watermarkText = std::move (name); repaint();
        }

    private:
        juce::Image  watermark;       // faint logo behind the traces
        juce::String watermarkText;   // plugin name, drawn beside the logo
        static constexpr int kOrder = 12, kSize = 1 << kOrder, kBins = kSize / 2;
        static constexpr float kMinDb = -96.0f, kMaxDb = 0.0f, kMinHz = 20.0f, kMaxHz = 20000.0f;

        struct Trace
        {
            AnalyzerTap* tap = nullptr;
            std::vector<float> history = std::vector<float> ((size_t) kSize, 0.0f);
            int pos = 0;
            std::vector<float> levelDb = std::vector<float> ((size_t) kBins, kMinDb);
        };

        void timerCallback() override;
        void analyse (Trace&);
        juce::Path makePath (const Trace&, juce::Rectangle<float> area, bool closed) const;
        float xForHz (float hz, juce::Rectangle<float> area) const;
        float yForDb (float db, juce::Rectangle<float> area) const;

        BassChainProcessor& proc;
        juce::dsp::FFT fft { kOrder };
        juce::dsp::WindowingFunction<float> window { (size_t) kSize, juce::dsp::WindowingFunction<float>::hann, false };
        std::vector<float> fftData = std::vector<float> ((size_t) kSize * 2, 0.0f);
        std::vector<float> scratch = std::vector<float> ((size_t) AnalyzerTap::kSize, 0.0f);
        Trace pre, post;
    };

    // Knob drawing. A slider with the "fillRight" property set draws its gold arc from
    // the pointer to the right-hand end instead of from the left-hand start (thresholds).
    class KnobLookAndFeel : public juce::LookAndFeel_V4
    {
    public:
        void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos,
                               float startAngle, float endAngle, juce::Slider&) override;
    };

    // A stage's title that doubles as its on/off switch: gold when on, dim when off.
    class TitleToggle : public juce::Button
    {
    public:
        TitleToggle();
        void paintButton (juce::Graphics&, bool highlighted, bool down) override;
        static juce::Font font();
    };

    // Small down-arrow button (opens a menu).
    class ArrowButton : public juce::Button
    {
    public:
        ArrowButton() : juce::Button ({}) { setMouseCursor (juce::MouseCursor::PointingHandCursor); }
        void paintButton (juce::Graphics&, bool highlighted, bool down) override;
    };

    // A round light you click to toggle: dark when off, glowing when on.
    class LedButton : public juce::Button
    {
    public:
        LedButton();
        void paintButton (juce::Graphics&, bool highlighted, bool down) override;
    };

    // A two-state switch that shows different text for off / on (e.g. MALE / FEMALE).
    class SwitchButton : public juce::Button
    {
    public:
        SwitchButton (juce::String offText, juce::String onText);
        void paintButton (juce::Graphics&, bool highlighted, bool down) override;
        void setOnLeft (bool l) { onLeft = l; repaint(); }   // draw the "on" option on the left
        // Text-only look: no box, selected word gold, the other grey. Click a word to pick it.
        void setTextOnly (bool t) { textOnly = t; setMouseCursor (juce::MouseCursor::PointingHandCursor); repaint(); }
        void mouseDown (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
    private:
        bool onSelectedSide (const juce::MouseEvent&) const;
        juce::String off, on;
        bool onLeft = false, textOnly = false, ignoreClick = false;
    };

    // Any control with a caption above it.
    class LabeledControl : public juce::Component
    {
    public:
        LabeledControl (const juce::String& caption, std::unique_ptr<juce::Component> control, bool compact,
                        int compactMaxWidth = 130);
        void resized() override;
        juce::Component& get() { return *control; }
        void setControlHeight (int h) { controlHeight = h; resized(); }   // compact only
    private:
        juce::Label label;
        std::unique_ptr<juce::Component> control;
        bool compact;
        int  maxWidth;
        int  controlHeight = 28;
    };

    // Multi-position switch (e.g. 1/8 | 1/4 | 1/2) bound to a choice parameter.
    class SegmentedSwitch : public juce::Component
    {
    public:
        explicit SegmentedSwitch (juce::AudioParameterChoice& param);
        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void setVertical (bool v) { vertical = v; repaint(); }   // options stacked top to bottom
        void setReversed (bool r) { reversed = r; repaint(); }   // show the last option first
        // Text-only look: no box, selected word gold, the others grey (like the stage names).
        void setTextOnly (bool t) { textOnly = t; setMouseCursor (juce::MouseCursor::PointingHandCursor); repaint(); }
        void setUpperCase() { for (auto& o : options) o = o.toUpperCase(); repaint(); }   // show the options in capitals
        // 0-1: lights the selected option's button from the left, like the knob meters.
        void enableMeter() { meterEnabled = true; repaint(); }
        void setMeter (float m) { if (std::abs (m - meter) > 0.004f || (m == 0.0f && meter != 0.0f)) { meter = m; repaint(); } }
        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;
    private:
        int slotFor (int option) const { return reversed ? options.size() - 1 - option : option; }
        juce::StringArray options;
        int selected = 0, hoverSlot = -1;
        bool vertical = false, reversed = false, textOnly = false;
        float meter = 0.0f;
        bool  meterEnabled = false;
        juce::ParameterAttachment attachment;
    };

    // A titled box for one stage of the chain, with its own on/off switch.
    class ModulePanel : public juce::Component
    {
    public:
        ModulePanel (juce::String number, juce::String title, juce::String subtitle, int rows);
        void add (std::unique_ptr<LabeledControl> c);
        void paint (juce::Graphics&) override;
        void resized() override;
        // Clicking the LED turns the stage on/off, same as clicking the title.
        // Clicking, dragging or scrolling any control in the stage turns it on if it is off.
        void mouseDown (const juce::MouseEvent&) override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
        void setActivity (float v);   // 0-1, drives the LED and meter bar
        // Split the meter into two bars side by side: left = v2 (setActivity2), right = setActivity.
        // ledBoth: the LED follows whichever bar is higher (otherwise only the right one).
        void setSplitMeter (bool s, bool ledBoth = false) { splitMeter = s; ledFollowsBoth = ledBoth; repaint(); }
        // Single meter bar = setActivity, LED = setActivity2 (e.g. stage 2: bar = de-esser, LED = gate).
        void setLedFromSecond (bool b) { ledFromSecond = b; repaint(); }
        void setActivity2 (float v);
        // Three bars side by side: left = setActivity2, middle = setActivity3, right = setActivity.
        void setTripleMeter (bool t) { tripleMeter = t; repaint(); }
        // Hide the meter bars under the title (for stages whose knobs show their own meters).
        void setShowMeterBars (bool b) { showBars = b; repaint(); }
        void setActivity3 (float v);
        void setNumber (const juce::String& n) { if (n != number) { number = n; repaint(); } }
        // Switches stacked in a column on the left, the last control (a knob) on the right.
        void setSideStack (bool s) { sideStack = s; resized(); }
        // The first control gets its own full-width strip on top; the rest go in a row below.
        void setTopWide (bool t) { topWide = t; resized(); }
        // Single row with relative column widths (e.g. { 4, 1, 4 } for a narrow middle column).
        void setColumnWeights (std::vector<int> w) { colWeights = std::move (w); resized(); }
        TitleToggle enable;   // the stage title; click to turn the stage on/off
    private:
        bool sideStack = false, topWide = false;
        std::vector<int> colWeights;
        juce::Rectangle<float> ledArea() const;
        void turnOnFromControl (const juce::MouseEvent&);
        juce::Rectangle<float> meterArea() const;
        float activity = 0.0f, activity2 = 0.0f, activity3 = 0.0f;
        bool  tripleMeter = false, showBars = true;
        bool  splitMeter = false, ledFollowsBoth = false, ledFromSecond = false;
        juce::String number, title, subtitle;
        int rows;
        std::vector<std::unique_ptr<LabeledControl>> controls;
    };
}

class BassChainEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit BassChainEditor (BassChainProcessor&);
    ~BassChainEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    using APVTS = juce::AudioProcessorValueTreeState;

    std::unique_ptr<ui::LabeledControl> knob (const char* id, const juce::String& caption);
    // A knob that shows one of the processor's knob meters in its coloured arc.
    std::unique_ptr<ui::LabeledControl> meteredKnob (const char* id, const juce::String& caption, int meterIndex);
    juce::Slider* meterKnobs[BassChainProcessor::kNumKnobMeters] {};
    float shownKnobMeter[BassChainProcessor::kNumKnobMeters] {};
    ui::SegmentedSwitch* stereoSwitch = nullptr;   // Mono Below buttons (show the stereo meter)
    std::unique_ptr<ui::LabeledControl> toggle (const char* id, const juce::String& caption,
                                                const juce::String& offText, const juce::String& onText);
    std::unique_ptr<ui::LabeledControl> choice (const char* id, const juce::String& caption);
    void bindEnable (ui::ModulePanel& panel, const char* id);
    void timerCallback() override;
    float shownActivity[BassChainProcessor::kNumStages] {};

    BassChainProcessor& proc;
    ui::KnobLookAndFeel lnf;
    float shownActivity2[BassChainProcessor::kNumStages] {};   // left-hand bars of split meters
    float shownActivity3[BassChainProcessor::kNumStages] {};   // middle bars of triple meters

    // Panels first so they are destroyed after the attachments below.
    ui::ModulePanel comp, harmonics, pultec, limiter, stereo, filters;
    juce::ComboBox timingBox;   // global Timing note value (scroll or click)
    juce::Label    bpmLabel;    // shows the project tempo
    // HARMONIC/EQ SWAP light: off = Harmonics (2) then Vintage EQ (3), the default; lit = EQ first.
    // Parameter "swapHarm" = EQ first, so the light simply shows it.
    ui::LedButton swapLed;
    std::unique_ptr<juce::ParameterAttachment> swapAtt;
    std::unique_ptr<juce::ParameterAttachment> peakAtt;   // Peak Reduction knob (shown inverted, right to left)
    bool swapped = false;
    void applySwap (bool s);
    juce::Slider inHpfSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };   // input high-pass
    juce::Slider inLpfSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };   // input low-pass

    ui::LedButton autoGainButton;                                  // output auto gain (light glows when on)
    ui::LedButton bypassButton;                                     // whole-plugin bypass (light glows orange when on)

    // Presets: 9 user slots
    juce::ComboBox   presetBox;
    juce::TextButton saveButton { "SAVE" };
    ui::ArrowButton  colourButton;   // opens the accent colour menu
    int  lastAccentVersion = -1;
    void applyAccentColours();      // push the current accent into every colour that uses it
    void showColourMenu();
    static constexpr int kInitPresetId = 1000;   // "-init-" item in the preset menu
    void refreshPresetList (int selectSlot = -1);
    void presetChosen();
    void showSaveDialog();

    // Top bar areas, set in resized() and drawn in paint()
    juce::Rectangle<int> capBypass, capHpf, capLpf, capTiming, capPreset, capAutoGain, capSwap, hdrDivider;

    ui::SpectrumAnalyzer analyzer;

    // External plugin insert strip (top-left of the analyzer) and the hosted plugin's window
    juce::Label      insertCaption;
    ui::LedButton    insertLed;                     // insert on/off
    juce::TextButton insertLoad { "LOAD VST3..." }; // shows the loaded plugin's name
    juce::TextButton insertOpen { "OPEN" }, insertRemove { "REMOVE" };
    juce::ComboBox   insertPosBox;                  // which stage it comes after
    std::unique_ptr<juce::DocumentWindow> insertWindow;
    std::unique_ptr<juce::FileChooser>    insertChooser;
    juce::Component::SafePointer<juce::CallOutBox> insertBrowserBox;   // the plugin browser while it is open
    juce::Component::SafePointer<juce::Component>  insertBrowser;      // its contents (to update makers)
    std::unique_ptr<juce::Thread> makerThread;                         // reads plugin makers in the background
    void refreshInsert();
    void showInsertMenu();
    void loadInsertFile (const juce::File&);
    void openInsertWindow();
    void closeInsertWindow();

    std::vector<std::unique_ptr<APVTS::SliderAttachment>>   sliderAtts;
    std::vector<std::unique_ptr<APVTS::ButtonAttachment>>   buttonAtts;
    std::vector<std::unique_ptr<APVTS::ComboBoxAttachment>> comboAtts;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BassChainEditor)
};
