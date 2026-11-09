#pragma once
// RouteMatrix: any input to any output, live, at the audio callback.
//
// It runs whether or not a song is open and has nothing to do with what gets
// captured: inputs are recorded before this ever touches them.
//
// Inputs are copied before the engine runs, because some devices hand over
// output buffers that share memory with the inputs. They're mixed into the
// outputs afterwards. Level changes ramp across a block, so a route coming or
// going never clicks. A route that's removed fades out first.
//
// Threads: `set` on the message thread (or wherever the device is being
// started); `readInputs` and `mix` on the audio thread, which never allocates.

#include <juce_audio_devices/juce_audio_devices.h>
#include <vector>
#include <memory>
#include <map>

namespace earfirst
{

class RouteMatrix
{
public:
    struct Route { int input = 0, output = 0; float gain = 1.0f; };

    // Replaces every route. `audioLock` is the lock the audio callback runs
    // under: it's held only to read the current levels and to swap a pointer.
    void set (const std::vector<Route>& routes, int blockCapacity, juce::CriticalSection& audioLock)
    {
        std::vector<Entry> was;
        if (auto reserved = entryCountNow.load(); reserved > 0) was.reserve ((size_t) reserved + 8);
        {
            const juce::ScopedLock sl (audioLock);
            if (plan != nullptr && plan->entries.size() <= was.capacity())
                was.assign (plan->entries.begin(), plan->entries.end());
        }

        auto next = std::make_unique<Plan>();
        next->capacity = std::max (1, blockCapacity);
        std::map<int, int> slotOf;
        const auto slotFor = [&] (int input)
        {
            auto [it, isNew] = slotOf.try_emplace (input, (int) next->slotInputs.size());
            if (isNew) next->slotInputs.push_back (input);
            return it->second;
        };
        std::map<std::pair<int, int>, float> currentLevel;
        for (auto& e : was) currentLevel[{ e.input, e.output }] = e.current;

        for (auto& r : routes)
        {
            if (r.input < 0 || r.output < 0) continue;
            auto level = currentLevel.find ({ r.input, r.output });
            next->entries.push_back ({ r.input, r.output, slotFor (r.input), r.gain, level != currentLevel.end() ? level->second : 0.0f });
            if (level != currentLevel.end()) currentLevel.erase (level);
        }
        // Whatever's left was just removed and may still be sounding: let it fade.
        for (auto& [key, level] : currentLevel)
            if (level > 0.0f)
                next->entries.push_back ({ key.first, key.second, slotFor (key.first), 0.0f, level });

        next->scratch.assign (next->slotInputs.size() * (size_t) next->capacity, 0.0f);
        next->slotPointers.assign (next->slotInputs.size(), nullptr);
        for (size_t s = 0; s < next->slotInputs.size(); ++s)
            next->slotPointers[s] = next->scratch.data() + s * (size_t) next->capacity;
        entryCountNow = (int) next->entries.size();

        {
            const juce::ScopedLock sl (audioLock);
            std::swap (plan, next);
        }
        // `next` is now the old plan, freed here and never on the audio thread.
    }

    // Before the engine runs. Returns the loudest routed sample, so speech can
    // wait for you to stop playing as it waits for the song.
    float readInputs (const float* const* in, int numIn, int n) noexcept
    {
        framesHeld = 0;
        auto* p = plan.get();
        if (p == nullptr || p->entries.empty()) return 0.0f;
        framesHeld = std::min (n, p->capacity);
        for (size_t s = 0; s < p->slotInputs.size(); ++s)
        {
            const int input = p->slotInputs[s];
            if (input < numIn && in[input] != nullptr)
                juce::FloatVectorOperations::copy (p->slotPointers[s], in[input], framesHeld);
            else
                juce::FloatVectorOperations::clear (p->slotPointers[s], framesHeld);
        }
        float peak = 0.0f;
        for (auto& e : p->entries)
            if (e.target > 0.0f)
            {
                auto range = juce::FloatVectorOperations::findMinAndMax (p->slotPointers[(size_t) e.slot], framesHeld);
                peak = std::max (peak, e.target * std::max (std::abs (range.getStart()), std::abs (range.getEnd())));
            }
        return peak;
    }

    // After the engine, and after anything else that writes the outputs.
    void mix (float* const* out, int numOut, int n) noexcept
    {
        auto* p = plan.get();
        if (p == nullptr || framesHeld == 0) return;
        const int frames = std::min (n, framesHeld);
        for (auto& e : p->entries)
        {
            if (e.output < numOut && out[e.output] != nullptr && (e.current > 0.0f || e.target > 0.0f))
            {
                const auto* from = p->slotPointers[(size_t) e.slot];
                auto* to = out[e.output];
                const float step = (e.target - e.current) / (float) frames;
                for (int i = 0; i < frames; ++i)
                    to[i] += from[i] * (e.current + step * (float) (i + 1));
            }
            e.current = e.target;
        }
    }

    int numEntries() const noexcept     { return entryCountNow.load(); }

private:
    struct Entry { int input, output, slot; float target, current; };
    struct Plan
    {
        std::vector<Entry> entries;
        std::vector<int> slotInputs;           // which input each scratch slot holds
        std::vector<float> scratch;
        std::vector<float*> slotPointers;
        int capacity = 0;
    };
    std::unique_ptr<Plan> plan;
    std::atomic<int> entryCountNow { 0 };
    int framesHeld = 0;                         // audio thread
};

} // namespace earfirst
