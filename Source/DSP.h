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

    static double clampFreq (double f, double fs) { return std::clamp (f, 10.0, 0.45 * fs); }

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

//==============================================================================
// 1. Rider (level rider in the style of NoiseWorks GainAim)
// Two parts, both stereo-linked:
//   - Ride: a loudness average of the singing only (gaps and breaths are left out)
//     moves the gain so the voice sits at the Target. Its speed comes from the
//     speed mode. While the voice is under the noise floor, the ride holds still,
//     so it never jumps up at the start of the next phrase.
//   - Noise floor: a fast (20 ms) level check. Anything under the floor (breaths,
//     room noise, bleed) is pulled down by as much as it is under, up to 12 dB.
// Speed modes:
//   Natural  - follows phrases (Timing note: 1x average, 1/4x down, 1x up), +/-12 dB
//   Balanced - slow, evens out sections (4x average, 1x down, 4x up), +/-12 dB
//   Fast     - follows words (50 ms / 10 ms / 50 ms), +/-12 dB
struct VolumeRider
{
    static constexpr float kFloorDepthDb = 12.0f;

    float ms = 0, fastMs = 0, rideDb = 0, floorDb = 0;
    float meter = 0;       // largest total gain change (dB, either direction) since last read
    float overMeter = 0;   // most the level went over the noise floor (dB) since last read
    float rmsC = 0, downC = 0, upC = 0, maxRangeDb = 12.0f;
    float fastC = 0, floorDownC = 0, floorUpC = 0;
    double fs = 44100, lastBeat = -1;
    int    lastMode = -1;

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        lastBeat = -1; lastMode = -1;
        setTiming (0.5, 0);
        fastC      = timeCoef (0.020, fs);   // noise-floor detector: 20 ms
        floorDownC = timeCoef (0.015, fs);   // duck noise over 15 ms
        floorUpC   = timeCoef (0.005, fs);   // let the voice back in over 5 ms
        ms = fastMs = 0; rideDb = floorDb = 0;
    }

    // 'beat' = the plugin's Timing note in seconds (Rider and Leveler follow it).
    void setTiming (double beat, int mode)
    {
        if (beat == lastBeat && mode == lastMode) return;
        lastBeat = beat; lastMode = mode;

        double avg, down, up;
        switch (mode)
        {
            case 1:  avg = beat * 4.0; down = beat;        up = beat * 4.0; maxRangeDb = 12.0f; break;  // Balanced
            case 2:  avg = 0.050;      down = 0.010;       up = 0.050;      maxRangeDb = 12.0f; break;  // Fast
            default: avg = beat;       down = beat * 0.25; up = beat;       maxRangeDb = 12.0f; break;  // Natural
        }
        rmsC  = timeCoef (avg,  fs);
        downC = timeCoef (down, fs);
        upC   = timeCoef (up,   fs);
    }

    void process (float* const* ch, int nch, int n, float floorThreshDb, float targetDb)
    {
        for (int i = 0; i < n; ++i)
        {
            float p = 0;
            for (int c = 0; c < nch; ++c) p += ch[c][i] * ch[c][i];
            p /= (float) nch;

            fastMs = fastC * fastMs + (1.0f - fastC) * p;
            const float fastDb = 10.0f * std::log10 (fastMs + 1.0e-12f);
            const bool  voice  = fastDb > floorThreshDb;

            // Ride: only listens to (and moves during) the singing.
            if (voice)
            {
                overMeter = std::max (overMeter, fastDb - floorThreshDb);
                ms = ms <= 0.0f ? fastMs : rmsC * ms + (1.0f - rmsC) * p;   // start from the first word's level
                const float levelDb = 10.0f * std::log10 (ms + 1.0e-12f);
                const float want = std::clamp (targetDb - levelDb, -maxRangeDb, maxRangeDb);
                const float c = want < rideDb ? downC : upC;
                rideDb = c * rideDb + (1.0f - c) * want;
            }

            // Noise floor: pull down by how far under the floor, up to 12 dB.
            const float floorWant = voice ? 0.0f : std::max (-kFloorDepthDb, fastDb - floorThreshDb);
            const float fc = floorWant < floorDb ? floorDownC : floorUpC;
            floorDb = fc * floorDb + (1.0f - fc) * floorWant;

            const float totalDb = rideDb + floorDb;
            meter = std::max (meter, std::abs (totalDb));
            const float g = dbToGain (totalDb);
            for (int ch_ = 0; ch_ < nch; ++ch_) ch[ch_][i] *= g;
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

    // Energy of the input, and of what the low and high boost sections each add,
    // summed since the last takeMeters() (for the stage's two meter bars).
    double eIn = 0, eLowAdd = 0, eHighAdd = 0;

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
                const float hb = highBoost.process (x, c);
                eHighAdd += (double) (hb - x) * (hb - x);
                x = highAtten.process (hb, c);
                d[i] = x;
            }
        }
    }

    // How much each boost changes the sound, in dB relative to the input
    // (-inf if silent). Resets the sums.
    void takeMeters (float& lowDb, float& highDb, int samples)
    {
        if (eIn / std::max (1, samples) < 1.0e-7) { lowDb = highDb = -200.0f; }   // near-silence
        else
        {
            lowDb  = 10.0f * std::log10 ((float) (eLowAdd  / eIn) + 1.0e-12f);
            highDb = 10.0f * std::log10 ((float) (eHighAdd / eIn) + 1.0e-12f);
        }
        eIn = eLowAdd = eHighAdd = 0;
    }
};

