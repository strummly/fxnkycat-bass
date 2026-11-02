#pragma once

// All of the signal processing for fxnkycat bass lives here.
// Everything is allocation-free on the audio thread (buffers are sized in prepare()).

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace vc
{
constexpr int    kMaxCh = 2;
constexpr double kPi    = 3.14159265358979323846;

inline float dbToGain (float db)  { return std::pow (10.0f, db * 0.05f); }
inline float gainToDb (float g)   { return 20.0f * std::log10 (std::max (g, 1.0e-9f)); }

// How much a stage changed the sound, ignoring any plain volume change: the energy left
// in the output after taking away the best-matched copy of the input, in dB relative to
// the input. Pure gain = -inf; added harmonics, echoes, reverb or tone changes show up.
// eIn = sum of in^2, eOut = sum of out^2, cross = sum of in*out. -200 for near-silence.
inline float effectDb (double eIn, double eOut, double cross, int samples)
{
    if (eIn / std::max (1, samples) < 1.0e-7) return -200.0f;   // below about -70 dB
    const double residual = std::max (0.0, eOut - cross * cross / eIn);
    return 10.0f * std::log10 ((float) (residual / eIn) + 1.0e-12f);
}
inline float timeCoef (double seconds, double fs) { return (float) std::exp (-1.0 / (seconds * fs)); }

// One-pole smoother step: rise with 'att', fall with 'rel'.
inline float follow (float current, float target, float att, float rel)
{
    const float c = target > current ? att : rel;
    return c * current + (1.0f - c) * target;
}


//==============================================================================
// RBJ biquad, transposed direct form II.
struct Biquad
{
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1[kMaxCh] {}, z2[kMaxCh] {};

    void reset() { for (int c = 0; c < kMaxCh; ++c) z1[c] = z2[c] = 0.0f; }

    void set (double B0, double B1, double B2, double A0, double A1, double A2)
    {
        b0 = (float) (B0 / A0); b1 = (float) (B1 / A0); b2 = (float) (B2 / A0);
        a1 = (float) (A1 / A0); a2 = (float) (A2 / A0);
    }

    static double clampFreq (double f, double fs) { return std::clamp (f, 2.0, 0.45 * fs); }

    void makePeak (double fs, double f, double q, double db)
    {
        const double A = std::pow (10.0, db / 40.0), w = 2.0 * kPi * clampFreq (f, fs) / fs;
        const double cw = std::cos (w), alpha = std::sin (w) / (2.0 * q);
        set (1 + alpha * A, -2 * cw, 1 - alpha * A, 1 + alpha / A, -2 * cw, 1 - alpha / A);
    }

    void makeLowShelf (double fs, double f, double q, double db)
    {
        const double A = std::pow (10.0, db / 40.0), w = 2.0 * kPi * clampFreq (f, fs) / fs;
        const double cw = std::cos (w), alpha = std::sin (w) / (2.0 * q), s = 2.0 * std::sqrt (A) * alpha;
        set (A * ((A + 1) - (A - 1) * cw + s),
             2 * A * ((A - 1) - (A + 1) * cw),
             A * ((A + 1) - (A - 1) * cw - s),
             (A + 1) + (A - 1) * cw + s,
             -2 * ((A - 1) + (A + 1) * cw),
             (A + 1) + (A - 1) * cw - s);
    }

    void makeHighShelf (double fs, double f, double q, double db)
    {
        const double A = std::pow (10.0, db / 40.0), w = 2.0 * kPi * clampFreq (f, fs) / fs;
        const double cw = std::cos (w), alpha = std::sin (w) / (2.0 * q), s = 2.0 * std::sqrt (A) * alpha;
        set (A * ((A + 1) + (A - 1) * cw + s),
             -2 * A * ((A - 1) + (A + 1) * cw),
             A * ((A + 1) + (A - 1) * cw - s),
             (A + 1) - (A - 1) * cw + s,
             2 * ((A - 1) - (A + 1) * cw),
             (A + 1) - (A - 1) * cw - s);
    }

    void makeHighPass (double fs, double f, double q)
    {
        const double w = 2.0 * kPi * clampFreq (f, fs) / fs;
        const double cw = std::cos (w), alpha = std::sin (w) / (2.0 * q);
        set ((1 + cw) / 2, -(1 + cw), (1 + cw) / 2, 1 + alpha, -2 * cw, 1 - alpha);
    }

    void makeLowPass (double fs, double f, double q)
    {
        const double w = 2.0 * kPi * clampFreq (f, fs) / fs;
        const double cw = std::cos (w), alpha = std::sin (w) / (2.0 * q);
        set ((1 - cw) / 2, 1 - cw, (1 - cw) / 2, 1 + alpha, -2 * cw, 1 - alpha);
    }

    inline float process (float x, int c)
    {
        const float y = b0 * x + z1[c];
        z1[c] = b1 * x - a1 * y + z2[c];
        z2[c] = b2 * x - a2 * y;
        return y;
    }
};

// Soft-knee compressor curve. Returns gain reduction in dB (positive number).
inline float computeGR (float levelDb, float threshDb, float ratio, float kneeDb)
{
    const float over  = levelDb - threshDb;
    const float slope = 1.0f - 1.0f / ratio;
    if (kneeDb > 0.0f && 2.0f * std::abs (over) <= kneeDb)
    {
        const float t = over + kneeDb * 0.5f;
        return slope * t * t / (2.0f * kneeDb);
    }
    return over > 0.0f ? slope * over : 0.0f;
}

//==============================================================================
// 1. Opto compressor (LA-2A style), stereo-linked.
//   - Peak Reduction knob (0-100) sets the threshold (0 to -40 dB); ratio 3:1 or 4:1,
//     6 dB soft knee, RMS detection (10 ms).
//   - Attack 50 ms.
//   - Two-stage, program-dependent release like an opto cell, timed from the tempo:
//     a fast part over 1/2 of the Timing note (1/8 note at the default 1/4), plus a
//     slow part that builds up during long, sustained compression (~1 s) and lets go
//     over 4 x the note (one bar in 4/4).
//   - Gain matched: the output is kept as loud as the input (~1 s averages, 0 to
//     +24 dB, held during silence), so it only evens the level out.
struct OptoComp
{
    static constexpr float kKneeDb = 6.0f, kMaxMakeupDb = 24.0f;

    double fs = 44100, lastUnit = -1;
    float rmsMs = 0, rmsC = 0;
    float fastGr = 0, slowGr = 0, attC = 0, relFastC = 0, slowAttC = 0, relSlowC = 0;
    float inAvg = 0, outAvg = 0, avgC = 0, makeupDb = 0, mkC = 0;
    float meter = 0;   // most gain reduction (dB) since last read

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        rmsC = timeCoef (0.010, fs);
        attC = timeCoef (0.050, fs);
        slowAttC = timeCoef (1.0, fs);
        avgC = timeCoef (1.0, fs);
        mkC  = timeCoef (0.5, fs);
        lastUnit = -1;
        setTiming (0.5);
        rmsMs = fastGr = slowGr = inAvg = outAvg = makeupDb = meter = 0;
    }

    // 'unit' = the top-bar Timing note in seconds.
    void setTiming (double unit)
    {
        if (unit == lastUnit) return;
        lastUnit = unit;
        relFastC = timeCoef (std::max (0.02, unit * 0.5), fs);
        relSlowC = timeCoef (std::max (0.2,  unit * 4.0), fs);
    }

    void process (float* const* ch, int nch, int n, float peakReduction, float ratio)
    {
        const float threshDb = -0.4f * std::clamp (peakReduction, 0.0f, 100.0f);
        for (int i = 0; i < n; ++i)
        {
            float p = 0;
            for (int c = 0; c < nch; ++c) p += ch[c][i] * ch[c][i];
            p /= (float) nch;

            rmsMs = rmsC * rmsMs + (1.0f - rmsC) * p;
            const float lvl = 10.0f * std::log10 (rmsMs + 1.0e-12f);
            const float target = computeGR (lvl, threshDb, ratio, kKneeDb);

            fastGr = target > fastGr ? attC * fastGr + (1.0f - attC) * target
                                     : relFastC * fastGr + (1.0f - relFastC) * target;
            slowGr = fastGr > slowGr ? slowAttC * slowGr + (1.0f - slowAttC) * fastGr
                                     : relSlowC * slowGr + (1.0f - relSlowC) * fastGr;
            const float grDb = std::max (fastGr, slowGr);
            meter = std::max (meter, grDb);

            const float g = dbToGain (-grDb);

            // Gain match: keep the output as loud as the input (held during silence).
            if (lvl > -60.0f)
            {
                inAvg  = avgC * inAvg  + (1.0f - avgC) * p;
                outAvg = avgC * outAvg + (1.0f - avgC) * p * g * g;
            }
            const float mkTarget = (inAvg > 1.0e-9f && outAvg > 1.0e-9f)
                                     ? std::clamp (10.0f * std::log10 (inAvg / outAvg), 0.0f, kMaxMakeupDb) : 0.0f;
            makeupDb = mkC * makeupDb + (1.0f - mkC) * mkTarget;

            const float total = g * dbToGain (makeupDb);
            for (int c = 0; c < nch; ++c) ch[c][i] *= total;
        }
    }
};

