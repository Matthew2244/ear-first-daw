#pragma once
// The speaking layer: one place decides what gets said, and when.
//
//  - Urgent things ("Recording dropped") cut in at once.
//  - Everything else waits for a gap in the music while the song plays: the
//    output going quiet for a moment, or the song stopping. A setting turns
//    the waiting off, and nothing ever waits longer than the longest wait.
//  - A message about something that keeps changing carries a topic. A newer
//    message on the same topic replaces one still waiting, so you never hear
//    stale positions or levels.
//  - Nothing is lost. Every message goes into a history you can step back
//    through, and "say that again" repeats the last one.
//  - Too many messages waiting at once collapse into one: "4 messages
//    waiting", read on request.
//
// The queue is pure logic with its own clock, so it is tested without a
// screen reader. Outputs (VoiceOver, NVDA/JAWS through Prism, a visible
// status line) plug in through SpeechOutput.

#include <juce_core/juce_core.h>
#include <atomic>
#include <deque>
#include <functional>

namespace earfirst
{

struct SpeechOutput
{
    virtual ~SpeechOutput() = default;
    // interrupt: stop whatever is being said and say this now.
    virtual void output (const juce::String& text, bool interrupt) = 0;
};

class Speech
{
public:
    enum class Priority { urgent, normal, quiet };   // quiet: can be dropped if replaced

    struct Settings
    {
        bool waitForGaps = true;        // while the song plays, hold normal messages for a gap
        double gapSeconds = 0.25;       // how long the output must be quiet to count as a gap
        double gapLevelDb = -45.0;      // quieter than this counts as quiet
        double longestWaitSeconds = 6.0;// say it anyway after this long
        int collapseOver = 3;           // more than this many waiting: "4 messages waiting"
        int historySize = 200;
    };

    explicit Speech (SpeechOutput&);

    // Message thread.
    // `historyText` is what goes into the history (the full message) when it
    // differs from what's spoken: a short version, or nothing at all.
    void say (const juce::String& text, Priority = Priority::normal, const juce::String& topic = {},
              const juce::String& historyText = {});
    void sayUrgent (const juce::String& text)      { say (text, Priority::urgent); }

    // Call regularly (a timer, ~20 times a second) with the time in seconds.
    void tick (double now);

    // The song's state. The level is set from the audio thread.
    void setPlaying (bool);
    // Keeps the loudest level since the last tick, so a short loud note between ticks still counts.
    void setOutputLevel (float peakGain) noexcept
    {
        auto prev = outputPeak.load (std::memory_order_relaxed);
        while (peakGain > prev && ! outputPeak.compare_exchange_weak (prev, peakGain, std::memory_order_relaxed)) {}
    }

    // What you can ask for.
    void repeatLast();                  // "say that again"
    void readWaiting();                 // speak everything waiting, now
    void historyBack();                 // step back one message
    void historyForward();
    void stopSpeaking();                // drop everything waiting
    int numWaiting() const              { return (int) (waiting.size() + parked.size()); }

    Settings settings;
    std::function<double()> clock;      // for tests; default is the real time

private:
    struct Item { juce::String text, topic; Priority priority; double queuedAt; };
    void deliver (const juce::String&, bool interrupt);
    void flushOne();
    double now() const;

    SpeechOutput& out;
    std::deque<Item> waiting;           // goes out at the next gap
    std::deque<Item> parked;            // a pile that collapsed; only "read waiting" brings it out
    juce::StringArray history;
    int historyCursor = -1;             // -1 = at the newest
    bool playing = false;
    std::atomic<float> outputPeak { 0.0f };
    double quietSince = -1.0;
    double lastTick = 0.0;
};

} // namespace earfirst
