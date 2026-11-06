// Ear First: the app window. Native menus built from the command list, keys
// routed through the keymap, a command palette, key help, and speech through
// the screen reader (JUCE's announcements: VoiceOver on the Mac, UI Automation
// on Windows) plus a status line anyone can read.
//
//   Ear First                 the app
//   ear-first-selftest        drives the real window parts, menus and keys with
//                             no audio device and no announcements, never shown

#include <JuceHeader.h>
#include "app/AppCore.h"
#include <iostream>

using namespace earfirst;

#if JUCE_MAC
namespace earfirst { void announceFromFocusedWindowUtf8 (const char*, bool high); }
#endif

namespace
{
void postToScreenReader (const juce::String& text, juce::AccessibilityHandler::AnnouncementPriority p)
{
   #if JUCE_MAC
    earfirst::announceFromFocusedWindowUtf8 (text.toRawUTF8(), p == juce::AccessibilityHandler::AnnouncementPriority::high);
   #else
    juce::AccessibilityHandler::postAnnouncement (text, p);
   #endif
}

//==============================================================================
// Key presses into the keymap's shared form.
KeyChord chordFromKeyPress (const juce::KeyPress& k)
{
    KeyChord c;
    auto m = k.getModifiers();
    if (m.isCommandDown()) c.mods |= KeyChord::primary;       // Command on the Mac, Control on Windows
    if (m.isShiftDown()) c.mods |= KeyChord::shift;
    if (m.isAltDown()) c.mods |= KeyChord::alt;
   #if JUCE_MAC
    if (m.isCtrlDown()) c.mods |= KeyChord::secondary;
   #endif

    const int code = k.getKeyCode();
    struct Named { int code; const char* name; };
    static const Named named[] = {
        { juce::KeyPress::spaceKey, "Space" }, { juce::KeyPress::returnKey, "Return" }, { juce::KeyPress::escapeKey, "Escape" },
        { juce::KeyPress::tabKey, "Tab" }, { juce::KeyPress::deleteKey, "Delete" }, { juce::KeyPress::backspaceKey, "Backspace" },
        { juce::KeyPress::leftKey, "Left" }, { juce::KeyPress::rightKey, "Right" }, { juce::KeyPress::upKey, "Up" },
        { juce::KeyPress::downKey, "Down" }, { juce::KeyPress::homeKey, "Home" }, { juce::KeyPress::endKey, "End" },
        { juce::KeyPress::pageUpKey, "PageUp" }, { juce::KeyPress::pageDownKey, "PageDown" }, { juce::KeyPress::insertKey, "Insert" },
        { juce::KeyPress::numberPad0, "Keypad0" }, { juce::KeyPress::numberPad1, "Keypad1" }, { juce::KeyPress::numberPad2, "Keypad2" },
        { juce::KeyPress::numberPad3, "Keypad3" }, { juce::KeyPress::numberPad4, "Keypad4" }, { juce::KeyPress::numberPad5, "Keypad5" },
        { juce::KeyPress::numberPad6, "Keypad6" }, { juce::KeyPress::numberPad7, "Keypad7" }, { juce::KeyPress::numberPad8, "Keypad8" },
        { juce::KeyPress::numberPad9, "Keypad9" }, { juce::KeyPress::numberPadAdd, "KeypadPlus" },
        { juce::KeyPress::numberPadSubtract, "KeypadMinus" }, { juce::KeyPress::numberPadMultiply, "KeypadMultiply" },
        { juce::KeyPress::numberPadDivide, "KeypadDivide" }, { juce::KeyPress::numberPadDecimalPoint, "KeypadDecimal" },
        { juce::KeyPress::numberPadEquals, "KeypadEquals" },
    };
    for (auto& n : named)
        if (code == n.code) { c.key = n.name; return c; }
    for (int f = 1; f <= 24; ++f)
        if (code == juce::KeyPress::F1Key + (f - 1)) { c.key = "F" + juce::String (f); return c; }

    // Letters and punctuation by the character, not the key's position, so
    // keys work on any keyboard layout.
    auto ch = (juce::juce_wchar) code;
    if (ch == ',') { c.key = "Comma"; return c; }
    if (ch == '<') { c.key = "Comma"; return c; }      // Shift+comma arrives as <
    if (ch == '>') { c.key = "."; return c; }
    if (ch > 32 && ch < 127) c.key = juce::String::charToString (ch).toUpperCase();
    return c;
}

//==============================================================================
// Where speech goes: the screen reader, plus a line on screen and a log.
struct ScreenReaderOutput : SpeechOutput
{
    std::function<void (const juce::String&)> onText;    // the status line
    juce::StringArray log;                                // for the self-test
    bool announce = true;