//==============================================================================
// 3. Vintage EQ (Pultec EQP-1A style).
// Knob values are 0-10 like the hardware. The low attenuation shelf sits above the
// boost shelf, so boosting and cutting together gives the classic low-end trick
// (more sub, less mud just above it).
struct PultecEQ
{
    static constexpr float kLowFreqs[]     = { 20.0f, 30.0f, 60.0f, 100.0f };
    static constexpr float kHighFreqs[]    = { 3000.0f, 4000.0f, 5000.0f, 8000.0f, 10000.0f, 12000.0f, 16000.0f };
    static constexpr float kHighAttFreqs[] = { 5000.0f, 10000.0f, 20000.0f };

    Biquad lowBoost, lowAtten, highBoost, highAtten;
    double fs = 44100;
    float last[8] {};
    bool  dirty = true;

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        lowBoost.reset(); lowAtten.reset(); highBoost.reset(); highAtten.reset();
        dirty = true;
    }

    void update (int lowFreqIdx, float lowBoostAmt, float lowAttenAmt,
                 int highFreqIdx, float highBoostAmt, float bandwidth,
                 int highAttFreqIdx, float highAttenAmt)
    {
        const float now[8] = { (float) lowFreqIdx, lowBoostAmt, lowAttenAmt, (float) highFreqIdx,
                               highBoostAmt, bandwidth, (float) highAttFreqIdx, highAttenAmt };
        if (! dirty && std::equal (now, now + 8, last)) return;
        std::copy (now, now + 8, last);
        dirty = false;

        const double lf = kLowFreqs[std::clamp (lowFreqIdx, 0, 3)];
        const double hf = kHighFreqs[std::clamp (highFreqIdx, 0, 6)];
        const double af = kHighAttFreqs[std::clamp (highAttFreqIdx, 0, 2)];

        lowBoost.makeLowShelf (fs, lf, 0.8, lowBoostAmt * 1.35);          // up to +13.5 dB
        lowAtten.makeLowShelf (fs, lf * 2.2, 0.5, -lowAttenAmt * 1.0);     // up to -10 dB

        // Bandwidth: 0 = sharp (Q 2.5), 10 = broad (Q 0.4)
        const double q = 2.5 * std::pow (0.4 / 2.5, bandwidth / 10.0);
        highBoost.makePeak (fs, hf, q, highBoostAmt * 1.6);                // up to +16 dB
        highAtten.makeHighShelf (fs, af, 0.6, -highAttenAmt * 1.6);        // up to -16 dB
    }

    // Energy of the input, and of what each of the four sections adds (boosts) or
    // takes away (attenuators), summed since the last takeMeters() (for the knob meters).
    double eIn = 0, eLowAdd = 0, eLowCut = 0, eHighAdd = 0, eHighCut = 0;

    void process (float* const* ch, int nch, int n)
    {
        for (int c = 0; c < nch; ++c)
        {
            float* d = ch[c];
            for (int i = 0; i < n; ++i)
            {
                float x = d[i];
                eIn += (double) x * x;
                const float lb = lowBoost.process (x, c);
                eLowAdd += (double) (lb - x) * (lb - x);
                x = lowAtten.process (lb, c);
                eLowCut += (double) (x - lb) * (x - lb);
                const float hb = highBoost.process (x, c);
                eHighAdd += (double) (hb - x) * (hb - x);
                x = highAtten.process (hb, c);
                eHighCut += (double) (x - hb) * (x - hb);
                d[i] = x;
            }
        }
    }

    // How loud the signal each section adds or removes is, in dB relative to the input
    // (-200 if silent). Resets the sums.
    void takeMeters (float& lowBoostDb, float& lowAttenDb, float& highBoostDb, float& highAttenDb, int samples)
    {
        auto db = [&] (double e) { return 10.0f * std::log10 ((float) (e / eIn) + 1.0e-12f); };
        if (eIn / std::max (1, samples) < 1.0e-7) lowBoostDb = lowAttenDb = highBoostDb = highAttenDb = -200.0f;   // near-silence
        else { lowBoostDb = db (eLowAdd); lowAttenDb = db (eLowCut); highBoostDb = db (eHighAdd); highAttenDb = db (eHighCut); }
        eIn = eLowAdd = eLowCut = eHighAdd = eHighCut = 0;
    }
};

