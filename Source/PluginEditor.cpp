#include "PluginEditor.h"
#include "BinaryData.h"
#include "PluginInfo.h"
#include <map>

namespace
{
    const juce::Colour bg       { 0xff15171c };
    const juce::Colour panelBg  { 0xff20232b };
    const juce::Colour edge     { 0xff343844 };
    const juce::Colour textCol  { 0xffe8e6e1 };
    const juce::Colour dimText  { 0xff8f939e };

    // Accent colour (menu next to SAVE). One choice shared by every open copy of the
    // plugin, saved next to the presets so it is remembered.
    struct AccentChoice { const char* name; juce::uint32 argb; };
    const AccentChoice kAccents[] = {
        { "Gold",   0xffe0a64a },
        { "Red",    0xffe0544a },
        { "Blue",   0xff4a9ee0 },
        { "Green",  0xff5cc46a },
        { "Purple", 0xffa77be0 },
    };
    constexpr int kNumAccents = (int) (sizeof (kAccents) / sizeof (kAccents[0]));

    // Dropdown items are shown in capitals.
    juce::StringArray allCaps (juce::StringArray items)
    {
        for (auto& s : items) s = s.toUpperCase();
        return items;
    }

    juce::Colour accent { kAccents[0].argb };
    int accentIndex = 0;
    int accentVersion = 0;   // bumps on every change, so open windows know to repaint

    juce::File settingsFile()
    {
        return BassChainProcessor::presetFolder().getParentDirectory().getChildFile ("settings.xml");
    }

    void setAccent (int index, bool save)
    {
        accentIndex = juce::jlimit (0, kNumAccents - 1, index);
        accent = juce::Colour (kAccents[accentIndex].argb);
        ++accentVersion;
        if (save)
        {
            juce::XmlElement xml ("FxnkycatBassSettings");
            xml.setAttribute ("accent", kAccents[accentIndex].name);
            settingsFile().getParentDirectory().createDirectory();
            xml.writeTo (settingsFile());
        }
    }

    void loadAccent()
    {
        static bool loaded = false;   // once per session; later windows share the current choice
        if (loaded) return;
        loaded = true;
        if (auto xml = juce::XmlDocument::parse (settingsFile()))
        {
            const auto name = xml->getStringAttribute ("accent");
            for (int i = 0; i < kNumAccents; ++i)
                if (name.equalsIgnoreCase (kAccents[i].name)) { setAccent (i, false); return; }
        }
    }
}

//==============================================================================
namespace ui
{
SwitchButton::SwitchButton (juce::String offText, juce::String onText)
    : juce::Button (offText), off (std::move (offText)), on (std::move (onText))
{
    setClickingTogglesState (true);
}

bool SwitchButton::onSelectedSide (const juce::MouseEvent& e) const
{
    const bool leftHalf = e.x < getWidth() / 2;
    const bool onSide   = onLeft ? leftHalf : ! leftHalf;   // the half that means "on"
    return onSide == getToggleState();
}

void SwitchButton::mouseDown (const juce::MouseEvent& e)
{
    // Text-only: clicking the word that is already selected does nothing.
    ignoreClick = textOnly && onSelectedSide (e);
    if (! ignoreClick) juce::Button::mouseDown (e);
}

void SwitchButton::mouseUp (const juce::MouseEvent& e)
{
    if (! ignoreClick) juce::Button::mouseUp (e);
    ignoreClick = false;
}

void SwitchButton::paintButton (juce::Graphics& g, bool highlighted, bool)
{
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    const bool state = getToggleState();

    if (textOnly)
    {
        // Just the two words: gold = selected, grey = not (like the stage names).
        auto left = r.removeFromLeft (r.getWidth() * 0.5f);
        const auto offArea = onLeft ? r : left, onArea = onLeft ? left : r;
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        auto col = [&] (bool sel) { return sel ? accent : (highlighted ? dimText.brighter (0.3f) : dimText); };
        g.setColour (col (! state));
        g.drawFittedText (off, offArea.toNearestInt(), juce::Justification::centred, 1);
        g.setColour (col (state));
        g.drawFittedText (on, onArea.toNearestInt(), juce::Justification::centred, 1);
        return;
    }

    g.setColour (highlighted ? edge.brighter (0.2f) : edge);
    g.fillRoundedRectangle (r, 5.0f);

    // Two halves; the active one is lit.
    auto left = r.removeFromLeft (r.getWidth() * 0.5f);
    auto right = r;
    const auto offArea = onLeft ? right : left, onArea = onLeft ? left : right;
    g.setColour (accent);
    g.fillRoundedRectangle ((state ? onArea : offArea).reduced (2.0f), 4.0f);

    g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    g.setColour (state ? dimText : bg);
    g.drawFittedText (off, offArea.toNearestInt(), juce::Justification::centred, 1);
    g.setColour (state ? bg : dimText);
    g.drawFittedText (on, onArea.toNearestInt(), juce::Justification::centred, 1);
}

//------------------------------------------------------------------------------
void KnobLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                                        float startAngle, float endAngle, juce::Slider& s)
{
    if (! (bool) s.getProperties().getWithDefault ("fillRight", false))
    {
        LookAndFeel_V4::drawRotarySlider (g, x, y, w, h, pos, startAngle, endAngle, s);
        return;
    }

    // Same look as LookAndFeel_V4, but the value arc runs from the pointer to the end.
    const auto bounds = juce::Rectangle<int> (x, y, w, h).toFloat().reduced (10.0f);
    const float radius  = juce::jmin (bounds.getWidth(), bounds.getHeight()) / 2.0f;
    const float toAngle = startAngle + pos * (endAngle - startAngle);
    const float lineW   = juce::jmin (8.0f, radius * 0.5f);
    const float arcR    = radius - lineW * 0.5f;
    const juce::PathStrokeType stroke (lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);

    juce::Path back;
    back.addCentredArc (bounds.getCentreX(), bounds.getCentreY(), arcR, arcR, 0.0f, startAngle, endAngle, true);
    g.setColour (s.findColour (juce::Slider::rotarySliderOutlineColourId));
    g.strokePath (back, stroke);

    if (s.isEnabled() && pos < 1.0f)
    {
        juce::Path value;
        value.addCentredArc (bounds.getCentreX(), bounds.getCentreY(), arcR, arcR, 0.0f, toAngle, endAngle, true);
        g.setColour (s.findColour (juce::Slider::rotarySliderFillColourId));
        g.strokePath (value, stroke);
    }

    const float thumbW = lineW * 2.0f;
    const juce::Point<float> thumb (bounds.getCentreX() + arcR * std::cos (toAngle - juce::MathConstants<float>::halfPi),
                                    bounds.getCentreY() + arcR * std::sin (toAngle - juce::MathConstants<float>::halfPi));
    g.setColour (s.findColour (juce::Slider::thumbColourId));
    g.fillEllipse (juce::Rectangle<float> (thumbW, thumbW).withCentre (thumb));
}

//------------------------------------------------------------------------------
LabeledControl::LabeledControl (const juce::String& caption, std::unique_ptr<juce::Component> c, bool isCompact,
                                int compactMaxWidth)
    : control (std::move (c)), compact (isCompact), maxWidth (compactMaxWidth)
{
    label.setText (caption, juce::dontSendNotification);
    label.setJustificationType (juce::Justification::centred);
    label.setFont (juce::FontOptions (12.5f));
    label.setColour (juce::Label::textColourId, dimText);
    addAndMakeVisible (label);
    addAndMakeVisible (*control);
}

void LabeledControl::resized()
{
    auto r = getLocalBounds();
    label.setBounds (r.removeFromTop (18));
    if (compact)
        control->setBounds (r.withSizeKeepingCentre (juce::jmin (r.getWidth() - 8, maxWidth),
                                                     juce::jmin (r.getHeight(), controlHeight)));
    else
        control->setBounds (r.reduced (2));
}

//------------------------------------------------------------------------------
SegmentedSwitch::SegmentedSwitch (juce::AudioParameterChoice& p)
    : options (p.choices),
      attachment (p, [this] (float v) { selected = juce::roundToInt (v); repaint(); })
{
    attachment.sendInitialUpdate();
}