    // VoiceOver drops an announcement posted while focus or a selection is
    // moving (checked with VoiceOver, November 2026). So when
    // the screen has just changed, the announcement waits until it settles.
    double settleSeconds = 0.5;
    void screenChanged()    { lastScreenChange = juce::Time::getMillisecondCounterHiRes() / 1000.0; }

    void output (const juce::String& text, bool interrupt) override
    {
        log.add (text);
        if (onText != nullptr && text.isNotEmpty()) onText (text);
        if (! announce || text.isEmpty()) return;

        const auto priority = interrupt ? juce::AccessibilityHandler::AnnouncementPriority::high
                                        : juce::AccessibilityHandler::AnnouncementPriority::medium;
        const double now = juce::Time::getMillisecondCounterHiRes() / 1000.0;
        const double wait = lastScreenChange + settleSeconds - now;
        if (wait <= 0)
        {
            postToScreenReader (text, priority);
            return;
        }
        juce::Timer::callAfterDelay (juce::roundToInt (wait * 1000.0), [text, priority]
        {
            postToScreenReader (text, priority);
        });
    }

private:
    double lastScreenChange = 0;
};

//==============================================================================
// Keys go to the keymap first. Sequences wait for their next key; key help
// says what a key does instead of doing it.
class KeyRouter
{
public:
    explicit KeyRouter (AppCore& c) : core (c) {}

    bool keyHelp = false;

    // True if the key was used.
    bool handle (const juce::KeyPress& kp, const juce::String& scope)
    {
        auto chord = chordFromKeyPress (kp);
        if (auto log = juce::SystemStats::getEnvironmentVariable ("EARFIRST_KEYLOG", {}); log.isNotEmpty())
            juce::File (log).appendText ("code " + juce::String (kp.getKeyCode()) + " char " + juce::String ((int) kp.getTextCharacter())
                                         + " mods " + juce::String (kp.getModifiers().getRawFlags()) + " -> " + keysToText ({ chord }) + " in " + scope + "\n");
        if (! chord.isValid()) return false;
        if (isModifierOnly (kp)) return false;

        const double now = juce::Time::getMillisecondCounterHiRes() / 1000.0;
        if (! pending.empty() && now - pendingAt > 3.0) pending.clear();
        pending.push_back (chord);
        pendingAt = now;

        auto& km = core.keymap();
        if (keyHelp)
        {
            // Key help: Escape or the key help key itself leaves it; anything else is described.
            auto text = keysToText (pending);
            if (auto b = km.lookup (pending, scope); b && b->commandId == "help.key-help")
            {
                pending.clear();
                core.commands().run ("help.key-help");
                return true;
            }
            if (km.isPrefix (pending, scope)) { core.speech().say (keysForPeople (pending) + ", then"); return true; }
            pending.clear();
            if (chord.key == "Escape" && chord.mods == 0) { core.commands().run ("help.key-help"); return true; }
            core.speech().say (km.describeKey (text, scope), Speech::Priority::urgent);
            return true;
        }

        if (auto b = km.lookup (pending, scope))
        {
            pending.clear();
            core.commands().run (b->commandId, b->args, CommandList::Source::key);
            return true;
        }
        if (km.isPrefix (pending, scope))
            return true;   // wait for the next key
        const bool wasSequence = pending.size() > 1;
        pending.clear();
        if (wasSequence)
        {
            core.speech().say ("That key sequence isn't used.");
            return true;
        }
        return false;
    }

private:
    static bool isModifierOnly (const juce::KeyPress& k)
    {
        return k.getKeyCode() == 0;
    }