//==============================================================================
// Tape saturation (first half of stage 2, Harmonics; the "Tape" knob).
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
// 5. Filters: high-pass + low-pass, 24 dB/octave each (two cascaded Butterworth stages).
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
        if (hpHz != lastHp) { lastHp = hpHz; hp1.makeHighPass (fs, hpHz, kQ1); hp2.makeHighPass (fs, hpHz, kQ2); }
        if (lpHz != lastLp) { lastLp = lpHz; lp1.makeLowPass (fs, lpHz, kQ1);  lp2.makeLowPass (fs, lpHz, kQ2); }

        for (int c = 0; c < nch; ++c)
        {
            float* d = ch[c];
            for (int i = 0; i < n; ++i)
                d[i] = lp2.process (lp1.process (hp2.process (hp1.process (d[i], c), c), c), c);
        }
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
// Harmonic exciter (second half of stage 2, Harmonics; the "Exciter" knob), 0-100 %.
// Takes the top of the bass (above 1.5 kHz: string, pick and fret detail),
// normalises it to a steady level so it works the same on quiet and loud notes,
// bends it through a soft asymmetric curve to create new upper harmonics, keeps only
// what lands above 3 kHz, and blends that in. Adds bite and definition without the
// harshness of a plain treble boost.
struct Exciter
{
    static constexpr float  kLevel = 2.0f;      // blend at 100 %
    static constexpr double kInHz  = 1500.0;    // top of the bass that gets excited
    static constexpr double kOutHz = 3000.0;    // new harmonics kept above this

    Biquad inHp1, inHp2, genHp1, genHp2;
    float  env[kMaxCh] {}, envAtt = 0, envRel = 0, lastAmt = 0;
    double fs = 44100;

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        inHp1.makeHighPass (fs, kInHz, 0.707);   inHp2.makeHighPass (fs, kInHz, 0.707);
        genHp1.makeHighPass (fs, kOutHz, 0.707); genHp2.makeHighPass (fs, kOutHz, 0.707);
        for (auto* b : { &inHp1, &inHp2, &genHp1, &genHp2 }) b->reset();
        envAtt = timeCoef (0.001, fs); envRel = timeCoef (0.030, fs);
        for (int c = 0; c < kMaxCh; ++c) env[c] = 0;
        lastAmt = 0;
        eIn = eAdd = 0;
    }

    void process (float* const* ch, int nch, int n, float amount)
    {
        const float a = std::clamp (amount * 0.01f, 0.0f, 1.0f);
        const float a0 = lastAmt;
        lastAmt = a;

        for (int c = 0; c < nch; ++c)
        {
            float* d = ch[c];
            for (int i = 0; i < n; ++i)
            {
                const float x = d[i];
                eIn += (double) x * x;
                if (a <= 0.0f && a0 <= 0.0f) continue;   // knob at 0: nothing to add
                const float am = a0 + (a - a0) * (float) (i + 1) / (float) n;   // smoothed (no zipper noise)
                const float band = inHp2.process (inHp1.process (x, c), c);
                env[c] = follow (env[c], std::abs (band), envAtt, envRel);
                const float u   = band / (env[c] + 1.0e-6f);              // steady level, ~ +/-1
                const float th  = std::tanh (3.0f * u);
                const float gen = (th + 0.3f * th * th) * env[c];         // asymmetric -> odd + even harmonics
                const float top = genHp2.process (genHp1.process (gen, c), c);
                const float added = top * am * kLevel;
                eAdd += (double) added * added;
                d[i] = x + added;
            }
        }
    }

    // How loud the added harmonics are, in dB relative to the input. Resets the sums.
    double eIn = 0, eAdd = 0;
    float takeMeter (int samples)
    {
        const float db = eIn / std::max (1, samples) < 1.0e-7 ? -200.0f
                       : 10.0f * std::log10 ((float) (eAdd / eIn) + 1.0e-12f);
        eIn = eAdd = 0;
        return db;
    }
};

//==============================================================================
// 4. Limiter (after the Vintage EQ, or before it with LIMIT/EQ SWAP).
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
