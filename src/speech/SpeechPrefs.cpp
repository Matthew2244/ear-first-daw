#include "speech/SpeechPrefs.h"

namespace earfirst
{

const std::vector<SpeechPrefs::Kind>& SpeechPrefs::kinds()
{
    static const std::vector<Kind> list = {
        { "Transport", "Playing, stopping, the playhead and looping.", Detail::full, true },
        { "Click", "The click turning on and off and its settings.", Detail::full, true },
        { "Song", "Tempo and time signature changes.", Detail::full, true },
        { "Capture", "Keeping takes and recovered audio.", Detail::full, true },
        { "Tracks", "Adding and moving between tracks.", Detail::full, true },
        { "Edit", "Undo and redo.", Detail::full, true },
        { "File", "New, open, save and close.", Detail::full, true },
        { "Audio", "The audio interface.", Detail::full, true },
        { "View", "Text size.", Detail::full, true },
        { "Speech", "Changes to these settings.", Detail::full, true },
        { "Help", "Key help and Find a command.", Detail::full, true },
        { "Search", "How many commands match as you type in Find a command.", Detail::full, true },
        { "Forms", "A form's first setting as it opens, and each new choice.", Detail::full, true },
        { "Startup", "What the app says when it opens.", Detail::full, true },
        { "Updates", "A new version being out.", Detail::full, true },
        { "Answers", "Reports you ask for, like Where am I. Never fully off.", Detail::full, false },
        { "Errors", "Something that couldn't be done, and why. Never fully off.", Detail::full, false },
    };
    return list;
}

SpeechPrefs::Detail SpeechPrefs::detailFor (const juce::String& kind) const
{
    if (auto it = chosen.find (kind); it != chosen.end()) return it->second;
    for (auto& k : kinds()) if (k.id == kind) return k.fallback;
    return Detail::full;
}

void SpeechPrefs::setDetail (const juce::String& kind, Detail d)
{
    for (auto& k : kinds())
        if (k.id == kind && ! k.canBeOff && d == Detail::off) d = Detail::brief;
    chosen[kind] = d;
}

juce::String SpeechPrefs::firstSentence (const juce::String& s)
{
    // Up to the first full stop that ends a sentence (not a decimal point).
    for (int i = 0; i < s.length(); ++i)
        if ((s[i] == '.' || s[i] == '?' || s[i] == '!') && (i + 1 == s.length() || s[i + 1] == ' '))
            return s.substring (0, i + 1);
    return s;
}

juce::String SpeechPrefs::choose (const Message& m) const
{
    if (m.full.isEmpty()) return {};
    auto kind = m.answer ? juce::String ("Answers") : m.kind;
    switch (detailFor (kind))
    {
        case Detail::off:   return {};
        case Detail::brief: return m.brief.isNotEmpty() ? m.brief : firstSentence (m.full);
        case Detail::full:  break;
    }
    return m.full;
}

juce::var SpeechPrefs::toVar() const
{
    auto* o = new juce::DynamicObject();
    for (auto& [k, d] : chosen)
        o->setProperty (k, detailNames()[(int) d]);
    o->setProperty ("_places", placeNames()[(int) places]);
    return juce::var (o);
}

void SpeechPrefs::fromVar (const juce::var& v)
{
    chosen.clear();
    if (auto* o = v.getDynamicObject())
        for (auto& p : o->getProperties())
        {
            if (p.name.toString() == "_places")
            {
                const int i = placeNames().indexOf (p.value.toString());
                if (i >= 0) places = (Places) i;
                continue;
            }
            const int i = detailNames().indexOf (p.value.toString());
            if (i >= 0) setDetail (p.name.toString(), (Detail) i);
        }
}

} // namespace earfirst