    AppCore& core;
    KeySequence pending;
    double pendingAt = 0;
};

//==============================================================================
// The command palette: type to find any command by name, tag or what it
// does; each result says its keys; Return runs it.
class Palette : public juce::Component, private juce::TextEditor::Listener, private juce::ListBoxModel,
                private juce::KeyListener, private juce::Timer
{
public:
    Palette (AppCore& c, std::function<void()> closeIt) : core (c), close (std::move (closeIt))
    {
        search.setTitle ("Find a command");
        search.setDescription ("Type part of a command's name or what it does. Down arrow to the results, Return to run.");
        search.addListener (this);
        search.addKeyListener (this);   // Down and Up go to the results, not the text
        search.setTextToShowWhenEmpty ("Find a command", juce::Colours::grey);
        addAndMakeVisible (search);

        results.setTitle ("Commands");
        results.setModel (this);
        results.setRowHeight (28);
        addAndMakeVisible (results);
        refresh();
        setSize (560, 420);
    }

    // Typing always goes to the search box: when the dialog opens, focus
    // lands there, never on the results.
    void parentHierarchyChanged() override
    {
        juce::Component::SafePointer<Palette> self (this);
        juce::MessageManager::callAsync ([self] { if (self != nullptr && self->isShowing()) self->search.grabKeyboardFocus(); });
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12);
        search.setBounds (r.removeFromTop (32));
        r.removeFromTop (8);
        results.setBounds (r);
    }

    void typeIn (const juce::String& s)          { search.setText (s, false); refresh(); }
    // Said a moment after typing stops: how many match, and the first.
    std::function<void (const juce::String&)> onSummary;
    juce::String summary() const
    {
        if (found.empty()) return "No commands match.";
        return juce::String ((int) found.size()) + (found.size() == 1 ? " command. " : " commands. First: ") + rowText (0);
    }
    int numResults() const                       { return (int) found.size(); }
    juce::String resultText (int row) const      { return rowText (row); }
    juce::String runRow (int row)
    {
        if (row < 0 || row >= (int) found.size()) return {};
        auto id = found[(size_t) row]->id;
        if (close) close();
        core.commands().run (id, {}, CommandList::Source::palette);
        return id;
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::escapeKey) { if (close) close(); return true; }
        if (k == juce::KeyPress::returnKey) { runRow (std::max (0, results.getSelectedRow())); return true; }
        if (k == juce::KeyPress::downKey && search.hasKeyboardFocus (false) && ! found.empty())
        {
            results.selectRow (0);
            results.grabKeyboardFocus();
            return true;
        }
        return false;
    }

