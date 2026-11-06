#pragma once
// What the app says, and how much: your choice, kind by kind.
//
// Every message has a kind (Transport, Capture, Click, Errors...) and, where
// it helps, a short version. For each kind you choose Full, Short or Off.
// Two kinds can't be turned all the way off: errors (something went wrong)
// and answers (a report you asked for); the least they go to is Short.
// Nothing is lost either way: everything goes into the speech history in full.

#include <juce_core/juce_core.h>
#include <map>

namespace earfirst
{

struct Message
{
    juce::String full, brief;      // brief: the short version; empty means "the first sentence"
    juce::String kind;             // "Transport", "Capture", "Errors"...
    juce::String topic;            // messages on the same topic replace each other while waiting
    bool urgent = false;
    bool answer = false;           // a report you asked for
    int toggle = -1;               // a toggle's new state: 1 on, 0 off, -1 not a toggle
};

class SpeechPrefs
{
public:
    enum class Detail { full, brief, sound, off };
    enum class Places { barsAndTime, bars, time };

    struct Kind { juce::String id, description; Detail fallback; bool canBeOff; };
    static const std::vector<Kind>& kinds();
    static juce::StringArray detailNames()      { return { "Full", "Short", "Sound", "Off" }; }
    static juce::StringArray placeNames()       { return { "Bars and time", "Bars and beats", "Time" }; }

    Detail detailFor (const juce::String& kind) const;
    void setDetail (const juce::String& kind, Detail);
    Places places = Places::barsAndTime;

    // What to say for this message, or empty to say nothing.
    juce::String choose (const Message&) const;
    // True if this message should be a short tone instead of words.
    bool wantsSound (const Message&) const;
    // The tone for a message: two pitches in hertz, each kind its own, rising
    // for on, falling for off, low and falling for an error.
    static std::pair<float, float> earconFor (const Message&);
    static juce::String firstSentence (const juce::String&);

    juce::var toVar() const;
    void fromVar (const juce::var&);

private:
    std::map<juce::String, Detail> chosen;
};

} // namespace earfirst