void SegmentedSwitch::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    const int n = juce::jmax (1, options.size());
    const float w = r.getWidth() / (float) n, h = r.getHeight() / (float) n;

    if (textOnly)
    {
        // Just the words: gold = selected, grey = not (like the stage names). Hovered word brightens.
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        for (int i = 0; i < n; ++i)
        {
            const int s = slotFor (i);
            auto seg = vertical ? juce::Rectangle<float> (r.getX(), r.getY() + h * (float) s, r.getWidth(), h)
                                : juce::Rectangle<float> (r.getX() + w * (float) s, r.getY(), w, r.getHeight());
            g.setColour (i == selected ? accent : (s == hoverSlot ? dimText.brighter (0.3f) : dimText));
            g.drawFittedText (options[i], seg.toNearestInt(), juce::Justification::centred, 1);
        }
        return;
    }

    g.setColour (edge);
    g.fillRoundedRectangle (r, 5.0f);
    g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    for (int i = 0; i < n; ++i)
    {
        const int s = slotFor (i);
        auto seg = vertical ? juce::Rectangle<float> (r.getX(), r.getY() + h * (float) s, r.getWidth(), h)
                            : juce::Rectangle<float> (r.getX() + w * (float) s, r.getY(), w, r.getHeight());
        if (i == selected)
        {
            g.setColour (accent);
            g.fillRoundedRectangle (seg.reduced (2.0f), 4.0f);
        }
        g.setColour (i == selected ? bg : dimText);
        g.drawFittedText (options[i], seg.toNearestInt(), juce::Justification::centred, 1);
    }
}

void SegmentedSwitch::mouseDown (const juce::MouseEvent& e)
{
    const int n = juce::jmax (1, options.size());
    const float pos = vertical ? (float) e.y / (float) getHeight() : (float) e.x / (float) getWidth();
    const int idx = slotFor (juce::jlimit (0, n - 1, (int) (pos * (float) n)));
    attachment.setValueAsCompleteGesture ((float) idx);
}

void SegmentedSwitch::mouseMove (const juce::MouseEvent& e)
{
    if (! textOnly) return;
    const int n = juce::jmax (1, options.size());
    const float pos = vertical ? (float) e.y / (float) getHeight() : (float) e.x / (float) getWidth();
    const int s = juce::jlimit (0, n - 1, (int) (pos * (float) n));
    if (s != hoverSlot) { hoverSlot = s; repaint(); }
}

void SegmentedSwitch::mouseExit (const juce::MouseEvent&)
{
    if (hoverSlot != -1) { hoverSlot = -1; repaint(); }
}

//------------------------------------------------------------------------------
SpectrumAnalyzer::SpectrumAnalyzer (BassChainProcessor& p) : proc (p)
{
    pre.tap  = &proc.preTap;
    post.tap = &proc.postTap;
    setOpaque (false);
    startTimerHz (30);
}

void SpectrumAnalyzer::timerCallback()
{
    analyse (pre);
    analyse (post);
    repaint();
}

void SpectrumAnalyzer::analyse (Trace& t)
{
    // Take whatever the audio thread has produced since last time.
    const int got = t.tap->pull (scratch.data(), (int) scratch.size());
    for (int i = 0; i < got; ++i)
    {
        t.history[(size_t) t.pos] = scratch[(size_t) i];
        t.pos = (t.pos + 1) % kSize;
    }

    // FFT of the most recent kSize samples.
    for (int i = 0; i < kSize; ++i)
        fftData[(size_t) i] = t.history[(size_t) ((t.pos + i) % kSize)];
    std::fill (fftData.begin() + kSize, fftData.end(), 0.0f);
    window.multiplyWithWindowingTable (fftData.data(), (size_t) kSize);
    fft.performFrequencyOnlyForwardTransform (fftData.data());

    // A full-scale sine reads 0 dB. Rise instantly, fall smoothly.
    const float norm = 4.0f / (float) kSize;
    for (int b = 0; b < kBins; ++b)
    {
        const float db = juce::jlimit (kMinDb, kMaxDb, juce::Decibels::gainToDecibels (fftData[(size_t) b] * norm, kMinDb));
        float& lvl = t.levelDb[(size_t) b];
        lvl = db > lvl ? db : juce::jmax (db, lvl - 1.2f);
    }
}

float SpectrumAnalyzer::xForHz (float hz, juce::Rectangle<float> a) const
{
    return a.getX() + a.getWidth() * std::log (hz / kMinHz) / std::log (kMaxHz / kMinHz);
}

float SpectrumAnalyzer::yForDb (float db, juce::Rectangle<float> a) const
{
    return juce::jmap (db, kMinDb, kMaxDb, a.getBottom(), a.getY());
}

juce::Path SpectrumAnalyzer::makePath (const Trace& t, juce::Rectangle<float> a, bool closed) const
{
    const double sr = proc.getSampleRate() > 0 ? proc.getSampleRate() : 44100.0;
    juce::Path path;
    bool started = false;

    for (float x = 0.0f; x <= a.getWidth(); x += 1.5f)
    {
        const float hz  = kMinHz * std::pow (kMaxHz / kMinHz, x / a.getWidth());
        const float bin = (float) (hz * kSize / sr);
        const int   b0  = juce::jlimit (0, kBins - 2, (int) bin);
        const float fr  = juce::jlimit (0.0f, 1.0f, bin - (float) b0);
        const float db  = t.levelDb[(size_t) b0] + fr * (t.levelDb[(size_t) b0 + 1] - t.levelDb[(size_t) b0]);
        const float px = a.getX() + x, py = yForDb (db, a);

        if (! started) { path.startNewSubPath (px, py); started = true; }
        else           path.lineTo (px, py);
    }

    if (closed && started)
    {
        path.lineTo (a.getRight(), a.getBottom());
        path.lineTo (a.getX(), a.getBottom());
        path.closeSubPath();
    }
    return path;
}

