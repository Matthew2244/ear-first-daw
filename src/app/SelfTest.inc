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
    stCheck (last().startsWith ("Bar 1, beat 1"), "Shift-Command-L says where the playhead is: \"" + last() + "\"");
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