private:
    bool keyPressed (const juce::KeyPress& k, juce::Component*) override
    {
        if ((k == juce::KeyPress::downKey || k == juce::KeyPress::upKey) && ! found.empty())
        {
            stopTimer();
            results.selectRow (k == juce::KeyPress::upKey ? (int) found.size() - 1 : 0);
            results.grabKeyboardFocus();
            return true;
        }
        return false;
    }

    void timerCallback() override
    {
        stopTimer();
        if (auto log = juce::SystemStats::getEnvironmentVariable ("EARFIRST_KEYLOG", {}); log.isNotEmpty())
            juce::File (log).appendText ("palette timer: focus " + juce::String ((int) search.hasKeyboardFocus (false))
                                         + " showing " + juce::String ((int) isShowing()) + " summary? " + juce::String ((int) (onSummary != nullptr)) + "\n");
        if (onSummary && isShowing() && ! results.hasKeyboardFocus (true)) onSummary (summary());
    }

    void textEditorTextChanged (juce::TextEditor&) override  { refresh(); startTimer (1500); }   // VoiceOver drops it sooner after typing
    void textEditorReturnKeyPressed (juce::TextEditor&) override { runRow (0); }
    void textEditorEscapeKeyPressed (juce::TextEditor&) override { if (close) close(); }

    void refresh()
    {
        auto q = search.getText().trim();
        found.clear();
        if (q.isEmpty())
        {
            for (auto& id : core.commands().allIds())
                if (auto* c = core.commands().find (id)) found.push_back (c);
            std::sort (found.begin(), found.end(), [] (auto* a, auto* b) { return a->name < b->name; });
        }
        else
            found = core.commands().search (q);
        results.updateContent();
        results.repaint();
    }

    juce::String rowText (int row) const
    {
        auto* c = found[(size_t) row];
        auto keys = core.keymap().keysFor (c->id);
        juce::String s = c->name;
        if (c->isToggle())
            if (auto st = c->toggleState()) s << ", " << (*st ? c->onWord : c->offWord);
        if (c->available)
            if (auto a = c->available(); ! a.enabled) s << ", not available now";
        s << (keys.isEmpty() ? juce::String (", no key") : ", " + keys.joinIntoString (" or "));
        s << ". " << c->description;
        return s;
    }

    int getNumRows() override                    { return (int) found.size(); }
    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
    {
        if (row < 0 || row >= (int) found.size()) return;
        if (selected) g.fillAll (findColour (juce::ListBox::backgroundColourId).contrasting (0.2f));
        g.setColour (findColour (juce::ListBox::textColourId));
        g.drawText (rowText (row), 8, 0, w - 16, h, juce::Justification::centredLeft, true);
    }
    juce::String getNameForRow (int row) override { return row >= 0 && row < (int) found.size() ? rowText (row) : juce::String(); }
    void returnKeyPressed (int row) override       { runRow (row); }
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override { runRow (row); }

    AppCore& core;
    std::function<void()> close;
    juce::TextEditor search;
    juce::ListBox results;
    std::vector<const Command*> found;
};

//==============================================================================
class MainComponent : public juce::Component, public juce::MenuBarModel, public juce::ListBoxModel
{
public:
    MainComponent (AppCore& c, ScreenReaderOutput& out) : core (c), router (c), output (out)
    {
        setTitle ("Ear First");
        status.setTitle ("Last message");
        status.setText ("Ready.", juce::dontSendNotification);
        status.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (status);
        out.onText = [this] (const juce::String& t) { status.setText (t, juce::dontSendNotification); };

        tracks.setTitle ("Tracks");
        tracks.setDescription ("Up and down move between tracks.");
        tracks.setModel (this);
        tracks.setRowHeight (30);
        tracks.setWantsKeyboardFocus (true);
        addAndMakeVisible (tracks);

        core.onSongChanged = [this] { refresh(); };
        registerWindowCommands();
        setWantsKeyboardFocus (true);
        setSize (900, 600);
        refresh();
    }

