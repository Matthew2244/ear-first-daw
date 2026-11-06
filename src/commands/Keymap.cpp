#include "commands/Keymap.h"

namespace earfirst
{

Platform currentPlatform()
{
   #if JUCE_MAC
    return Platform::mac;
   #else
    return Platform::windows;
   #endif
}

namespace
{
juce::String canonicalKeyName (juce::String k)
{
    k = k.trim();
    if (k.length() == 1) return k == "," ? juce::String ("Comma") : k.toUpperCase();
    static const std::pair<const char*, const char*> words[] = { { "Comma", "Comma" }, { "Period", "." }, { "Slash", "/" },
        { "Semicolon", ";" }, { "Minus", "-" }, { "Equals", "=" }, { "LeftBracket", "[" }, { "RightBracket", "]" },
        { "Backslash", "\\" }, { "Apostrophe", "'" }, { "Grave", "`" } };
    for (auto& [w, key] : words)
        if (k.removeCharacters (" ").equalsIgnoreCase (w)) return key;
    static const char* named[] = { "Space", "Return", "Enter", "Escape", "Tab", "Delete", "Backspace", "Left", "Right", "Up", "Down",
                                   "Home", "End", "PageUp", "PageDown", "Insert", "CapsLock", "Keypad0", "Keypad1", "Keypad2",
                                   "Keypad3", "Keypad4", "Keypad5", "Keypad6", "Keypad7", "Keypad8", "Keypad9", "KeypadPlus",
                                   "KeypadMinus", "KeypadMultiply", "KeypadDivide", "KeypadDecimal", "KeypadEnter", "KeypadEquals",
                                   "Clear" };
    auto squashed = k.removeCharacters (" ");
    for (auto* n : named)
        if (squashed.equalsIgnoreCase (n)) return n;
    if (squashed.equalsIgnoreCase ("Esc")) return "Escape";
    if (squashed.equalsIgnoreCase ("Del")) return "Delete";
    if ((squashed.startsWithIgnoreCase ("F")) && squashed.substring (1).containsOnly ("0123456789"))
    {
        auto n = squashed.substring (1).getIntValue();
        if (n >= 1 && n <= 24) return "F" + juce::String (n);
    }
    return {};
}

int modifierFromWord (const juce::String& w, Platform p)
{
    auto s = w.trim().toLowerCase();
    if (s == "primary") return KeyChord::primary;
    if (s == "secondary") return KeyChord::secondary;
    if (s == "shift") return KeyChord::shift;
    if (s == "alt" || s == "option" || s == "opt") return KeyChord::alt;
    if (s == "command" || s == "cmd") return KeyChord::primary;
    if (s == "win" || s == "windows" || s == "meta") return KeyChord::secondary;
    if (s == "control" || s == "ctrl") return p == Platform::mac ? KeyChord::secondary : KeyChord::primary;
    return -1;
}

KeyChord parseChord (const juce::String& text, Platform p)
{
    // Split on + or -, but a lone "+" or "-" (or a trailing one) is the key itself.
    juce::StringArray parts;
    juce::String cur;
    for (int i = 0; i < text.length(); ++i)
    {
        auto ch = text[i];
        if ((ch == '+' || ch == '-') && cur.trim().isNotEmpty() && i < text.length() - 1)
        {
            parts.add (cur.trim());
            cur.clear();
        }
        else
            cur += juce::String::charToString (ch);
    }
    if (cur.trim().isNotEmpty() || cur == " ") parts.add (cur.trim().isEmpty() ? juce::String ("Space") : cur.trim());

    KeyChord c;
    for (int i = 0; i < parts.size(); ++i)
    {
        if (i < parts.size() - 1)
        {
            auto m = modifierFromWord (parts[i], p);
            if (m < 0) return {};
            c.mods |= m;
        }
        else
        {
            c.key = canonicalKeyName (parts[i]);
        }
    }
    return c;
}

juce::String keyForPeople (const juce::String& k)
{
    if (k.startsWith ("Keypad"))
    {
        auto rest = k.substring (6);
        if (rest == "Plus") rest = "plus"; else if (rest == "Minus") rest = "minus";
        else if (rest == "Multiply") rest = "times"; else if (rest == "Divide") rest = "slash";
        else if (rest == "Decimal") rest = "period"; else if (rest == "Enter") rest = "Enter";
        else if (rest == "Equals") rest = "equals";
        return "keypad " + rest;
    }
    if (k == "PageUp") return "Page Up";
    if (k == "PageDown") return "Page Down";
    if (k == "CapsLock") return "Caps Lock";
    if (k == "Comma") return "comma";
    static const std::pair<const char*, const char*> punct[] = { { "[", "left bracket" }, { "]", "right bracket" }, { ",", "comma" },
        { ".", "period" }, { "/", "slash" }, { "\\", "backslash" }, { ";", "semicolon" }, { "'", "apostrophe" }, { "`", "grave" },
        { "=", "equals" }, { "-", "minus" } };
    for (auto& [c, word] : punct)
        if (k == c) return word;
    return k;
}
} // namespace

KeySequence parseKeys (const juce::String& text, Platform p)
{
    KeySequence seq;
    for (auto& part : juce::StringArray::fromTokens (text, ",", ""))
    {
        if (part.trim().isEmpty()) continue;
        auto c = parseChord (part.trim(), p);
        if (! c.isValid()) return {};
        seq.push_back (c);
    }
    // "Primary+," style: a comma key is written as the word.
    return seq;
}

juce::String keysToText (const KeySequence& seq)
{
    juce::StringArray chords;
    for (auto& c : seq)
    {
        juce::StringArray p;
        if (c.mods & KeyChord::primary) p.add ("Primary");
        if (c.mods & KeyChord::secondary) p.add ("Secondary");
        if (c.mods & KeyChord::alt) p.add ("Alt");
        if (c.mods & KeyChord::shift) p.add ("Shift");
        p.add (c.key);
        chords.add (p.joinIntoString ("+"));
    }
    return chords.joinIntoString (", ");
}

juce::String keysForPeople (const KeySequence& seq, Platform plat)
{
    const bool mac = plat == Platform::mac;
    juce::StringArray chords;
    for (auto& c : seq)
    {
        // Each system's own order: Mac Control-Option-Shift-Command, Windows Win+Ctrl+Alt+Shift.
        juce::StringArray p;
        if (mac)
        {
            if (c.mods & KeyChord::secondary) p.add ("Control");
            if (c.mods & KeyChord::alt) p.add ("Option");
            if (c.mods & KeyChord::shift) p.add ("Shift");
            if (c.mods & KeyChord::primary) p.add ("Command");
        }
        else
        {
            if (c.mods & KeyChord::secondary) p.add ("Windows");
            if (c.mods & KeyChord::primary) p.add ("Control");
            if (c.mods & KeyChord::alt) p.add ("Alt");
            if (c.mods & KeyChord::shift) p.add ("Shift");
        }
        p.add (keyForPeople (c.key));
        chords.add (p.joinIntoString (mac ? "-" : "+"));
    }
    return chords.joinIntoString (mac ? ", then " : ", then ");
}

juce::String KeyBinding::identity() const
{
    juce::StringArray a;
    for (int i = 0; i < args.size(); ++i)
        a.add (args.getName (i).toString() + "=" + args.getValueAt (i).toString());
    a.sort (false);
    return commandId + "|" + scope + "|" + a.joinIntoString (";");
}

Keymap::Keymap (const CommandList& c) : commands (c) {}

void Keymap::addScope (const juce::String& scope, const juce::String& parent, const juce::String& words)
{
    parents[scope] = parent;
    scopeNames[scope] = words == "?" ? "in " + scope : words;
}

juce::String Keymap::scopeWords (const juce::String& scope) const
{
    if (scope == "global") return {};
    auto it = scopeNames.find (scope);
    return it != scopeNames.end() ? it->second : "in " + scope;
}

juce::StringArray Keymap::scopeChain (const juce::String& scope) const
{
    juce::StringArray chain;
    auto s = scope.isEmpty() ? juce::String ("global") : scope;
    while (s.isNotEmpty() && ! chain.contains (s))
    {
        chain.add (s);
        if (s == "global") break;
        auto it = parents.find (s);
        s = it != parents.end() ? it->second : juce::String ("global");
    }
    if (! chain.contains ("global")) chain.add ("global");
    return chain;
}

void Keymap::setDefaults (std::vector<KeyBinding> d, int version)
{
    for (auto& b : d) b.keys = keysToText (parseKeys (b.keys));
    defaults = std::move (d);
    defaultsVersion = version;
}

void Keymap::addDefaults (std::vector<KeyBinding> more)
{
    for (auto& b : more)
    {
        b.keys = keysToText (parseKeys (b.keys));
        defaults.push_back (b);
    }
}

std::vector<KeyBinding> Keymap::effective() const
{
    std::vector<KeyBinding> out;
    for (auto& b : defaults)
        if (! removed.contains (b.keys + "@" + b.scope + "@" + b.identity()))
            out.push_back (b);
    // Your keys win: a key you bound replaces the default on that key in that scope.
    for (auto& a : added)
    {
        out.erase (std::remove_if (out.begin(), out.end(), [&] (auto& b) { return b.keys == a.keys && b.scope == a.scope; }), out.end());
        out.push_back (a);
    }
    return out;
}

juce::Result Keymap::bind (const juce::String& keys, const juce::String& commandId, const juce::String& scope, const CommandArgs& args)
{
    auto seq = parseKeys (keys);
    if (seq.empty()) return juce::Result::fail ("I couldn't read the keys \"" + keys + "\".");
    if (commands.find (commandId) == nullptr) return juce::Result::fail ("There's no command called " + commandId + ".");
    KeyBinding b { keysToText (seq), commandId, scope, args };
    added.erase (std::remove_if (added.begin(), added.end(), [&] (auto& a) { return a.keys == b.keys && a.scope == b.scope; }), added.end());
    added.push_back (b);
    return juce::Result::ok();
}

juce::Result Keymap::unbind (const juce::String& keys, const juce::String& scope)
{
    auto text = keysToText (parseKeys (keys));
    bool any = false;
    auto before = added.size();
    added.erase (std::remove_if (added.begin(), added.end(), [&] (auto& a) { return a.keys == text && a.scope == scope; }), added.end());
    any = added.size() != before;
    for (auto& d : defaults)
        if (d.keys == text && d.scope == scope)
        {
            removed.addIfNotAlreadyThere (d.keys + "@" + d.scope + "@" + d.identity());
            any = true;
        }
    return any ? juce::Result::ok() : juce::Result::fail (keysForPeople (parseKeys (keys)) + " isn't used there.");
}

void Keymap::resetToDefaults()
{
    added.clear();
    removed.clear();
}

std::optional<KeyBinding> Keymap::lookup (const KeySequence& seq, const juce::String& focusedScope) const
{
    auto all = effective();
    auto text = keysToText (seq);
    for (auto& s : scopeChain (focusedScope))
        for (auto& b : all)
            if (b.scope == s && b.keys == text)
                return b;
    return std::nullopt;
}

bool Keymap::isPrefix (const KeySequence& seq, const juce::String& focusedScope) const
{
    auto text = keysToText (seq) + ", ";
    auto chain = scopeChain (focusedScope);
    for (auto& b : effective())
        if (chain.contains (b.scope) && b.keys.startsWith (text))
            return true;
    return false;
}

juce::String Keymap::describeKey (const juce::String& keys, const juce::String& focusedScope) const
{
    auto seq = parseKeys (keys);
    if (seq.empty()) return "I couldn't read the keys \"" + keys + "\".";
    auto people = keysForPeople (seq);
    if (auto b = lookup (seq, focusedScope))
    {
        auto* c = commands.find (b->commandId);
        if (b->args.size() == 0 || c == nullptr)
            return people + ": " + commands.describe (b->commandId);
        // This key sets its own values, so say those, not the defaults.
        juce::StringArray parts;
        for (auto& a : c->args)
            parts.add (a.name + " " + describeArgValue (a, b->args.contains (a.name) ? b->args[a.name] : a.defaultValue));
        return people + ": " + c->name + ", " + parts.joinIntoString (", ") + ". " + c->description;
    }
    if (isPrefix (seq, focusedScope))
        return people + " starts a longer key. Press the next key to hear what it does.";
    auto reserved = reservedBy (seq, currentPlatform());
    return people + " does nothing here." + (reserved.isNotEmpty() ? " " + reserved : juce::String());
}

juce::StringArray Keymap::keysFor (const juce::String& commandId) const
{
    juce::StringArray out;
    for (auto& b : effective())
        if (b.commandId == commandId)
        {
            auto where = scopeWords (b.scope);
            out.add (keysForPeople (parseKeys (b.keys)) + (where.isNotEmpty() ? " " + where : juce::String()));
        }
    return out;
}

std::vector<Keymap::Conflict> Keymap::conflicts() const
{
    std::vector<Conflict> out;
    auto all = effective();
    for (size_t i = 0; i < all.size(); ++i)
        for (size_t j = i + 1; j < all.size(); ++j)
        {
            auto& a = all[i];
            auto& b = all[j];
            if (a.scope != b.scope) continue;
            const bool same = a.keys == b.keys;
            const bool prefix = b.keys.startsWith (a.keys + ", ") || a.keys.startsWith (b.keys + ", ");
            if (! same && ! prefix) continue;
            auto* ca = commands.find (a.commandId);
            auto* cb = commands.find (b.commandId);
            auto an = ca ? ca->name : a.commandId, bn = cb ? cb->name : b.commandId;
            auto shorter = a.keys.length() <= b.keys.length() ? a : b;
            Conflict c { a.scope, shorter.keys, { a.commandId, b.commandId }, {} };
            const auto words = scopeWords (a.scope);
            const auto where = words.isEmpty() ? juce::String ("everywhere") : words;
            c.sentence = same ? keysForPeople (parseKeys (a.keys)) + " runs both " + an + " and " + bn + " " + where + "."
                              : keysForPeople (parseKeys (shorter.keys)) + " runs " + (shorter.commandId == a.commandId ? an : bn)
                                    + " " + where + ", so the longer key that starts with it can never be reached.";
            out.push_back (c);
        }
    return out;
}

juce::String Keymap::reservedBy (const KeySequence& seq, Platform p)
{
    if (seq.empty()) return {};
    auto& c = seq.front();
    const auto m = c.mods;
    using K = KeyChord;
    if (p == Platform::mac)
    {
        if ((m & K::secondary) && (m & K::alt))
            return "VoiceOver uses Control-Option with any key.";
        if (c.key == "CapsLock")
            return "VoiceOver can use Caps Lock as its key.";
        if (m == K::primary && c.key == "Space") return "macOS uses Command-Space for Spotlight.";
        if ((m & K::primary) && c.key == "Tab") return "macOS uses Command-Tab to switch apps.";
        if (m == (K::primary | K::shift) && (c.key == "3" || c.key == "4" || c.key == "5")) return "macOS uses it for screenshots.";
        if (m == K::secondary && (c.key == "Left" || c.key == "Right" || c.key == "Up" || c.key == "Down"))
            return "macOS uses Control with the arrows for Mission Control and spaces.";
        if (m == (K::primary | K::alt) && c.key == "Escape") return "macOS uses it for Force Quit.";
        if (m == K::secondary && c.key == "F2") return "macOS uses Control-F2 to reach the menu bar.";
    }
    else
    {
        if (c.key == "Insert" || c.key == "CapsLock")
            return "NVDA and JAWS use " + juce::String (c.key == "Insert" ? "Insert" : "Caps Lock") + " as their key.";
        if ((m & K::primary) && (m & K::alt))
            return "On many keyboards Control+Alt is AltGr, which types letters, so keys with it can fire while typing.";
        if (m & K::secondary)
            return "Windows uses the Windows key for its own shortcuts.";
        if (m == K::alt && c.key == "F4") return "Windows uses Alt+F4 to close windows.";
        if (m == K::primary && c.key == "Escape") return "Windows uses Control+Escape for the Start menu.";
        if (m == 0 && c.key.startsWith ("Keypad") && c.key != "KeypadEnter")
            return "NVDA uses the keypad for reading when Num Lock is off.";
    }
    return {};
}

juce::var Keymap::userChangesToJson() const
{
    auto* root = new juce::DynamicObject();
    root->setProperty ("format", "ear-first-keymap");
    root->setProperty ("version", 1);
    root->setProperty ("defaultsVersion", defaultsVersion);

    auto bindingToVar = [] (const KeyBinding& b)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("keys", b.keys);
        o->setProperty ("command", b.commandId);
        o->setProperty ("scope", b.scope);
        if (b.args.size() > 0)
        {
            auto* a = new juce::DynamicObject();
            for (int i = 0; i < b.args.size(); ++i) a->setProperty (b.args.getName (i), b.args.getValueAt (i));
            o->setProperty ("args", juce::var (a));
        }
        return juce::var (o);
    };