//==============================================================================
// Tape saturation (stage 2, Harmonics: the main path, "Tape" knob).
// Drive pushes the signal into an asymmetric soft clipper (tape-style even +
// odd harmonics), with a gentle low "head bump" and high-frequency roll-off that
// both grow with the amount. Output level is compensated so louder != better.
struct TapeSaturation
{
    Biquad headBump, hfLoss;
    double fs = 44100;
    float  lastAmount = -1;
    float  dcX[kMaxCh] {}, dcY[kMaxCh] {}, dcR = 0.995f;

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        headBump.reset(); hfLoss.reset();
        lastAmount = -1; lastWet = -1;
        dcR = 1.0f - (float) (2.0 * kPi * 10.0 / fs);   // 10 Hz DC blocker
        for (int c = 0; c < kMaxCh; ++c) dcX[c] = dcY[c] = 0;
    }

    static inline float shape (float x)
    {
        // Asymmetric soft clip: slightly different curve for positive/negative.
        return x >= 0.0f ? std::tanh (x) : std::tanh (x * 0.9f) / 0.9f * 0.95f;
    }

    // amount: 0-100 %.
    // Normal: amount sets how hard the tape is driven.
    // Overdrive: the tape is driven flat out (full saturation + 18 dB more drive, full
    // head bump and roll-off) and amount becomes a dry/wet mix of that driven sound.
    void process (float* const* ch, int nch, int n, float amount, bool overdrive)
    {
        const float knob = std::clamp (amount * 0.01f, 0.0f, 1.0f);
        if (knob <= 0.0f) { lastWet = 0.0f; return; }   // nothing to add (dry only)

        const float a      = overdrive ? 1.0f : knob;      // drive amount
        const float wetMix = overdrive ? knob : 1.0f;
        const float colour = a;                            // tone shaping amount
        const float key = colour;
        if (key != lastAmount)
        {
            lastAmount = key;
            headBump.makePeak (fs, 90.0, 0.9, 2.0 * colour);           // up to +2 dB at 90 Hz
            hfLoss.makeHighShelf (fs, 9000.0, 0.7, -4.0 * colour);     // up to -4 dB above ~9 kHz
        }

        const float driveDb = 18.0f * a + (overdrive ? 18.0f : 0.0f);
        const float drive = dbToGain (driveDb);                      // into the "tape"
        const float comp  = 1.0f / shape (drive * 0.25f) * 0.25f;    // keep level roughly constant

        // Glide the mix across the block so turning the knob doesn't click.
        const float wet0 = lastWet < 0.0f ? wetMix : lastWet;
        lastWet = wetMix;

        for (int c = 0; c < nch; ++c)
        {
            float* d = ch[c];
            for (int i = 0; i < n; ++i)
            {
                const float dry = d[i];
                float x = headBump.process (dry, c);
                x = shape (x * drive) * comp;
                // remove DC created by the asymmetric curve
                const float y = x - dcX[c] + dcR * dcY[c];
                dcX[c] = x; dcY[c] = y;
                x = hfLoss.process (y, c);
                const float w = wet0 + (wetMix - wet0) * (float) (i + 1) / (float) n;
                d[i] = dry + (x - dry) * w;
            }
        }
    }

    float lastWet = -1.0f;
};

