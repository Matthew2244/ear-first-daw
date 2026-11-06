#pragma once
// The click: a metronome that lives in the engine, locked to the song's
// tempo and meter to the sample, on its own track that never goes into a mix.
//
// What it does now:
//  - Different sounds for the first beat of the bar, the other beats, and
//    subdivisions, from a small set made in code (no samples to lose).
//  - Each beat loud, normal, soft or silent, from a pattern like "L N S N".
//  - Subdivisions (eighths, triplets, sixteenths, up to 8 per beat), with swing.
//  - Count-in, and click only during the count-in if you want.
// Coming next on the same track: grooves as the click (a loop, a MIDI file,
// your own drum take, any instrument), layers, and a spoken count.

#include <JuceHeader.h>

namespace earfirst
{

struct ClickSettings
{
    int subdivision = 1;             // ticks per beat: 1 beats only, 2 eighths, 3 triplets, 4 sixteenths
    float swing = 0.0f;              // 0..0.75: how late the offbeat subdivisions fall
    juce::String accents;            // per beat in the bar: L loud, N normal, S soft, 0 silent (repeats); empty = the one loud
    float subdivisionLevel = 0.5f;   // subdivisions, relative to a normal beat
    juce::String barSound = "Woodblock", beatSound = "Woodblock", subdivisionSound = "Woodblock";
    float level = 0.8f;
    int countInBars = 0;
    bool countInOnly = false;        // click only during the count-in, then stop

    static juce::StringArray sounds()   { return { "Woodblock", "Beep", "Clave", "Rim", "Cowbell", "Hi-hat" }; }
    juce::String describe() const;   // one sentence, for the status key
};

struct ClickTick
{
    enum class Kind { bar, beat, subdivision };
    tracktion::TimePosition time;
    Kind kind;
    float level;                     // 0 = silent
};

// Every tick that falls in [range), in order. Pure, so it's tested directly.
std::vector<ClickTick> clickTicks (const tracktion::TempoSequence&, tracktion::TimeRange, const ClickSettings&);

class ClickPlugin : public tracktion::Plugin
{
public:
    ClickPlugin (tracktion::PluginCreationInfo);
    ~ClickPlugin() override;

    static const char* getPluginName()                  { return "Ear First Click"; }
    static const char* xmlTypeName;
    juce::String getName() const override               { return getPluginName(); }
    juce::String getPluginType() override               { return xmlTypeName; }
    juce::String getShortName (int) override            { return "Click"; }
    juce::String getSelectableDescription() override    { return getName(); }
    bool producesAudioWhenNoAudioInput() override       { return true; }
    bool isSynth() override                             { return true; }
    bool takesAudioInput() override                     { return false; }
    int getNumOutputChannelsGivenInputs (int) override  { return 2; }
    BusLayout getBusses() const override                { return BusLayout::singleStereoInOut(); }

    void initialise (const tracktion::PluginInitialisationInfo&) override;
    void deinitialise() override;
    void applyToBuffer (const tracktion::PluginRenderContext&) override;

    ClickSettings getSettings() const;
    void setSettings (const ClickSettings&);

    // Set when playing with a count-in: where the count-in ends on the timeline.
    std::atomic<double> countInEndsAt { -1.0e9 };

    juce::CachedValue<int> subdivision, countInBars;
    juce::CachedValue<float> swing, subdivisionLevel, level;
    juce::CachedValue<juce::String> accents, barSound, beatSound, subdivisionSound;
    juce::CachedValue<bool> countInOnly;

private:
    struct Voice { int sound = 0; double pitch = 1.0, phase = 0, phase2 = 0; float amp = 0; int age = 0, length = 0; juce::uint32 noise = 1; };
    void start (int sound, double pitch, float amp, int offset);
    void render (float* left, float* right, int start, int num);

    std::vector<Voice> voices;
    std::vector<std::pair<int, Voice>> pending;   // voices starting part-way through this block
    juce::CriticalSection settingsLock;
    ClickSettings cached;
};

} // namespace earfirst
