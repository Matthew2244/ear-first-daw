// The app's self-test: the real main window parts, menus, keys, palette and
// key help, driven with no audio device, nothing shown and nothing announced.
// Included into Main.cpp so it tests exactly the code the app runs.

int stFailures = 0;
void stCheck (bool ok, const juce::String& what)
{
    if (! ok) ++stFailures;
    std::cout << (ok ? "Pass: " : "FAIL: ") << what << std::endl;
}
void stSay (const juce::String& s)  { std::cout << s << std::endl; }

juce::KeyPress stKey (int code, int mods = 0)    { return juce::KeyPress (code, juce::ModifierKeys (mods), 0); }
constexpr int cmd = juce::ModifierKeys::commandModifier, shift = juce::ModifierKeys::shiftModifier;

int runSelfTest()
{
    stSay ("Ear First app self-test. No audio device, nothing shown, nothing announced.");
    auto root = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("EarFirstAppSelfTest");
    root.deleteRecursively();
    root.createDirectory();

    ScreenReaderOutput out;
    out.announce = false;
    AppCore::Options o;
    o.openAudioDevice = false;
    o.songsFolder = root;
    o.saveSettings = false;
    auto core = std::make_unique<AppCore> (out, o);
    auto mc = std::make_unique<MainComponent> (*core, out);
    auto last = [&out] { return out.log.isEmpty() ? juce::String() : out.log[out.log.size() - 1]; };
    auto press = [&] (juce::KeyPress k, const juce::String& scope = "main") { return mc->router.handle (k, scope); };

    stSay ("");
    stSay ("Keys and menus before any song is open");
    for (auto& b : core->keymap().effective())
    {
        auto seq = parseKeys (b.keys);
        auto mac = Keymap::reservedBy (seq, Platform::mac), win = Keymap::reservedBy (seq, Platform::windows);
        if (mac.isNotEmpty() || win.isNotEmpty())
            stCheck (false, "default key " + keysForPeople (seq, Platform::mac) + " for " + b.commandId + " clashes: " + mac + " " + win);
    }
    stCheck (true, "checked every default key against VoiceOver, NVDA, JAWS and system keys.");
    auto conflicts = core->keymap().conflicts();
    stCheck (conflicts.empty(), "the default keys have no conflicts" + (conflicts.empty() ? juce::String (".") : ": " + conflicts[0].sentence));

    int commandsInMenus = 0, withKeys = 0;
    for (auto& name : mc->getMenuBarNames())
    {
        auto m = mc->getMenuForIndex (0, name);
        for (juce::PopupMenu::MenuItemIterator it (m); it.next();)
        {
            ++commandsInMenus;
            if (it.getItem().shortcutKeyDescription.isNotEmpty()) ++withKeys;
        }
    }
    stCheck (commandsInMenus == core->commands().size(), "every command is in a menu: " + juce::String (commandsInMenus)
             + " of " + juce::String (core->commands().size()) + ", " + juce::String (withKeys) + " showing their key.");
    {
        auto file = mc->getMenuForIndex (0, "File");
        bool saveDisabled = false;
        for (juce::PopupMenu::MenuItemIterator it (file); it.next();)
            if (it.getItem().text == "Save") saveDisabled = ! it.getItem().isEnabled;
        stCheck (saveDisabled, "with no song open, Save is greyed out in the File menu.");
    }
    press (stKey ('s', cmd));
    stCheck (last().startsWith ("No song is open."), "pressing Command-S with no song says why: \"" + last() + "\"");

    stSay ("");
    stSay ("A new song, by key");
    press (stKey ('n', cmd));
    stCheck (core->session() != nullptr && last().startsWith ("New song, Untitled."), "Command-N makes a song: \"" + last() + "\"");
    stCheck (mc->tracks.getNumRowsOnScreen() >= 0 && mc->getNumRows() == 1, "the track list shows its one track.");

    press (stKey ('c'));
    stCheck (last() == "Click on", "C turns the click on: \"" + last() + "\"");
    press (stKey ('c'));
    stCheck (last() == "Click off", "and off again.");
    press (stKey ('l'));
    stCheck (last() == "Loop on", "L turns looping on.");

    press (stKey ('t', cmd));
    stCheck (mc->getNumRows() == 2 && last().startsWith ("Added"), "Command-T adds a track: \"" + last() + "\"");
    press (stKey ('z', cmd));
    stCheck (mc->getNumRows() == 1 && last().startsWith ("Undid"), "Command-Z undoes it: \"" + last() + "\"");
    press (stKey ('z', cmd | shift));
    stCheck (mc->getNumRows() == 2 && last().startsWith ("Redid"), "Shift-Command-Z redoes it: \"" + last() + "\"");

    core->selectedTrack = 0;
    stCheck (! press (stKey (juce::KeyPress::downKey), "tracks"), "Down in the track list is left to the list itself, which the screen reader reads natively.");
    core->commands().run ("tracks.next");
    stCheck (last().contains ("2 of 2"), "the Next track command, for surfaces and the palette, says where: \"" + last() + "\"");
    core->commands().run ("tracks.next");
    stCheck (last().startsWith ("Last track."), "at the end it says so: \"" + last() + "\"");

    press (stKey ('l', cmd | shift));
    stCheck (last().startsWith ("Bar 1 beat 1"), "Shift-Command-L says where the playhead is: \"" + last() + "\"");
    press (stKey ('i', cmd | shift));
    stCheck (last().contains ("2 tracks") && last().contains ("unsaved"), "Shift-Command-I sums up the song: \"" + last() + "\"");

    press (stKey ('k'));
    stCheck (last().startsWith ("Nothing is being captured"), "K with no audio inputs says why: \"" + last() + "\"");

    stSay ("");
    stSay ("Key help");
    press (stKey ('/', cmd));
    stCheck (last() == "Key help on", "Command-slash turns key help on.");
    press (stKey ('c'));
    stCheck (last().startsWith ("C: Click, off.") , "pressing C now says what it does: \"" + last() + "\"");
    stCheck (! core->commands().find ("transport.metronome")->toggleState().value_or (true), "and the metronome didn't change.");
    press (stKey ('q'));
    stCheck (last().contains ("does nothing here"), "a key with no command says so: \"" + last() + "\"");
    press (stKey (juce::KeyPress::escapeKey));
    stCheck (last() == "Key help off", "Escape turns key help off.");

    stSay ("");
    stSay ("Find a command");
    {
        Palette p (*core, [] {});
        p.typeIn ("click");
        stCheck (p.numResults() > 0 && p.resultText (0).startsWith ("Click, off, C."), "typing click finds the click, with its state and key: \""
                 + (p.numResults() > 0 ? p.resultText (0) : juce::String ("nothing")) + "\"");
        p.runRow (0);
        stCheck (last() == "Click on", "Return runs it from the list.");
        p.typeIn ("keep");
        stCheck (p.numResults() > 0 && p.resultText (0).startsWith ("Keep, not available now"), "a command that can't run now says so in the list: \""
                 + (p.numResults() > 0 ? p.resultText (0) : juce::String()) + "\"");
        p.typeIn ("zzz");
        stCheck (p.numResults() == 0 && p.summary() == "No commands match.", "nonsense finds nothing, and says so.");
        p.typeIn ("metro");
        stCheck (p.summary().contains ("Click"), "the summary says how many and the first: \"" + p.summary() + "\"");
    }

    stSay ("");
    stSay ("The click, tempo and meter");
    press (stKey ('t', shift));
    stCheck (last() == "120 beats a minute, 4 4 time.", "Shift-T says the tempo and meter: \"" + last() + "\"");
    {
        CommandArgs t; t.set ("beats a minute", 96);
        core->commands().run ("song.tempo", t);
        stCheck (last().startsWith ("96 beats a minute"), "setting the tempo to 96 says so: \"" + last() + "\"");
        CommandArgs m; m.set ("time signature", "7/8");
        core->commands().run ("song.time-signature", m);
        stCheck (last() == "96 beats a minute, 7 8 time.", "and the meter to 7/8: \"" + last() + "\"");
        m.set ("time signature", "7/9");
        auto r = core->commands().run ("song.time-signature", m);
        stCheck (! r.ok, "7/9 is refused with a reason: \"" + last() + "\"");
    }
    press (stKey ('c', shift));
    stCheck (last().startsWith ("Click on. 96 beats a minute, 7 8") && last().contains ("the one accented"), "Shift-C sums up the click: \"" + last() + "\"");
    mc->lastSettingsForm = nullptr;
    core->runFromUser ("click.settings", CommandList::Source::menu);
    {
        std::unique_ptr<SettingsForm> form (mc->lastSettingsForm);
        stCheck (form != nullptr && form->fields.size() == 9, "Click settings opens a form with nine labelled settings.");
        CommandArgs a = form != nullptr ? form->values() : CommandArgs();
        a.set ("subdivision", "eighths"); a.set ("accents", "L N L N L N N"); a.set ("count-in bars", 1);
        core->commands().run ("click.settings", a);
        stCheck (last().startsWith ("Click: eighths, accents loud normal loud") && last().contains ("count-in 1 bar"), "changing it says what it's set to: \"" + last() + "\"");
        a.set ("accents", "LX");
        auto r = core->commands().run ("click.settings", a);
        stCheck (! r.ok, "accents with letters it doesn't know are refused: \"" + last() + "\"");
    }
    {
        auto& ts = core->session()->getEdit().tempoSequence;
        const auto from = ts.toTime (tracktion::BeatPosition::fromBeats (14));
        const double start = core->countInStart (from, 1).inSeconds();
        const double expected = ts.toTime (tracktion::BeatPosition::fromBeats (7)).inSeconds();
        stCheck (std::abs (start - expected) < 0.0001, "a one-bar count-in in 7/8 from bar 3 starts at bar 2, " + juce::String (start, 3)
                 + " seconds, expected " + juce::String (expected, 3) + ".");
        // From the very start, a one-bar count-in starts a bar before zero, not cut short.
        auto& tc = core->session()->getEdit().getTransport();
        const double oneBar = ts.toTime (tracktion::BeatPosition::fromBeats (7)).inSeconds();
        tc.setPosition (core->countInStart (tracktion::TimePosition(), 1));
        stCheck (std::abs (tc.getPosition().inSeconds() + oneBar) < 0.01, "from the very start, the count-in starts a whole bar before zero, at "
                 + juce::String (tc.getPosition().inSeconds(), 3) + " seconds.");
        tc.setPosition (tracktion::TimePosition());
    }

    stSay ("");
    stSay ("Choosing what's said");
    mc->lastSettingsForm = nullptr;
    core->runFromUser ("speech.settings", CommandList::Source::menu);
    {
        std::unique_ptr<SettingsForm> form (mc->lastSettingsForm);
        stCheck (form != nullptr && (int) form->fields.size() == (int) SpeechPrefs::kinds().size() + 1,
                 "Speech settings opens a form with one choice per kind of message, plus how places are said.");
        CommandArgs a = form != nullptr ? form->values() : CommandArgs();
        a.set ("Transport", "Short"); a.set ("say places as", "Bars and beats");
        core->commands().run ("speech.settings", a);
        press (stKey ('c'));
        stCheck (last() == "Click off", "with Transport short, the click toggle still says its state: \"" + last() + "\"");
        press (stKey ('l', cmd | shift));
        stCheck (last() == "Bar 1 beat 1.", "places said as bars and beats only: \"" + last() + "\"");
        press (stKey ('c'));   // back on
        press (stKey ('x', cmd | shift));
        stCheck (last().startsWith ("Click messages are off"), "Don't say that kind turns the click's messages off: \"" + last() + "\"");
        const auto before = out.log.size();
        press (stKey ('c'));
        stCheck (out.log.size() == before, "then the click toggle says nothing.");
        press (stKey ('r', cmd | shift));
        stCheck (last().startsWith ("Click"), "but it's still in the history, in full: \"" + last() + "\"");
        a.set ("Click", "Sound");
        core->commands().run ("speech.settings", a);
        const auto said = out.log.size();
        press (stKey ('c'));
        const auto first = core->lastEarcon;
        press (stKey ('c'));
        const auto second = core->lastEarcon;
        stCheck (out.log.size() == said && first.first > 0, "with Click set to Sound, the toggle plays a tone and says nothing.");
        stCheck ((first.second > first.first) != (second.second > second.first), "one way it rises, the other it falls ("
                 + juce::String (first.first, 0) + " to " + juce::String (first.second, 0) + ", then " + juce::String (second.first, 0)
                 + " to " + juce::String (second.second, 0) + ").");
        // Put things back for the rest of the test.
        for (auto& k : SpeechPrefs::kinds()) a.set (k.id, "Full");
        a.set ("say places as", "Bars and time");
        core->commands().run ("speech.settings", a);
        press (stKey ('c'));
    }

    stSay ("");
    stSay ("MIDI controls");
    {
        CommandArgs a; a.set ("command", "Keep");
        core->commands().run ("controls.learn", a);
        stCheck (last().startsWith ("Press the pedal, pad, key or button you want for Keep."), "Assign a MIDI control asks for the control: \"" + last() + "\"");
        core->handleMidiControl ("Test Footswitch", juce::MidiMessage::controllerEvent (1, 85, 0));
        stCheck (last().startsWith ("Press the pedal"), "a footswitch coming up doesn't count; it keeps waiting.");
        core->handleMidiControl ("Test Footswitch", juce::MidiMessage::controllerEvent (1, 85, 127));
        stCheck (last() == "Controller 85 on channel 1 from Test Footswitch now runs Keep.", "pressing it assigns it: \"" + last() + "\"");
        const auto before = out.log.size();
        core->handleMidiControl ("Test Footswitch", juce::MidiMessage::controllerEvent (1, 85, 127));
        stCheck (out.log.size() == before, "holding it down doesn't run it again.");
        core->handleMidiControl ("Test Footswitch", juce::MidiMessage::controllerEvent (1, 85, 0));
        core->handleMidiControl ("Test Footswitch", juce::MidiMessage::controllerEvent (1, 85, 127));
        stCheck (last().startsWith ("Nothing is being captured"), "release and press again, and it runs Keep: \"" + last() + "\"");
        core->handleMidiControl ("Other Keyboard", juce::MidiMessage::controllerEvent (1, 85, 127));
        stCheck (out.log.size() == before + 1, "the same controller on a different device does nothing.");

        a.set ("command", "Play or stop");
        core->commands().run ("controls.learn", a);
        core->handleMidiControl ("Test Keyboard", juce::MidiMessage::controllerEvent (1, 64, 127));
        stCheck (last().contains ("sustain pedal") && last().contains ("also hold notes"), "learning the sustain pedal warns that it still holds notes: \"" + last() + "\"");
        core->handleMidiControl ("Test Keyboard", juce::MidiMessage::controllerEvent (1, 64, 0));

        core->commands().run ("controls.list");
        stCheck (last().startsWith ("2 MIDI controls:") && last().contains ("runs Keep") && last().contains ("runs Play or stop"), "MIDI controls lists both: \"" + last() + "\"");
        a.set ("command", "Where am I");
        core->commands().run ("controls.learn", a);
        press (stKey (juce::KeyPress::escapeKey));
        stCheck (last() == "Cancelled. Nothing was assigned.", "Escape cancels learning.");
        CommandArgs r; r.set ("control", core->midiControls().all()[0].describe() + ", Keep");
        core->commands().run ("controls.remove", r);
        stCheck (core->midiControls().all().size() == 1 && last() == "Removed.", "removing one leaves the other.");
    }

    stSay ("");
    stSay ("Speech history");
    const auto lastSaid = last();
    press (stKey ('r', cmd | shift));
    stCheck (last() == lastSaid, "Shift-Command-R says the last message again: \"" + last() + "\"");
    press (stKey (',', cmd | shift));
    stCheck (last().isNotEmpty() && last() != "Click on", "Shift-Command-comma steps back: \"" + last() + "\"");

    stSay ("");
    stSay ("Saving, closing, reopening");
    press (stKey ('w', cmd));
    stCheck (core->session() != nullptr && last().contains ("unsaved changes"), "closing with unsaved changes is refused, and says why: \"" + last() + "\"");
    press (stKey ('s', cmd));
    stCheck (last() == "Saved.", "Command-S saves.");
    auto folder = core->session()->getFolder();
    press (stKey ('w', cmd));
    stCheck (core->session() == nullptr && last().startsWith ("Closed"), "then Command-W closes it.");
    CommandArgs a; a.set ("folder", folder.getFullPathName());
    core->commands().run ("file.open-song", a);
    stCheck (core->session() != nullptr && last().startsWith ("Opened Untitled, 2 tracks."), "opening it again: \"" + last() + "\"");

    stSay ("");
    stSay ("Forms for commands with settings");
    mc->showDialogs = false;
    mc->lastSettingsForm = nullptr;
    core->runFromUser ("file.new-song-named", CommandList::Source::menu);
    {
        std::unique_ptr<SettingsForm> form (mc->lastSettingsForm);
        stCheck (form != nullptr && form->fields.size() == 1 && form->fields[0].control->getTitle() == "Name",
                 "New Song with a name opens a form with a labelled Name box.");
        if (form != nullptr)
        {
            if (auto* e = dynamic_cast<juce::TextEditor*> (form->fields[0].control.get())) e->setText ("Morning Sketch", false);
            core->session()->save();
            form->run();
            stCheck (core->session() != nullptr && core->session()->getName() == "Morning Sketch", "typing a name and pressing Return makes the song with that name: \"" + last() + "\"");
        }
    }
    core->commands().run ("audio.report");
    stCheck (last() == "No audio interface is open.", "with no interface open, Audio interface says so.");
    mc->lastSettingsForm = nullptr;
    core->runFromUser ("audio.choose-input", CommandList::Source::menu);
    std::unique_ptr<SettingsForm> deviceForm (mc->lastSettingsForm);
    stCheck (last().startsWith ("No audio interfaces are available") || deviceForm != nullptr,
             "Record from either says why it can't, or opens its form: \"" + last() + "\"");

    stSay ("");
    stSay ("Moving around a song");
    {
        auto run = [&] (const char* id, CommandArgs a = {}) { return core->commands().run (id, a); };
        auto& transport = core->session()->getEdit().getTransport();
        auto& tempo = core->session()->getEdit().tempoSequence;
        auto barNow = [&] { return tempo.toBarsAndBeats (transport.getPosition()).bars + 1; };
        transport.setPosition (tracktion::TimePosition());
        stCheck (! mc->tracks.keyPressed (stKey (juce::KeyPress::pageDownKey)) && ! mc->tracks.keyPressed (stKey (juce::KeyPress::homeKey))
                 && ! mc->tracks.keyPressed (stKey (juce::KeyPress::backspaceKey)), "the track list passes Page Down, Home and Delete on to the keymap.");
        press (stKey (juce::KeyPress::pageDownKey));
        stCheck (barNow() == 2 && last().isNotEmpty(), "Page Down goes to bar 2 and says so, as in REAPER: \"" + last() + "\"");
        press (stKey (juce::KeyPress::rightKey));
        stCheck (tempo.toBarsAndBeats (transport.getPosition()).getWholeBeats() == 1, "Right steps one beat.");
        press (stKey (juce::KeyPress::homeKey, juce::ModifierKeys::altModifier));
        stCheck (barNow() == 2 && tempo.toBarsAndBeats (transport.getPosition()).getWholeBeats() == 0, "Option-Home goes to the start of this bar.");
        press (stKey (juce::KeyPress::pageUpKey));
        press (stKey (juce::KeyPress::pageUpKey));
        stCheck (barNow() == 1 && transport.getPosition().inSeconds() == 0.0, "Page Up stops at the start of the song.");
        press (stKey (juce::KeyPress::pageDownKey, cmd));
        stCheck (tempo.toBarsAndBeats (transport.getPosition()).getWholeBeats() == 1, "Command-Page Down steps one beat.");
        press (stKey (juce::KeyPress::homeKey));
        stCheck (transport.getPosition().inSeconds() == 0.0, "Home goes to the start.");
        CommandArgs where; where.set ("bar", 9); where.set ("beat", 3.0);
        run ("transport.go-to-bar", where);
        stCheck (barNow() == 9 && tempo.toBarsAndBeats (transport.getPosition()).getWholeBeats() == 2, "Go to bar lands on bar 9, beat 3: \"" + last() + "\"");
        run ("transport.rewind");
        stCheck (transport.getPosition().inSeconds() < tempo.toTime (tracktion::tempo::BarsAndBeats { 8, tracktion::BeatDuration::fromBeats (2.0) }).inSeconds(), "Rewind moves back.");

        stSay ("Markers");
        run ("markers.next");
        stCheck (last().contains ("no markers") || last().contains ("M adds"), "with no markers, Next marker says so: \"" + last() + "\"");
        CommandArgs verse; verse.set ("name", "Verse");
        where.set ("bar", 5); where.set ("beat", 1.0); run ("transport.go-to-bar", where); run ("markers.add", verse);
        where.set ("bar", 2); run ("transport.go-to-bar", where); press (stKey ('m'));
        transport.setPosition (tracktion::TimePosition());
        press (stKey ('\''));
        stCheck (barNow() == 2 && last().startsWith ("Marker"), "Next marker finds the nearest one first: \"" + last() + "\"");
        run ("markers.next");
        stCheck (barNow() == 5 && last().startsWith ("Verse"), "then the one after: \"" + last() + "\"");
        run ("markers.next");
        stCheck (barNow() == 5 && last().contains ("No more"), "and says when there are no more.");
        run ("markers.list");
        stCheck (last().startsWith ("2 markers") && last().indexOf ("Marker") < last().indexOf ("Verse"), "List markers goes in song order: \"" + last() + "\"");
        run ("markers.delete");
        stCheck (last() == "Deleted Verse." && core->session()->getEdit().getMarkerManager().getMarkers().size() == 1, "Delete marker removes the one here.");
        run ("edit.undo");
        stCheck (core->session()->getEdit().getMarkerManager().getMarkers().size() == 2, "and Undo brings it back.");
        transport.setPosition (tracktion::TimePosition());
        press (stKey ('2'));
        stCheck (barNow() == 2, "2 goes to marker 2, as in REAPER: \"" + last() + "\"");
        where.set ("bar", 7); run ("transport.go-to-bar", where);
        press (stKey ('2', cmd));
        transport.setPosition (tracktion::TimePosition());
        press (stKey ('2'));
        stCheck (barNow() == 7, "Command-2 moves marker 2 to the playhead: \"" + last() + "\"");
        press (stKey (';'));
        stCheck (last().contains ("Verse") || barNow() < 7, "Semicolon goes to the previous marker: \"" + last() + "\"");
        where.set ("bar", 3); run ("transport.go-to-bar", where); press (stKey ('['));
        where.set ("bar", 5); run ("transport.go-to-bar", where); press (stKey (']'));
        auto loop = transport.getLoopRange();
        stCheck (std::abs (loop.getStart().inSeconds() - tempo.toTime (tracktion::tempo::BarsAndBeats { 2, {} }).inSeconds()) < 0.001
                 && std::abs (loop.getEnd().inSeconds() - tempo.toTime (tracktion::tempo::BarsAndBeats { 4, {} }).inSeconds()) < 0.001,
                 "[ and ] set the loop from bar 3 to bar 5: \"" + last() + "\"");
    }

    stSay ("");
    stSay ("Track basics");
    {
        auto run = [&] (const char* id, CommandArgs a = {}) { return core->commands().run (id, a); };
        auto list = [&] { return contentTracks (core->session()->getEdit()); };
        while (list().size() < 3) run ("tracks.add");
        core->selectedTrack = 0;
        CommandArgs named; named.set ("name", "Piano");
        run ("tracks.rename", named);
        stCheck (list()[0]->getName() == "Piano", "Rename track renames it: \"" + last() + "\"");
        run ("tracks.move-down");
        stCheck (list()[1]->getName() == "Piano" && core->selectedTrack == 1, "Move track down: \"" + last() + "\"");
        run ("tracks.move-up");
        stCheck (list()[0]->getName() == "Piano" && core->selectedTrack == 0, "Move track up, back to the top.");
        run ("tracks.move-up");
        stCheck (last().contains ("already"), "and it says when it can't go further.");
        press (stKey (juce::KeyPress::F5Key));
        stCheck (list()[0]->isMuted (false) && last().startsWithIgnoreCase ("muted") && mc->getNameForRow (0).contains ("muted"), "F5 mutes, as in REAPER, says so and shows in the list: \"" + last() + "\"");
        press (stKey (juce::KeyPress::F5Key, juce::ModifierKeys::altModifier | shift));
        stCheck (last().startsWith ("1 muted: Piano"), "Option-Shift-F5 says which tracks are muted: \"" + last() + "\"");
        press (stKey (juce::KeyPress::F5Key, juce::ModifierKeys::altModifier));
        stCheck (! list()[0]->isMuted (false), "Option-F5 unmutes all: \"" + last() + "\"");
        press (stKey (juce::KeyPress::F6Key));
        stCheck (list()[0]->isSolo (false) && mc->getNameForRow (0).contains ("soloed"), "F6 solos: \"" + last() + "\"");
        press (stKey (juce::KeyPress::F6Key));
        press (stKey (juce::KeyPress::upKey, juce::ModifierKeys::altModifier));
        stCheck (std::abs (list()[0]->getVolumePlugin()->getVolumeDb() - 1.0f) < 0.1f, "Option-Up raises by 1 dB: \"" + last() + "\"");
        CommandArgs vol; vol.set ("volume", -6.0);
        run ("tracks.volume", vol);
        stCheck (std::abs (list()[0]->getVolumePlugin()->getVolumeDb() + 6.0f) < 0.1f, "Track volume sets it: \"" + last() + "\"");
        CommandArgs pan; pan.set ("pan", -30.0);
        run ("tracks.pan", pan);
        stCheck (last() == "30 left.", "Track pan says where: \"" + last() + "\"");
        run ("tracks.report");
        stCheck (last().contains ("Volume -6") && last().contains ("30 left"), "About this track: \"" + last() + "\"");
        const int before = list().size();
        run ("tracks.delete");
        stCheck (list().size() == before - 1 && last().startsWith ("Deleted Piano"), "Delete track: \"" + last() + "\"");
        run ("edit.undo");
        stCheck (list().size() == before && list()[0]->getName() == "Piano", "Undo brings it back, name and all.");
        run ("tracks.input");
        stCheck (last().isNotEmpty(), "Track input answers even with no interface: \"" + last() + "\"");
    }

    stSay ("");
    stSay ("Importing, editing and rendering audio");
    {
        auto run = [&] (const char* id, CommandArgs a = {}) { return core->commands().run (id, a); };
        auto wav = root.getChildFile ("Four Second Tone.wav");
        {
            juce::WavAudioFormat fmt;
            std::unique_ptr<juce::AudioFormatWriter> w (fmt.createWriterFor (new juce::FileOutputStream (wav), 48000.0, 1, 24, {}, 0));
            juce::AudioBuffer<float> b (1, 192000);
            for (int i = 0; i < b.getNumSamples(); ++i) b.setSample (0, i, 0.5f * (float) std::sin (i * 0.05));
            w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
        }
        auto& edit = core->session()->getEdit();
        auto& transport = edit.getTransport();
        transport.setPosition (tracktion::TimePosition());
        core->selectedTrack = 0;
        auto track = [&] { return contentTracks (core->session()->getEdit())[0]; };
        const int clipsBefore = track()->getClips().size();
        CommandArgs imp; imp.set ("file", wav.getFullPathName());
        run ("file.import-audio", imp);
        stCheck (track()->getClips().size() == clipsBefore + 1 && last().startsWith ("Imported"), "Import audio places the file: \"" + last() + "\"");
        stCheck (core->session()->getFolder().getChildFile ("Audio").getChildFile (wav.getFileName()).existsAsFile(), "and the song keeps its own copy.");
        CommandArgs notAudio; auto bad = root.getChildFile ("not audio.txt"); bad.replaceWithText ("hello"); notAudio.set ("file", bad.getFullPathName());
        run ("file.import-audio", notAudio);
        stCheck (last().startsWith ("Nothing was imported"), "A file that isn't audio is refused: \"" + last() + "\"");
        run ("clips.report");
        stCheck (last().startsWith ("Four Second Tone"), "About this clip: \"" + last() + "\"");

        transport.setPosition (tracktion::TimePosition::fromSeconds (1.0));
        press (stKey ('s'));
        stCheck (track()->getClips().size() == clipsBefore + 2, "S splits at the playhead, as in REAPER: \"" + last() + "\"");
        run ("edit.undo");
        stCheck (track()->getClips().size() == clipsBefore + 1, "Undo joins it back.");
        run ("clips.trim-end");
        stCheck (std::abs (track()->getClips()[clipsBefore]->getPosition().getLength().inSeconds() - 1.0) < 0.01, "Trim end leaves one second: \"" + last() + "\"");
        run ("edit.undo");
        transport.setPosition (tracktion::TimePosition::fromSeconds (1.0));
        run ("clips.trim-start");
        auto* trimmed = track()->getClips()[clipsBefore];
        stCheck (std::abs (trimmed->getPosition().getStart().inSeconds() - 1.0) < 0.01 && std::abs (trimmed->getPosition().getOffset().inSeconds() - 1.0) < 0.01,
                 "Trim start keeps the audio where it was: \"" + last() + "\"");
        run ("edit.undo");
        transport.setPosition (tracktion::TimePosition::fromSeconds (0.5));
        CommandArgs by; by.set ("beats", 4.0);
        run ("clips.move", by);
        stCheck (std::abs (track()->getClips()[clipsBefore]->getPosition().getStart().inSeconds() - 2.0) < 0.01, "Move clip by four beats: \"" + last() + "\"");
        transport.setPosition (tracktion::TimePosition());
        run ("clips.next");
        stCheck (std::abs (transport.getPosition().inSeconds() - 2.0) < 0.01, "Next clip goes to it: \"" + last() + "\"");

        CommandArgs r; r.set ("name", "Mix One"); r.set ("format", "WAV 24-bit");
        run ("file.render", r);
        stCheck (last().startsWith ("Rendering"), "Render says it's started: \"" + last() + "\"");
        for (int i = 0; i < 300 && core->isRendering(); ++i) juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        stCheck (! core->isRendering() && core->lastRender.existsAsFile(), "and it finishes: \"" + last() + "\"");
        if (core->lastRender.existsAsFile())
        {
            juce::WavAudioFormat fmt;
            std::unique_ptr<juce::AudioFormatReader> rd (fmt.createReaderFor (core->lastRender.createInputStream().release(), true));
            juce::Range<float> range;
            if (rd != nullptr) { juce::Range<float> lv[2]; rd->readMaxLevels (0, rd->lengthInSamples, lv, 2); range = lv[0]; }
            stCheck (rd != nullptr && rd->lengthInSamples / rd->sampleRate > 5.9 && rd->numChannels == 2 && rd->bitsPerSample == 24 && range.getEnd() > 0.2f,
                     "The render has the song's length and sound: " + juce::String (rd != nullptr ? rd->lengthInSamples / rd->sampleRate : 0.0, 2) + " seconds, peak " + juce::String (range.getEnd(), 2) + ".");
        }
        run ("levels.report");
        stCheck (last().contains ("No audio interface"), "What's the level answers with no interface: \"" + last() + "\"");
        transport.setPosition (tracktion::TimePosition::fromSeconds (2.5));
        press (stKey (juce::KeyPress::backspaceKey));
        stCheck (track()->getClips().size() == clipsBefore, "Delete removes the clip: \"" + last() + "\"");
    }

    stSay ("");
    stSay ("What a sighted person sees");
    {
        mc->setSize (1200, 800);
        mc->resized();
        int titled = 0, total = 0;
        for (auto& b : mc->transportBar.buttons)
        {
            ++total;
            if (b->getTitle().isNotEmpty() && b->getDescription().isNotEmpty() && b->getWantsKeyboardFocus() && ! b->getBounds().isEmpty()) ++titled;
            else stSay ("  not ready: " + b->commandId() + " title \"" + b->getTitle() + "\" " + juce::String (b->getDescription().length()) + " " + b->getBounds().toString());
        }
        stCheck (total >= 10 && titled == total, "every transport button has a title, a description, a place on screen and takes the keyboard (" + juce::String (titled) + " of " + juce::String (total) + ").");
        CommandButton* playButton = nullptr;
        for (auto& b : mc->transportBar.buttons) if (b->commandId() == "transport.play-stop") playButton = b.get();
        stCheck (playButton != nullptr && playButton->getButtonText() == "Play", "the Play button says Play while stopped.");
        CommandButton* clickButton = nullptr;
        for (auto& b : mc->transportBar.buttons) if (b->commandId() == "transport.metronome") clickButton = b.get();
        if (clickButton != nullptr)
        {
            const bool was = clickButton->getToggleState();
            clickButton->triggerClick();
            juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
            clickButton->refresh();
            stCheck (clickButton->getToggleState() != was && clickButton->getButtonText().endsWith (was ? "off" : "on"), "the Click button runs the command and says its state in words: \"" + clickButton->getButtonText() + "\"");
            clickButton->triggerClick();
            juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
        }
        stCheck (! mc->timeline.getWantsKeyboardFocus() && mc->timeline.getTitle() == "Timeline" && ! mc->timeline.getBounds().isEmpty(), "the timeline is on screen, titled, and out of the Tab order.");
        stCheck (! mc->meterStrip.getWantsKeyboardFocus() && ! mc->meterStrip.getBounds().isEmpty(), "the meters are on screen and out of the Tab order.");
        stCheck (mc->history.getWantsKeyboardFocus() && mc->history.getNumRows() > 0 && mc->history.getNameForRow (mc->history.getNumRows() - 1) == last(),
                 "the message history holds what was said, newest last: \"" + mc->history.getNameForRow (mc->history.getNumRows() - 1) + "\"");
        stCheck (mc->tracks.getRowHeight() == mc->timeline.laneHeight() && mc->timeline.laneTop() == mc->tracks.getY() - mc->timeline.getY(), "timeline lanes line up with the track list rows.");

        auto snapDir = juce::SystemStats::getEnvironmentVariable ("EARFIRST_SNAPSHOTS", {});
        for (auto theme : Theme::choices())
        {
            CommandArgs t; t.set ("colours", theme);
            core->commands().run ("view.theme", t);
            const auto resolved = Theme::make (core->theme);
            stCheck (mc->look.theme.name == resolved.name && resolved.text.getPerceivedBrightness() != resolved.background.getPerceivedBrightness(),
                     "Colours, " + theme + ": " + last());
            // Text against its background must reach 4.5 to 1 (WCAG AA), in every theme.
            auto contrast = [] (juce::Colour a, juce::Colour b)
            {
                auto lum = [] (juce::Colour c)
                {
                    auto ch = [] (float v) { return v <= 0.03928f ? v / 12.92f : std::pow ((v + 0.055f) / 1.055f, 2.4f); };
                    return 0.2126f * ch (c.getFloatRed()) + 0.7152f * ch (c.getFloatGreen()) + 0.0722f * ch (c.getFloatBlue());
                };
                auto l1 = lum (a), l2 = lum (b);
                return (std::max (l1, l2) + 0.05f) / (std::min (l1, l2) + 0.05f);
            };
            auto& th = mc->look.theme;
            const float worst = std::min ({ contrast (th.text, th.background), contrast (th.text, th.panel), contrast (th.dim, th.panel),
                                            contrast (th.selectionText, th.selection), contrast (th.clipText, th.clip), contrast (th.accentText, th.accent) });
            stCheck (worst >= 4.5f, "  every text colour in " + th.name + " reaches 4.5 to 1 against its background (worst " + juce::String (worst, 1) + ").");
            if (snapDir.isNotEmpty())
            {
                juce::File (snapDir).createDirectory();
                auto img = mc->createComponentSnapshot (mc->getLocalBounds(), true, 1.0f);
                juce::FileOutputStream fo (juce::File (snapDir).getChildFile ("window " + theme + ".png"));
                fo.setPosition (0); fo.truncate();
                juce::PNGImageFormat().writeImageToStream (img, fo);
            }
        }
        core->commands().run ("view.beat-flash");
        stCheck (core->beatFlash && last().startsWithIgnoreCase ("Flash on the beat on"), "Flash on the beat turns on: \"" + last() + "\"");
        core->commands().run ("view.beat-flash");
        stCheck (WelcomeComponent::welcomeText().contains ("Command-N") && WelcomeComponent::welcomeText().contains ("to keep the last 30 seconds"), "the welcome says how to start and how to keep what you played.");
    }

    stSay ("");
    stSay ("Updates");
    core->commands().run ("app.check-for-updates");
    stCheck (last().contains ("version"), "Check for Updates says honestly where things stand: \"" + last() + "\"");

    stSay ("");
    stSay ("What a screen reader sees");
    stCheck (mc->tracks.getTitle() == "Tracks" && mc->tracks.isAccessible() && mc->tracks.getWantsKeyboardFocus(), "the track list is titled, accessible and takes keyboard focus.");
    stCheck (mc->getNameForRow (0).isNotEmpty(), "each row has a name to read: \"" + mc->getNameForRow (0) + "\"");
    stCheck (mc->status.isAccessible() && mc->status.getTitle().isNotEmpty() && mc->status.getText() == last(), "the status line shows the last message for anyone reading the screen.");

    core->closeSong();
    mc.reset();
    core.reset();
    root.deleteRecursively();
    stSay ("");
    stSay (stFailures == 0 ? "All checks passed." : juce::String (stFailures) + " checks failed.");
    return stFailures == 0 ? 0 : 1;
}