//==============================================================================
// 6. Filters: high-pass (0 Hz = off) + low-pass, 24 dB/octave each (two cascaded Butterworth stages).
struct PassFilters
{
    static constexpr double kQ1 = 0.5411961, kQ2 = 1.3065630;  // 4th-order Butterworth

    Biquad hp1, hp2, lp1, lp2;
    double fs = 44100;
    float lastHp = -1, lastLp = -1;

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        hp1.reset(); hp2.reset(); lp1.reset(); lp2.reset();
        lastHp = lastLp = -1;
    }

    void process (float* const* ch, int nch, int n, float hpHz, float lpHz)
    {
        const bool useHp = hpHz >= 1.0f;   // 0 Hz = no high-pass
        if (hpHz != lastHp) { lastHp = hpHz; hp1.makeHighPass (fs, hpHz, kQ1); hp2.makeHighPass (fs, hpHz, kQ2); }
        if (lpHz != lastLp) { lastLp = lpHz; lp1.makeLowPass (fs, lpHz, kQ1);  lp2.makeLowPass (fs, lpHz, kQ2); }

        for (int c = 0; c < nch; ++c)
        {
            float* d = ch[c];
            for (int i = 0; i < n; ++i)
            {
                eIn += (double) d[i] * d[i];
                const float x = useHp ? hp2.process (hp1.process (d[i], c), c) : d[i];
                eMid += (double) x * x;
                d[i] = lp2.process (lp1.process (x, c), c);
                eOut += (double) d[i] * d[i];
            }
        }
    }

    // How much each filter takes away (energy lost, so phase shifts don't count), in dB
    // relative to the input; -200 if nothing or silent. Resets the sums.
    double eIn = 0, eMid = 0, eOut = 0;
    void takeMeters (float& hpDb, float& lpDb, int samples)
    {
        auto lost = [&] (double before, double after)
        {
            return eIn / std::max (1, samples) < 1.0e-7 || before <= after ? -200.0f
                 : 10.0f * std::log10 ((float) ((before - after) / eIn) + 1.0e-12f);
        };
        hpDb = lost (eIn, eMid);
        lpDb = lost (eMid, eOut);
        eIn = eMid = eOut = 0;
    }
};

