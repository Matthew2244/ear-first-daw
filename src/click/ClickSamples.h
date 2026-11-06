#pragma once
// Your own click sounds: any audio file, imported once, then chosen in Click
// settings beside the built-in ones.
//
// Importing copies the file into the Click Sounds folder (so moving or
// deleting the original never breaks the click) under its own name. When a
// sound is used it's read once, made ready for the click and kept in memory
// for as long as the app runs:
//  - silence at the start is trimmed (to a quarter millisecond before the
//    sound), so the hit lands on the beat;
//  - it's brought to the session's sample rate;
//  - its loudest point is set to match the built-in sounds, so changing sound
//    never jumps the level; the click's own level and accents still apply;
//  - anything past two seconds is faded and cut: it's a click, not a loop.

#include <JuceHeader.h>
#include <map>
#include <mutex>

namespace earfirst
{

struct ClickSample
{
    juce::AudioBuffer<float> audio;   // stereo, at the rate it was made for
    double sampleRate = 0;
};

class ClickSamples
{
public:
    static ClickSamples& get();

    // Where imported sounds live. The app sets it at start; tests point it at a temp folder.
    void setFolder (const juce::File& f)        { std::lock_guard<std::mutex> g (lock); folder = f; }
    juce::File getFolder() const                { std::lock_guard<std::mutex> g (lock); return folder; }

    // The recorded sounds that come with Ear First, picked by ear.
    static juce::StringArray builtInNames()     { return { "Clave", "Cowbell 1", "Cowbell 2", "Hi-hat" }; }
    // Names of the imported sounds, in alphabetical order.
    juce::StringArray names() const;
    static juce::StringArray audioExtensions()  { return { "wav", "aif", "aiff", "flac", "mp3", "m4a", "ogg", "caf" }; }

    // Copies files in. Returns the names they're known by; `problems` says what couldn't be.
    juce::StringArray import (const juce::Array<juce::File>&, juce::StringArray& problems);
    bool remove (const juce::String& name);

    // Message thread: the sound ready to play at this rate (built in or
    // imported), or null if it's missing or unreadable. The result stays valid until the app quits, so
    // the audio thread can play it without holding a reference.
    std::shared_ptr<const ClickSample> load (const juce::String& name, double sampleRate);

private:
    juce::File fileFor (const juce::String& name) const;
    mutable std::mutex lock;
    juce::File folder;
    std::map<juce::String, std::shared_ptr<const ClickSample>> cache;   // never emptied: voices may still be playing
};

} // namespace earfirst
