// commands-test: the command list and keymap, checked without any window.
// Output is one plain sentence per line, written for a screen reader.

#include <JuceHeader.h>
#include "commands/Commands.h"
#include "commands/Keymap.h"
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

struct TestHost : CommandHost
{
    juce::StringArray spoken;
    int undoBegins = 0, undoEnds = 0;
    juce::StringArray undoNames;
    void speak (const juce::String& s, bool) override   { spoken.add (s); }
    void beginUndoStep (const juce::String& n) override  { ++undoBegins; undoNames.add (n); }
    void endUndoStep() override                          { ++undoEnds; }
    juce::String last() const                            { return spoken.isEmpty() ? juce::String() : spoken[spoken.size() - 1]; }
};

// A pretend session for the commands to act on.
struct Session
{
    bool playing = false, metronome = false;
    double position = 0;
    int selectedTrack = -1;
    juce::Array<bool> armed { false, false, false, false };
    int playCount = 0;
};

void addCommands (CommandList& list, Session& s)
{
    Command play;
    play.id = "transport.play"; play.name = "Play"; play.category = "Transport";
    play.description = "Starts the song from the playhead.";
    play.tags = { "start", "go" };
    play.run = [&s] (const CommandArgs&) { s.playing = true; ++s.playCount; return CommandResult::done(); };
    check (list.add (play).wasOk(), "a command with a good id is accepted.");

    Command start;
    start.id = "transport.go-to-start"; start.name = "Go to start"; start.category = "Transport";
    start.description = "Moves the playhead to the beginning of the song.";
    start.tags = { "home", "beginning" };
    start.run = [&s] (const CommandArgs&) { s.position = 0; return CommandResult::done ("Start."); };
    list.add (start);

    Command metro;
    metro.id = "transport.metronome"; metro.name = "Metronome"; metro.category = "Transport";
    metro.description = "Turns the click on or off.";
    metro.tags = { "click", "count" };
    metro.run = [&s] (const CommandArgs&) { s.metronome = ! s.metronome; return CommandResult::done(); };
    metro.toggleState = [&s] { return std::optional<bool> (s.metronome); };
    list.add (metro);

    Command nudge;
    nudge.id = "edit.nudge"; nudge.name = "Nudge"; nudge.category = "Editing";
    nudge.description = "Moves the playhead by an amount.";
    nudge.undoable = true;
    ArgSpec amount; amount.name = "amount"; amount.defaultValue = 1.0; amount.minimum = 0.0; amount.maximum = 16.0; amount.unit = "beats";
    ArgSpec dir; dir.name = "direction"; dir.type = ArgSpec::Type::choice; dir.choices = { "forward", "back" }; dir.defaultValue = "forward";
    nudge.args = { amount, dir };
    nudge.run = [&s] (const CommandArgs& a)
    {
        double by = a["amount"];
        s.position += a["direction"].toString() == "back" ? -by : by;
        return CommandResult::done ("Beat " + juce::String (s.position + 1.0, 2).trimCharactersAtEnd ("0").trimCharactersAtEnd ("."));
    };
    list.add (nudge);

    Command arm;
    arm.id = "track.arm"; arm.name = "Arm"; arm.category = "Tracks";
    arm.description = "Arms the selected track for recording.";
    arm.onWord = "armed"; arm.offWord = "not armed";
    arm.available = [&s] { return s.selectedTrack < 0 ? Availability::no ("No track is selected.") : Availability::yes(); };
    arm.run = [&s] (const CommandArgs&) { s.armed.set (s.selectedTrack, ! s.armed[s.selectedTrack]); return CommandResult::done(); };
    arm.toggleState = [&s] { return s.selectedTrack < 0 ? std::optional<bool>() : std::optional<bool> (s.armed[s.selectedTrack]); };
    list.add (arm);

    Command where;
    where.id = "report.position"; where.name = "Where am I"; where.category = "Reports"; where.isReport = true;
    where.description = "Says where the playhead is. Changes nothing.";
    where.run = [&s] (const CommandArgs&) { return CommandResult::done ("Beat " + juce::String ((int) s.position + 1) + "."); };
    list.add (where);

    Command broken;
    broken.id = "test.throws"; broken.name = "Broken thing"; broken.category = "Test";
    broken.description = "Always fails.";
    broken.run = [] (const CommandArgs&) -> CommandResult { throw std::runtime_error ("the disk went away"); };
    list.add (broken);
}
} // namespace