//==============================================================================
// Input high-pass (top bar), 24 dB/octave, before stage 1. Bypassed at its lowest setting.
struct InputHighPass
{
    Biquad s1, s2;
    double fs = 44100;
    float last = -1;

    void prepare (double sampleRate) { fs = sampleRate; s1.reset(); s2.reset(); last = -1; }

    void process (float* const* ch, int nch, int n, float hz)
    {
        if (hz != last)
        {
            last = hz;
            s1.makeHighPass (fs, hz, PassFilters::kQ1);
            s2.makeHighPass (fs, hz, PassFilters::kQ2);
        }
        for (int c = 0; c < nch; ++c)
            for (int i = 0; i < n; ++i)
                ch[c][i] = s2.process (s1.process (ch[c][i], c), c);
    }
};

//==============================================================================
// Input low-pass (top bar, right of the high-pass), 24 dB/octave, before stage 1.
// Bypassed at its highest setting.
struct InputLowPass
{
    Biquad s1, s2;
    double fs = 44100;
    float last = -1;

    void prepare (double sampleRate) { fs = sampleRate; s1.reset(); s2.reset(); last = -1; }

    void process (float* const* ch, int nch, int n, float hz)
    {
        if (hz != last)
        {
            last = hz;
            s1.makeLowPass (fs, hz, PassFilters::kQ1);
            s2.makeLowPass (fs, hz, PassFilters::kQ2);
        }
        for (int c = 0; c < nch; ++c)
            for (int i = 0; i < n; ++i)
                ch[c][i] = s2.process (s1.process (ch[c][i], c), c);
    }
};

//==============================================================================
// Stage 2, Harmonics: three paths that each take the stage's input on their own and
// are then added together:  out = Tape(in) + Sub * SubOctave(in) + Overdrive * FundDrive(in).
// Tape is the main path (at 0 it is the dry signal). SubOctave and FundDrive return
// only their own new layer ("wet only"), which the knob blends in. Both layers are made
// from the fundamental alone (the note itself, not the string's upper harmonics).

// Fundamental tracker shared by the two layers. Works on the mid (L+R).
//   - The fundamental is isolated with a 300 Hz low-pass (24 dB/oct).
//   - Its level is followed (1 ms up, 50 ms down).
//   - Rising zero crossings (re-armed only after the wave has gone below -25 % of its
//     level, so the string's harmonics can't add extra crossings) give the note's
//     period, to a fraction of a sample. Periods outside 25-400 Hz are ignored.
struct FundamentalTracker
{
    Biquad lp1, lp2;
    float  f = 0, prev = 0, env = 0, envAtt = 0, envRel = 0;
    bool   armed = false;
    double sinceCross = 0;   // samples since the last rising crossing
    double period = 0;       // last good period (samples), 0 = none yet
    bool   crossed = false;  // a rising crossing happened on this sample
    double crossFrac = 0;    // how far back in this sample it happened (0-1)
    double fs = 44100;

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        lp1.makeLowPass (fs, 300.0, PassFilters::kQ1); lp2.makeLowPass (fs, 300.0, PassFilters::kQ2);
        lp1.reset(); lp2.reset();
        f = prev = env = 0; armed = false; sinceCross = 0; period = 0; crossed = false;
        envAtt = timeCoef (0.001, fs); envRel = timeCoef (0.050, fs);
    }

    void step (float x)
    {
        prev = f;
        f = lp2.process (lp1.process (x, 0), 0);
        env = follow (env, std::abs (f), envAtt, envRel);
        sinceCross += 1.0;
        crossed = false;

        if (f < -0.25f * env) armed = true;
        if (armed && prev < 0.0f && f >= 0.0f && env > 1.0e-4f)
        {
            armed = false;
            crossFrac = (double) f / ((double) f - (double) prev + 1.0e-12);   // 0 = exactly now
            const double p = sinceCross - crossFrac;
            if (p >= fs / 400.0 && p <= fs / 25.0) period = p;
            sinceCross = crossFrac;
            crossed = true;
        }
    }
};