void SpectrumAnalyzer::paint (juce::Graphics& g)
{
    auto outer = getLocalBounds().toFloat().reduced (4.0f);
    g.setColour (panelBg);
    g.fillRoundedRectangle (outer, 8.0f);
    g.setColour (edge);
    g.drawRoundedRectangle (outer, 8.0f, 1.2f);

    auto a = outer.reduced (36.0f, 22.0f).withTrimmedLeft (-8.0f);

    // Watermark: logo + plugin name side by side, very faint and centred, under the grid and traces.
    // The name is stacked one word per line (FXNKYCAT over BASS).
    {
        const auto wm = outer.reduced (12.0f);
        const float logoSize = watermark.isValid() ? wm.getHeight() : 0.0f;
        const float gap = (logoSize > 0.0f && watermarkText.isNotEmpty()) ? wm.getHeight() * 0.04f : 0.0f;
        const auto lines = juce::StringArray::fromTokens (watermarkText, " ", {});
        const int nLines = juce::jmax (1, lines.size());

        // Letter shapes per unit of font height: where the capitals start and end relative to the baseline.
        juce::Font font (juce::FontOptions (100.0f, juce::Font::bold));
        juce::Rectangle<float> ink;
        {
            juce::GlyphArrangement ga;
            ga.addLineOfText (font, lines.joinIntoString (""), 0.0f, 0.0f);
            juce::Path p; ga.createPath (p);
            ink = p.getBounds() / 100.0f;   // ink.getY() < 0 (above baseline), ink.getBottom() ~ 0
        }
        const float inkTop = -ink.getY(), inkH = juce::jmax (0.01f, ink.getHeight());

        // Outer edges stay where the original layout put them (lines of 0.88 x height / n,
        // font 1.05 x line); only the gap between the words shrinks as the letters grow.
        const float oldLineH = wm.getHeight() * 0.88f / (float) nLines, oldF = oldLineH * 1.05f;
        const float oldBaseline0 = wm.getCentreY() - oldLineH * (float) nLines * 0.5f
                                 + (oldLineH - oldF) * 0.5f + font.getAscent() / 100.0f * oldF;
        const float blockTop    = oldBaseline0 - inkTop * oldF;
        const float blockBottom = oldBaseline0 + oldLineH * (float) (nLines - 1) + (ink.getBottom()) * oldF;

        constexpr float wordGap = 0.08f;   // space between the words, as a fraction of the letter height
        const float capH  = (blockBottom - blockTop) / ((float) nLines + wordGap * (float) (nLines - 1));
        const float fontH = capH / inkH, pitch = capH * (1.0f + wordGap);
        font = font.withHeight (fontH);
        float textW = 0.0f;
        for (auto& l : lines)
            textW = juce::jmax (textW, juce::GlyphArrangement::getStringWidth (font, l));

        // Logo stays centred; the name sits to its left. Shrink everything if the
        // left half of the analyzer is too narrow for half the logo + gap + name.
        float scale = 1.0f;
        const float need = logoSize * 0.5f + gap + textW;
        if (need > wm.getWidth() * 0.5f)
            scale = wm.getWidth() * 0.5f / need;
        const float ls = logoSize * scale, gp = gap * scale, tw = textW * scale;
        font = font.withHeight (font.getHeight() * scale);

        const float logoX = wm.getCentreX() - ls * 0.5f;
        g.setColour (textCol.withAlpha (0.07f));
        if (ls > 0.0f)
            g.drawImage (watermark, { logoX, wm.getCentreY() - ls * 0.5f, ls, ls }, juce::RectanglePlacement::centred, true);
        if (tw > 0.0f)
        {
            // Right-aligned against the logo: both words end next to it.
            const float right = logoX - gp;
            const float top = wm.getCentreY() + (blockTop - wm.getCentreY()) * scale;
            float baseline = top + inkTop * fontH * scale;
            juce::GlyphArrangement ga;
            for (auto& l : lines)
            {
                const float w = juce::GlyphArrangement::getStringWidth (font, l);
                ga.addLineOfText (font, l, right - w, baseline);
                baseline += pitch * scale;
            }
            ga.draw (g);
        }
    }

    // Grid
    g.setFont (juce::FontOptions (10.5f));
    for (float hz : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
    {
        const float x = xForHz (hz, a);
        g.setColour (edge.withAlpha (0.7f));
        g.drawVerticalLine ((int) x, a.getY(), a.getBottom());
        g.setColour (dimText);
        g.drawText (hz >= 1000.0f ? juce::String (hz / 1000.0f, 0) + "k" : juce::String ((int) hz),
                    juce::Rectangle<float> (x - 20.0f, a.getBottom() + 2.0f, 40.0f, 14.0f), juce::Justification::centred);
    }
    for (float db = kMaxDb; db >= kMinDb; db -= 24.0f)
    {
        const float y = yForDb (db, a);
        g.setColour (edge.withAlpha (0.7f));
        g.drawHorizontalLine ((int) y, a.getX(), a.getRight());
        g.setColour (dimText);
        if (db < kMaxDb)   // the 0 dB label would sit under the insert strip
            g.drawText (juce::String ((int) db), juce::Rectangle<float> (a.getX() - 34.0f, y - 7.0f, 30.0f, 14.0f),
                        juce::Justification::centredRight);
    }

    g.saveState();
    g.reduceClipRegion (a.toNearestInt());

    // Before: soft grey fill
    g.setColour (dimText.withAlpha (0.22f));
    g.fillPath (makePath (pre, a, true));
    g.setColour (dimText.withAlpha (0.7f));
    g.strokePath (makePath (pre, a, false), juce::PathStrokeType (1.0f));

    // After: accent line on top
    g.setColour (accent.withAlpha (0.12f));
    g.fillPath (makePath (post, a, true));
    g.setColour (accent);
    g.strokePath (makePath (post, a, false), juce::PathStrokeType (1.6f));

    g.restoreState();

    // Legend
    auto legend = outer.reduced (14.0f, 6.0f).removeFromTop (14.0f).removeFromRight (150.0f);
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    g.setColour (dimText);
    g.fillRect (legend.getX(), legend.getCentreY() - 1.0f, 14.0f, 2.0f);
    g.drawText ("BEFORE", legend.withTrimmedLeft (18.0f), juce::Justification::centredLeft);
    g.setColour (accent);
    g.fillRect (legend.getX() + 78.0f, legend.getCentreY() - 1.0f, 14.0f, 2.0f);
    g.drawText ("AFTER", legend.withTrimmedLeft (96.0f), juce::Justification::centredLeft);
}

//------------------------------------------------------------------------------
void ArrowButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    g.setColour (highlighted ? edge.brighter (0.2f) : edge);
    g.fillRoundedRectangle (r, 5.0f);

    const auto c = r.getCentre();
    const float w = 9.0f, h = 5.0f;
    juce::Path tri;
    tri.addTriangle (c.x - w * 0.5f, c.y - h * 0.5f + (down ? 1.0f : 0.0f),
                     c.x + w * 0.5f, c.y - h * 0.5f + (down ? 1.0f : 0.0f),
                     c.x,            c.y + h * 0.5f + (down ? 1.0f : 0.0f));
    g.setColour (accent);
    g.fillPath (tri);
}

LedButton::LedButton() : juce::Button ({})
{
    setClickingTogglesState (true);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void LedButton::paintButton (juce::Graphics& g, bool highlighted, bool)
{
    const float d = juce::jmin (16.0f, (float) juce::jmin (getWidth(), getHeight()) - 8.0f);
    const auto led = getLocalBounds().toFloat().withSizeKeepingCentre (d, d);
    const juce::Colour hot { 0xffff7a3d };
    if (getToggleState())
    {
        g.setColour (hot.withAlpha (0.35f));
        g.fillEllipse (led.expanded (4.0f));   // glow
        g.setColour (hot);
    }
    else
        g.setColour (highlighted ? edge.brighter (0.3f) : edge);
    g.fillEllipse (led);
    g.setColour (bg.withAlpha (0.6f));
    g.drawEllipse (led, 1.0f);
}

TitleToggle::TitleToggle() : juce::Button ({})
{
    setClickingTogglesState (true);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

juce::Font TitleToggle::font() { return juce::Font (juce::FontOptions (15.0f, juce::Font::bold)); }

void TitleToggle::paintButton (juce::Graphics& g, bool highlighted, bool)
{
    auto c = getToggleState() ? accent : dimText;
    if (highlighted) c = c.brighter (0.25f);
    g.setColour (c);
    g.setFont (font());
    g.drawText (getButtonText(), getLocalBounds(), juce::Justification::topLeft, true);
}

ModulePanel::ModulePanel (juce::String num, juce::String t, juce::String sub, int r)
    : number (std::move (num)), title (std::move (t)), subtitle (std::move (sub)), rows (r)
{
    enable.setButtonText (title);
    addAndMakeVisible (enable);
}

void ModulePanel::add (std::unique_ptr<LabeledControl> c)
{
    addAndMakeVisible (*c);
    c->addMouseListener (this, true);   // touching a control turns the stage on (see turnOnFromControl)
    controls.push_back (std::move (c));
}

juce::Rectangle<float> ModulePanel::ledArea() const
{
    // Top-right corner of the panel.
    const auto r = getLocalBounds().reduced (14, 10).toFloat();
    return { r.getRight() - 12.0f, r.getY() + 3.0f, 12.0f, 12.0f };
}

juce::Rectangle<float> ModulePanel::meterArea() const
{
    // Thin bar along the bottom of the header.
    auto r = getLocalBounds().reduced (14, 10).toFloat();
    return { r.getX(), r.getY() + 40.0f, r.getWidth(), 3.0f };
}

void ModulePanel::setActivity (float v)
{
    if (std::abs (v - activity) < 0.004f) return;
    activity = v;
    repaint (ledArea().expanded (8.0f).getSmallestIntegerContainer());
    repaint (meterArea().expanded (2.0f).getSmallestIntegerContainer());
}

void ModulePanel::setActivity2 (float v)
{
    if (std::abs (v - activity2) < 0.004f) return;
    activity2 = v;
    repaint (ledArea().expanded (8.0f).getSmallestIntegerContainer());
    repaint (meterArea().expanded (2.0f).getSmallestIntegerContainer());
}

void ModulePanel::setActivity3 (float v)
{
    if (std::abs (v - activity3) < 0.004f) return;
    activity3 = v;
    repaint (ledArea().expanded (8.0f).getSmallestIntegerContainer());
    repaint (meterArea().expanded (2.0f).getSmallestIntegerContainer());
}

void ModulePanel::turnOnFromControl (const juce::MouseEvent& e)
{
    // Only for the user's own clicks / scrolls on a control in this stage, so presets,
    // automation and reopening a session never switch a stage on by themselves.
    if (e.eventComponent != this && ! enable.getToggleState())
        enable.setToggleState (true, juce::sendNotificationSync);   // also updates the DAW parameter
}

void ModulePanel::mouseDown (const juce::MouseEvent& e)
{
    if (e.eventComponent != this) { turnOnFromControl (e); return; }
    if (ledArea().expanded (6.0f).contains (e.position))
        enable.triggerClick();   // same as clicking the stage name (keeps the DAW parameter in sync)
}

void ModulePanel::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (e.eventComponent != this) { turnOnFromControl (e); return; }
    juce::Component::mouseWheelMove (e, w);
}

void ModulePanel::mouseMove (const juce::MouseEvent& e)
{
    if (e.eventComponent != this) return;   // cursor only for the panel itself (the LED)
    setMouseCursor (ledArea().expanded (6.0f).contains (e.position) ? juce::MouseCursor::PointingHandCursor
                                                                     : juce::MouseCursor::NormalCursor);
}

void ModulePanel::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (4.0f);
    g.setColour (panelBg);
    g.fillRoundedRectangle (r, 8.0f);
    g.setColour (enable.getToggleState() ? accent.withAlpha (0.55f) : edge);
    g.drawRoundedRectangle (r, 8.0f, 1.2f);

    // Activity LED: dark when idle, glows brighter the harder the stage works.
    const auto led = ledArea();
    const bool  isOn = enable.getToggleState();
    const float a  = isOn ? activity : 0.0f;
    const float a2 = isOn && (splitMeter || ledFromSecond) ? activity2 : 0.0f;
    const juce::Colour hot { 0xffff7a3d };
    float la = ledFromSecond ? a2 : (ledFollowsBoth ? juce::jmax (a, a2) : a);
    if (tripleMeter && isOn) la = juce::jmax (a, a2, activity3);   // LED follows whichever bar is highest
    if (la > 0.02f)
    {
        g.setColour (hot.withAlpha (0.35f * la));
        g.fillEllipse (led.expanded (5.0f * la));   // glow
    }
    g.setColour (edge.interpolatedWith (hot, la));
    g.fillEllipse (led);
    g.setColour (bg.withAlpha (0.6f));
    g.drawEllipse (led, 1.0f);

    // Activity meter bar (or two side by side: left = activity2, right = activity)
    auto drawBar = [&] (juce::Rectangle<float> m, float v)
    {
        g.setColour (bg);
        g.fillRoundedRectangle (m, 1.5f);
        if (v > 0.0f)
        {
            g.setColour (accent.interpolatedWith (hot, v));
            g.fillRoundedRectangle (m.withWidth (m.getWidth() * v), 1.5f);
        }
    };
    if (tripleMeter)
    {
        // Three bars: left = activity2, middle = activity3, right = activity
        auto m = meterArea();
        constexpr float barGap = 8.0f;
        const float third = (m.getWidth() - 2.0f * barGap) / 3.0f;
        drawBar (m.removeFromLeft (third), a2);
        m.removeFromLeft (barGap);
        drawBar (m.removeFromLeft (third), isOn ? activity3 : 0.0f);
        drawBar (m.removeFromRight (third), a);
    }
    else if (splitMeter)
    {
        auto m = meterArea();
        constexpr float barGap = 8.0f;
        const float half = (m.getWidth() - barGap) * 0.5f;
        drawBar (m.removeFromLeft (half), a2);
        drawBar (m.removeFromRight (half), a);
    }
    else
        drawBar (meterArea(), a);

    auto header = getLocalBounds().reduced (14, 10).removeFromTop (34).withTrimmedRight (24);  // leave room for the LED
    g.setColour (accent);
    g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    g.drawText (number, header.removeFromLeft (22), juce::Justification::topLeft);

    header.removeFromTop (18);   // the title is drawn by the enable button
    g.setColour (dimText);
    g.setFont (juce::FontOptions (11.5f));
    g.drawText (subtitle, header, juce::Justification::topLeft);
}

