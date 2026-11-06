#include "click/ClickSounds.h"
#include <cmath>
#include <algorithm>

namespace earfirst
{

namespace { constexpr double pi = 3.14159265358979323846; }

void ClickVoice::Biquad::bandPass (double f, double q, double rate)
{
    const double w = 2 * pi * std::min (f, rate * 0.45) / rate, alpha = std::sin (w) / (2 * q), a0 = 1 + alpha;
    b0 = alpha / a0; b1 = 0; b2 = -alpha / a0; a1 = -2 * std::cos (w) / a0; a2 = (1 - alpha) / a0; z1 = z2 = 0;
}

void ClickVoice::Biquad::highPass (double f, double q, double rate)
{
    const double w = 2 * pi * std::min (f, rate * 0.45) / rate, alpha = std::sin (w) / (2 * q), c = std::cos (w), a0 = 1 + alpha;
    b0 = (1 + c) / 2 / a0; b1 = -(1 + c) / a0; b2 = (1 + c) / 2 / a0; a1 = -2 * c / a0; a2 = (1 - alpha) / a0; z1 = z2 = 0;
}

double ClickVoice::Biquad::process (double x)
{
    const double y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
}

void ClickVoice::Mode::set (double f, double decaySeconds, double g, double rate)
{
    const double r = std::exp (-1.0 / (decaySeconds * rate));
    a1 = 2 * r * std::cos (2 * pi * std::min (f, rate * 0.45) / rate);
    a2 = r * r;
    y1 = y2 = 0;
    gain = g * std::sin (2 * pi * std::min (f, rate * 0.45) / rate);   // struck once, it rings at g whatever its pitch
}

double ClickVoice::Mode::process (double x)
{
    const double y = a1 * y1 - a2 * y2 + x;
    y2 = y1; y1 = y;
    return y * gain;
}

double ClickVoice::whiteNoise()
{
    noise = noise * 1664525u + 1013904223u;
    return (double) (noise >> 8) / 8388608.0 - 1.0;
}

void ClickVoice::startSample (const float* left, const float* right, int num, float a)
{
    sampleL = left; sampleR = right != nullptr ? right : left;
    amp = a; age = 0; length = num;
}

void ClickVoice::nextStereo (float& left, float& right)
{
    if (sampleL != nullptr)
    {
        if (age >= length) { left = right = 0; return; }
        left = sampleL[age] * amp; right = sampleR[age] * amp;
        ++age;
        return;
    }
    left = right = next();
}

void ClickVoice::start (int s, double pitch, float a, double rate)
{
    sampleL = sampleR = nullptr;
    sound = std::clamp (s, 0, numSounds - 1);
    sr = rate; amp = a; age = 0; numSquares = 0;
    for (auto& p : phases) p = 0;
    const auto ms = [rate] (double m) { return (int) (m * 0.001 * rate); };
    switch (sound)
    {
        case woodblock:
            base = 820 * pitch;
            modes[0].set (base,        0.030,  1.00, sr);
            modes[1].set (base * 2.71, 0.012,  0.55, sr);
            modes[2].set (base * 4.53, 0.006,  0.30, sr);
            filter[0].bandPass (base * 3.5, 0.7, sr);       // the knock that strikes it
            length = ms (160);
            break;
        case beep:
            base = 1000 * pitch;
            length = ms (60);
            break;
        case clave:
            base = 2350 * pitch;
            modes[0].set (base,        0.045, 1.00, sr);
            modes[1].set (base * 3.93, 0.004, 0.12, sr);
            length = ms (220);
            break;
        case rim:
            base = 1.0 * pitch;
            modes[0].set (430 * pitch,  0.018, 0.80, sr);   // the shell
            modes[1].set (1030 * pitch, 0.010, 0.45, sr);
            modes[2].set (1880 * pitch, 0.006, 0.30, sr);
            filter[0].bandPass (3200 * pitch, 0.9, sr);     // the stick's snap
            length = ms (110);
            break;
        case cowbell:
            base = 540 * std::sqrt (pitch);
            numSquares = 2; freqs[0] = base; freqs[1] = base * 1.4815;
            filter[0].bandPass (base * 4.9, 1.6, sr);
            filter[1].bandPass (base * 1.5, 1.2, sr);
            length = ms (320);
            break;
        case hihat:
        default:
        {
            const double k = 2.0 * std::pow (pitch, 0.35);
            const double ratios[] = { 205.3, 304.4, 369.6, 522.7, 540.0, 800.0 };
            numSquares = 6;
            for (int i = 0; i < 6; ++i) freqs[i] = ratios[i] * k;
            filter[0].highPass (7200, 0.7, sr);
            filter[1].highPass (7200, 0.7, sr);
            filter[2].bandPass (10500, 0.8, sr);
            length = ms (110);
            break;
        }
    }
}

float ClickVoice::next()
{
    if (age >= length) return 0;
    const double t = age / sr;
    double x = 0;
    switch (sound)
    {
        case woodblock:
        {
            const double strike = age == 0 ? 1.0 : 0.0;
            const double knock = age < (int) (0.0015 * sr) ? filter[0].process (whiteNoise()) * 0.35 * std::exp (-t / 0.0006) : 0.0;
            x = (modes[0].process (strike) + modes[1].process (strike) + modes[2].process (strike)) * 0.42 + knock;
            break;
        }
        case beep:
        {
            const double edge = std::min ({ 1.0, t / 0.002, (length - age) / (0.004 * sr) });
            x = 0.5 * edge * std::sin (2 * pi * base * t);
            break;
        }
        case clave:
        {
            const double strike = age == 0 ? 1.0 : 0.0;
            x = (modes[0].process (strike) + modes[1].process (strike)) * 0.6;
            break;
        }
        case rim:
        {
            const double strike = age == 0 ? 1.0 : 0.0;
            const double snap = filter[0].process (whiteNoise()) * std::exp (-t / 0.004) * 1.4;
            x = (modes[0].process (strike) + modes[1].process (strike) + modes[2].process (strike)) * 0.45 + snap * 0.6;
            break;
        }
        case cowbell:
        {
            double sq = 0;
            for (int i = 0; i < 2; ++i)
            {
                phases[i] += freqs[i] / sr; phases[i] -= std::floor (phases[i]);
                sq += phases[i] < 0.5 ? 1.0 : -1.0;
            }
            const double env = 0.65 * std::exp (-t / 0.010) + 0.35 * std::exp (-t / 0.085);
            x = (filter[0].process (sq) * 0.9 + filter[1].process (sq) * 0.25) * env * std::min (1.0, t / 0.0005) * 0.55;
            break;
        }
        case hihat:
        default:
        {
            double sq = 0;
            for (int i = 0; i < numSquares; ++i)
            {
                phases[i] += freqs[i] / sr; phases[i] -= std::floor (phases[i]);
                sq += phases[i] < 0.5 ? 1.0 : -1.0;
            }
            sq = sq / 6 + whiteNoise() * 0.25;
            const double hp = filter[1].process (filter[0].process (sq));
            x = (hp * 0.8 + filter[2].process (sq) * 0.5) * std::exp (-t / 0.018) * 1.4;
            break;
        }
    }
    ++age;
    return (float) (x * amp);
}

} // namespace earfirst
