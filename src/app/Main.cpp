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
#include "click/ClickSamples.h"
#include "session/Tracks.h"
#include "app/Views.h"
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
    // Other shifted characters on a US layout arrive as the character itself;
    // the binding names the key, with Shift already in the modifiers.
    static const std::pair<juce::juce_wchar, const char*> shifted[] = {
        { '"', "'" }, { ':', ";" }, { '{', "[" }, { '}', "]" }, { '?', "/" }, { '+', "=" }, { '_', "-" }, { '|', "\\" }, { '~', "`" },
        { '!', "1" }, { '@', "2" }, { '#', "3" }, { '$', "4" }, { '%', "5" }, { '^', "6" }, { '&', "7" }, { '*', "8" }, { '(', "9" }, { ')', "0" } };
    for (auto& [from, to] : shifted)
        if (ch == from) { c.key = to; return c; }
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
    explicit KeyRouter (AppCore& c) : keyHelp (c.keyHelpOn), core (c) {}

    bool& keyHelp;   // the app core's, so the command and the router agree

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
            if (km.isPrefix (pending, scope)) { core.say (keysForPeople (pending) + ", then", "Help"); return true; }
            pending.clear();
            if (chord.key == "Escape" && chord.mods == 0) { core.commands().run ("help.key-help"); return true; }
            core.say (km.describeKey (text, scope), "Help", {}, Speech::Priority::urgent);
            return true;
        }

        if (auto b = km.lookup (pending, scope))
        {
            pending.clear();
            if (b->args.size() > 0) core.commands().run (b->commandId, b->args, CommandList::Source::key);
            else core.runFromUser (b->commandId, CommandList::Source::key);
            return true;
        }
        if (km.isPrefix (pending, scope))
            return true;   // wait for the next key
        const bool wasSequence = pending.size() > 1;
        pending.clear();
        if (wasSequence)
        {
            core.say ("That key sequence isn't used.", "Errors");
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
        core.runFromUser (id, CommandList::Source::palette);
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
// A form for any command's settings, built from its list of settings: a
// popup list for choices, a text box for numbers and names, a checkbox for
// yes or no. Every control is labelled; Return runs, Escape cancels.
class SettingsForm : public juce::Component, private juce::KeyListener
{
public:
    SettingsForm (AppCore& c, const Command& cmd, std::function<void()> closeIt)
        : core (c), id (cmd.id), close (std::move (closeIt))
    {
        setTitle (cmd.name);
        about.setText (cmd.description, juce::dontSendNotification);
        about.setTitle ("About");
        addAndMakeVisible (about);

        for (auto& a : cmd.args)
        {
            Field f;
            f.spec = a;
            f.label = std::make_unique<juce::Label> (juce::String(), capitalise (a.name) + (a.unit.isNotEmpty() ? " (" + a.unit + ")" : juce::String()));
            addAndMakeVisible (*f.label);
            const auto start = a.currentValue ? a.currentValue() : juce::var();
            const auto value = ! start.isVoid() && start.toString().isNotEmpty() ? start : a.defaultValue;

            if (a.type == ArgSpec::Type::choice)
            {
                auto box = std::make_unique<juce::ComboBox>();
                auto options = a.choicesNow();
                for (int i = 0; i < options.size(); ++i) box->addItem (options[i], i + 1);
                box->setSelectedItemIndex (std::max (0, options.indexOf (value.toString())), juce::dontSendNotification);
                // In a form, Return means the main button. A popup would
                // otherwise take it to open its list; Space still opens it.
                box->addKeyListener (this);
                // VoiceOver doesn't say a popup's new choice as you arrow
                // through it (heard 6 November 2026), so the app does.
                auto* raw = box.get();
                box->onChange = [this, raw] { core.say (raw->getText(), "Forms", {}, Speech::Priority::quiet, "form-choice"); };
                f.control = std::move (box);
            }
            else if (a.type == ArgSpec::Type::boolean)
            {
                auto t = std::make_unique<juce::ToggleButton> (capitalise (a.name));
                t->setToggleState ((bool) value, juce::dontSendNotification);
                f.control = std::move (t);
            }
            else
            {
                auto t = std::make_unique<juce::TextEditor>();
                t->setText (value.toString(), false);
                t->setSelectAllWhenFocused (true);   // typing replaces what's there; arrow keys keep it
                t->onReturnKey = [this] { run(); };
                t->onEscapeKey = [this] { if (close) close(); };
                f.control = std::move (t);
            }
            f.control->setTitle (f.label->getText());
            f.label->attachToComponent (f.control.get(), false);
            addAndMakeVisible (*f.control);
            fields.push_back (std::move (f));
        }

        ok.setButtonText (cmd.name);
        ok.onClick = [this] { run(); };
        cancel.onClick = [this] { if (close) close(); };
        addAndMakeVisible (ok);
        addAndMakeVisible (cancel);
        setSize (480, 140 + 64 * (int) fields.size());
    }

    void parentHierarchyChanged() override
    {
        juce::Component::SafePointer<SettingsForm> self (this);
        juce::MessageManager::callAsync ([self] { if (self != nullptr && self->isShowing() && ! self->fields.empty()) self->fields[0].control->grabKeyboardFocus(); });
        // Once VoiceOver has finished with the window, say where you are and what to do.
        juce::Timer::callAfterDelay (1600, [self]
        {
            if (self == nullptr || ! self->isShowing() || self->fields.empty()) return;
            self->core.say (self->firstFieldSentence(), "Forms", {}, Speech::Priority::quiet, "form");
        });
    }

    juce::String firstFieldSentence() const
    {
        auto& f = fields[0];
        juce::String value;
        if (auto* box = dynamic_cast<juce::ComboBox*> (f.control.get())) value = box->getText();
        else if (auto* e = dynamic_cast<juce::TextEditor*> (f.control.get())) value = e->getText();
        const bool isChoice = dynamic_cast<juce::ComboBox*> (f.control.get()) != nullptr;
        return f.label->getText() + ", " + (value.isNotEmpty() ? value : juce::String ("empty")) + ". "
               + (isChoice ? "Up and Down to choose, " : "") + "Return to " + ok.getButtonText() + ", Escape to cancel.";
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (16);
        about.setBounds (r.removeFromTop (40));
        for (auto& f : fields)
        {
            r.removeFromTop (24);   // the label sits above its control
            f.control->setBounds (r.removeFromTop (30));
            r.removeFromTop (8);
        }
        auto buttons = r.removeFromBottom (32);
        cancel.setBounds (buttons.removeFromRight (100));
        buttons.removeFromRight (8);
        ok.setBounds (buttons.removeFromRight (160));
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::returnKey) { run(); return true; }
        if (k == juce::KeyPress::escapeKey) { if (close) close(); return true; }
        return false;
    }

    bool keyPressed (const juce::KeyPress& k, juce::Component*) override   { return keyPressed (k); }

    CommandArgs values() const
    {
        CommandArgs out;
        for (auto& f : fields)
        {
            if (auto* box = dynamic_cast<juce::ComboBox*> (f.control.get())) out.set (f.spec.name, box->getText());
            else if (auto* t = dynamic_cast<juce::ToggleButton*> (f.control.get())) out.set (f.spec.name, t->getToggleState());
            else if (auto* e = dynamic_cast<juce::TextEditor*> (f.control.get())) out.set (f.spec.name, e->getText());
        }
        return out;
    }

    void run()
    {
        auto args = values();
        if (close) close();
        core.commands().run (id, args, CommandList::Source::menu);
    }

    struct Field { ArgSpec spec; std::unique_ptr<juce::Label> label; std::unique_ptr<juce::Component> control; };
    std::vector<Field> fields;

private:
    static juce::String capitalise (const juce::String& s)   { return s.substring (0, 1).toUpperCase() + s.substring (1); }

    AppCore& core;
    juce::String id;
    std::function<void()> close;
    juce::Label about;
    PressButton ok, cancel { "Cancel" };
};

