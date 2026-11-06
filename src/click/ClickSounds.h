#pragma once
// The click's sounds, each made the way the real instrument makes its sound,
// so they're told apart at once by ear, not just by pitch:
//
//   Woodblock  a hollow block: three wooden modes, the lowest ringing longest,
//              struck with a soft knock.
//   Beep       the electronic metronome: a pure tone, square edges.
//   Clave      two hardwood sticks: one bright, glassy ring.
//   Rim        a cross-stick: the drum shell's low knock plus the stick's snap.
//   Cowbell    the drum machine's: two square waves a fifth-ish apart through
//              a narrow band, a quick hit and a short tail.
//   Hi-hat     closed: six square waves at metal's uneven ratios, only the
//              top left, very short.
//
// Plain C++, no allocation: the audio thread starts and renders voices.

#include <cstdint>

namespace earfirst
{

class ClickVoice
{
public:
    enum Sound { woodblock, beep, clave, rim, cowbell, hihat, numSounds };

    // pitch: 1 for the beat, higher for the one, lower for subdivisions.
    void start (int sound, double pitch, float amp, double sampleRate);
    // Or a sample of your own (left and right, `num` long), played as it is.
    // The audio must outlive the voice: ClickSamples keeps it for the whole run.
    void startSample (const float* left, const float* right, int num, float amp);
    float next();                    // one sample (mono sounds)
    void nextStereo (float& left, float& right);
    bool finished() const            { return age >= length; }

private:
    struct Biquad
    {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        void bandPass (double f, double q, double sr);
        void highPass (double f, double q, double sr);
        double process (double x);
    };
    struct Mode   // a decaying sine: one way the instrument rings
    {
        double a1 = 0, a2 = 0, y1 = 0, y2 = 0, gain = 0;
        void set (double f, double decaySeconds, double g, double sr);
        double process (double x);
    };

    int sound = 0;
    double sr = 48000, base = 1000;
    float amp = 0;
    int age = 0, length = 0;
    uint32_t noise = 22222;
    Mode modes[3];
    Biquad filter[3];
    double phases[6] {}, freqs[6] {};
    int numSquares = 0;
    const float* sampleL = nullptr;
    const float* sampleR = nullptr;
    double whiteNoise();
};

} // namespace earfirst