    ~MainComponent() override
    {
        core.onSongChanged = nullptr;
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12);
        status.setBounds (r.removeFromBottom (32));
        r.removeFromBottom (8);
        tracks.setBounds (r);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));
    }

    void refresh()
    {
        output.screenChanged();
        tracks.updateContent();
        if (auto* s = core.session())
        {
            auto n = tracksList().size();
            tracks.selectRow (juce::jlimit (0, std::max (0, n - 1), core.selectedTrack), true, true);
            if (auto* w = findParentComponentOfClass<juce::DocumentWindow>())
                w->setName ("Ear First - " + s->getName());
        }
        tracks.repaint();
        menuItemsChanged();
    }

    // The scope keys work in: the track list, or the main window.
    juce::String focusedScope() const
    {
        return tracks.hasKeyboardFocus (true) ? "tracks" : "main";
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        return router.handle (k, focusedScope());
    }

    // Menus: built from the command list, so every command is in a menu and
    // every menu item says its key. Toggles are ticked.
    juce::StringArray getMenuBarNames() override
    {
        return { "File", "Edit", "Transport", "Capture", "Tracks", "Reports", "Speech", "Help" };
    }

    juce::PopupMenu getMenuForIndex (int, const juce::String& menuName) override
    {
        juce::PopupMenu m;
        auto ids = core.commands().allIds();
        std::vector<const Command*> cmds;
        for (auto& id : ids)
            if (auto* c = core.commands().find (id); c != nullptr && c->category == menuName)
                cmds.push_back (c);
        // Commands keep a fixed order within a menu: the order they were added.
        std::stable_sort (cmds.begin(), cmds.end(), [this] (auto* a, auto* b) { return order (a->id) < order (b->id); });
        for (auto* c : cmds)
        {
            juce::PopupMenu::Item item (c->name);
            item.itemID = menuIdFor (c->id);
            auto keys = core.keymap().keysFor (c->id);
            if (! keys.isEmpty()) item.shortcutKeyDescription = keys[0];
            if (c->isToggle()) if (auto st = c->toggleState()) item.isTicked = *st;
            if (c->available) item.isEnabled = c->available().enabled;
            m.addItem (item);
        }
        return m;
    }

    void menuItemSelected (int itemId, int) override
    {
        auto it = menuIds.find (itemId);
        if (it != menuIds.end())
            core.commands().run (it->second, {}, CommandList::Source::menu);
    }

    void showPalette()
    {
        auto* p = new Palette (core, [this]
        {
            // Focus is about to jump back to the window: hold what the
            // command says until it lands, or VoiceOver drops it.
            output.screenChanged();
            if (paletteWindow) paletteWindow->exitModalState (0);
        });
        p->onSummary = [this] (const juce::String& s) { core.speech().say (s, Speech::Priority::quiet, "palette"); };
        palette = p;
        juce::DialogWindow::LaunchOptions o;
        o.content.setOwned (p);
        o.dialogTitle = "Find a command";
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = true;
        paletteWindow = o.launchAsync();
    }

    Palette* palette = nullptr;          // for the self-test
    juce::Component::SafePointer<juce::DialogWindow> paletteWindow;
    KeyRouter router;
    juce::ListBox tracks;
    juce::Label status;

    juce::Array<tracktion::AudioTrack*> tracksList() const
    {
        if (auto* s = core.session()) return tracktion::getAudioTracks (s->getEdit());
        return {};
    }

    int getNumRows() override            { return tracksList().size(); }
    juce::String trackText (int row) const
    {
        auto list = tracksList();
        if (row < 0 || row >= list.size()) return {};
        auto* t = list[row];
        auto n = t->getClips().size();
        return t->getName() + ", " + (n == 0 ? juce::String ("empty") : juce::String (n) + (n == 1 ? " clip" : " clips"));
    }
    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
    {
        if (selected) g.fillAll (findColour (juce::ListBox::backgroundColourId).contrasting (0.2f));
        g.setColour (findColour (juce::ListBox::textColourId));
        g.drawText (trackText (row), 8, 0, w - 16, h, juce::Justification::centredLeft, true);
    }
    juce::String getNameForRow (int row) override   { return trackText (row); }
    void selectedRowsChanged (int row) override     { if (row >= 0) core.selectedTrack = row; }