void ModulePanel::resized()
{
    auto r = getLocalBounds().reduced (12, 10);
    auto header = r.removeFromTop (38);
    {
        // Title (click target) sits right after the stage number, as wide as its text.
        auto t = getLocalBounds().reduced (14, 10).removeFromTop (18).withTrimmedRight (24);
        t.removeFromLeft (22);
        const int tw = (int) std::ceil (juce::GlyphArrangement::getStringWidth (TitleToggle::font(), title)) + 4;
        enable.setBounds (t.withWidth (juce::jmin (tw, t.getWidth())));
    }
    r.removeFromTop (10);   // room for the activity meter

    const int n = (int) controls.size();
    if (n == 0) return;

    if ((int) colWeights.size() == n)
    {
        int total = 0;
        for (int w : colWeights) total += w;
        const int fullW = r.getWidth();
        for (int i = 0; i < n; ++i)
            controls[(size_t) i]->setBounds (r.removeFromLeft (fullW * colWeights[(size_t) i] / juce::jmax (1, total)));
        return;
    }

    if (topWide && n > 1)
    {
        controls.front()->setBounds (r.removeFromTop (50));
        r.removeFromTop (4);
        const int cw = r.getWidth() / (n - 1);
        for (int i = 1; i < n; ++i)
            controls[(size_t) i]->setBounds (r.getX() + (i - 1) * cw, r.getY(), cw, r.getHeight());
        return;
    }

    if (sideStack && n > 1)
    {
        // Left column: every control but the last, stacked top to bottom. Right: the last one.
        auto left = r.removeFromLeft (r.getWidth() / 2);
        controls.back()->setBounds (r);
        // Several switches: up to 110 px each. A single (e.g. tall vertical) control can use the full height.
        const int stackH = n - 1 == 1 ? left.getHeight() : juce::jmin (left.getHeight() / (n - 1), 110);
        left = left.withSizeKeepingCentre (left.getWidth(), stackH * (n - 1));
        for (int i = 0; i < n - 1; ++i)
            controls[(size_t) i]->setBounds (left.removeFromTop (stackH));
        return;
    }

    const int cols = (n + rows - 1) / rows;
    const int cw = r.getWidth() / cols, rh = r.getHeight() / rows;

    for (int i = 0; i < n; ++i)
    {
        const int row = i / cols, col = i % cols;
        controls[(size_t) i]->setBounds (r.getX() + col * cw, r.getY() + row * rh, cw, rh);
    }
}
} // namespace ui