// Sub octave layer (the "Sub Bass" knob, 0-100 % = how much is added).
// A pure sine exactly one octave below the fundamental, and nothing else: a sine
// oscillator runs at half the tracked note frequency and is phase-locked to every
// other zero crossing of the fundamental, so it follows the notes as they change.
// Its level follows the fundamental's, so it replicates the playing; at 100 % the sub
// is about as loud as the original fundamental. A shallow low-pass (6 dB/oct) at the
// Stereo stage's Mono Below frequency (200 / 250 / 300 Hz; it follows that setting even
// when the Stereo stage is off) rounds off the top of the sub. The same on both channels.
struct SubOctave
{
    FundamentalTracker trk;
    double phase = 0, fs = 44100;   // phase in cycles, 0-1
    float  amp = 0, ampC = 0;
    float  lpY = 0, lpA = 0, lastLp = -1;   // one-pole low-pass (6 dB/oct)
    bool   evenCross = false;

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        trk.prepare (sampleRate);
        phase = 0; amp = 0; evenCross = false;
        ampC = timeCoef (0.003, sampleRate);   // smooths the level (no zipper / clicks)
        lpY  = 0; lastLp = -1;
        setLowPass (250.0f);
    }

    void setLowPass (float hz)
    {
        if (hz == lastLp) return;
        lastLp = hz;
        lpA = (float) std::exp (-2.0 * kPi * hz / fs);
    }

    // In place: dry input -> sub octave layer only. lpHz = the Stereo stage's Mono Below frequency.
    void process (float* const* ch, int nch, int n, float lpHz)
    {
        setLowPass (lpHz);
        for (int i = 0; i < n; ++i)
        {
            float x = 0;
            for (int c = 0; c < nch; ++c) x += ch[c][i];
            trk.step (x / (float) nch);

            const double inc = trk.period > 0 ? 0.5 / trk.period : 0.0;   // half the note's frequency
            phase += inc;
            if (trk.crossed && trk.period > 0)
            {
                // Lock: every other crossing should land at phase 0, the ones between at 0.5
                // (plus the bit of this sample that has passed since the crossing).
                evenCross = ! evenCross;
                const double want = (evenCross ? 0.0 : 0.5) + trk.crossFrac * inc;
                double err = want - phase;
                err -= std::floor (err + 0.5);       // wrap to -0.5 .. 0.5
                phase += 0.25 * err;                 // gentle pull, so note changes glide in
            }
            phase -= std::floor (phase);

            amp = ampC * amp + (1.0f - ampC) * (trk.period > 0 ? trk.env : 0.0f);
            const float sine = amp * (float) std::sin (2.0 * kPi * phase);
            lpY = (1.0f - lpA) * sine + lpA * lpY;   // shallow low-pass at the Mono Below frequency
            for (int c = 0; c < nch; ++c) ch[c][i] = lpY;
        }
    }
};

// Saturation layer (the "Overdrive" knob, 0-100 % = how much is added).
// Only the fundamental is driven: it is taken at a steady level and pushed (+10.5 dB)
// into a soft, biased tube-style curve - between a gentle saturation and a hard
// overdrive - which brings out a clear harmonic series (strong warm 2nd, then 3rd, 5th...)
// while the high, scratchy harmonics stay low, so it stays clean rather than fuzzy.
// Only part of what comes out is kept (gentle 12 dB/oct filters each side):
//   - High-pass at the Stereo stage's Mono Below frequency (200 / 250 / 300 Hz), so the
//     layer stays out of the mono low end. It follows that setting even when the
//     Stereo stage is off.
//   - Low-pass that moves with the Overdrive knob: 2 kHz when the knob is barely up,
//     sliding down to 800 Hz at 100 %, so more saturation gets darker instead of fizzier.
// The string's own upper harmonics are left alone. The layer's level follows the
// fundamental, so it replicates the playing. The same on both channels.
struct FundDrive
{
    static constexpr double kLpTopHz = 2000.0, kLpBottomHz = 800.0;
    static constexpr float  kDriveDb = 10.5f;
    static constexpr float  kBias    = 0.3f;   // asymmetry -> warm even harmonics
    static constexpr float  kLevel   = 1.8f;   // at 100 %: the layer peaks about as high as the note (less on very low notes)