int main()
{
    say ("Command list and keymap test. No window is used.");
    TestHost host;
    CommandList list (host);
    Session s;

    say ("");
    say ("Registering");
    addCommands (list, s);
    Command dup; dup.id = "transport.play"; dup.name = "Play again"; dup.run = [] (const CommandArgs&) { return CommandResult::done(); };
    auto r = list.add (dup);
    check (r.failed(), "a second command with the same id is refused: " + r.getErrorMessage());
    Command bad; bad.id = "Transport Play"; bad.name = "x"; bad.run = dup.run;
    r = list.add (bad);
    check (r.failed(), "an id with capitals and spaces is refused: " + r.getErrorMessage());

    say ("");
    say ("Search");
    auto hits = list.search ("metro");
    check (! hits.empty() && hits[0]->id == "transport.metronome", "typing metro finds Metronome first.");
    hits = list.search ("click");
    check (! hits.empty() && hits[0]->id == "transport.metronome", "typing click finds Metronome through its tags.");
    hits = list.search ("go start");
    check (! hits.empty() && hits[0]->id == "transport.go-to-start", "typing go start finds Go to start first.");
    hits = list.search ("xylophone");
    check (hits.empty(), "a word that matches nothing finds nothing.");

    say ("");
    say ("Running and speaking");
    host.spoken.clear();
    list.run ("transport.metronome");
    check (host.last() == "Metronome on", "a toggle says its new state: \"" + host.last() + "\".");
    list.run ("transport.metronome");
    check (host.last() == "Metronome off", "and again: \"" + host.last() + "\".");

    auto res = list.run ("track.arm");
    check (! res.ok && host.last() == "No track is selected.", "a command that can't run says why: \"" + host.last() + "\".");
    s.selectedTrack = 2;
    list.run ("track.arm");
    check (s.armed[2] && host.last() == "Arm armed", "once a track is selected it runs and says \"" + host.last() + "\".");

    res = list.run ("test.throws");
    check (! res.ok && host.last().contains ("the disk went away"), "a command that throws reports the problem instead of crashing: \"" + host.last() + "\".");
    res = list.run ("transport.nonexistent");
    check (! res.ok && host.last().contains ("no command"), "an unknown command says so: \"" + host.last() + "\".");

    say ("");
    say ("Settings on commands");
    s.position = 0;
    list.run ("edit.nudge");
    check (s.position == 1.0, "nudge with no settings uses its default, one beat. Says \"" + host.last() + "\".");
    CommandArgs a; a.set ("amount", 4); a.set ("direction", "back");
    s.position = 8;
    list.run ("edit.nudge", a);
    check (s.position == 4.0, "nudge back 4 beats from 8 lands on 4.");
    CommandArgs tooBig; tooBig.set ("amount", 40);
    res = list.run ("edit.nudge", tooBig);
    check (! res.ok && s.position == 4.0, "a setting out of range is refused and nothing moves: \"" + host.last() + "\".");
    CommandArgs wrongName; wrongName.set ("speed", 2);
    res = list.run ("edit.nudge", wrongName);
    check (! res.ok, "a setting the command doesn't have is refused: \"" + host.last() + "\".");
    CommandArgs badChoice; badChoice.set ("direction", "sideways");
    res = list.run ("edit.nudge", badChoice);
    check (! res.ok, "a choice that isn't on the list is refused: \"" + host.last() + "\".");
    CommandArgs typed; typed.set ("amount", "2.5");
    s.position = 0;
    list.run ("edit.nudge", typed);
    check (s.position == 2.5, "a number typed as text works too, as it would from speech or a script.");

    say ("");
    say ("Undo");
    host.undoBegins = host.undoEnds = 0;
    list.run ("edit.nudge");
    check (host.undoBegins == 1 && host.undoEnds == 1 && host.undoNames[host.undoNames.size() - 1] == "Nudge",
           "a command that changes the song is one undo step, named Nudge.");
    list.run ("transport.play");
    check (host.undoBegins == 1, "playing isn't an undo step.");

    say ("");
    say ("Repeat");
    a.clear(); a.set ("amount", 3);
    s.position = 0;
    list.run ("edit.nudge", a);
    list.run ("report.position");
    list.repeatLast();
    check (s.position == 6.0, "repeat runs the last change again with the same setting, and skips the report in between.");

    say ("");
    say ("Macros");
    MacroStep toStart { "transport.go-to-start", {} }, playIt { "transport.play", {} };
    r = list.addMacro ("macro.play-from-start", "Play from start", "Goes to the start and plays.", { toStart, playIt });
    check (r.wasOk(), "a macro made from two commands is accepted.");
    host.spoken.clear(); host.undoBegins = host.undoEnds = 0;
    s.position = 12; s.playing = false;
    list.run ("macro.play-from-start");
    check (s.position == 0 && s.playing, "the macro goes to the start and plays.");
    check (host.spoken.size() == 1, "it speaks once, not once per step (" + juce::String (host.spoken.size()) + " sentences).");
    check (host.undoBegins == 1 && host.undoEnds == 1, "the whole macro is one undo step.");
    r = list.addMacro ("macro.bad", "Bad", "Uses a missing command.", { { "nothing.here", {} } });
    check (r.failed(), "a macro using a command that doesn't exist is refused: " + r.getErrorMessage());
    list.addMacro ("macro.arm-then-play", "Arm and play", "Arms the track, then plays.", { { "track.arm", {} }, playIt });
    s.selectedTrack = -1; s.playCount = 0; host.spoken.clear();
    res = list.run ("macro.arm-then-play");
    check (! res.ok && s.playCount == 0, "a macro stops at the step that can't run, and doesn't go on to play.");
    check (host.spoken.size() == 1 && host.last().contains ("step 1"), "it says once which step stopped it: \"" + host.last() + "\".");

    say ("");
    say ("Describing a command without running it");
    s.metronome = true;
    auto d = list.describe ("transport.metronome");
    check (d.startsWith ("Metronome, on.") && s.metronome, "\"" + d + "\"");
    d = list.describe ("edit.nudge");
    check (d.contains ("amount, normally 1 beat;") && d.contains ("direction, normally forward"), "\"" + d + "\"");

    say ("");
    say ("Keys: writing them for both systems");
    auto k = parseKeys ("Primary+Shift+K");
    check (keysForPeople (k, Platform::mac) == "Shift-Command-K", "Primary+Shift+K reads as Shift-Command-K on the Mac.");
    check (keysForPeople (k, Platform::windows) == "Control+Shift+K", "and Control+Shift+K on Windows.");
    check (keysToText (parseKeys ("Command-Shift-K", Platform::mac)) == "Primary+Shift+K", "a Mac spelling reads back to the shared form.");
    check (keysToText (parseKeys ("Ctrl+Shift+K", Platform::windows)) == "Primary+Shift+K", "so does a Windows spelling.");
    check (keysToText (parseKeys ("Control-Option-Space", Platform::mac)) == "Secondary+Alt+Space", "Mac Control is Secondary, Option is Alt.");
    check (keysForPeople (parseKeys ("Keypad5"), Platform::mac) == "keypad 5", "keypad keys say keypad.");
    check (keysForPeople (parseKeys ("Primary+K, M"), Platform::mac) == "Command-K, then M", "a two-key sequence says then.");
    check (keysToText (parseKeys ("Primary+Comma")) == "Primary+Comma" && keysForPeople (parseKeys ("Primary+Comma"), Platform::mac) == "Command-comma",
           "the comma key has a name, since a comma separates sequences.");
    check (keysToText (parseKeys ("Primary+-")) == "Primary+-", "the minus key works on its own after a plus.");
    check (parseKeys ("Banana+K").empty(), "a modifier that doesn't exist is refused.");

    say ("");
    say ("Keymap");
    Keymap km (list);
    km.addScope ("main", "global");
    km.addScope ("tracks", "main");
    km.setDefaults ({ { "Space", "transport.play", "global", {} },
                      { "Primary+M", "transport.metronome", "global", {} },
                      { "Alt+Right", "edit.nudge", "tracks", [] { CommandArgs x; x.set ("amount", 1); return x; }() },
                      { "Alt+Left", "edit.nudge", "tracks", [] { CommandArgs x; x.set ("amount", 1); x.set ("direction", "back"); return x; }() },
                      { "R", "track.arm", "tracks", {} } }, 1);

    auto hit = km.lookup (parseKeys ("Space"), "tracks");
    check (hit && hit->commandId == "transport.play", "Space in the track list falls through to Play from global.");
    hit = km.lookup (parseKeys ("R"), "global");
    check (! hit, "R does nothing outside the track list.");
    hit = km.lookup (parseKeys ("Alt+Left"), "tracks");
    check (hit && hit->args["direction"].toString() == "back", "Alt+Left in tracks is nudge, with direction back preset.");
    auto dk = km.describeKey ("Alt+Left", "tracks");
    check (dk.contains ("Nudge, amount 1 beat, direction back."), "what does this key do: \"" + dk + "\"");
    dk = km.describeKey ("Primary+J", "tracks");
    check (dk.contains ("does nothing here"), "\"" + dk + "\"");
    check (km.keysFor ("transport.metronome").contains (keysForPeople (parseKeys ("Primary+M"))), "a command can list its keys for menus and the palette.");
    check (km.conflicts().empty(), "the defaults have no conflicts.");

    km.bind ("Primary+M", "transport.go-to-start");
    hit = km.lookup (parseKeys ("Primary+M"), "global");
    check (hit && hit->commandId == "transport.go-to-start", "your key replaces the default on that key.");
    km.bind ("Primary+K, M", "transport.metronome");
    km.bind ("Primary+K", "transport.play");
    auto cs = km.conflicts();
    check (cs.size() == 1, "a key that cuts off a longer key is found as a conflict: " + (cs.empty() ? juce::String ("none") : cs[0].sentence));
    km.unbind ("Primary+K");
    check (km.conflicts().empty() && km.isPrefix (parseKeys ("Primary+K"), "global"), "remove it, and Primary+K now waits for the next key.");

    say ("");
    say ("Keys the system or a screen reader uses");
    auto why = Keymap::reservedBy (parseKeys ("Secondary+Alt+K"), Platform::mac);
    check (why.contains ("VoiceOver"), "\"" + why + "\"");
    why = Keymap::reservedBy (parseKeys ("Primary+Space"), Platform::mac);
    check (why.contains ("Spotlight"), "\"" + why + "\"");
    why = Keymap::reservedBy (parseKeys ("Primary+Alt+K"), Platform::windows);
    check (why.contains ("AltGr"), "\"" + why + "\"");
    why = Keymap::reservedBy (parseKeys ("Keypad5"), Platform::windows);
    check (why.contains ("NVDA"), "\"" + why + "\"");
    check (Keymap::reservedBy (parseKeys ("Keypad5"), Platform::mac).isEmpty(), "the keypad is free on the Mac.");
    check (Keymap::reservedBy (parseKeys ("Primary+M"), Platform::mac).isEmpty(), "Primary+M is free.");

    say ("");
    say ("Your keys survive a new version, and moved defaults are announced once");
    auto saved = km.userChangesToJson();
    auto json = juce::JSON::toString (saved);
    check (saved["added"].size() == 2 && saved["removed"].size() == 0,
           "your file holds only your two changes, not every default (" + juce::String (saved["added"].size()) + " added).");

    Keymap v2 (list);
    v2.addScope ("main", "global"); v2.addScope ("tracks", "main");
    // Version 2 moves the metronome default from Primary+M to Primary+Shift+M and Play from Space to Return.
    v2.setDefaults ({ { "Return", "transport.play", "global", {} },
                      { "Primary+Shift+M", "transport.metronome", "global", {} },
                      { "Alt+Right", "edit.nudge", "tracks", [] { CommandArgs x; x.set ("amount", 1); return x; }() },
                      { "Alt+Left", "edit.nudge", "tracks", [] { CommandArgs x; x.set ("amount", 1); x.set ("direction", "back"); return x; }() },
                      { "R", "track.arm", "tracks", {} } }, 2);
    juce::StringArray moved;
    r = v2.loadUserChanges (juce::JSON::parse (json), moved);
    check (r.wasOk(), "your keys load into the new version.");
    hit = v2.lookup (parseKeys ("Primary+M"), "global");
    check (hit && hit->commandId == "transport.go-to-start", "your own Primary+M still goes to the start.");
    hit = v2.lookup (parseKeys ("Primary+K, M"), "global");
    check (hit && hit->commandId == "transport.metronome", "your two-key sequence still works.");
    check (moved.contains ("Play moved from Space to Return."), "the moved default is announced: " + moved.joinIntoString (" "));
    check (moved.size() == 1, "the metronome's default moved too, but you have your own key for it, so that one isn't announced ("
           + juce::String (moved.size()) + " announced).");
    hit = v2.lookup (parseKeys ("Primary+Shift+M"), "global");
    check (hit && hit->commandId == "transport.metronome", "the metronome's new default key works alongside yours.");
    juce::StringArray movedAgain;
    v2.loadUserChanges (v2.userChangesToJson(), movedAgain);
    check (movedAgain.isEmpty(), "next time, nothing is announced again.");

    say ("");
    say ("A readable list of every key");
    auto list1 = v2.exportReadable (Platform::mac);
    check (list1.contains ("Transport") && list1.contains ("Play: Return") && list1.contains ("(in tracks)"), "the list is grouped by category, in Mac words.");
    say (list1.replace ("\n\n", "\n").replace ("\n", ". "));

    say ("");
    say (failures == 0 ? "All checks passed." : juce::String (failures) + " checks failed.");
    return failures == 0 ? 0 : 1;
}