//==============================================================================
BassChainEditor::BassChainEditor (BassChainProcessor& p)
    : AudioProcessorEditor (p), proc (p),
      comp      ("1", "Opto Comp",  "LA-2A style  |  tempo release  |  gain matched", 1),
      harmonics ("2", "Harmonics",  "Sub octave  |  tape  |  overdrive, in parallel", 1),
      pultec    ("3", "Vintage EQ", "EQP-1A style program equalizer", 2),
      limiter   ("4", "Limiter",    "Brickwall  |  release follows tempo", 1),
      stereo    ("5", "Stereo",     "Mono and centred below", 1),
      filters   ("6", "Filters",    "High / low pass, 24 dB/oct", 1),
      analyzer  (p)
{
    lnf.setColour (juce::Slider::rotarySliderFillColourId, accent);
    lnf.setColour (juce::Slider::rotarySliderOutlineColourId, edge);
    lnf.setColour (juce::Slider::thumbColourId, textCol);
    lnf.setColour (juce::Slider::textBoxTextColourId, textCol);
    lnf.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    lnf.setColour (juce::ComboBox::backgroundColourId, bg);
    lnf.setColour (juce::ComboBox::outlineColourId, edge);
    lnf.setColour (juce::ComboBox::textColourId, textCol);
    lnf.setColour (juce::ComboBox::arrowColourId, accent);
    lnf.setColour (juce::PopupMenu::backgroundColourId, panelBg);
    lnf.setColour (juce::PopupMenu::highlightedBackgroundColourId, accent);
    lnf.setColour (juce::PopupMenu::highlightedTextColourId, bg);
    setLookAndFeel (&lnf);
    loadAccent();   // saved accent colour (first window of the session)

    analyzer.setWatermark (juce::ImageCache::getFromMemory (BinaryData::logo_png, BinaryData::logo_pngSize), "FXNKYCAT BASS");
    // 1. Opto compressor: ratio switch on the left, Peak Reduction knob on the right
    if (auto* ratio = dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter ("compRatio")))
    {
        // Stacked: 4:1 on top, 3:1 (default) in the middle, 2:1 at the bottom
        auto sw = std::make_unique<ui::SegmentedSwitch> (*ratio);
        sw->setVertical (true);
        sw->setReversed (true);
        auto lc = std::make_unique<ui::LabeledControl> ("Ratio", std::move (sw), true, 70);
        lc->setControlHeight (96);
        comp.add (std::move (lc));
    }
    comp.add (knob ("compPeak", "Peak Reduction"));
    comp.setSideStack (true);
    bindEnable (comp, "compOn");

    // 2. Harmonics: Sub Bass, Tape and Overdrive side by side (each runs on its own, then they are summed)
    harmonics.add (knob ("subBass", "Sub Bass"));
    harmonics.add (knob ("tapeAmount", "Tape"));
    harmonics.add (knob ("odMix", "Overdrive"));
    harmonics.setSplitMeter (true, true);
    harmonics.setTripleMeter (true);   // three bars, one per knob: sub bass, tape, overdrive
    bindEnable (harmonics, "colorOn");

    // 3. Vintage EQ — laid out like the hardware: low section on top, high section below.
    pultec.add (choice ("pLowFreq", "Low Freq (CPS)"));
    pultec.add (knob ("pLowBoost", "Low Boost"));
    pultec.add (knob ("pLowAtten", "Low Atten"));
    pultec.add (choice ("pHighAttFreq", "Atten Sel (KCS)"));
    pultec.add (choice ("pHighFreq", "High Freq (KCS)"));
    pultec.add (knob ("pHighBoost", "High Boost"));
    pultec.add (knob ("pBandwidth", "Bandwidth (Sharp-Broad)"));
    pultec.add (knob ("pHighAtten", "High Atten"));
    pultec.setSplitMeter (true, true);   // left bar: low boost, right bar: high boost
    bindEnable (pultec, "pultecOn");

    // 4. Limiter: threshold only (release follows the tempo)
    limiter.add (knob ("limThresh", "Threshold"));
    bindEnable (limiter, "limOn");

    // 5. Stereo: mono below 200 | 250 | 300 Hz
    if (auto* mf = dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter ("monoFreq")))
    {
        auto sw = std::make_unique<ui::SegmentedSwitch> (*mf);
        sw->setVertical (true);
        sw->setReversed (true);   // 300 Hz on top, 200 Hz at the bottom
        auto lc = std::make_unique<ui::LabeledControl> ("Mono Below", std::move (sw), true, 90);
        lc->setControlHeight (96);
        stereo.add (std::move (lc));
    }
    bindEnable (stereo, "stereoOn");

    // 6. Filters: high / low pass
    filters.add (knob ("hpFreq", "High Pass"));
    filters.add (knob ("lpFreq", "Low Pass"));
    bindEnable (filters, "filterOn");

    for (auto* panel : { &comp, &harmonics, &pultec, &limiter, &stereo, &filters })
        addAndMakeVisible (panel);

    // Global Timing note value, top right. Scroll the mouse wheel over it or click to pick.
    timingBox.addItemList (allCaps (BassChainProcessor::divisionNames()), 1);
    timingBox.setJustificationType (juce::Justification::centred);
    timingBox.setScrollWheelEnabled (true);
    comboAtts.push_back (std::make_unique<APVTS::ComboBoxAttachment> (proc.apvts, "division", timingBox));
    addAndMakeVisible (timingBox);

    // Input high-pass (all the way left = Off) and low-pass (all the way right = Off), top bar
    for (auto* s : { &inHpfSlider, &inLpfSlider })
    {
        s->setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 22);
        s->setColour (juce::Slider::trackColourId, accent);
        s->setColour (juce::Slider::backgroundColourId, edge);
        addAndMakeVisible (*s);
    }
    sliderAtts.push_back (std::make_unique<APVTS::SliderAttachment> (proc.apvts, "inHpf", inHpfSlider));
    sliderAtts.push_back (std::make_unique<APVTS::SliderAttachment> (proc.apvts, "inLpf", inLpfSlider));

    // Harmonics / EQ order switch, top bar
    if (auto* sp = proc.apvts.getParameter ("swapHarm"))
    {
        swapAtt = std::make_unique<juce::ParameterAttachment> (*sp, [this] (float v)
        {
            swapLed.setToggleState (v > 0.5f, juce::dontSendNotification);   // lit = swapped (EQ first)
            applySwap (v > 0.5f);
        });
        swapAtt->sendInitialUpdate();
    }
    swapLed.onClick = [this] { if (swapAtt != nullptr) swapAtt->setValueAsCompleteGesture (swapLed.getToggleState() ? 1.0f : 0.0f); };
    addAndMakeVisible (swapLed);

    bpmLabel.setJustificationType (juce::Justification::centredLeft);
    bpmLabel.setColour (juce::Label::textColourId, dimText);
    bpmLabel.setFont (juce::FontOptions (12.5f, juce::Font::bold));
    addAndMakeVisible (bpmLabel);

    // Output auto gain switch, top bar
    buttonAtts.push_back (std::make_unique<APVTS::ButtonAttachment> (proc.apvts, "autoGain", autoGainButton));
    addAndMakeVisible (autoGainButton);

    // Plugin bypass, top bar (also the DAW's bypass for this plugin)
    buttonAtts.push_back (std::make_unique<APVTS::ButtonAttachment> (proc.apvts, "bypass", bypassButton));
    addAndMakeVisible (bypassButton);

    // Presets: pick a slot to load it; SAVE stores the current settings in a slot.
    presetBox.setTextWhenNothingSelected ("CHOOSE PRESET...");
    presetBox.setTextWhenNoChoicesAvailable ("NO PRESETS");
    presetBox.onChange = [this] { presetChosen(); };
    addAndMakeVisible (presetBox);
    saveButton.setColour (juce::TextButton::buttonColourId, edge);
    saveButton.setColour (juce::TextButton::textColourOffId, accent);
    saveButton.onClick = [this] { showSaveDialog(); };
    addAndMakeVisible (saveButton);

    // Accent colour menu (arrow right of SAVE)
    colourButton.onClick = [this] { showColourMenu(); };
    addAndMakeVisible (colourButton);
    applyAccentColours();
    refreshPresetList();

    addAndMakeVisible (analyzer);

    // External plugin insert strip, drawn over the top-left of the analyzer
    insertCaption.setText ("INSERT", juce::dontSendNotification);
    insertCaption.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    insertCaption.setColour (juce::Label::textColourId, dimText);
    insertCaption.setJustificationType (juce::Justification::centredRight);
    buttonAtts.push_back (std::make_unique<APVTS::ButtonAttachment> (proc.apvts, "insertOn", insertLed));
    for (auto* b : { &insertLoad, &insertOpen, &insertRemove })
    {
        b->setColour (juce::TextButton::buttonColourId, edge);
        b->setColour (juce::TextButton::textColourOffId, textCol);
    }
    insertLoad.onClick   = [this] { showInsertMenu(); };
    insertOpen.onClick   = [this] { openInsertWindow(); };
    insertRemove.onClick = [this] { proc.removeInsert(); };
    insertPosBox.addItemList (allCaps (BassChainProcessor::insertPositionNames()), 1);
    insertPosBox.setJustificationType (juce::Justification::centred);
    comboAtts.push_back (std::make_unique<APVTS::ComboBoxAttachment> (proc.apvts, "insertPos", insertPosBox));
    for (auto* c : std::initializer_list<juce::Component*> { &insertCaption, &insertLed, &insertLoad, &insertOpen, &insertRemove, &insertPosBox })
        addAndMakeVisible (*c);   // added after the analyzer, so they sit on top of it

    proc.onInsertAboutToChange = [this] { closeInsertWindow(); };   // never leave a window on a deleted plugin
    proc.onInsertChanged       = [this] { refreshInsert(); };
    refreshInsert();

    // Resizable from the bottom-right corner; panels, knobs and analyzer scale with the window.
    setResizable (true, true);
    setResizeLimits (1460, 700, 2680, 1600);
    setSize (1510, 780);
    startTimerHz (30);
}

void BassChainEditor::timerCallback()
{
    if (lastAccentVersion != accentVersion) applyAccentColours();   // another window changed the colour

    ui::ModulePanel* panels[BassChainProcessor::kNumStages] =
        { &comp, &harmonics, &pultec, &limiter, &stereo, &filters };

    for (int s = 0; s < BassChainProcessor::kNumStages; ++s)
    {
        const float v = proc.activity[s].exchange (0.0f);
        // Jump up instantly, fade out over roughly a quarter second.
        shownActivity[s] = juce::jmax (v, shownActivity[s] * 0.82f);
        if (shownActivity[s] < 0.01f) shownActivity[s] = 0.0f;
        panels[s]->setActivity (shownActivity[s]);

        // Left-hand bar of split meters (ignored by panels without one)
        shownActivity2[s] = juce::jmax (proc.activity2[s].exchange (0.0f), shownActivity2[s] * 0.82f);
        if (shownActivity2[s] < 0.01f) shownActivity2[s] = 0.0f;
        panels[s]->setActivity2 (shownActivity2[s]);

        // Middle bar of triple meters (ignored by panels without one)
        shownActivity3[s] = juce::jmax (proc.activity3[s].exchange (0.0f), shownActivity3[s] * 0.82f);
        if (shownActivity3[s] < 0.01f) shownActivity3[s] = 0.0f;
        panels[s]->setActivity3 (shownActivity3[s]);
    }


    const auto bpmText = juce::String (proc.hostBpm.load(), 1) + " BPM";
    if (bpmLabel.getText() != bpmText)
        bpmLabel.setText (bpmText, juce::dontSendNotification);
}

//------------------------------------------------------------------------------
// External plugin insert
namespace
{
    // Window that shows the hosted plugin's own controls.
    class InsertWindow : public juce::DocumentWindow
    {
    public:
        InsertWindow (const juce::String& name, std::function<void()> onClose)
            : juce::DocumentWindow (name, bg, juce::DocumentWindow::closeButton), closeFn (std::move (onClose))
        {
            setUsingNativeTitleBar (true);
        }
        void closeButtonPressed() override { if (closeFn) closeFn(); }
    private:
        std::function<void()> closeFn;
    };

