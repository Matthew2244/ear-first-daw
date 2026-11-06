#pragma once
// The song's tracks, as people mean them: the click has a track of its own
// that never takes an input, never shows in the track list and never goes
// into a mix, so everything that counts tracks uses these instead.

#include <JuceHeader.h>

namespace earfirst
{
inline bool isClickTrack (const tracktion::Track& t)
{
    return t.state.getProperty ("earFirstRole").toString() == "click";
}

inline juce::Array<tracktion::AudioTrack*> contentTracks (tracktion::Edit& e)
{
    auto all = tracktion::getAudioTracks (e);
    all.removeIf ([] (auto* t) { return isClickTrack (*t); });
    return all;
}

// At least `n` content tracks, new ones added after the last content track.
inline void ensureContentTracks (tracktion::Edit& e, int n)
{
    while (contentTracks (e).size() < n)
    {
        auto list = contentTracks (e);
        e.insertNewAudioTrack (tracktion::TrackInsertPoint (nullptr, list.isEmpty() ? nullptr : list.getLast()), nullptr);
    }
}
} // namespace earfirst
