// return-finder-test: telling the computer's own sound apart from everything
// else coming in, on signals made here: a return (a copy), a copy with hiss,
// a mic in a room, a speaker that dulls the top, unrelated sound, a sweep,
// and a single tone (which can't be judged, and mustn't be guessed).

#include "capture/ReturnFinder.h"
#include <cstdio>
#include <random>
#include <string>

using namespace earfirst;

static int failures = 0;
static void check (bool ok, const std::string& what)
{
    std::printf ("%s: %s\n", ok ? "ok" : "FAIL", what.c_str());
    if (! ok) ++failures;
}

int main()
{
    const double sr = 48000;
    const int len = 16384, maxLag = 8192, total = len + maxLag;
    std::mt19937 rng (7);
    std::normal_distribution<float> gauss (0.0f, 0.1f);

    std::vector<float> noise ((size_t) total * 2);
    for (auto& x : noise) x = gauss (rng);
    // out[i] is noise[i + maxLag]; an input L samples behind sees out[maxLag + i - L].
    auto outFrom = [&] (const std::vector<float>& src) { return std::vector<float> (src.begin() + maxLag, src.begin() + maxLag + total); };
    auto delayed = [&] (const std::vector<float>& src, int lag, float gain)
    {
        std::vector<float> in ((size_t) len);
        for (int i = 0; i < len; ++i) in[(size_t) i] = gain * src[(size_t) (2 * maxLag + i - lag)];
        return in;
    };
    auto say = [] (const char* name, const ReturnCheck& r)
    {
        std::printf ("  %s: correlation %.3f, spread %.1f dB over %d bands, delay %d\n", name, r.correlation, r.spreadDb, r.bands, r.lag);
    };

    const auto out = outFrom (noise);

    {
        auto in = delayed (noise, 123, 0.5f);
        auto r = checkReturn (out.data(), in.data(), len, maxLag, sr);
        say ("copy", r);
        check (r.isReturn && r.lag == 123, "A copy of the output, 123 samples late and 6 dB down, is a return found at 123.");
    }
    {
        auto in = delayed (noise, 600, 0.3f);
        for (auto& x : in) x += gauss (rng) * 0.01f;
        auto r = checkReturn (out.data(), in.data(), len, maxLag, sr);
        say ("copy with hiss", r);
        check (r.isReturn, "A copy with hiss 30 dB down is still a return.");
    }
    {
        // A mic in a room: the direct sound, then reflections, then the mic's own noise.
        auto in = delayed (noise, 400, 0.4f);
        const std::pair<int, float> echoes[] = { { 431, 0.5f }, { 517, 0.35f }, { 700, 0.3f }, { 1300, 0.2f }, { 2100, 0.12f } };
        for (auto [d, g] : echoes)
        {
            auto e = delayed (noise, d, 0.4f * g);
            for (int i = 0; i < len; ++i) in[(size_t) i] += e[(size_t) i];
        }
        for (auto& x : in) x += gauss (rng) * 0.005f;
        auto r = checkReturn (out.data(), in.data(), len, maxLag, sr);
        say ("mic in a room", r);
        check (! r.isReturn, "A mic hearing the speakers in a room is not a return.");
    }
    {
        // A return with a steady offset on it, like channel 64 on a StudioLive 64S.
        auto in = delayed (noise, 77, 0.5f);
        for (auto& x : in) x += 0.044f;
        auto r = checkReturn (out.data(), in.data(), len, maxLag, sr);
        say ("copy with DC", r);
        check (r.isReturn, "A return with a steady offset on it is still a return.");
    }
    {
        // A mic close to a speaker: the direct sound, one weak reflection off
        // the desk, and the speaker's top end a little soft.
        auto in = delayed (noise, 90, 0.7f);
        auto e = delayed (noise, 150, 0.7f * 0.25f);
        for (int i = 0; i < len; ++i) in[(size_t) i] += e[(size_t) i];
        const float a = std::exp (-2.0f * 3.14159265f * 9000.0f / (float) sr);
        float y = 0;
        for (auto& x : in) { y = (1 - a) * x + a * y; x = y; }
        auto r = checkReturn (out.data(), in.data(), len, maxLag, sr);
        say ("close mic", r);
        check (! r.isReturn, "A mic right in front of a speaker is not a return.");
    }
    {
        // A speaker that rolls off the top: a one-pole low-pass near 2 kHz.
        auto in = delayed (noise, 250, 1.0f);
        const float a = std::exp (-2.0f * 3.14159265f * 2000.0f / (float) sr);
        float y = 0;
        for (auto& x : in) { y = (1 - a) * x + a * y; x = y; }
        auto r = checkReturn (out.data(), in.data(), len, maxLag, sr);
        say ("dull speaker", r);
        check (! r.isReturn, "A copy with the top rolled off is not a return.");
    }
    {
        std::vector<float> in ((size_t) len);
        for (auto& x : in) x = gauss (rng);
        auto r = checkReturn (out.data(), in.data(), len, maxLag, sr);
        say ("unrelated", r);
        check (! r.isReturn && r.enoughSignal, "Unrelated sound is not a return.");
    }
    {
        std::vector<float> in ((size_t) len, 0.0f);
        auto r = checkReturn (out.data(), in.data(), len, maxLag, sr);
        check (! r.isReturn && ! r.enoughSignal, "A silent input can't be judged and isn't called a return.");
        std::vector<float> silentOut ((size_t) total, 0.0f);
        auto copy = delayed (noise, 10, 1.0f);
        auto r2 = checkReturn (silentOut.data(), copy.data(), len, maxLag, sr);
        check (! r2.isReturn && ! r2.enoughSignal, "With nothing played, nothing can be judged.");
    }
    {
        // The sweep Ear First plays to check on purpose, the whole 0.6 seconds in view.
        auto sweep = makeSweep (sr);
        const int sl = 32768, sm = 4096;
        std::vector<float> o ((size_t) (sl + sm), 0.0f), in ((size_t) sl, 0.0f);
        for (size_t i = 0; i < sweep.size() && i + 1000 < o.size(); ++i) o[i + 1000] = sweep[i];
        for (int i = 0; i < sl; ++i) { const int j = sm + i - 300; if (j >= 0) in[(size_t) i] = 0.8f * o[(size_t) j]; }
        auto r = checkReturn (o.data(), in.data(), sl, sm, sr);
        say ("sweep copy", r);
        check (r.isReturn && r.lag == 300, "The sweep, copied back 300 samples late, is a return found at 300.");
        for (int i = 0; i < sl; ++i) in[(size_t) i] = 0;
        float y = 0; const float a = std::exp (-2.0f * 3.14159265f * 1500.0f / (float) sr);
        for (int i = 0; i < sl; ++i) { const int j = sm + i - 300; const float x = j >= 0 ? o[(size_t) j] : 0.0f; y = (1 - a) * x + a * y; in[(size_t) i] = y; }
        auto r2 = checkReturn (o.data(), in.data(), sl, sm, sr);
        say ("sweep through a dull speaker", r2);
        check (! r2.isReturn, "The sweep through a dull speaker is not a return.");
    }
    {
        std::vector<float> o ((size_t) total), in ((size_t) len);
        for (int i = 0; i < total; ++i) o[(size_t) i] = 0.2f * std::sin (2 * 3.14159265 * 440.0 * i / sr);
        for (int i = 0; i < len; ++i) in[(size_t) i] = o[(size_t) (maxLag + i - 50)];
        auto r = checkReturn (o.data(), in.data(), len, maxLag, sr);
        say ("one tone", r);
        check (! r.isReturn, "A single tone can't tell a return from a mic, so it isn't called one.");
    }

    std::printf (failures == 0 ? "All checks passed.\n" : "%d checks failed.\n", failures);
    return failures == 0 ? 0 : 1;
}
