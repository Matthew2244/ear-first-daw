#pragma once
// Keymap: which keys run which commands, and where.
//
//  - Keys are written one way on both systems: "Primary+Shift+K". Primary is
//    Command on the Mac and Control on Windows; Secondary is Control on the
//    Mac and the Windows key on Windows; Alt is Option on the Mac. So one
//    keymap serves both, and each system reads it in its own words.
//  - A key can be a sequence: "Primary+K, M".
//  - Scopes say where a key works ("tracks", "mixer"). A key not used in the
//    focused scope passes through to its parent, up to "global".
//  - Shipped defaults and your own changes are stored apart. Your file holds
//    only what you changed, so a new version can never move your keys. If a
//    shipped default moves, you're told once, old key and new.
//  - Keys the system or a screen reader already uses are known, and binding
//    one gets a warning that says who uses it.

#include "commands/Commands.h"

namespace earfirst
{

enum class Platform { mac, windows };
Platform currentPlatform();

struct KeyChord
{
    enum Mods { primary = 1, shift = 2, alt = 4, secondary = 8 };
    int mods = 0;
    juce::String key;           // "K", "5", "F12", "Space", "Keypad5", "[", "Comma" (a comma separates sequences), ...

    bool operator== (const KeyChord& o) const   { return mods == o.mods && key == o.key; }
    bool isValid() const                        { return key.isNotEmpty(); }
};

using KeySequence = std::vector<KeyChord>;

// Parsing and writing. Parsing accepts Mac and Windows spellings too
// ("Command-Shift-K", "Ctrl+Shift+K") and returns an empty sequence if it can't.
KeySequence parseKeys (const juce::String&, Platform = currentPlatform());
juce::String keysToText (const KeySequence&);                         // canonical: "Primary+Shift+K"
juce::String keysForPeople (const KeySequence&, Platform = currentPlatform());   // "Command-Shift-K" / "Control+Shift+K"

struct KeyBinding
{
    juce::String keys;          // canonical text
    juce::String commandId;
    juce::String scope = "global";
    CommandArgs args;           // preset settings: Alt+Right = nudge by one beat

    juce::String identity() const;   // what makes two bindings the same binding
};

class Keymap
{
public:
    explicit Keymap (const CommandList&);

    // Scopes form a chain toward "global": addScope ("tracks", "main").
    void addScope (const juce::String& scope, const juce::String& parent);
    juce::StringArray scopeChain (const juce::String& scope) const;

    // The shipped defaults, with a version number that goes up whenever they change.
    void setDefaults (std::vector<KeyBinding>, int version);

    // Your changes.
    juce::Result bind (const juce::String& keys, const juce::String& commandId,
                       const juce::String& scope = "global", const CommandArgs& args = {});
    juce::Result unbind (const juce::String& keys, const juce::String& scope = "global");
    void resetToDefaults();

    // What runs. Looks in the focused scope, then its parents.
    std::optional<KeyBinding> lookup (const KeySequence&, const juce::String& focusedScope) const;
    // True if these keys start a longer sequence here, so wait for the next key.
    bool isPrefix (const KeySequence&, const juce::String& focusedScope) const;

    // "What does this key do?" without running it.
    juce::String describeKey (const juce::String& keys, const juce::String& focusedScope) const;
    // The keys for a command, for menus and the command palette.
    juce::StringArray keysFor (const juce::String& commandId) const;

    std::vector<KeyBinding> effective() const;

    struct Conflict { juce::String scope, keys; juce::StringArray commandIds; juce::String sentence; };
    std::vector<Conflict> conflicts() const;

    // Who already uses these keys on this system, or empty if nobody.
    static juce::String reservedBy (const KeySequence&, Platform);

    // Your changes as a file, and back. Loading also returns any sentences
    // about shipped defaults that moved since you last opened the app.
    juce::var userChangesToJson() const;
    juce::Result loadUserChanges (const juce::var&, juce::StringArray& movedNotices);

    // A readable list of every key, by category, in this system's words.
    juce::String exportReadable (Platform = currentPlatform()) const;

private:
    const CommandList& commands;
    std::map<juce::String, juce::String> parents;
    std::vector<KeyBinding> defaults, added;
    juce::StringArray removed;                    // identities of defaults you removed
    int defaultsVersion = 0;
    std::map<juce::String, juce::String> seenDefaults;   // command|scope|args -> keys, as last seen
};

} // namespace earfirst
