#include "click/ClickSounds.h"
#include <cmath>
#include <algorithm>

namespace earfirst
{

void ClickVoice::startBeep (double pitch, float a, double rate)
{
    sampleL = sampleR = nullptr;
    sr = rate; freq = 1000 * pitch; amp = a; age = 0;
    length = (int) (0.06 * rate);
}

void ClickVoice::startSample (const float* left, const float* right, int num, float a)
{
    sampleL = left; sampleR = right != nullptr ? right : left;
    amp = a; age = 0; length = num;
}

void ClickVoice::nextStereo (float& left, float& right)
{
    if (age >= length) { left = right = 0; return; }
    if (sampleL != nullptr)
    {
        left = sampleL[age] * amp; right = sampleR[age] * amp;
        ++age;
        return;
    }
    // A pure tone with edges just soft enough not to click.
    const double t = age / sr;
    const double edge = std::min ({ 1.0, t / 0.002, (length - age) / (0.004 * sr) });
    left = right = (float) (0.5 * edge * std::sin (2 * 3.14159265358979323846 * freq * t)) * amp;
    ++age;
}

} // namespace earfirst