    FundamentalTracker trk;
    Biquad hp, lp;   // 12 dB/oct each
    double fs = 44100;
    float  lastHp = -1, lastLp = -1;

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        trk.prepare (sampleRate);
        hp.reset(); lp.reset();
        lastHp = lastLp = -1;
        setFilters (0.0f, 250.0f);
    }

    // mix 0-1 (the Overdrive knob), hpHz = the Stereo stage's Mono Below frequency.
    void setFilters (float mix, float hpHz)
    {
        const float lpHz = (float) (kLpTopHz * std::pow (kLpBottomHz / kLpTopHz, (double) std::clamp (mix, 0.0f, 1.0f)));
        if (hpHz != lastHp) { lastHp = hpHz; hp.makeHighPass (fs, hpHz, 0.70710678); }
        if (std::abs (lpHz - lastLp) > 0.5f) { lastLp = lpHz; lp.makeLowPass (fs, lpHz, 0.70710678); }
    }

    // Soft tube-style curve: biased tanh, centred and scaled so small signals pass at unity.
    static inline float soft (float x)
    {
        const float t0 = std::tanh (kBias);
        return (std::tanh (x + kBias) - t0) / (1.0f - t0 * t0);
    }

    // In place: dry input -> distorted-fundamental layer only.
    void process (float* const* ch, int nch, int n, float mix, float hpHz)
    {
        setFilters (mix, hpHz);
        const float drive = dbToGain (kDriveDb);
        for (int i = 0; i < n; ++i)
        {
            float x = 0;
            for (int c = 0; c < nch; ++c) x += ch[c][i];
            trk.step (x / (float) nch);

            const float u = trk.f / (trk.env + 1.0e-6f);              // steady level, ~ +/-1
            const float y = soft (u * drive);
            const float band = lp.process (hp.process (y, 0), 0);
            const float out = band * trk.env * kLevel;
            for (int c = 0; c < nch; ++c) ch[c][i] = out;
        }
    }
};

//==============================================================================
// 4. Limiter (after the Harmonics and Vintage EQ, in either HARMONIC/EQ SWAP order).
// Stereo-linked brickwall peak limiter with 2 ms look-ahead:
//   - every sample works out the gain it needs to stay under the threshold,
//   - the lowest of those over the look-ahead window is held, then averaged over the
//     same window, so the gain is fully down by the time the peak comes out of the
//     delay (no overshoot, smooth attack),
//   - release follows the song tempo (set from the top-bar Timing note).
// The Threshold knob pushes the level into the limiter: everything is turned up by
// as much as the threshold is turned down, so peaks always land at the -0.3 dBFS ceiling.
// The look-ahead delay is always there (also when the stage is off), so the plugin's
// latency never changes.
struct Limiter
{
    static constexpr float  kCeilingDb    = -0.3f;
    static constexpr double kLookaheadSec = 0.002;

    double fs = 44100, lastRel = -1;
    int    L = 2, D = 1;                         // window and delay (samples)
    std::vector<float> delayBuf[kMaxCh];
    int    dPos = 0;
    std::vector<float> dqV;                      // sliding minimum (monotonic queue)
    std::vector<long long> dqI;
    int    dqHead = 0, dqCount = 0;
    long long t = 0;
    std::vector<float> box;                      // moving average of the held gain
    int    boxPos = 0;
    double boxSum = 0;
    float  env = 1.0f, relC = 0, mix = 0;
    float  meter = 0;                            // most gain reduction (dB) since last read