private:

    void registerWindowCommands()
    {
        Command help;
        help.id = "help.key-help"; help.name = "Key help"; help.category = "Help";
        help.description = "While on, pressing a key says what it does instead of doing it. Press Escape or this key again to turn it off.";
        help.tags = { "what does this key do", "describe key", "learn" };
        help.toggleState = [this] { return std::optional<bool> (router.keyHelp); };
        help.run = [this] (const CommandArgs&) { router.keyHelp = ! router.keyHelp; return CommandResult::done(); };
        core.commands().add (help);

        Command pal;
        pal.id = "help.find-command"; pal.name = "Find a command"; pal.category = "Help";
        pal.description = "Opens a search of every command. Type part of a name or what it does, and press Return to run it.";
        pal.tags = { "palette", "search", "action list", "everything" };
        pal.run = [this] (const CommandArgs&) { showPalette(); return CommandResult::done(); };
        core.commands().add (pal);

        // Shipped keys, not your changes. Command-/ is free on the Mac and Ctrl+/ on Windows.
        core.keymap().addDefaults ({ { "Primary+/", "help.key-help", "global", {} },
                                     { "Primary+Shift+P", "help.find-command", "global", {} } });

        for (auto& id : core.commands().allIds())
            menuIdFor (id);
    }

    int menuIdFor (const juce::String& id)
    {
        for (auto& [k, v] : menuIds)
            if (v == id) return k;
        const int newId = (int) menuIds.size() + 1;
        menuIds[newId] = id;
        return newId;
    }

    int order (const juce::String& id) const
    {
        for (auto& [k, v] : menuIds) if (v == id) return k;
        return 1 << 20;
    }

    AppCore& core;
    ScreenReaderOutput& output;
    std::map<int, juce::String> menuIds;
};

//==============================================================================
class MainWindow : public juce::DocumentWindow
{
public:
    MainWindow (AppCore& core, ScreenReaderOutput& out)
        : DocumentWindow ("Ear First", juce::Desktop::getInstance().getDefaultLookAndFeel()
                                           .findColour (juce::ResizableWindow::backgroundColourId), allButtons)
    {
        setUsingNativeTitleBar (true);
        content = new MainComponent (core, out);
        setContentOwned (content, true);
        setResizable (true, true);
        centreWithSize (getWidth(), getHeight());
       #if JUCE_MAC
        juce::MenuBarModel::setMacMainMenu (content);
       #else
        setMenuBar (content);
       #endif
    }

    ~MainWindow() override
    {
       #if JUCE_MAC
        juce::MenuBarModel::setMacMainMenu (nullptr);
       #else
        setMenuBar (nullptr);
       #endif
    }

    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }

    MainComponent* content = nullptr;
};

//==============================================================================
int runSelfTest();

class EarFirstApp : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return "Ear First"; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override          { return false; }

    void initialise (const juce::String& args) override
    {
        AppCore::Options o;
        core = std::make_unique<AppCore> (output, o);
        window = std::make_unique<MainWindow> (*core, output);
        core->onOpenSongRequested = [this] { chooseSong(); };
        window->setVisible (true);
        window->content->tracks.grabKeyboardFocus();
        output.screenChanged();

        // Said once the window has focus and VoiceOver has finished
        // describing it, or it gets dropped.
        auto hello = core->startupSentence();
        juce::Timer::callAfterDelay (1500, [this, hello]
        {
            if (core == nullptr) return;
            core->speech().say ("Press Command-N for a new song, Command-O to open one, or Command-Shift-P to find any command."
                                + (hello.isNotEmpty() ? " " + hello : juce::String()));
        });
    }

    void shutdown() override
    {
        window.reset();
        core.reset();
    }

    void systemRequestedQuit() override
    {
        if (core != nullptr)
            if (auto* s = core->session(); s != nullptr && s->hasUnsavedChanges())
            {
                core->speech().say (s->getName() + " has unsaved changes. Save with Command-S, then quit.", Speech::Priority::urgent);
                return;
            }
        quit();
    }

private:
    void chooseSong()
    {
        chooser = std::make_unique<juce::FileChooser> ("Open a song folder",
            juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("Ear First Songs"));
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [this] (const juce::FileChooser& fc)
                              {
                                  if (fc.getResult() == juce::File()) return;
                                  CommandArgs a; a.set ("folder", fc.getResult().getFullPathName());
                                  core->commands().run ("file.open-song", a, CommandList::Source::menu);
                              });
    }

    ScreenReaderOutput output;
    std::unique_ptr<AppCore> core;
    std::unique_ptr<MainWindow> window;
    std::unique_ptr<juce::FileChooser> chooser;
};

#include "app/SelfTest.inc"
} // namespace

#if EARFIRST_SELFTEST_MAIN
int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    return runSelfTest();
}
#else
START_JUCE_APPLICATION (EarFirstApp)
#endif
