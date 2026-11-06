#include "speech/Speech.h"

namespace earfirst
{

Speech::Speech (SpeechOutput& o) : out (o) {}

double Speech::now() const
{
    return clock != nullptr ? clock() : juce::Time::getMillisecondCounterHiRes() / 1000.0;
}

void Speech::deliver (const juce::String& text, bool interrupt)
{
    if (text.isNotEmpty())
        out.output (text, interrupt);
}

void Speech::say (const juce::String& text, Priority priority, const juce::String& topic)
{
    if (text.isEmpty()) return;

    history.add (text);
    while (history.size() > settings.historySize) history.remove (0);
    historyCursor = -1;

    // A newer message on the same topic replaces one still waiting.
    if (topic.isNotEmpty())
        waiting.erase (std::remove_if (waiting.begin(), waiting.end(), [&] (auto& i) { return i.topic == topic; }), waiting.end());

    if (priority == Priority::urgent)
    {
        deliver (text, true);
        return;
    }
    if (! playing || ! settings.waitForGaps)
    {
        deliver (text, false);
        return;
    }
    waiting.push_back ({ text, topic, priority, now() });
}

void Speech::setPlaying (bool p)
{
    playing = p;
    if (! p) quietSince = -1.0;
}

void Speech::tick (double t)
{
    lastTick = t;
    const float threshold = std::pow (10.0f, (float) settings.gapLevelDb / 20.0f);
    const float peak = outputPeak.exchange (0.0f, std::memory_order_relaxed);

    if (peak < threshold) { if (quietSince < 0) quietSince = t; }
    else quietSince = -1.0;

    if (waiting.empty()) return;

    const bool gap = quietSince >= 0 && t - quietSince >= settings.gapSeconds;
    const bool tooLong = t - waiting.front().queuedAt >= settings.longestWaitSeconds;
    if (! playing || ! settings.waitForGaps || gap || tooLong)
        flushOne();
}

void Speech::flushOne()
{
    // Everything that's been waiting goes out together at the gap. If it's a
    // pile, say the newest and how many more there are, and keep the rest
    // for "read waiting" rather than talking through the next phrase.
    if ((int) waiting.size() > settings.collapseOver)
    {
        auto newest = waiting.back().text;
        waiting.pop_back();
        for (auto& i : waiting) parked.push_back (i);
        waiting.clear();
        deliver (newest + " " + juce::String ((int) parked.size()) + " more waiting.", false);
        return;
    }
    while (! waiting.empty())
    {
        deliver (waiting.front().text, false);
        waiting.pop_front();
    }
}

void Speech::readWaiting()
{
    if (waiting.empty() && parked.empty()) { deliver ("Nothing waiting.", true); return; }
    juce::StringArray all;
    for (auto& i : parked) all.add (i.text);
    for (auto& i : waiting) all.add (i.text);
    parked.clear();
    waiting.clear();
    deliver (all.joinIntoString (" "), true);
}

void Speech::repeatLast()
{
    historyCursor = -1;
    deliver (history.isEmpty() ? juce::String ("Nothing has been said yet.") : history[history.size() - 1], true);
}

void Speech::historyBack()
{
    if (history.isEmpty()) { deliver ("Nothing has been said yet.", true); return; }
    if (historyCursor < 0) historyCursor = history.size() - 1;
    if (historyCursor == 0) { deliver ("That's the oldest. " + history[0], true); return; }
    --historyCursor;
    deliver (history[historyCursor], true);
}

void Speech::historyForward()
{
    if (historyCursor < 0 || historyCursor >= history.size() - 1)
    {
        historyCursor = -1;
        deliver ("That's the newest." + (history.isEmpty() ? juce::String() : " " + history[history.size() - 1]), true);
        return;
    }
    ++historyCursor;
    deliver (history[historyCursor], true);
}

void Speech::stopSpeaking()
{
    waiting.clear();
    parked.clear();
    out.output ({}, true);
}

} // namespace earfirst
