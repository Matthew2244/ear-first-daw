#pragma once
// ReturnFinder: does an input carry the computer's own sound?
//
// Many interfaces send what the computer plays back in on a pair of inputs
// (a "loopback" or "return": channels 63 and 64 on a StudioLive 64S, others
// elsewhere). Nobody should have to know which. So Ear First compares what it
// sent out with what came back on each input.
//
// A return is a copy of the output: after the right delay it lines up almost
// exactly, at the same level across the whole range. A microphone hearing the
// speakers can line up too, but the room and the speakers color it: some
// bands come back much louder than others. So both must hold:
//   - correlation above 0.9 at the best delay, and
//   - the level difference between output and input is the same, within 3 dB,
//     in every third-octave band the output had sound in (at least ten).
// A pure tone can't tell the two apart, so a check needs sound across the
// range: the song, the click, or the short sweep Ear First can play.

#include <vector>
#include <cstdint>

namespace earfirst
{

struct ReturnCheck
{
    bool enoughSignal = false;   // output and input both had sound to compare
    bool isReturn = false;
    double correlation = 0;      // at the best delay, 0 to 1
    double spreadDb = 0;         // largest band-to-band level difference
    int bands = 0;               // octave bands compared
    int lag = 0;                 // samples the input runs behind the output
};

// `out` holds maxLag + len samples of output: out[maxLag + i] went out at the
// same moment in[i] came in. Delays from 0 to maxLag samples are tried.
ReturnCheck checkReturn (const float* out, const float* in, int len, int maxLag, double sampleRate);

// A soft rising sweep across the whole range, for checking on purpose.
std::vector<float> makeSweep (double sampleRate, double seconds = 0.6, float level = 0.1f);

} // namespace earfirst
