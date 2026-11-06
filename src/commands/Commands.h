#pragma once
// The command list: every action in the DAW is one named command, and keys,
// speech, control surfaces, scripts and Faith all run the same commands.
//
// Design rules, from how other DAWs and blind-made tools do it (research notes
// in iCloud, Ear First DAW):
//  - A command has a stable string id ("transport.play"), never a number or
//    its display name, so keymaps and scripts never break when names change.
//  - Commands take typed arguments (nudge by how much), so one command does
//    what other DAWs need dozens of near-copies for.
//  - The command system itself speaks results. A toggle always says its new
//    state, new state first. A command that can't run says why.
//  - Report commands only answer a question and change nothing.
//  - Macros are ordinary commands: their middle steps are silent, they speak
//    once at the end, and they undo as one step.
//  - Repeat last command repeats it with the same arguments.

#include <juce_core/juce_core.h>
#include <functional>
#include <optional>
#include <map>

namespace earfirst
{

// What a command needs from the rest of the app. The speech layer and the
// engine's undo manager plug in here.
struct CommandHost
{
    virtual ~CommandHost() = default;
    virtual void speak (const juce::String& sentence, bool urgent) = 0;
    virtual void beginUndoStep (const juce::String& name) { juce::ignoreUnused (name); }
    virtual void endUndoStep() {}
};

struct ArgSpec
{
    enum class Type { number, integer, boolean, text, choice };
    juce::String name, description;
    Type type = Type::number;
    juce::var defaultValue;
    double minimum = -1.0e300, maximum = 1.0e300;   // number and integer
    juce::StringArray choices;                     // choice
    juce::String unit;                             // spoken after a value: "dB", "beats"
};

using CommandArgs = juce::NamedValueSet;

struct CommandResult
{
    bool ok = true;
    juce::String spoken;        // what to say; empty means say nothing
    bool urgent = false;

    static CommandResult done (juce::String s = {})     { return { true, std::move (s), false }; }
    static CommandResult failed (juce::String s)        { return { false, std::move (s), true }; }
};

struct Availability
{
    bool enabled = true;
    juce::String reason;        // spoken when disabled: "Nothing is selected."
    static Availability yes()                           { return {}; }
    static Availability no (juce::String why)           { return { false, std::move (why) }; }
};

struct Command
{
    juce::String id;            // "transport.play"; lowercase, dot-separated, stable forever
    juce::String name;          // "Play", shown and spoken
    juce::String category;      // "Transport"
    juce::String description;   // one plain sentence
    juce::StringArray tags;     // extra search words: "click" for the metronome
    std::vector<ArgSpec> args;
    bool undoable = false;      // wraps the run in an undo step
    bool isReport = false;      // only answers a question; never changes anything

    std::function<CommandResult (const CommandArgs&)> run;
    std::function<Availability()> available;            // optional; default always available
    std::function<std::optional<bool>()> toggleState;   // optional; present means this is a toggle
    juce::String onWord = "on", offWord = "off";        // "Metronome on", "Track 3 armed"
    bool sayNameWithState = true;                      // false: just "Playing" / "Stopped"

    bool isToggle() const   { return toggleState != nullptr; }
};

struct MacroStep
{
    juce::String commandId;
    CommandArgs args;
};

class CommandList
{
public:
    explicit CommandList (CommandHost&);

    // Registering. A duplicate or malformed id is refused, with the reason.
    juce::Result add (Command);
    juce::Result addMacro (juce::String id, juce::String name, juce::String description,
                           std::vector<MacroStep> steps, juce::StringArray tags = {});

    const Command* find (const juce::String& id) const;
    juce::StringArray allIds() const;
    int size() const                                    { return (int) commands.size(); }

    // Search by name, tags, category and description, best match first.
    // Every word typed must match somewhere.
    std::vector<const Command*> search (const juce::String& query, int maxResults = 0) const;

    enum class Source { key, menu, palette, surface, midi, script, speech, macro, repeat, test };

    // Run a command. Checks it's available, fills in default arguments, checks
    // them against their ranges, runs it, then speaks: a toggle's new state
    // first, then whatever the command said.
    CommandResult run (const juce::String& id, const CommandArgs& args = {}, Source source = Source::key);

    // Repeat the last command that changed something, with the same arguments.
    CommandResult repeatLast();

    // Describe a command without running it: its name, its state if a toggle,
    // whether it can run now, and what it does.
    juce::String describe (const juce::String& id) const;

    // Speaking is held while a macro runs its middle steps.
    bool isSpeechHeld() const                           { return holdSpeech > 0; }

private:
    CommandResult runInternal (const Command&, const CommandArgs&, Source);
    juce::Result checkArgs (const Command&, const CommandArgs& given, CommandArgs& filled) const;
    void say (const juce::String&, bool urgent);

    CommandHost& host;
    std::map<juce::String, Command> commands;
    std::optional<MacroStep> lastRepeatable;
    int holdSpeech = 0, macroDepth = 0;
};

juce::String describeArgValue (const ArgSpec&, const juce::var&);

} // namespace earfirst