//==============================================================================
// The track list keeps only plain Up and Down for itself. Home, End, Page Up,
// Page Down, Delete and the rest go on to the keymap, so they move around the
// song and edit it, as in REAPER, instead of jumping around the list.
class TrackList : public juce::ListBox
{
public:
    bool keyPressed (const juce::KeyPress& k) override
    {
        const bool plain = ! k.getModifiers().isAnyModifierKeyDown();
        if (plain && (k.isKeyCode (juce::KeyPress::upKey) || k.isKeyCode (juce::KeyPress::downKey)))
            return juce::ListBox::keyPressed (k);
        return false;
    }
};

class MainComponent : public juce::Component, public juce::MenuBarModel, public juce::ListBoxModel, private juce::Timer,
                      private juce::DarkModeSettingListener
{
public:
    MainComponent (AppCore& c, ScreenReaderOutput& out)
        : core (c), router (c), output (out), transportBar (c), timeline (c), meterStrip (c), history (out.log)
    {
        setLookAndFeel (&look);
        applyTheme();
        juce::Desktop::getInstance().addDarkModeSettingListener (this);
        addAndMakeVisible (transportBar);
        addAndMakeVisible (timeline);
        addAndMakeVisible (meterStrip);
        addAndMakeVisible (history);
        tracksHeader.setText ("Tracks", juce::dontSendNotification);
        tracksHeader.setWantsKeyboardFocus (false);
        tracksHeader.setAccessible (false);   // the list says its own name
        addAndMakeVisible (tracksHeader);
        timeline.laneTop = [this] { return tracks.getY() - timeline.getY() - (tracks.getViewport() != nullptr ? tracks.getViewport()->getViewPositionY() : 0); };
        timeline.laneHeight = [this] { return tracks.getRowHeight(); };
        timeline.onTrackClicked = [this] (int row) { tracks.selectRow (row); };
        core.onViewChanged = [this] { applyTheme(); resized(); repaint(); };
        core.onShowMessages = [this] { history.grabKeyboardFocus(); if (history.getNumRows() > 0) history.selectRow (history.getNumRows() - 1); };
        setTitle ("Ear First");
        status.setTitle ("Last message");
        status.setText ("Ready.", juce::dontSendNotification);
        status.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (status);
        out.onText = [this] (const juce::String& t) { status.setText (t, juce::dontSendNotification); };

        // For anyone reading the screen: the song, where it is, what capture
        // is doing. Not in the keyboard order; the same facts are a key away
        // as spoken reports, so a screen reader isn't read the clock.
        for (auto* l : { &songLine, &transportInfo, &captureInfo })
        {
            l->setJustificationType (juce::Justification::centredLeft);
            l->setWantsKeyboardFocus (false);
            addAndMakeVisible (*l);
        }
        songLine.setTitle ("Song");
        transportInfo.setTitle ("Transport");
        captureInfo.setTitle ("Capture");
        startTimerHz (30);

        tracks.setTitle ("Tracks");
        tracks.setDescription ("Up and down move between tracks.");
        tracks.setModel (this);
        tracks.setRowHeight (30);
        tracks.setWantsKeyboardFocus (true);
        addAndMakeVisible (tracks);

        core.onSongChanged = [this] { refresh(); };
        core.onSettingsNeeded = [this] (const Command& c) { showSettings (c); };
        registerWindowCommands();
        setWantsKeyboardFocus (true);
        setSize (900, 600);
        refresh();
    }

    ~MainComponent() override
    {
        core.onSongChanged = nullptr;
        core.onViewChanged = nullptr;
        core.onShowMessages = nullptr;
        juce::Desktop::getInstance().removeDarkModeSettingListener (this);
        setLookAndFeel (nullptr);
    }

    void darkModeSettingChanged() override   { applyTheme(); repaint(); }

    void applyTheme()
    {
        lastContrast = systemWantsHighContrast();
        look.apply (Theme::make (core.theme));
        juce::LookAndFeel::setDefaultLookAndFeel (&look);   // dialogs, menus and forms too
        if (auto* w = findParentComponentOfClass<juce::DocumentWindow>()) w->setBackgroundColour (look.theme.background);
        sendLookAndFeelChange();
        transportBar.refresh();
    }

    void resized() override
    {
        const float s = core.textScale;
        songLine.setFont (juce::FontOptions (26.0f * s, juce::Font::bold));
        transportInfo.setFont (juce::FontOptions (18.0f * s));
        captureInfo.setFont (juce::FontOptions (16.0f * s));
        status.setFont (juce::FontOptions (16.0f * s));
        tracks.setRowHeight (juce::roundToInt (48 * s));

        transportBar.scale = timeline.scale = meterStrip.scale = history.scale = s;
        tracksHeader.setFont (juce::FontOptions (14.0f * s, juce::Font::bold));
        history.setRowHeight (juce::roundToInt (22 * s));

        auto r = getLocalBounds().reduced (12);
        songLine.setBounds (r.removeFromTop (juce::roundToInt (36 * s)));
        transportBar.setBounds (r.removeFromTop (transportBar.heightFor (r.getWidth(), s)));
        r.removeFromTop (4);
        transportInfo.setBounds (r.removeFromTop (juce::roundToInt (28 * s)));
        captureInfo.setBounds (r.removeFromTop (juce::roundToInt (24 * s)));
        r.removeFromTop (8);
        status.setBounds (r.removeFromBottom (juce::roundToInt (32 * s)));
        history.setBounds (r.removeFromBottom (juce::roundToInt (22 * s) * 4 + 2));
        r.removeFromBottom (6);
        meterStrip.setBounds (r.removeFromBottom (juce::roundToInt (54 * s)));
        r.removeFromBottom (8);
        // The track list and the timeline side by side, rows lined up.
        auto left = r.removeFromLeft (juce::jlimit (juce::roundToInt (180 * s), juce::roundToInt (360 * s), r.getWidth() / 3));
        r.removeFromLeft (6);
        timeline.rulerHeight = juce::roundToInt (34 * s);
        tracksHeader.setBounds (left.removeFromTop (timeline.rulerHeight));
        tracks.setBounds (left);
        timeline.setBounds (r);
    }

    void timerCallback() override
    {
        songLine.setText (core.session() ? core.session()->getName() + (core.session()->looksUnsaved() ? "  (not saved)" : "")
                                         : juce::String ("No song open"), juce::dontSendNotification);
        transportInfo.setText (core.transportLine(), juce::dontSendNotification);
        captureInfo.setText (core.captureLine(), juce::dontSendNotification);
        transportBar.refresh();
        if (transportBar.beatLight.update() && core.beatFlash) { flashOne = transportBar.beatLight.isOne(); flashUntil = juce::Time::getMillisecondCounterHiRes() + 140.0; repaint(); }
        else if (flashUntil > 0 && juce::Time::getMillisecondCounterHiRes() > flashUntil) { flashUntil = 0; repaint(); }
        timeline.repaint();
        meterStrip.repaint();
        history.update();
        // The system's high contrast setting has no notification, so look every few seconds.
        if (++ticks % 30 == 0 && core.theme == "Follow the system" && systemWantsHighContrast() != lastContrast) { applyTheme(); repaint(); }
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (look.theme.background);
        g.setColour (look.theme.outline);
        g.drawRect (history.getBounds().expanded (1));
        g.drawRect (tracks.getBounds().expanded (1));
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        if (flashUntil <= 0) return;
        g.setColour (flashOne ? look.theme.accent : look.theme.accent.withAlpha (0.6f));
        g.drawRect (getLocalBounds(), flashOne ? 12 : 8);
    }

    EarFirstLook look;
    juce::TooltipWindow tooltips { this, 600 };
    TransportBar transportBar;
    Timeline timeline;
    MeterStrip meterStrip;
    MessageHistory history;
    juce::Label tracksHeader;
    double flashUntil = 0;
    bool flashOne = false, lastContrast = false;
    int ticks = 0;

    void refresh()
    {
        timeline.songChanged();
        output.screenChanged();
        resized();
        tracks.updateContent();
        if (auto* s = core.session())
        {
            auto n = tracksList().size();
            tracks.selectRow (juce::jlimit (0, std::max (0, n - 1), core.selectedTrack), true, true);
        }
        if (auto* w = findParentComponentOfClass<juce::DocumentWindow>())
            w->setName (core.session() ? "Ear First - " + core.session()->getName() : juce::String ("Ear First"));
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
        return { "File", "Edit", "Transport", "Click", "Song", "Markers", "Capture", "Tracks", "Clips", "Audio", "View", "Reports", "Speech", "Help" };
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
            core.runFromUser (it->second, CommandList::Source::menu);
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
        p->onSummary = [this] (const juce::String& s) { core.say (s, "Search", SpeechPrefs::firstSentence (s), Speech::Priority::quiet, "palette"); };
        palette = p;
        juce::DialogWindow::LaunchOptions o;
        o.content.setOwned (p);
        o.dialogTitle = "Find a command";
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = true;
        paletteWindow = o.launchAsync();
    }

    void showSettings (const Command& c)
    {
        auto* f = new SettingsForm (core, c, [this]
        {
            output.screenChanged();
            if (settingsWindow) settingsWindow->exitModalState (0);
        });
        lastSettingsForm = f;
        if (! showDialogs) return;   // the self-test drives the form without a window
        juce::DialogWindow::LaunchOptions o;
        o.content.setOwned (f);
        o.dialogTitle = c.name;
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        settingsWindow = o.launchAsync();
    }

    bool showDialogs = true;
    SettingsForm* lastSettingsForm = nullptr;     // for the self-test (owned by its window, or leaked to the test)
    juce::Component::SafePointer<juce::DialogWindow> settingsWindow;
    Palette* palette = nullptr;          // for the self-test
    juce::Component::SafePointer<juce::DialogWindow> paletteWindow;
    KeyRouter router;
    TrackList tracks;
    juce::Label status, songLine, transportInfo, captureInfo;

    juce::Array<tracktion::AudioTrack*> tracksList() const
    {
        if (auto* s = core.session()) return contentTracks (s->getEdit());
        return {};
    }

    int getNumRows() override            { return tracksList().size(); }
    juce::String trackText (int row) const
    {
        auto list = tracksList();
        if (row < 0 || row >= list.size()) return {};
        auto* t = list[row];
        auto n = t->getClips().size();
        return t->getName() + ", " + (n == 0 ? juce::String ("empty") : juce::String (n) + (n == 1 ? " clip" : " clips"))
               + (t->isMuted (false) ? ", muted" : "") + (t->isSolo (false) ? ", soloed" : "");
    }
    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
    {
        if (selected)
        {
            g.fillAll (look.theme.selection);
            if (tracks.hasKeyboardFocus (true)) { g.setColour (look.theme.focus.contrasting (0.0f)); g.drawRect (0, 0, w, h, 2); }
        }
        g.setColour (selected ? look.theme.selectionText : look.theme.text);
        g.setFont (juce::FontOptions (17.0f * core.textScale));
        g.drawText (trackText (row), 8, 0, w - 16, h, juce::Justification::centredLeft, true);
    }
    juce::String getNameForRow (int row) override   { return trackText (row); }
    void selectedRowsChanged (int row) override     { if (row >= 0) core.selectedTrack = row; }

private:

    void registerWindowCommands()
    {
        // Key help and Find a command live in the app core (so your keys for
        // them load at startup); the window only provides the palette.
        core.onShowPalette = [this] { showPalette(); };

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
        core->onImportClickSoundsRequested = [this] { chooseClickSounds(); };
        core->onImportAudioRequested = [this] { chooseAudioToImport(); };
        core->onShowWelcome = [this] { showWelcome(); };
        core->onUnsavedClose = [this] (std::function<void()> closeIt) { askToSave (core->session()->getName(), "close it", std::move (closeIt)); };
        window->setVisible (true);
        window->content->tracks.grabKeyboardFocus();
        output.screenChanged();

        // Said once the window has focus and VoiceOver has finished
        // describing it, or it gets dropped. Each part is its own kind, so a
        // short welcome never cuts off a problem.
        // The interface opens after the window, so the app answers at once even
        // while a big interface takes its time.
        core->startAudio ([this]
        {
            if (core == nullptr) return;
            if (window != nullptr) window->content->refresh();
            // A new interface gets checked once for the computer's own sound, after the welcome.
            juce::Timer::callAfterDelay (3500, [this] { if (core != nullptr) core->findComputerInputs (true); });
            if (! welcomed) return;   // the welcome hasn't been said yet; it says any problem itself
            if (auto p = core->startupProblemSentence(); p.isNotEmpty()) core->say (p, "Errors");
        });
        if (! core->welcomeSeen)
        {
            // The first time: the welcome window says it all, so nothing is spoken over it.
            welcomed = true;
            core->welcomeSeen = true;
            core->saveAppSettings();
            juce::Timer::callAfterDelay (800, [this] { if (core != nullptr) showWelcome(); });
        }
        else juce::Timer::callAfterDelay (1500, [this]
        {
            if (core == nullptr) return;
            welcomed = true;
            core->say ("Press Command-N for a new song, Command-O to open one, or Command-Shift-P to find any command.", "Startup", "Ready.");
            if (auto p = core->startupProblemSentence(); p.isNotEmpty()) core->say (p, "Errors");
            if (auto l = core->leftoverSentence(); l.isNotEmpty()) core->say (l, "Capture");
        });
        runCommandsFrom (args, 3000);
    }

    // Commands named on the command line run once the app is ready, each as if
    // chosen from the menu: open -a "Ear First" --args --run audio.find-computer-input
    // To reach a copy that's already running, start the app's own program
    // ("Ear First.app/Contents/MacOS/Ear First" --run ...): it hands the
    // commands over and quits. (open -a only brings the running copy forward.)
    void runCommandsFrom (const juce::String& args, int delayMs)
    {
        auto tokens = juce::StringArray::fromTokens (args, true);
        juce::StringArray ids;
        for (int i = 0; i + 1 < tokens.size(); ++i)
            if (tokens[i] == "--run") ids.add (tokens[i + 1].unquoted());
        if (ids.isEmpty()) return;
        juce::Timer::callAfterDelay (delayMs, [this, ids]
        {
            if (core == nullptr) return;
            for (auto& id : ids)
                if (core->commands().find (id) == nullptr) core->say ("There's no command called " + id + ".", "Errors");
                else core->runFromUser (id, CommandList::Source::menu);
        });
    }

    bool welcomed = false;
    juce::Component::SafePointer<juce::DialogWindow> welcomeWindow;

    void showWelcome()
    {
        if (welcomeWindow != nullptr) { welcomeWindow->toFront (true); return; }
        auto* w = new WelcomeComponent (*core, [this] { if (welcomeWindow) welcomeWindow->exitModalState (0); });
        juce::DialogWindow::LaunchOptions o;
        o.content.setOwned (w);
        o.dialogTitle = "Welcome to Ear First";
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = true;
        welcomeWindow = o.launchAsync();
        // Focus can only land once the window is really up and in front.
        juce::Component::SafePointer<WelcomeComponent> safe (w);
        juce::Timer::callAfterDelay (250, [this, safe]
        {
            if (welcomeWindow != nullptr) welcomeWindow->toFront (true);
            if (safe != nullptr) safe->text.grabKeyboardFocus();
        });
    }

    void anotherInstanceStarted (const juce::String& args) override   { runCommandsFrom (args, 0); }

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
                askToSave (s->getName(), "quit", [this] { quit(); });
                return;
            }
        quit();
    }

    // "Save and quit", "Don't save", "Cancel", in the system's own alert: it
    // reads its message as it opens, Space presses the button you're on,
    // Escape cancels. (JUCE's own alert didn't take Space, heard 6 Nov 2026.)
    void askToSave (const juce::String& songName, const juce::String& verb, std::function<void()> then)
    {
        auto opts = juce::MessageBoxOptions()
                        .withIconType (juce::MessageBoxIconType::QuestionIcon)
                        .withTitle (songName + " has unsaved changes")
                        .withMessage ("Save " + songName + " before you " + verb + "?")
                        .withButton ("Save and " + verb, 1)
                        .withButton ("Don't save", 2)
                        .withButton ("Cancel", 0);
        juce::NativeMessageBox::showForOptionsAsync (opts, [this, then] (int result)
        {
            if (core == nullptr || core->session() == nullptr) return;
            if (result == 1)   // save
            {
                auto said = core->session()->save();
                if (said != "Saved.") { core->say (said, "Errors", {}, Speech::Priority::urgent); return; }
                then();
            }
            else if (result == 2)   // don't save
                then();
        });
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

    void chooseClickSounds()
    {
        juce::StringArray patterns;
        for (auto& e : ClickSamples::audioExtensions()) patterns.add ("*." + e);
        chooser = std::make_unique<juce::FileChooser> ("Choose sounds for the click",
            juce::File::getSpecialLocation (juce::File::userMusicDirectory), patterns.joinIntoString (";"));
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                                | juce::FileBrowserComponent::canSelectMultipleItems,
                              [this] (const juce::FileChooser& fc) { if (core) core->importClickSounds (fc.getResults()); });
    }

    ScreenReaderOutput output;
    std::unique_ptr<AppCore> core;
    std::unique_ptr<MainWindow> window;
    void chooseAudioToImport()
    {
        chooser = std::make_unique<juce::FileChooser> ("Choose audio to import", juce::File::getSpecialLocation (juce::File::userMusicDirectory), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.m4a;*.ogg");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::canSelectMultipleItems,
                              [this] (const juce::FileChooser& fc)
                              {
                                  if (core == nullptr || fc.getResults().isEmpty()) return;
                                  core->say (core->importAudio (fc.getResults()), "Import");
                              });
    }

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
