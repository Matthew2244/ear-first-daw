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
        { "Answers", "Reports you ask for, like Where am I. Always said, full or short.", Detail::full, false },
        { "Errors", "Something that couldn't be done, and why. Never silent: full, short or a sound.", Detail::full, false },
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
    if (kind == "Answers" && d == Detail::sound) d = Detail::brief;   // you asked a question: words
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
        case Detail::off:
        case Detail::sound: return {};
        case Detail::brief: return m.brief.isNotEmpty() ? m.brief : firstSentence (m.full);
        case Detail::full:  break;
    }
    return m.full;
}

bool SpeechPrefs::wantsSound (const Message& m) const
{
    if (m.full.isEmpty()) return false;
    return detailFor (m.answer ? juce::String ("Answers") : m.kind) == Detail::sound;
}

std::pair<float, float> SpeechPrefs::earconFor (const Message& m)
{
    // Each kind its own note, from a pentatonic scale so any two sound well together.
    static const float scale[] = { 523.3f, 587.3f, 659.3f, 784.0f, 880.0f, 1046.5f, 1174.7f, 1318.5f, 1568.0f, 1760.0f };
    int index = 0;
    for (size_t i = 0; i < kinds().size(); ++i)
        if (kinds()[i].id == m.kind) index = (int) i;
    const float f = scale[index % 10];
    if (m.kind == "Errors") return { 330.0f, 247.0f };   // low and falling
    if (m.toggle == 1) return { f, f * 1.5f };           // on: rising a fifth
    if (m.toggle == 0) return { f * 1.5f, f };           // off: falling
    return { f, f };                                     // done: one note, twice as long
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
