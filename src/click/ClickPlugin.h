#pragma once
// The click: a metronome that lives in the engine, locked to the song's
// tempo and meter to the sample, on its own track that never goes into a mix.
//
// What it does now:
//  - Different sounds for the first beat of the bar, the other beats, and
//    subdivisions: recorded sounds that come with Ear First (picked by ear),
//    the Beep, or sounds you import.
//  - Each beat loud, normal, soft or silent, from a pattern like "L N S N".
//  - Subdivisions (eighths, triplets, sixteenths, up to 8 per beat), with swing.
//  - Count-in, and click only during the count-in if you want.
// Coming next on the same track: grooves as the click (a loop, a MIDI file,
// your own drum take, any instrument), layers, and a spoken count.

#include "click/ClickSounds.h"
#include "click/ClickSamples.h"
#include <JuceHeader.h>

namespace earfirst
{

struct ClickSettings
{
    int subdivision = 1;             // ticks per beat: 1 beats only, 2 eighths, 3 triplets, 4 sixteenths
    float swing = 0.0f;              // 0..0.75: how late the offbeat subdivisions fall
    juce::String accents;            // per beat in the bar: L loud, N normal, S soft, 0 silent (repeats); empty = the one loud
    float subdivisionLevel = 0.5f;   // subdivisions, relative to a normal beat
    juce::String barSound = "Cowbell 1", beatSound = "Clave", subdivisionSound = "Hi-hat";
    float level = 0.8f;
    int countInBars = 0;
    bool countInOnly = false;        // click only during the count-in, then stop

    static juce::StringArray builtInSounds()   { auto s = ClickSamples::builtInNames(); s.add ("Beep"); return s; }
    // The built-in sounds, then any you've imported.
    static juce::StringArray sounds()          { auto s = builtInSounds(); s.addArray (ClickSamples::get().names()); return s; }
    juce::String describe() const;   // one sentence, for the status key
};

struct ClickTick
{
    enum class Kind { bar, beat, subdivision };
    tracktion::TimePosition time;
    Kind kind;
    float level;                     // 0 = silent
};

// Every tick that falls in [range), in order, appended to `out`. Works with
// the edit's tempo sequence or a copy of it (what the audio thread uses).
// Negative times are counted in too, so a count-in before bar 1 clicks.
template <typename TempoMap>
void clickTicks (const TempoMap&, tracktion::TimeRange, const ClickSettings&, std::vector<ClickTick>& out);

template <typename TempoMap>
std::vector<ClickTick> clickTicks (const TempoMap& t, tracktion::TimeRange r, const ClickSettings& s)
{
    std::vector<ClickTick> out;
    clickTicks (t, r, s, out);
    return out;
}

class ClickPlugin : public tracktion::Plugin, private juce::AsyncUpdater
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
    void refreshNow()                                   { rebuildSnapshot(); }   // for tests: don't wait for the message loop

    // Set when playing with a count-in: where the count-in ends on the timeline.
    std::atomic<double> countInEndsAt { -1.0e9 };

    juce::CachedValue<int> subdivision, countInBars;
    juce::CachedValue<float> swing, subdivisionLevel, level;
    juce::CachedValue<juce::String> accents, barSound, beatSound, subdivisionSound;
    juce::CachedValue<bool> countInOnly;

private:
    using Voice = ClickVoice;
    void render (float* left, float* right, int start, int num);

    // Everything the audio thread needs, made on the message thread whenever
    // the tempo, the meter or the click's settings change (undo included).
    // The audio thread only reads it: no locks, no tempo rebuilds, no
    // allocation there.
    struct Snapshot
    {
        tracktion::tempo::Sequence tempo;
        ClickSettings settings;
        // The recorded sound for each (null: the Beep). Kept alive by
        // ClickSamples for the whole run, so voices can point into them.
        std::shared_ptr<const ClickSample> barSample, beatSample, subdivisionSample;
    };
    void rebuildSnapshot();
    void handleAsyncUpdate() override                           { rebuildSnapshot(); }
    struct Watcher : juce::ValueTree::Listener
    {
        explicit Watcher (ClickPlugin& p) : owner (p) {}
        void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override { owner.triggerAsyncUpdate(); }
        void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override             { owner.triggerAsyncUpdate(); }
        void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override      { owner.triggerAsyncUpdate(); }
        ClickPlugin& owner;
    };
    Watcher watcher { *this };

    std::shared_ptr<const Snapshot> snapshot;
    std::vector<std::shared_ptr<const Snapshot>> retired;   // freed here on the message thread, never on the audio thread
    juce::ValueTree tempoState;

    std::vector<Voice> voices;
    std::vector<std::pair<int, Voice>> pending;   // voices starting part-way through this block
    std::vector<ClickTick> ticks;
};

} // namespace earfirst
