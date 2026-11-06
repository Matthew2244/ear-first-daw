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
    stCheck (last() == "Metronome on", "C turns the metronome on: \"" + last() + "\"");
    press (stKey ('c'));
    stCheck (last() == "Metronome off", "and off again.");
    press (stKey ('l'));
    stCheck (last() == "Loop on", "L turns looping on.");

    press (stKey ('t', cmd));
    stCheck (mc->getNumRows() == 2 && last().startsWith ("Added"), "Command-T adds a track: \"" + last() + "\"");
    press (stKey ('z', cmd));
    stCheck (mc->getNumRows() == 1 && last().startsWith ("Undid"), "Command-Z undoes it: \"" + last() + "\"");
    press (stKey ('z', cmd | shift));
    stCheck (mc->getNumRows() == 2 && last().startsWith ("Redid"), "Shift-Command-Z redoes it: \"" + last() + "\"");

    core->selectedTrack = 0;
    press (stKey (juce::KeyPress::downKey), "tracks");
    stCheck (last().contains ("2 of 2"), "Down in the track list moves and says where: \"" + last() + "\"");
    press (stKey (juce::KeyPress::downKey), "tracks");
    stCheck (last().startsWith ("Last track."), "at the end it says so: \"" + last() + "\"");
    stCheck (! press (stKey (juce::KeyPress::downKey), "main") , "Down outside the track list is left alone for the window.");

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
    stCheck (last().startsWith ("C: Metronome, off.") , "pressing C now says what it does: \"" + last() + "\"");
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
        stCheck (p.numResults() > 0 && p.resultText (0).startsWith ("Metronome, off, C."), "typing click finds the metronome, with its state and key: \""
                 + (p.numResults() > 0 ? p.resultText (0) : juce::String ("nothing")) + "\"");
        p.runRow (0);
        stCheck (last() == "Metronome on", "Return runs it from the list.");
        p.typeIn ("keep");
        stCheck (p.numResults() > 0 && p.resultText (0).startsWith ("Keep, not available now"), "a command that can't run now says so in the list: \""
                 + (p.numResults() > 0 ? p.resultText (0) : juce::String()) + "\"");
        p.typeIn ("zzz");
        stCheck (p.numResults() == 0, "nonsense finds nothing.");
    }

    stSay ("");
    stSay ("Speech history");
    press (stKey ('r', cmd | shift));
    stCheck (last() == "Metronome on", "Shift-Command-R says the last message again.");
    press (stKey (',', cmd | shift));
    stCheck (last().isNotEmpty() && last() != "Metronome on", "Shift-Command-comma steps back: \"" + last() + "\"");

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
