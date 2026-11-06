#pragma once
// One click voice: either a recorded sound (the built-in ones Matthew picked
// by ear, or one you imported), played as it is, or the Beep, the one sound
// that's electronic by nature and so made in code.
//
// Plain C++, no allocation: the audio thread starts and renders voices.

namespace earfirst
{

class ClickVoice
{
public:
    // The Beep. pitch: 1 for the beat, higher for the one, lower for subdivisions.
    void startBeep (double pitch, float amp, double sampleRate);
    // A recorded sound (left and right, `num` long), played as it is.
    // The audio must outlive the voice: ClickSamples keeps it for the whole run.
    void startSample (const float* left, const float* right, int num, float amp);
    void nextStereo (float& left, float& right);
    bool finished() const            { return age >= length; }

private:
    double sr = 48000, freq = 1000;
    float amp = 0;
    int age = 0, length = 0;
    const float* sampleL = nullptr;
    const float* sampleR = nullptr;
};

} // namespace earfirst
