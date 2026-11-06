#include "commands/MidiControls.h"

namespace earfirst
{

namespace
{
juce::String typeWord (MidiControl::Type t)
{
    return t == MidiControl::Type::note ? "Note" : t == MidiControl::Type::program ? "Program change" : "Controller";
}

juce::String noteName (int n)
{
    return juce::MidiMessage::getMidiNoteName (n, true, true, 4);
}
} // namespace

bool MidiControl::pressedBy (const juce::String& from, const juce::MidiMessage& m, int previous) const
{
    if (device.isNotEmpty() && device != from) return false;
    if (channel != 0 && m.getChannel() != channel) return false;
    switch (type)
    {
        case Type::note:       return m.isNoteOn() && m.getNoteNumber() == number;
        case Type::program:    return m.isProgramChange() && m.getProgramChangeNumber() == number;
        case Type::controller: return m.isController() && m.getControllerNumber() == number
                                      && m.getControllerValue() >= 64 && previous < 64;
    }
    return false;
}

juce::String MidiControl::describe() const
{
    juce::String s = typeWord (type) + " ";
    s << (type == Type::note ? noteName (number) : juce::String (number));
    if (type == Type::controller && number == 64) s << " (sustain pedal)";
    if (channel != 0) s << " on channel " << channel;
    if (device.isNotEmpty()) s << " from " << device;
    return s;
}

std::optional<MidiControl> MidiControls::controlFor (const juce::String& device, const juce::MidiMessage& m)
{
    MidiControl c;
    c.device = device;
    c.channel = m.getChannel();
    if (m.isNoteOn()) { c.type = MidiControl::Type::note; c.number = m.getNoteNumber(); return c; }
    if (m.isController() && m.getControllerValue() >= 64) { c.type = MidiControl::Type::controller; c.number = m.getControllerNumber(); return c; }
    if (m.isProgramChange()) { c.type = MidiControl::Type::program; c.number = m.getProgramChangeNumber(); return c; }
    return std::nullopt;
}

void MidiControls::add (MidiControl c)
{
    bindings.erase (std::remove_if (bindings.begin(), bindings.end(), [&] (auto& b)
    {
        return b.device == c.device && b.type == c.type && b.channel == c.channel && b.number == c.number;
    }), bindings.end());
    bindings.push_back (std::move (c));
}

bool MidiControls::remove (int index)
{
    if (index < 0 || index >= (int) bindings.size()) return false;
    bindings.erase (bindings.begin() + index);
    return true;
}

const MidiControl* MidiControls::match (const juce::String& device, const juce::MidiMessage& m)
{
    int previous = 0;
    if (m.isController())
    {
        auto key = device + "|" + juce::String (m.getChannel()) + "|" + juce::String (m.getControllerNumber());
        previous = lastValue.count (key) ? lastValue[key] : 0;
        lastValue[key] = m.getControllerValue();
    }
    for (auto& b : bindings)
        if (b.pressedBy (device, m, previous))
            return &b;
    return nullptr;
}

void MidiControls::remember (const juce::String& device, const juce::MidiMessage& m)
{
    if (m.isController())
        lastValue[device + "|" + juce::String (m.getChannel()) + "|" + juce::String (m.getControllerNumber())] = m.getControllerValue();
}

juce::String MidiControls::listSentence() const
{
    if (bindings.empty()) return "No MIDI controls are assigned. Assign a MIDI control sets one up.";
    juce::StringArray parts;
    for (auto& b : bindings)
    {
        auto* c = commands.find (b.commandId);
        parts.add (b.describe() + " runs " + (c ? c->name : b.commandId));
    }
    return juce::String ((int) bindings.size()) + (bindings.size() == 1 ? " MIDI control: " : " MIDI controls: ")
           + parts.joinIntoString ("; ") + ".";
}

juce::var MidiControls::toVar() const
{
    juce::Array<juce::var> list;
    for (auto& b : bindings)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("device", b.device);
        o->setProperty ("type", typeWord (b.type));
        o->setProperty ("channel", b.channel);
        o->setProperty ("number", b.number);
        o->setProperty ("command", b.commandId);
        list.add (juce::var (o));
    }
    return list;
}

void MidiControls::fromVar (const juce::var& v)
{
    bindings.clear();
    if (auto* arr = v.getArray())
        for (auto& o : *arr)
        {
            MidiControl c;
            c.device = o["device"].toString();
            auto t = o["type"].toString();
            c.type = t == "Note" ? MidiControl::Type::note : t == "Program change" ? MidiControl::Type::program : MidiControl::Type::controller;
            c.channel = (int) o["channel"];
            c.number = (int) o["number"];
            c.commandId = o["command"].toString();
            if (commands.find (c.commandId) != nullptr) bindings.push_back (c);
        }
}

} // namespace earfirst