    // Plugin browser for the LOAD button: search box, A-Z / MAKER sort, list, BROWSE...
    // Click a plugin to load it. Shown in a call-out under the LOAD button.
    class InsertBrowser : public juce::Component, private juce::ListBoxModel, private juce::TextEditor::Listener
    {
    public:
        struct Entry { juce::String path, name, maker; };

        InsertBrowser (std::vector<Entry> entries, std::function<void (const juce::File&)> pick, std::function<void()> browseFn)
            : all (std::move (entries)), onPick (std::move (pick)), onBrowse (std::move (browseFn))
        {
            search.setTextToShowWhenEmpty ("SEARCH...", dimText);
            search.setColour (juce::TextEditor::backgroundColourId, bg);
            search.setColour (juce::TextEditor::outlineColourId, edge);
            search.setColour (juce::TextEditor::focusedOutlineColourId, accent);
            search.setColour (juce::TextEditor::textColourId, textCol);
            search.setFont (juce::FontOptions (14.0f));
            search.addListener (this);
            addAndMakeVisible (search);

            for (auto* b : { &sortAZ, &sortMaker, &browse })
            {
                b->setColour (juce::TextButton::buttonColourId, edge);
                b->setColour (juce::TextButton::buttonOnColourId, accent);
                b->setColour (juce::TextButton::textColourOffId, textCol);
                b->setColour (juce::TextButton::textColourOnId, bg);
                addAndMakeVisible (*b);
            }
            sortAZ.setClickingTogglesState (true);    sortAZ.setRadioGroupId (1);
            sortMaker.setClickingTogglesState (true); sortMaker.setRadioGroupId (1);
            (byMaker ? sortMaker : sortAZ).setToggleState (true, juce::dontSendNotification);
            sortAZ.onClick    = [this] { byMaker = false; refilter(); };
            sortMaker.onClick = [this] { byMaker = true;  refilter(); };
            browse.onClick    = [this] { auto fn = onBrowse; dismiss(); if (fn) fn(); };

            list.setModel (this);
            list.setRowHeight (24);
            list.setColour (juce::ListBox::backgroundColourId, panelBg);
            list.setColour (juce::ListBox::outlineColourId, edge);
            list.setOutlineThickness (1);
            addAndMakeVisible (list);

            refilter();
            setSize (480, 440);
        }

        void visibilityChanged() override { if (isShowing()) search.grabKeyboardFocus(); }

        void resized() override
        {
            auto r = getLocalBounds().reduced (8);
            auto top = r.removeFromTop (28);
            sortMaker.setBounds (top.removeFromRight (70));
            top.removeFromRight (4);
            sortAZ.setBounds (top.removeFromRight (50));
            top.removeFromRight (8);
            search.setBounds (top);
            r.removeFromTop (6);
            browse.setBounds (r.removeFromBottom (28).removeFromRight (110));
            r.removeFromBottom (6);
            list.setBounds (r);
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (panelBg);
            if (reading)
            {
                g.setColour (dimText);
                g.setFont (juce::FontOptions (12.0f));
                g.drawText ("READING MAKERS...", getLocalBounds().reduced (10, 8).removeFromBottom (28),
                            juce::Justification::centredLeft, false);
            }
        }

        // Makers arrive from the background reader: update them, keeping the search and selection.
        std::function<juce::String (const juce::String&)> makerOf;
        void refreshMakers (bool stillReading)
        {
            reading = stillReading;
            if (makerOf) for (auto& e : all) e.maker = makerOf (e.path);
            refilter();
            repaint();
        }
        bool reading = false;

    private:
        static inline bool byMaker = false;   // remembered between openings

        int getNumRows() override { return (int) shown.size(); }

        void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
        {
            if (row < 0 || row >= (int) shown.size()) return;
            const auto& e = shown[(size_t) row];
            if (selected) { g.setColour (accent.withAlpha (0.25f)); g.fillRect (0, 0, w, h); }
            const int makerW = w * 2 / 5;
            g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
            g.setColour (selected ? accent : textCol);
            g.drawText (e.name.toUpperCase(), 10, 0, w - makerW - 16, h, juce::Justification::centredLeft, true);
            g.setFont (juce::FontOptions (12.0f));
            g.setColour (dimText);
            g.drawText (e.maker.isEmpty() ? juce::String ("-") : e.maker.toUpperCase(), w - makerW, 0, makerW - 10, h,
                        juce::Justification::centredRight, true);
        }

        void listBoxItemClicked (int row, const juce::MouseEvent&) override { pickRow (row); }
        void selectedRowsChanged (int row) override
        {
            if (row >= 0 && row < (int) shown.size()) selectedPath = shown[(size_t) row].path;
        }
        void returnKeyPressed (int row) override { pickRow (row); }
        void textEditorTextChanged (juce::TextEditor&) override { refilter(); }
        void textEditorReturnKeyPressed (juce::TextEditor&) override { pickRow (list.getSelectedRow() >= 0 ? list.getSelectedRow() : 0); }

        void pickRow (int row)
        {
            if (row < 0 || row >= (int) shown.size()) return;
            const juce::File f (shown[(size_t) row].path);
            auto fn = onPick;
            dismiss();
            if (fn) fn (f);
        }

        void dismiss()
        {
            if (auto* box = findParentComponentOfClass<juce::CallOutBox>())
                box->dismiss();
        }

        void refilter()
        {
            // Every word typed must appear in the name or the maker.
            const auto words = juce::StringArray::fromTokens (search.getText().trim(), " ", {});
            shown.clear();
            for (const auto& e : all)
            {
                bool match = true;
                for (const auto& w : words)
                    if (! e.name.containsIgnoreCase (w) && ! e.maker.containsIgnoreCase (w)) { match = false; break; }
                if (match) shown.push_back (e);
            }
            std::sort (shown.begin(), shown.end(), [] (const Entry& a, const Entry& b)
            {
                if (byMaker)
                {
                    // Plugins without a known maker go last.
                    if (a.maker.isEmpty() != b.maker.isEmpty()) return b.maker.isEmpty();
                    const int c = a.maker.compareIgnoreCase (b.maker);
                    if (c != 0) return c < 0;
                }
                return a.name.compareIgnoreCase (b.name) < 0;
            });
            list.updateContent();
            int sel = 0;
            for (size_t i = 0; i < shown.size(); ++i)
                if (shown[i].path == selectedPath) { sel = (int) i; break; }
            list.selectRow (shown.empty() ? -1 : sel);
            if (! shown.empty()) selectedPath = shown[(size_t) sel].path;
            list.repaint();
        }

        std::vector<Entry> all, shown;
        juce::String selectedPath;
        std::function<void (const juce::File&)> onPick;
        std::function<void()> onBrowse;
        juce::TextEditor search;
        juce::TextButton sortAZ { "A-Z" }, sortMaker { "MAKER" }, browse { "BROWSE..." };
        juce::ListBox list;
    };

    // Plugin makers, remembered in a small file next to the presets (path + date -> maker),
    // so they are only read once per plugin. Reading is slow (Windows scans each plugin
    // file), so it happens on a background thread while the list is already open.
    struct MakerCache
    {
        juce::CriticalSection lock;
        std::map<juce::String, std::pair<juce::int64, juce::String>> map;   // path -> (modified time, maker)
        bool loaded = false;

        static juce::File file() { return settingsFile().getSiblingFile ("plugin-makers.xml"); }

        void load()
        {
            const juce::ScopedLock sl (lock);
            if (loaded) return;
            loaded = true;
            if (auto xml = juce::XmlDocument::parse (file()))
                for (auto* e : xml->getChildIterator())
                    map[e->getStringAttribute ("path")] = { e->getStringAttribute ("time").getLargeIntValue(),
                                                            e->getStringAttribute ("maker") };
        }

        void save()
        {
            juce::XmlElement xml ("PluginMakers");
            {
                const juce::ScopedLock sl (lock);
                for (auto& [path, v] : map)
                {
                    auto* e = xml.createNewChildElement ("P");
                    e->setAttribute ("path", path);
                    e->setAttribute ("time", juce::String (v.first));
                    e->setAttribute ("maker", v.second);
                }
            }
            file().getParentDirectory().createDirectory();
            xml.writeTo (file());
        }

        // true if known (and the plugin hasn't changed since)
        bool lookup (const juce::String& path, juce::String& maker)
        {
            const auto t = juce::File (path).getLastModificationTime().toMilliseconds();
            const juce::ScopedLock sl (lock);
            auto it = map.find (path);
            if (it == map.end() || it->second.first != t) return false;
            maker = it->second.second;
            return true;
        }