    juce::Array<juce::var> add, rem;
    for (auto& b : added) add.add (bindingToVar (b));
    for (auto& r : removed) rem.add (r);
    root->setProperty ("added", add);
    root->setProperty ("removed", rem);

    // What the defaults looked like when you last used the app, so a moved
    // default can be announced once.
    auto* seen = new juce::DynamicObject();
    for (auto& d : defaults) seen->setProperty (d.identity(), d.keys);
    root->setProperty ("seenDefaults", juce::var (seen));
    return juce::var (root);
}

juce::Result Keymap::loadUserChanges (const juce::var& v, juce::StringArray& moved)
{
    if (v["format"].toString() != "ear-first-keymap")
        return juce::Result::fail ("That isn't an Ear First keymap file.");

    added.clear();
    removed.clear();
    juce::StringArray problems;
    if (auto* arr = v["added"].getArray())
        for (auto& b : *arr)
        {
            CommandArgs args;
            if (auto* o = b["args"].getDynamicObject())
                for (auto& p : o->getProperties()) args.set (p.name, p.value);
            auto r = bind (b["keys"].toString(), b["command"].toString(), b["scope"].toString().isEmpty() ? "global" : b["scope"].toString(), args);
            if (r.failed()) problems.add (r.getErrorMessage());
        }
    if (auto* arr = v["removed"].getArray())
        for (auto& r : *arr) removed.add (r.toString());

    // Shipped defaults that moved since last time, unless you'd changed that key yourself.
    if (auto* seen = v["seenDefaults"].getDynamicObject())
        for (auto& d : defaults)
        {
            auto id = d.identity();
            if (! seen->hasProperty (id)) continue;
            auto before = seen->getProperty (id).toString();
            if (before == d.keys) continue;
            bool yours = false;
            for (auto& a : added) yours |= a.identity() == id;
            if (yours) continue;
            auto* c = commands.find (d.commandId);
            moved.add ((c ? c->name : d.commandId) + " moved from " + keysForPeople (parseKeys (before)) + " to "
                       + keysForPeople (parseKeys (d.keys)) + ".");
        }

    if (! problems.isEmpty())
        return juce::Result::fail ("Some of your keys couldn't be loaded: " + problems.joinIntoString (" "));
    return juce::Result::ok();
}

juce::String Keymap::exportReadable (Platform p) const
{
    std::map<juce::String, juce::StringArray> byCategory;
    for (auto& b : effective())
    {
        auto* c = commands.find (b.commandId);
        auto line = (c ? c->name : b.commandId) + ": " + keysForPeople (parseKeys (b.keys), p)
                    + (scopeWords (b.scope).isNotEmpty() ? " (" + scopeWords (b.scope) + ")" : juce::String());
        byCategory[c ? c->category : juce::String ("Other")].add (line);
    }
    juce::String out;
    for (auto& [cat, lines] : byCategory)
    {
        lines.sort (true);
        out << cat << "\n" << lines.joinIntoString ("\n") << "\n\n";
    }
    return out.trimEnd();
}

} // namespace earfirst
