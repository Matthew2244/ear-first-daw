#pragma once
// TimelineMap: which moment on the song timeline each captured sample belongs to.
//
// The capture recorder counts samples from the moment it starts. The song's
// transport starts, stops, jumps and loops. The audio thread notes the
// transport's state for every block, but only stores an anchor when something
// changes (play, stop, a jump, a loop), so an hour of steady playback costs one
// anchor. Keeping a take asks the map where its first sample goes.
//
// Audio thread: note(). Any other thread: everything else (one reader at a time).

#include <juce_core/juce_core.h>
#include <atomic>
#include <mutex>
#include <optional>
#include <vector>

namespace earfirst
{

class TimelineMap
{
public:
    struct Anchor
    {
        int64_t captureSample = 0;   // recorder's sample count where this stretch begins
        int64_t editSample = 0;      // song timeline position there, in samples (playing only)
        bool playing = false;
    };

    explicit TimelineMap (int capacity = 65536)
        : fifo (capacity), ring ((size_t) capacity) {}

    // Audio thread. No locks, no allocation.
    void note (int64_t captureSample, bool playing, int64_t editSample) noexcept
    {
        if (hasLast)
        {
            if (! playing && ! last.playing)
                return;
            if (playing && last.playing)
            {
                auto expected = last.editSample + (captureSample - last.captureSample);
                if (std::abs (editSample - expected) <= 1)
                    return;
            }
        }

        last = { captureSample, playing ? editSample : 0, playing };
        hasLast = true;

        int s1, n1, s2, n2;
        fifo.prepareToWrite (1, s1, n1, s2, n2);
        if (n1 + n2 == 0) { overflowed.store (true); return; }
        ring[(size_t) s1] = last;
        fifo.finishedWrite (1);
    }

    // Where on the timeline the take [start, end) begins, in timeline samples.
    // The take is lined up by the longest steady stretch of playback inside
    // it, so the part you played along to lands where it was played. That
    // also steps over the first block after pressing play, which the engine
    // treats as a transition, and over jumps and loops, which line up with
    // whichever stretch is longest. If the song never played during the take,
    // nothing.
    std::optional<int64_t> timelineStartFor (int64_t start, int64_t end)
    {
        std::lock_guard<std::mutex> lock (readMutex);
        collect();

        std::optional<int64_t> best;
        int64_t bestLength = 0;
        for (size_t i = 0; i < anchors.size(); ++i)
        {
            const auto& a = anchors[i];
            const auto stretchEnd = std::min (end, i + 1 < anchors.size() ? anchors[i + 1].captureSample : end);
            const auto stretchStart = std::max (start, a.captureSample);
            const auto length = stretchEnd - stretchStart;
            if (! a.playing || length <= bestLength)
                continue;
            bestLength = length;
            best = a.editSample - (a.captureSample - start);
        }
        return best;
    }

    // The take [start, end) cut into stretches where the song was steadily
    // playing (or stopped). A loop shows up as playing stretches that go back
    // over the same part of the timeline.
    struct Stretch
    {
        int64_t captureStart = 0, captureEnd = 0;
        int64_t editStart = 0;          // playing only
        bool playing = false;
        int64_t length() const          { return captureEnd - captureStart; }
    };

    std::vector<Stretch> stretches (int64_t start, int64_t end)
    {
        std::lock_guard<std::mutex> lock (readMutex);
        collect();
        std::vector<Stretch> out;
        for (size_t i = 0; i < anchors.size(); ++i)
        {
            const auto& a = anchors[i];
            const auto s0 = std::max (start, a.captureSample);
            const auto s1 = std::min (end, i + 1 < anchors.size() ? anchors[i + 1].captureSample : end);
            if (s1 <= s0) continue;
            out.push_back ({ s0, s1, a.playing ? a.editSample + (s0 - a.captureSample) : 0, a.playing });
        }
        return out;
    }

    bool hasOverflowed() const      { return overflowed.load(); }

    std::vector<Anchor> getAnchors()
    {
        std::lock_guard<std::mutex> lock (readMutex);
        collect();
        return anchors;
    }

private:
    void collect()
    {
        int s1, n1, s2, n2;
        fifo.prepareToRead (fifo.getNumReady(), s1, n1, s2, n2);
        for (int i = 0; i < n1; ++i) anchors.push_back (ring[(size_t) (s1 + i)]);
        for (int i = 0; i < n2; ++i) anchors.push_back (ring[(size_t) (s2 + i)]);
        fifo.finishedRead (n1 + n2);
    }

    juce::AbstractFifo fifo;
    std::vector<Anchor> ring;
    Anchor last;
    bool hasLast = false;                  // audio thread only
    std::atomic<bool> overflowed { false };

    std::mutex readMutex;
    std::vector<Anchor> anchors;           // reader side, in capture order
};

} // namespace earfirst