        void store (const juce::String& path, const juce::String& maker)
        {
            const auto t = juce::File (path).getLastModificationTime().toMilliseconds();
            const juce::ScopedLock sl (lock);
            map[path] = { t, maker };
        }
    };

    MakerCache& makers()
    {
        static MakerCache c;
        return c;
    }

    // Reads the makers that aren't known yet, reporting progress on the message thread.
    class MakerScanThread : public juce::Thread
    {
    public:
        MakerScanThread (juce::StringArray todo, std::function<void (bool finished)> progress)
            : juce::Thread ("fxnkycat bass plugin makers"), paths (std::move (todo)), onProgress (std::move (progress)) {}

        void run() override
        {
            int n = 0;
            for (const auto& path : paths)
            {
                if (threadShouldExit()) return;
                makers().store (path, vc::vst3Maker (juce::File (path)));
                if (++n % 8 == 0) onProgress (false);
            }
            if (! threadShouldExit()) onProgress (true);
        }

    private:
        juce::StringArray paths;
        std::function<void (bool)> onProgress;
    };
}

void BassChainEditor::refreshInsert()
{
    const auto name = proc.insertName();
    insertLoad.setButtonText (name.isEmpty() ? "LOAD VST3..." : name.toUpperCase());
    insertOpen.setEnabled (name.isNotEmpty());
    insertRemove.setEnabled (name.isNotEmpty());
}

void BassChainEditor::showInsertMenu()
{
    // The VST3s installed in the standard folders, with their makers (read from the
    // plugins' files, not by loading them; remembered after the first time).
    makers().load();
    std::vector<InsertBrowser::Entry> entries;
    juce::StringArray unknown;
    for (const auto& path : proc.findInstalledVst3s())
    {
        juce::String maker;
        if (! makers().lookup (path, maker)) unknown.add (path);   // read in the background below
        entries.push_back ({ path, juce::File (path).getFileNameWithoutExtension(), maker });
    }

    juce::Component::SafePointer<BassChainEditor> safe (this);
    auto browser = std::make_unique<InsertBrowser> (std::move (entries),
        [safe] (const juce::File& f)
        {
            // Load after the call-out has closed.
            juce::MessageManager::callAsync ([safe, f] { if (safe != nullptr) safe->loadInsertFile (f); });
        },
        [safe]
        {
            juce::MessageManager::callAsync ([safe]
            {
                if (safe == nullptr) return;
                // Browse for any .vst3, starting in the standard VST3 folder
                auto start = juce::File::getSpecialLocation (juce::File::globalApplicationsDirectory);
               #if JUCE_WINDOWS
                start = juce::File ("C:\\Program Files\\Common Files\\VST3");
               #elif JUCE_MAC
                start = juce::File ("/Library/Audio/Plug-Ins/VST3");
               #endif
                safe->insertChooser = std::make_unique<juce::FileChooser> ("Choose a VST3 plugin", start, "*.vst3");
                safe->insertChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                                  [safe] (const juce::FileChooser& fc)
                                                  {
                                                      if (safe != nullptr && fc.getResult() != juce::File())
                                                          safe->loadInsertFile (fc.getResult());
                                                  });
            });
        });
    browser->setLookAndFeel (&lnf);
    browser->makerOf = [] (const juce::String& path) { juce::String m; makers().lookup (path, m); return m; };
    browser->reading = ! unknown.isEmpty();
    auto* browserPtr = browser.get();
    auto& box = juce::CallOutBox::launchAsynchronously (std::move (browser), insertLoad.getBounds(), this);
    box.setLookAndFeel (&lnf);
    insertBrowserBox = &box;   // closed with the window if still open
    insertBrowser = browserPtr;

    // Read the makers we don't know yet, without blocking (Windows scans each plugin file).
    if (! unknown.isEmpty() && (makerThread == nullptr || ! makerThread->isThreadRunning()))
    {
        juce::Component::SafePointer<BassChainEditor> safeEd (this);
        makerThread = std::make_unique<MakerScanThread> (unknown, [safeEd] (bool finished)
        {
            juce::MessageManager::callAsync ([safeEd, finished]
            {
                if (finished) makers().save();
                if (safeEd == nullptr) return;
                if (auto* b = dynamic_cast<InsertBrowser*> (safeEd->insertBrowser.getComponent()))
                    b->refreshMakers (! finished);
            });
        });
        makerThread->startThread (juce::Thread::Priority::low);
    }
}

void BassChainEditor::loadInsertFile (const juce::File& file)
{
    juce::String error;
    if (proc.loadInsert (file, error))
    {
        if (auto* prm = proc.apvts.getParameter ("insertOn"))   // adding a plugin switches the insert on
            prm->setValueNotifyingHost (1.0f);
        openInsertWindow();   // show the new plugin straight away
    }
    else
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Couldn't load the plugin", error, "OK", this);
}

void BassChainEditor::openInsertWindow()
{
    auto* inst = proc.getInsert();
    if (inst == nullptr) return;
    if (insertWindow != nullptr) { insertWindow->toFront (true); return; }

    juce::AudioProcessorEditor* ed = inst->hasEditor() ? inst->createEditorIfNeeded() : nullptr;
    if (ed == nullptr) ed = new juce::GenericAudioProcessorEditor (*inst);   // plugins without their own window

    juce::Component::SafePointer<BassChainEditor> safe (this);
    auto w = std::make_unique<InsertWindow> (inst->getName(), [safe]
    {
        // Close after the click has finished, not inside the window's own callback.
        juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->closeInsertWindow(); });
    });
    w->setContentOwned (ed, true);
    w->setResizable (ed->isResizable(), false);
    w->centreAroundComponent (this, w->getWidth(), w->getHeight());
    w->setVisible (true);
    insertWindow = std::move (w);
}

void BassChainEditor::closeInsertWindow()
{
    insertWindow.reset();   // deletes the hosted plugin's editor too
}

void BassChainEditor::applyAccentColours()
{
    lastAccentVersion = accentVersion;
    lnf.setColour (juce::Slider::rotarySliderFillColourId, accent);
    lnf.setColour (juce::ComboBox::arrowColourId, accent);
    lnf.setColour (juce::PopupMenu::highlightedBackgroundColourId, accent);
    saveButton.setColour (juce::TextButton::textColourOffId, accent);
    sendLookAndFeelChange();   // sliders/combos pick up the new look-and-feel colours
    repaint();                 // everything else reads the accent when it paints
}

void BassChainEditor::showColourMenu()
{
    juce::PopupMenu m;
    m.setLookAndFeel (&lnf);
    m.addSectionHeader ("ACCENT COLOR");
    for (int i = 0; i < kNumAccents; ++i)
        m.addItem (i + 1, juce::String (kAccents[i].name).toUpperCase(), true, i == accentIndex);

    juce::Component::SafePointer<BassChainEditor> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&colourButton),
                     [safe] (int result)
                     {
                         if (safe == nullptr || result <= 0) return;
                         setAccent (result - 1, true);
                         safe->applyAccentColours();
                     });
}

void BassChainEditor::applySwap (bool s)
{
    swapped = s;
    harmonics.setNumber (s ? "3" : "2");
    pultec.setNumber    (s ? "2" : "3");
    if (getWidth() > 0) resized();
    repaint();
}

//------------------------------------------------------------------------------
// Presets
void BassChainEditor::refreshPresetList (int selectSlot)
{
    presetBox.clear (juce::dontSendNotification);
    presetBox.addItem ("-INIT-", kInitPresetId);   // resets everything to the defaults
    presetBox.addSeparator();
    for (int s = 0; s < BassChainProcessor::kNumPresets; ++s)
    {
        const auto name = proc.presetName (s);
        presetBox.addItem (juce::String (s + 1) + "   " + (name.isEmpty() ? juce::String ("(EMPTY)") : name.toUpperCase()), s + 1);
        presetBox.setItemEnabled (s + 1, name.isNotEmpty());   // empty slots can't be loaded
    }
    if (selectSlot >= 0)
        presetBox.setSelectedId (selectSlot + 1, juce::dontSendNotification);
}

void BassChainEditor::presetChosen()
{
    if (presetBox.getSelectedId() == kInitPresetId) { proc.resetToDefaults(); return; }
    const int slot = presetBox.getSelectedId() - 1;
    if (slot >= 0 && ! proc.loadPreset (slot))
        presetBox.setSelectedId (0, juce::dontSendNotification);
}

