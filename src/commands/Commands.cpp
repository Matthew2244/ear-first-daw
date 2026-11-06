#include "commands/Commands.h"

namespace earfirst
{

namespace
{
bool validId (const juce::String& id)
{
    if (id.isEmpty() || id.startsWithChar ('.') || id.endsWithChar ('.') || id.contains (".."))
        return false;
    return id.containsOnly ("abcdefghijklmnopqrstuvwxyz0123456789._-") && id.containsChar ('.');
}

juce::String capitalised (const juce::String& w)
{
    return w.substring (0, 1).toUpperCase() + w.substring (1);
}

juce::String numberText (double v)
{
    auto r = std::round (v * 1000.0) / 1000.0;
    return r == std::floor (r) ? juce::String ((juce::int64) r) : juce::String (r, 3).trimCharactersAtEnd ("0");
}
} // namespace

juce::String describeArgValue (const ArgSpec& a, const juce::var& v)
{
    juce::String s;
    switch (a.type)
    {
        case ArgSpec::Type::boolean: s = (bool) v ? "yes" : "no"; break;
        case ArgSpec::Type::number:
        case ArgSpec::Type::integer: s = numberText ((double) v); break;
        default: s = v.toString(); break;
    }
    if (a.unit.isEmpty()) return s;
    // "1 beat", "2 beats"
    auto unit = a.unit;
    if (s == "1" && unit.endsWithChar ('s')) unit = unit.dropLastCharacters (1);
    return s + " " + unit;
}

CommandList::CommandList (CommandHost& h) : host (h) {}

juce::Result CommandList::add (Command c)
{
    if (! validId (c.id))
        return juce::Result::fail ("The command id \"" + c.id + "\" isn't valid. Ids are lowercase words joined by dots, like transport.play.");
    if (commands.count (c.id) > 0)
        return juce::Result::fail ("There's already a command called " + c.id + ".");
    if (c.name.isEmpty() || c.run == nullptr)
        return juce::Result::fail ("The command " + c.id + " needs a name and something to run.");
    for (auto& a : c.args)
        if (a.type == ArgSpec::Type::choice && ! a.choices.contains (a.defaultValue.toString()))
            return juce::Result::fail ("The default for " + a.name + " in " + c.id + " isn't one of its choices.");
    commands.emplace (c.id, std::move (c));
    return juce::Result::ok();
}

juce::Result CommandList::addMacro (juce::String id, juce::String name, juce::String description,
                                    std::vector<MacroStep> steps, juce::StringArray tags)
{
    for (auto& s : steps)
        if (find (s.commandId) == nullptr)
            return juce::Result::fail ("The macro " + name + " uses " + s.commandId + ", which doesn't exist.");
    if (steps.empty())
        return juce::Result::fail ("The macro " + name + " has no steps.");

    Command c;
    c.id = std::move (id);
    c.name = name;
    c.category = "Macros";
    c.description = std::move (description);
    c.tags = std::move (tags);
    c.undoable = true;
    c.run = [this, steps, name] (const CommandArgs&)
    {
        // Middle steps stay quiet; the macro speaks once, at the end.
        ++holdSpeech;
        CommandResult last;
        for (size_t i = 0; i < steps.size(); ++i)
        {
            last = run (steps[i].commandId, steps[i].args, Source::macro);
            if (! last.ok)
            {
                --holdSpeech;
                auto* failed = find (steps[i].commandId);
                return CommandResult::failed (name + " stopped at step " + juce::String ((int) i + 1) + ", "
                                              + (failed != nullptr ? failed->name : steps[i].commandId) + ". " + last.spoken);
            }
        }
        --holdSpeech;
        return CommandResult::done (last.spoken.isNotEmpty() ? last.spoken : name + " done.");
    };
    return add (std::move (c));
}

const Command* CommandList::find (const juce::String& id) const
{
    auto it = commands.find (id);
    return it == commands.end() ? nullptr : &it->second;
}

juce::StringArray CommandList::allIds() const
{
    juce::StringArray ids;
    for (auto& [id, c] : commands) ids.add (id);
    return ids;
}

std::vector<const Command*> CommandList::search (const juce::String& query, int maxResults) const
{
    auto words = juce::StringArray::fromTokens (query.toLowerCase(), " ,", "\"");
    words.removeEmptyStrings();
    std::vector<std::pair<int, const Command*>> scored;

    for (auto& [id, c] : commands)
    {
        auto name = c.name.toLowerCase();
        auto nameWords = juce::StringArray::fromTokens (name, " -/", "");
        auto tags = c.tags.joinIntoString (" ").toLowerCase();
        auto cat = c.category.toLowerCase(), desc = c.description.toLowerCase();
        int score = 0;
        bool all = true;
        for (auto& w : words)
        {
            int best = 0;
            for (auto& nw : nameWords)
                if (nw.startsWith (w)) best = std::max (best, nw == w ? 120 : 100);
            if (best == 0 && name.contains (w)) best = 60;
            if (tags.contains (w)) best = std::max (best, 50);
            if (cat.contains (w)) best = std::max (best, 25);
            if (id.contains (w)) best = std::max (best, 15);
            if (desc.contains (w)) best = std::max (best, 10);
            if (best == 0) { all = false; break; }
            score += best;
        }
        if (all && ! words.isEmpty())
            scored.push_back ({ score, &c });
    }

    std::sort (scored.begin(), scored.end(), [] (auto& a, auto& b)
    {
        if (a.first != b.first) return a.first > b.first;
        if (a.second->name.length() != b.second->name.length()) return a.second->name.length() < b.second->name.length();
        return a.second->name < b.second->name;
    });

    std::vector<const Command*> out;
    for (auto& s : scored)
    {
        if (maxResults > 0 && (int) out.size() >= maxResults) break;
        out.push_back (s.second);
    }
    return out;
}

juce::Result CommandList::checkArgs (const Command& c, const CommandArgs& given, CommandArgs& filled) const
{
    for (int i = 0; i < given.size(); ++i)
    {
        auto n = given.getName (i).toString();
        bool known = false;
        for (auto& a : c.args) known |= a.name == n;
        if (! known)
            return juce::Result::fail (c.name + " doesn't take a setting called " + n + ".");
    }

    for (auto& a : c.args)
    {
        auto v = given.contains (a.name) ? given[a.name] : a.defaultValue;
        switch (a.type)
        {
            case ArgSpec::Type::number:
            case ArgSpec::Type::integer:
            {
                bool numeric = v.isDouble() || v.isInt() || v.isInt64()
                               || (v.isString() && v.toString().trim().containsOnly ("-+.0123456789") && v.toString().isNotEmpty());
                if (! numeric)
                    return juce::Result::fail (capitalised (a.name) + " for " + c.name + " needs to be a number.");
                double d = v.isString() ? v.toString().getDoubleValue() : (double) v;
                if (a.type == ArgSpec::Type::integer && d != std::floor (d))
                    return juce::Result::fail (capitalised (a.name) + " for " + c.name + " needs to be a whole number.");
                if (d < a.minimum || d > a.maximum)
                    return juce::Result::fail (capitalised (a.name) + " for " + c.name + " has to be between "
                                               + describeArgValue (a, a.minimum) + " and " + describeArgValue (a, a.maximum) + ".");
                v = a.type == ArgSpec::Type::integer ? juce::var ((juce::int64) d) : juce::var (d);
                break;
            }
            case ArgSpec::Type::boolean:
                v = v.isString() ? juce::var (v.toString().equalsIgnoreCase ("yes") || v.toString() == "1" || v.toString().equalsIgnoreCase ("true"))
                                 : juce::var ((bool) v);
                break;
            case ArgSpec::Type::choice:
                if (! a.choices.contains (v.toString()))
                    return juce::Result::fail (capitalised (a.name) + " for " + c.name + " has to be one of: " + a.choices.joinIntoString (", ") + ".");
                break;
            case ArgSpec::Type::text:
                v = v.toString();
                break;
        }
        filled.set (a.name, v);
    }
    return juce::Result::ok();
}

void CommandList::say (const juce::String& s, bool urgent)
{
    if (s.isNotEmpty() && holdSpeech == 0)   // a macro speaks once, at the end, failures included
        host.speak (s, urgent);
}

CommandResult CommandList::runInternal (const Command& c, const CommandArgs& args, Source source)
{
    if (c.available != nullptr)
        if (auto a = c.available(); ! a.enabled)
            return CommandResult::failed (a.reason.isNotEmpty() ? a.reason : c.name + " isn't available right now.");

    CommandArgs filled;
    if (auto r = checkArgs (c, args, filled); r.failed())
        return CommandResult::failed (r.getErrorMessage());

    const bool wrapUndo = c.undoable && macroDepth == 0;
    if (wrapUndo) host.beginUndoStep (c.name);
    const bool isMacro = c.category == "Macros";
    if (isMacro) ++macroDepth;

    CommandResult r;
    try { r = c.run (filled); }
    catch (const std::exception& e) { r = CommandResult::failed (c.name + " ran into a problem: " + juce::String (e.what())); }

    if (isMacro) --macroDepth;
    if (wrapUndo) host.endUndoStep();

    // A toggle says its new state, new state first, whatever the command said.
    if (r.ok && c.isToggle())
        if (auto state = c.toggleState())
            r.spoken = (c.sayNameWithState ? c.name + " " : juce::String()) + (*state ? c.onWord : c.offWord)
                       + (r.spoken.isNotEmpty() ? ". " + r.spoken : juce::String());

    if (r.ok && ! c.isReport && source != Source::repeat && source != Source::macro)
        lastRepeatable = MacroStep { c.id, filled };
    return r;
}

CommandResult CommandList::run (const juce::String& id, const CommandArgs& args, Source source)
{
    auto* c = find (id);
    if (c == nullptr)
    {
        auto r = CommandResult::failed ("There's no command called " + id + ".");
        say (r.spoken, true);
        return r;
    }
    auto r = runInternal (*c, args, source);
    say (r.spoken, r.urgent);
    return r;
}

CommandResult CommandList::repeatLast()
{
    if (! lastRepeatable.has_value())
    {
        auto r = CommandResult::failed ("There's nothing to repeat yet.");
        say (r.spoken, true);
        return r;
    }
    auto step = *lastRepeatable;
    return run (step.commandId, step.args, Source::repeat);
}

juce::String CommandList::describe (const juce::String& id) const
{
    auto* c = find (id);
    if (c == nullptr) return "There's no command called " + id + ".";

    juce::String s = c->name;
    if (c->isToggle())
        if (auto st = c->toggleState())
            s << ", " << (*st ? c->onWord : c->offWord);
    if (c->available != nullptr)
        if (auto a = c->available(); ! a.enabled)
            s << ", not available now: " << (a.reason.isNotEmpty() ? a.reason : juce::String ("no reason given"));
    s << ". " << c->description;
    if (! c->args.empty())
    {
        juce::StringArray parts;
        for (auto& a : c->args)
            parts.add (a.name + ", normally " + describeArgValue (a, a.defaultValue));
        s << " Settings: " << parts.joinIntoString ("; ") << ".";
    }
    return s;
}

} // namespace earfirst