    int latency() const { return D; }

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        L = std::max (2, (int) std::lround (kLookaheadSec * fs));
        D = L - 1;
        for (auto& b : delayBuf) b.assign ((size_t) D, 0.0f);
        dPos = 0;
        dqV.assign ((size_t) L + 1, 1.0f);
        dqI.assign ((size_t) L + 1, 0);
        dqHead = dqCount = 0; t = 0;
        box.assign ((size_t) L, 1.0f);
        boxPos = 0; boxSum = (double) L;
        env = 1.0f; mix = 0; meter = 0;
        lastRel = -1;
        setRelease (0.125);
    }

    void setRelease (double seconds)
    {
        if (seconds == lastRel) return;
        lastRel = seconds;
        relC = timeCoef (std::max (0.01, seconds), fs);
    }

    void process (float* const* ch, int nch, int n, float threshDb, bool on)
    {
        const float thr     = dbToGain (std::min (threshDb, 0.0f));
        const float ceiling = dbToGain (kCeilingDb);
        const float makeup  = ceiling / thr;
        const float m0 = mix, m1 = on ? 1.0f : 0.0f;   // fade in / out across the block when switched
        mix = m1;
        const int cap = (int) dqV.size();

        for (int i = 0; i < n; ++i)
        {
            float peak = 0;
            for (int c = 0; c < nch; ++c) peak = std::max (peak, std::abs (ch[c][i]));
            const float need = peak > thr ? thr / peak : 1.0f;

            // Lowest gain needed over the last L samples
            while (dqCount > 0 && dqV[(size_t) ((dqHead + dqCount - 1) % cap)] >= need) --dqCount;
            const int idx = (dqHead + dqCount) % cap;
            dqV[(size_t) idx] = need; dqI[(size_t) idx] = t; ++dqCount;
            while (dqI[(size_t) dqHead] <= t - L) { dqHead = (dqHead + 1) % cap; --dqCount; }
            const float held = dqV[(size_t) dqHead];
            ++t;

            // ...averaged over the same L samples (a smooth ramp that is fully down in time)
            boxSum += (double) held - (double) box[(size_t) boxPos];
            box[(size_t) boxPos] = held;
            if (++boxPos == L)
            {
                boxPos = 0;
                boxSum = 0; for (float v : box) boxSum += v;   // stop rounding drift
            }
            const float avg = std::min (1.0f, (float) (boxSum / (double) L));

            // Instant down (the ramp is already in avg), tempo-synced release back up
            env = avg < env ? avg : avg + (env - avg) * relC;
            if (on) meter = std::max (meter, -gainToDb (env));

            const float mm = m0 + (m1 - m0) * (float) (i + 1) / (float) n;
            const float g  = 1.0f + (env * makeup - 1.0f) * mm;
            for (int c = 0; c < nch; ++c)
            {
                float& slot = delayBuf[c][(size_t) dPos];
                const float delayed = slot;
                slot = ch[c][i];
                float y = delayed * g;
                if (mm > 0.0f) y = std::clamp (y, -ceiling, ceiling);   // safety net (also while fading in)
                ch[c][i] = y;
            }
            if (++dPos == D) dPos = 0;
        }
    }
};

//==============================================================================
// 5. Stereo (before the Filters): everything below the chosen frequency (200 / 250 /
// 300 Hz) is made mono and centred; everything above keeps its stereo width.
// Works on mid/side: the side signal goes through a 24 dB/oct Linkwitz-Riley high-pass
// (so the side below the frequency is removed), and the mid goes through the matching
// all-pass (LR4 low-pass + high-pass), so mid and side keep the same phase and the
// image above the crossover is unchanged. A mono track passes straight through.
struct MonoBelow
{
    static constexpr float kFreqs[] = { 200.0f, 250.0f, 300.0f };

    Biquad sHp1, sHp2, mLp1, mLp2, mHp1, mHp2, sLp1, sLp2;   // sLp: only measures what is removed
    double fs = 44100;
    int    lastIdx = -1;
    double eIn = 0, eRemoved = 0;

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        lastIdx = -1;
        for (auto* b : { &sHp1, &sHp2, &mLp1, &mLp2, &mHp1, &mHp2, &sLp1, &sLp2 }) b->reset();
        eIn = eRemoved = 0;
    }

    void process (float* const* ch, int nch, int n, int freqIdx)
    {
        if (nch < 2) return;
        freqIdx = std::clamp (freqIdx, 0, 2);
        if (freqIdx != lastIdx)
        {
            lastIdx = freqIdx;
            const double f = kFreqs[freqIdx];
            for (auto* b : { &sHp1, &sHp2, &mHp1, &mHp2 }) b->makeHighPass (fs, f, 0.70710678);
            for (auto* b : { &mLp1, &mLp2, &sLp1, &sLp2 }) b->makeLowPass  (fs, f, 0.70710678);
        }
        float* L = ch[0];
        float* R = ch[1];
        for (int i = 0; i < n; ++i)
        {
            const float m = 0.5f * (L[i] + R[i]), s = 0.5f * (L[i] - R[i]);
            eIn += (double) L[i] * L[i] + (double) R[i] * R[i];
            const float lowS = sLp2.process (sLp1.process (s, 0), 0);
            eRemoved += 2.0 * (double) lowS * lowS;
            const float mOut = mLp2.process (mLp1.process (m, 0), 0) + mHp2.process (mHp1.process (m, 0), 0);
            const float sOut = sHp2.process (sHp1.process (s, 0), 0);
            L[i] = mOut + sOut;
            R[i] = mOut - sOut;
        }
    }

    // How much low-end stereo was folded to the centre, in dB relative to the input.
    float takeMeter (int samples)
    {
        const float db = eIn / std::max (1, samples) < 1.0e-7 ? -200.0f
                       : 10.0f * std::log10 ((float) (eRemoved / eIn) + 1.0e-12f);
        eIn = eRemoved = 0;
        return db;
    }
};

} // namespace vc