void BassChainEditor::showSaveDialog()
{
    auto* w = new juce::AlertWindow ("Save preset", "Name it and choose one of the 9 slots.",
                                     juce::MessageBoxIconType::NoIcon, this);
    w->setLookAndFeel (&lnf);

    const int current = presetBox.getSelectedId() - 1;
    const bool onSlot = current >= 0 && current < BassChainProcessor::kNumPresets;   // not on -init-
    w->addTextEditor ("name", onSlot ? proc.presetName (current) : juce::String(), "Preset name:");

    juce::StringArray slots;
    int firstEmpty = -1;
    for (int s = 0; s < BassChainProcessor::kNumPresets; ++s)
    {
        const auto name = proc.presetName (s);
        if (name.isEmpty() && firstEmpty < 0) firstEmpty = s;
        slots.add ("SLOT " + juce::String (s + 1) + (name.isEmpty() ? juce::String ("  (EMPTY)") : "  (REPLACE \"" + name.toUpperCase() + "\")"));
    }
    w->addComboBox ("slot", slots, "Save to:");
    w->getComboBoxComponent ("slot")->setSelectedItemIndex (onSlot ? current : juce::jmax (0, firstEmpty));

    w->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    juce::Component::SafePointer<BassChainEditor> safeThis (this);
    w->enterModalState (true, juce::ModalCallbackFunction::create ([safeThis, w] (int result)
    {
        if (result == 1 && safeThis != nullptr)
        {
            const int slot = w->getComboBoxComponent ("slot")->getSelectedItemIndex();
            const auto name = w->getTextEditorContents ("name");
            if (safeThis->proc.savePreset (slot, name))
                safeThis->refreshPresetList (slot);
        }
        w->setLookAndFeel (nullptr);
    }), true);
}

BassChainEditor::~BassChainEditor()
{
    stopTimer();
    proc.onInsertAboutToChange = nullptr;
    proc.onInsertChanged = nullptr;
    closeInsertWindow();
    // Close the plugin browser if it is still open. JUCE's pop-up system owns and deletes it,
    // so dismiss it rather than deleting it here (and stop it using this window's look-and-feel).
    if (auto* box = insertBrowserBox.getComponent())
    {
        if (auto* b = insertBrowser.getComponent()) b->setLookAndFeel (nullptr);
        box->setLookAndFeel (nullptr);
        box->exitModalState (0);
    }
    if (makerThread != nullptr)
    {
        makerThread->stopThread (4000);        // stop reading makers; keep what was read
        makers().save();
    }
    sliderAtts.clear();
    buttonAtts.clear();
    comboAtts.clear();
    setLookAndFeel (nullptr);
}

std::unique_ptr<ui::LabeledControl> BassChainEditor::knob (const char* id, const juce::String& caption)
{
    auto s = std::make_unique<juce::Slider> (juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow);
    s->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 80, 20);
    if (juce::String (id).endsWith ("Thresh"))
        s->getProperties().set ("fillRight", true);   // gold arc from the pointer to the right end
    sliderAtts.push_back (std::make_unique<APVTS::SliderAttachment> (proc.apvts, id, *s));
    return std::make_unique<ui::LabeledControl> (caption, std::move (s), false);
}

std::unique_ptr<ui::LabeledControl> BassChainEditor::toggle (const char* id, const juce::String& caption,
                                                              const juce::String& offText, const juce::String& onText)
{
    auto b = std::make_unique<ui::SwitchButton> (offText, onText);
    buttonAtts.push_back (std::make_unique<APVTS::ButtonAttachment> (proc.apvts, id, *b));
    return std::make_unique<ui::LabeledControl> (caption, std::move (b), true);
}

std::unique_ptr<ui::LabeledControl> BassChainEditor::choice (const char* id, const juce::String& caption)
{
    auto box = std::make_unique<juce::ComboBox>();
    if (auto* param = dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter (id)))
        box->addItemList (allCaps (param->choices), 1);
    box->setJustificationType (juce::Justification::centred);
    comboAtts.push_back (std::make_unique<APVTS::ComboBoxAttachment> (proc.apvts, id, *box));
    return std::make_unique<ui::LabeledControl> (caption, std::move (box), true);
}

void BassChainEditor::bindEnable (ui::ModulePanel& panel, const char* id)
{
    buttonAtts.push_back (std::make_unique<APVTS::ButtonAttachment> (proc.apvts, id, panel.enable));
    panel.enable.onStateChange = [&panel] { panel.repaint(); };
}

void BassChainEditor::paint (juce::Graphics& g)
{
    g.fillAll (bg);

    g.setColour (accent);
    g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    g.drawText ("BYPASS",    capBypass,   juce::Justification::centredRight);
    g.drawText ("INPUT HPF", capHpf,      juce::Justification::centredRight);
    g.drawText ("INPUT LPF", capLpf,      juce::Justification::centredRight);
    g.drawText ("TIMING",    capTiming,   juce::Justification::centredRight);
    g.drawText ("PRESET",    capPreset,   juce::Justification::centredRight);
    g.drawText ("HARMONIC/EQ SWAP", capSwap, juce::Justification::centredRight);
    g.drawText ("AUTO GAIN", capAutoGain, juce::Justification::centredRight);

    // Thin divider under the top bar
    g.setColour (edge);
    g.fillRect (hdrDivider);
}

void BassChainEditor::resized()
{
    auto r = getLocalBounds().reduced (10, 0);
    auto top = r.removeFromTop (52).reduced (8, 4);
    hdrDivider = { r.getX() + 8, r.getY() - 1, r.getWidth() - 16, 1 };

    // One row. Right side (right to left): presets, auto gain, then the harmonics/EQ order switch.
    colourButton.setBounds (top.removeFromRight (24).withSizeKeepingCentre (24, 28));
    top.removeFromRight (4);
    saveButton.setBounds (top.removeFromRight (58).withSizeKeepingCentre (58, 28));
    top.removeFromRight (6);
    presetBox.setBounds (top.removeFromRight (170).withSizeKeepingCentre (170, 30));
    capPreset = top.removeFromRight (52);
    top.removeFromRight (14);
    autoGainButton.setBounds (top.removeFromRight (26).withSizeKeepingCentre (26, 26));
    capAutoGain = top.removeFromRight (72);
    top.removeFromRight (8);
    swapLed.setBounds (top.removeFromRight (26).withSizeKeepingCentre (26, 26));   // harmonics / EQ order
    capSwap = top.removeFromRight (130);

    // Left side (left to right): bypass light, input HPF, input LPF, timing + project tempo.
    capBypass = top.removeFromLeft (56);
    bypassButton.setBounds (top.removeFromLeft (26).withSizeKeepingCentre (26, 26));
    top.removeFromLeft (8);
    capHpf = top.removeFromLeft (70);
    inHpfSlider.setBounds (top.removeFromLeft (150).withSizeKeepingCentre (150, 26));
    top.removeFromLeft (8);
    capLpf = top.removeFromLeft (70);
    inLpfSlider.setBounds (top.removeFromLeft (160).withSizeKeepingCentre (160, 26));
    top.removeFromLeft (8);
    capTiming = top.removeFromLeft (50);
    timingBox.setBounds (top.removeFromLeft (100).withSizeKeepingCentre (100, 30));
    top.removeFromLeft (6);
    bpmLabel.setBounds (top.removeFromLeft (84).withSizeKeepingCentre (84, 28));

    r.removeFromBottom (8);

    analyzer.setBounds (r.removeFromTop (r.getHeight() * 42 / 100));
    {
        // Insert strip over the analyzer's top-left corner
        auto strip = analyzer.getBounds().reduced (16, 10).removeFromTop (26);
        insertCaption.setBounds (strip.removeFromLeft (54));
        insertLed.setBounds (strip.removeFromLeft (26).withSizeKeepingCentre (26, 26));
        strip.removeFromLeft (6);
        insertLoad.setBounds (strip.removeFromLeft (210));
        strip.removeFromLeft (4);
        insertOpen.setBounds (strip.removeFromLeft (56));
        strip.removeFromLeft (4);
        insertRemove.setBounds (strip.removeFromLeft (74));
        strip.removeFromLeft (8);
        insertPosBox.setBounds (strip.removeFromLeft (200));
    }

    // One row of stages, in signal order. The Harmonics and Vintage EQ trade places when swapped.
    auto row = r;
    const int w = row.getWidth();
    comp.setBounds (row.removeFromLeft (w * 14 / 100));
    if (swapped)
    {
        pultec.setBounds    (row.removeFromLeft (w * 29 / 100));
        harmonics.setBounds (row.removeFromLeft (w * 24 / 100));
    }
    else
    {
        harmonics.setBounds (row.removeFromLeft (w * 24 / 100));
        pultec.setBounds    (row.removeFromLeft (w * 29 / 100));
    }
    limiter.setBounds (row.removeFromLeft (w * 10 / 100));
    stereo.setBounds  (row.removeFromLeft (w * 9 / 100));
    filters.setBounds (row);
}
