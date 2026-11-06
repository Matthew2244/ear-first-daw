// speech-test: the speaking layer's timing rules, checked with a pretend clock.
// Output is one plain sentence per line, written for a screen reader.

#include <JuceHeader.h>
#include "speech/Speech.h"
#include <iostream>

using namespace earfirst;

namespace
{
void say (const juce::String& s)    { std::cout << s << std::endl; }

int failures = 0;
void check (bool ok, const juce::String& what)
{
    if (! ok) ++failures;
    say ((ok ? "Pass: " : "FAIL: ") + what);
}

struct Recorder : SpeechOutput
{
    struct Line { juce::String text; bool interrupt; double at; };
    std::vector<Line> lines;
    double* clock = nullptr;
    void output (const juce::String& t, bool i) override   { lines.push_back ({ t, i, *clock }); }
    juce::String last() const   { return lines.empty() ? juce::String() : lines.back().text; }
};

// Run the speech clock forward in 50 ms ticks with the music at a level.
void run (Speech& s, double& t, double seconds, float level)
{
    for (double end = t + seconds; t < end - 1e-9; t += 0.05)
    {
        s.setOutputLevel (level);
        s.tick (t);
    }
}
} // namespace

int main()
{
    say ("Speaking layer test, with a pretend clock. Nothing is spoken aloud.");
    double t = 0;
    Recorder rec; rec.clock = &t;
    Speech sp (rec);
    sp.clock = [&t] { return t; };
    const float loud = 0.5f, silent = 0.0f;

    say ("");
    say ("Song stopped");
    sp.say ("Metronome on");
    check (rec.lines.size() == 1 && ! rec.lines[0].interrupt, "with the song stopped, a message is said at once, without cutting anything off.");

    say ("");
    say ("Song playing, music loud");
    sp.setPlaying (true);
    rec.lines.clear();
    sp.say ("Kept 30 seconds on 8 inputs.");
    run (sp, t, 2.0, loud);
    check (rec.lines.empty(), "while the music is loud, a normal message waits.");
    run (sp, t, 0.2, silent);
    check (rec.lines.empty(), "a quiet moment shorter than a quarter second isn't a gap.");
    run (sp, t, 0.4, silent);
    check (rec.lines.size() == 1 && rec.last() == "Kept 30 seconds on 8 inputs.", "at a real gap it's said.");

    say ("");
    say ("Urgent");
    rec.lines.clear();
    sp.say ("Recording dropped on input 3.", Speech::Priority::urgent);
    check (rec.lines.size() == 1 && rec.lines[0].interrupt, "an urgent message cuts in at once, even mid-song.");

    say ("");
    say ("The longest wait");
    rec.lines.clear();
    const double queued = t;
    sp.say ("Saved.");
    run (sp, t, 7.0, loud);
    check (rec.lines.size() == 1 && rec.lines[0].at - queued <= 6.05, "in music with no gap, it's said anyway after the longest wait, "
           + juce::String (rec.lines.empty() ? -1.0 : rec.lines[0].at - queued, 2) + " seconds.");

    say ("");
    say ("Same topic replaces what's waiting");
    rec.lines.clear();
    sp.say ("Bar 5.", Speech::Priority::quiet, "position");
    run (sp, t, 0.5, loud);
    sp.say ("Bar 6.", Speech::Priority::quiet, "position");
    run (sp, t, 0.5, loud);
    sp.say ("Bar 7.", Speech::Priority::quiet, "position");
    run (sp, t, 0.6, silent);
    check (rec.lines.size() == 1 && rec.last() == "Bar 7.", "only the newest position is said, never stale ones ("
           + juce::String ((int) rec.lines.size()) + " said: " + rec.last() + ").");

    say ("");
    say ("A pile of messages");
    rec.lines.clear();
    for (auto m : { "Track 1 armed.", "Track 2 armed.", "Track 3 armed.", "Track 4 armed.", "Track 5 armed." })
        sp.say (m);
    run (sp, t, 0.6, silent);
    check (rec.lines.size() == 1 && rec.last() == "Track 5 armed. 4 more waiting.", "five at once collapse: \"" + rec.last() + "\"");
    check (sp.numWaiting() == 4, "the other four are kept.");
    run (sp, t, 2.0, silent);
    check (rec.lines.size() == 1, "and they don't come out by themselves to talk over the next phrase.");
    sp.readWaiting();
    check (rec.last() == "Track 1 armed. Track 2 armed. Track 3 armed. Track 4 armed." && sp.numWaiting() == 0, "read waiting says them all, in order.");

    say ("");
    say ("Stopping the song lets waiting messages out");
    rec.lines.clear();
    sp.say ("Loop on.");
    run (sp, t, 1.0, loud);
    sp.setPlaying (false);
    run (sp, t, 0.05, loud);
    check (rec.lines.size() == 1 && rec.last() == "Loop on.", "as soon as the song stops, it's said.");

    say ("");
    say ("Waiting can be turned off");
    sp.setPlaying (true);
    sp.settings.waitForGaps = false;
    rec.lines.clear();
    sp.say ("Mute on.");
    check (rec.lines.size() == 1, "with waiting off, it's said at once even while the song plays.");
    sp.settings.waitForGaps = true;

    say ("");
    say ("History");
    rec.lines.clear();
    sp.repeatLast();
    check (rec.last() == "Mute on.", "say that again repeats the last message.");
    sp.historyBack();
    check (rec.last() == "Loop on.", "stepping back gives the one before.");
    sp.historyForward();
    check (rec.last() == "Mute on.", "stepping forward comes back.");
    sp.historyForward();
    check (rec.last().startsWith ("That's the newest."), "past the newest it says so.");

    say ("");
    say ("Stop speaking");
    sp.setPlaying (true);
    sp.say ("One."); sp.say ("Two.");
    sp.stopSpeaking();
    check (sp.numWaiting() == 0 && rec.lines.back().text.isEmpty() && rec.lines.back().interrupt, "stop drops everything waiting and silences the voice.");

    say ("");
    say (failures == 0 ? "All checks passed." : juce::String (failures) + " checks failed.");
    return failures == 0 ? 0 : 1;
}
