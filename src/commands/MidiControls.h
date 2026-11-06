#pragma once
// MIDI controls: any pedal, footswitch, pad, key or button on a MIDI device
// can run any command, the way keys do.
//
// A control is a note being pressed, a controller rising past the halfway
// point (a footswitch going down), or a program change. Learning one is two
// steps: choose the command, then press the control.

#include "commands/Commands.h"
#include <juce_audio_basics/juce_audio_basics.h>

namespace earfirst
{

struct MidiControl
{
    enum class Type { note, controller, program };
    juce::String device;          // empty = any device
    Type type = Type::controller;
    int channel = 0;              // 1-16; 0 = any channel
    int number = 0;               // note, controller or program number
    juce::String commandId;
    CommandArgs args;

    // Does this message press this control? (Releases and the second half of a
    // footswitch don't count, so a press runs the command once.)
    bool pressedBy (const juce::String& fromDevice, const juce::MidiMessage&, int previousValue) const;
    juce::String describe() const;   // "Controller 85 on channel 1 from KONTROL S88"
};

class MidiControls
{
public:
    explicit MidiControls (const CommandList& c) : commands (c) {}

    // The control a message would make, for learning; nothing if it isn't one.
    static std::optional<MidiControl> controlFor (const juce::String& device, const juce::MidiMessage&);

    void add (MidiControl);          // replaces any binding for the same control
    bool remove (int index);
    const std::vector<MidiControl>& all() const     { return bindings; }

    // Which command, if any, a message runs. Keeps track of controller values
    // so a footswitch held down doesn't fire again on each wobble.
    const MidiControl* match (const juce::String& device, const juce::MidiMessage&);

    // Note where a controller is without running anything (the press that
    // was just learned, so holding it doesn't fire straight away).
    void remember (const juce::String& device, const juce::MidiMessage&);

    juce::String listSentence() const;

    juce::var toVar() const;
    void fromVar (const juce::var&);

private:
    const CommandList& commands;
    std::vector<MidiControl> bindings;
    std::map<juce::String, int> lastValue;   // device|channel|controller -> last value
};

} // namespace earfirst
