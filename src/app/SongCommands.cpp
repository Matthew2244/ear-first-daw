// Commands for moving around a song, its markers, and the basics of a track.
// Part of AppCore; it's in its own file because registerCommands is long enough.

#include "app/AppCore.h"
#include "session/Tracks.h"

using namespace tracktion;

namespace earfirst
{

namespace
{
constexpr double nearEnough = 1.0e-6;

juce::String panWords (double pan)
{
    const int percent = juce::roundToInt (std::abs (pan) * 100.0);
    if (percent == 0) return "centre";
    return juce::String (percent) + (pan < 0 ? " left" : " right");
}

juce::String dbWords (double db)
{
    const auto v = std::abs (db) < 0.05 ? juce::String ("0") : juce::String (db, 1).trimCharactersAtEnd (".0").isEmpty() ? juce::String ("0")
                 : juce::String (db, 1).endsWith (".0") ? juce::String (juce::roundToInt (db)) : juce::String (db, 1);
    return v + " dB";
}
} // namespace

tracktion::AudioTrack* AppCore::selectedAudioTrack() const
{
    if (song == nullptr) return nullptr;
    auto tracks = contentTracks (song->getEdit());
    return tracks.isEmpty() ? nullptr : tracks[juce::jlimit (0, tracks.size() - 1, selectedTrack)];
}

juce::String AppCore::trackSentence (tracktion::AudioTrack& t) const
{
    auto* vol = t.getVolumePlugin();
    const auto clips = t.getClips().size();
    juce::String s = t.getName() + ". ";
    s << (clips == 0 ? juce::String ("Empty") : juce::String (clips) + (clips == 1 ? " clip" : " clips")) << ". ";
    if (vol != nullptr) s << "Volume " << dbWords (vol->getVolumeDb()) << ", pan " << panWords (vol->getPan()) << ". ";
    if (t.isMuted (false)) s << "Muted. ";
    if (t.isSolo (false)) s << "Soloed. ";
    return s.trimEnd();
}

void AppCore::registerSongCommands()
{
    auto add = [this] (Command c)
    {
        auto r = commandList.add (std::move (c));
        jassert (r.wasOk());
        juce::ignoreUnused (r);
    };
    auto needSong = [this] { return song == nullptr ? Availability::no ("No song is open. Press Command-N for a new one.") : Availability::yes(); };
    auto needTrack = [this, needSong]
    {
        if (auto ok = needSong(); ! ok.enabled) return ok;
        return selectedAudioTrack() == nullptr ? Availability::no ("There are no tracks.") : Availability::yes();
    };
    auto songChanged = [this] { if (onSongChanged) onSongChanged(); };
    auto arrive = [this] (TimePosition t)
    {
        song->getEdit().getTransport().setPosition (std::max (TimePosition(), t));
        return CommandResult::done (positionSentence());
    };

    //==========================================================================
    // Moving around
    auto stepBar = [this, arrive] (int direction)
    {
        auto& tempo = song->getEdit().tempoSequence;
        auto here = tempo.toBarsAndBeats (song->getEdit().getTransport().getPosition());
        tempo::BarsAndBeats to;
        to.bars = here.bars + direction;
        if (direction < 0 && here.beats.inBeats() > nearEnough) to.bars = here.bars;   // back to the start of this bar first
        to.bars = std::max (0, to.bars);
        return arrive (tempo.toTime (to));
    };
    auto stepBeat = [this, arrive] (int direction)
    {
        auto& tempo = song->getEdit().tempoSequence;
        const double beat = tempo.toBeats (song->getEdit().getTransport().getPosition()).inBeats();
        const double to = direction > 0 ? std::floor (beat + nearEnough) + 1.0 : std::ceil (beat - nearEnough) - 1.0;
        return arrive (tempo.toTime (BeatPosition::fromBeats (std::max (0.0, to))));
    };
    struct Step { const char* id; const char* name; const char* description; std::function<CommandResult()> go; };
    for (auto& s : std::vector<Step> {
             { "transport.next-bar",      "Next bar",      "Moves the playhead to the start of the next bar and says where it is.",     [stepBar]  { return stepBar (1); } },
             { "transport.previous-bar",  "Previous bar",  "Moves the playhead back to the start of a bar and says where it is.",       [stepBar]  { return stepBar (-1); } },
             { "transport.next-beat",     "Next beat",     "Moves the playhead to the next beat and says where it is.",                 [stepBeat] { return stepBeat (1); } },
             { "transport.previous-beat", "Previous beat", "Moves the playhead to the previous beat and says where it is.",             [stepBeat] { return stepBeat (-1); } } })
    {
        Command c; c.id = s.id; c.name = s.name; c.category = "Transport"; c.description = s.description;
        c.tags = { "move", "navigate", "playhead", "step" };
        c.available = needSong;
        c.run = [go = s.go] (const CommandArgs&) { return go(); };
        add (c);
    }
    auto addJump = [&] (const char* id, const char* name, const char* description, double direction)
    {
        Command c; c.id = id; c.name = name; c.category = "Transport"; c.description = description;
        c.tags = { "move", "navigate", "playhead", "seek" };
        auto a = ArgSpec(); a.name = "seconds"; a.defaultValue = 5.0; a.minimum = 0.0; a.unit = "seconds";
        c.args = { a };
        c.available = needSong;
        c.run = [this, arrive, direction] (const CommandArgs& args)
        {
            return arrive (song->getEdit().getTransport().getPosition() + TimeDuration::fromSeconds (direction * (double) args["seconds"]));
        };
        add (c);
    };
    addJump ("transport.rewind", "Rewind", "Moves the playhead back a few seconds and says where it is.", -1.0);
    addJump ("transport.fast-forward", "Fast forward", "Moves the playhead forward a few seconds and says where it is.", 1.0);
    {
        Command c; c.id = "transport.go-to-end"; c.name = "Go to end"; c.category = "Transport";
        c.description = "Moves the playhead to the end of the last clip.";
        c.tags = { "move", "navigate", "last" };
        c.available = needSong;
        c.run = [this, arrive] (const CommandArgs&) { return arrive (toPosition (song->getEdit().getLength())); };
        add (c);
    }
    {
        Command c; c.id = "transport.go-to-bar"; c.name = "Go to bar"; c.category = "Transport";
        c.description = "Moves the playhead to the bar and beat you name.";
        c.tags = { "move", "navigate", "jump", "locate" };
        ArgSpec bar; bar.name = "bar"; bar.type = ArgSpec::Type::integer; bar.defaultValue = 1; bar.minimum = 1; 
        bar.currentValue = [this]() -> juce::var { return song ? song->getEdit().tempoSequence.toBarsAndBeats (song->getEdit().getTransport().getPosition()).bars + 1 : 1; };
        ArgSpec beat; beat.name = "beat"; beat.defaultValue = 1.0; beat.minimum = 1.0;
        c.args = { bar, beat };
        c.askForSettings = true;
        c.available = needSong;
        c.run = [this, arrive] (const CommandArgs& args)
        {
            tempo::BarsAndBeats to;
            to.bars = (int) args["bar"] - 1;
            to.beats = BeatDuration::fromBeats ((double) args["beat"] - 1.0);
            return arrive (song->getEdit().tempoSequence.toTime (to));
        };
        add (c);
    }

    {
        Command c; c.id = "transport.bar-start"; c.name = "Start of this bar"; c.category = "Transport";
        c.description = "Moves the playhead back to the start of the bar it's in.";
        c.tags = { "move", "navigate", "measure" };
        c.available = needSong;
        c.run = [this, arrive] (const CommandArgs&)
        {
            auto& tempo = song->getEdit().tempoSequence;
            tempo::BarsAndBeats to; to.bars = tempo.toBarsAndBeats (song->getEdit().getTransport().getPosition()).bars;
            return arrive (tempo.toTime (to));
        };
        add (c);
    }
    auto setLoopEdge = [this] (bool start)
    {
        auto& t = song->getEdit().getTransport();
        const auto here = t.getPosition();
        auto range = t.getLoopRange();
        if (start) range = { here, range.getEnd() > here ? range.getEnd() : here + TimeDuration::fromSeconds (std::max (1.0, range.getLength().inSeconds())) };
        else       range = { range.getStart() < here ? range.getStart() : TimePosition(), here };
        if (range.getLength() <= TimeDuration()) return CommandResult::failed ("The loop's end has to come after its start.");
        t.setLoopRange (range);
        auto& e = song->getEdit();
        return CommandResult::done ("Loop from " + placeText (e, range.getStart()) + " to " + placeText (e, range.getEnd()) + ".",
                                    start ? "Loop start." : "Loop end.");
    };
    {
        Command c; c.id = "transport.loop-start"; c.name = "Loop starts here"; c.category = "Transport";
        c.description = "Sets where the loop starts to the playhead.";
        c.tags = { "cycle", "repeat", "time selection", "in point" };
        c.available = needSong;
        c.run = [setLoopEdge] (const CommandArgs&) { return setLoopEdge (true); };
        add (c);
    }
    {
        Command c; c.id = "transport.loop-end"; c.name = "Loop ends here"; c.category = "Transport";
        c.description = "Sets where the loop ends to the playhead.";
        c.tags = { "cycle", "repeat", "time selection", "out point" };
        c.available = needSong;
        c.run = [setLoopEdge] (const CommandArgs&) { return setLoopEdge (false); };
        add (c);
    }

    //==========================================================================
    // Markers
    auto markerAtOrBefore = [this] () -> MarkerClip*
    {
        auto& e = song->getEdit();
        const auto now = e.getTransport().getPosition().inSeconds() + nearEnough;
        MarkerClip* best = nullptr;
        for (auto* m : e.getMarkerManager().getMarkers())
            if (m->getPosition().getStart().inSeconds() <= now
                && (best == nullptr || m->getPosition().getStart() > best->getPosition().getStart()))
                best = m;
        return best;
    };
    auto needMarker = [this, needSong]
    {
        if (auto ok = needSong(); ! ok.enabled) return ok;
        return song->getEdit().getMarkerManager().getMarkers().isEmpty() ? Availability::no ("There are no markers. M adds one here.") : Availability::yes();
    };
    {
        Command c; c.id = "markers.add"; c.name = "Add marker"; c.category = "Markers";
        c.description = "Puts a marker at the playhead.";
        c.tags = { "cue", "locator", "section", "drop" };
        c.args = { [] { ArgSpec a; a.name = "name"; a.type = ArgSpec::Type::text; a.defaultValue = ""; return a; }() };
        c.undoable = true;
        c.available = needSong;
        c.run = [this, songChanged] (const CommandArgs& args)
        {
            auto& e = song->getEdit();
            auto& mm = e.getMarkerManager();
            const auto at = e.getTransport().getPosition();
            const int number = mm.getNextUniqueID();
            auto m = mm.createMarker (number, at, TimeDuration::fromSeconds (1.0), nullptr);
            if (m == nullptr) return CommandResult::failed ("The marker couldn't be added.");
            const auto typed = args["name"].toString().trim();
            m->setName (typed.isNotEmpty() ? typed : "Marker " + juce::String (number));
            songChanged();
            return CommandResult::done (m->getName() + " at " + placeText (e, at) + ".");
        };
        add (c);
    }
    auto toMarker = [this, arrive] (int direction)
    {
        auto& e = song->getEdit();
        const auto now = e.getTransport().getPosition().inSeconds();
        MarkerClip* best = nullptr;
        for (auto* m : e.getMarkerManager().getMarkers())
        {
            const auto at = m->getPosition().getStart().inSeconds();
            const bool ahead = direction > 0 ? at > now + nearEnough : at < now - nearEnough;
            if (ahead && (best == nullptr
                          || (direction > 0 ? at < best->getPosition().getStart().inSeconds() : at > best->getPosition().getStart().inSeconds())))
                best = m;
        }
        if (best == nullptr) return CommandResult::done (direction > 0 ? "No more markers after this." : "No markers before this.");
        const auto name = best->getName();
        auto moved = arrive (best->getPosition().getStart());
        moved.spoken = name + ". " + moved.spoken;
        moved.brief = name + ".";
        return moved;
    };
    {
        Command c; c.id = "markers.next"; c.name = "Next marker"; c.category = "Markers";
        c.description = "Moves the playhead to the next marker and says its name.";
        c.available = needMarker;
        c.run = [toMarker] (const CommandArgs&) { return toMarker (1); };
        add (c);
    }
    {
        Command c; c.id = "markers.previous"; c.name = "Previous marker"; c.category = "Markers";
        c.description = "Moves the playhead to the previous marker and says its name.";
        c.available = needMarker;
        c.run = [toMarker] (const CommandArgs&) { return toMarker (-1); };
        add (c);
    }
    {
        Command c; c.id = "markers.go-to"; c.name = "Go to marker number"; c.category = "Markers";
        c.description = "Moves the playhead to the marker with that number.";
        c.tags = { "jump", "locate" };
        ArgSpec a; a.name = "number"; a.type = ArgSpec::Type::integer; a.defaultValue = 1; a.minimum = 1;
        c.args = { a };
        c.askForSettings = true;
        c.available = needSong;
        c.run = [this, arrive] (const CommandArgs& args)
        {
            const int n = (int) args["number"];
            auto* m = song->getEdit().getMarkerManager().getMarkerByID (n);
            if (m == nullptr) return CommandResult::failed ("There's no marker " + juce::String (n) + ".");
            const auto name = m->getName();
            auto moved = arrive (m->getPosition().getStart());
            moved.spoken = name + ". " + moved.spoken;
            moved.brief = name + ".";
            return moved;
        };
        add (c);
    }
    {
        Command c; c.id = "markers.set"; c.name = "Put marker number here"; c.category = "Markers";
        c.description = "Puts the marker with that number at the playhead, moving it if it's already somewhere else.";
        c.tags = { "add", "move", "drop" };
        ArgSpec a; a.name = "number"; a.type = ArgSpec::Type::integer; a.defaultValue = 1; a.minimum = 1;
        c.args = { a };
        c.askForSettings = true; c.undoable = true;
        c.available = needSong;
        c.run = [this, songChanged] (const CommandArgs& args)
        {
            auto& e = song->getEdit();
            const int n = (int) args["number"];
            const auto at = e.getTransport().getPosition();
            auto* m = e.getMarkerManager().getMarkerByID (n);
            const bool moved = m != nullptr;
            if (m == nullptr)
            {
                auto made = e.getMarkerManager().createMarker (n, at, TimeDuration::fromSeconds (1.0), nullptr);
                if (made == nullptr) return CommandResult::failed ("The marker couldn't be added.");
                made->setName ("Marker " + juce::String (n));
                m = dynamic_cast<MarkerClip*> (made.get());
            }
            else m->setStart (at, false, true);
            songChanged();
            return CommandResult::done (m->getName() + (moved ? " moved to " : " at ") + placeText (e, at) + ".");
        };
        add (c);
    }
    {
        Command c; c.id = "markers.rename"; c.name = "Rename marker"; c.category = "Markers";
        c.description = "Renames the marker at the playhead, or the nearest one before it.";
        ArgSpec a; a.name = "name"; a.type = ArgSpec::Type::text; a.defaultValue = "";
        a.currentValue = [this, markerAtOrBefore]() -> juce::var { if (song == nullptr) return ""; auto* m = markerAtOrBefore(); return m != nullptr ? m->getName() : juce::String(); };
        c.args = { a };
        c.askForSettings = true;
        c.undoable = true;
        c.available = needMarker;
        c.run = [markerAtOrBefore, songChanged] (const CommandArgs& args)
        {
            auto* m = markerAtOrBefore();
            const auto name = args["name"].toString().trim();
            if (m == nullptr) return CommandResult::failed ("There's no marker at or before the playhead.");
            if (name.isEmpty()) return CommandResult::failed ("A marker needs a name.");
            m->setName (name);
            songChanged();
            return CommandResult::done ("Renamed to " + name + ".");
        };
        add (c);
    }
    {
        Command c; c.id = "markers.delete"; c.name = "Delete marker"; c.category = "Markers";
        c.description = "Deletes the marker at the playhead, or the nearest one before it.";
        c.tags = { "remove" };
        c.undoable = true;
        c.available = needMarker;
        c.run = [markerAtOrBefore, songChanged] (const CommandArgs&)
        {
            auto* m = markerAtOrBefore();
            if (m == nullptr) return CommandResult::failed ("There's no marker at or before the playhead.");
            const auto name = m->getName();
            m->removeFromParent();
            songChanged();
            return CommandResult::done ("Deleted " + name + ".");
        };
        add (c);
    }
    {
        Command c; c.id = "markers.list"; c.name = "List markers"; c.category = "Markers"; c.isReport = true;
        c.description = "Says every marker and where it is. Changes nothing.";
        c.available = needSong;
        c.run = [this] (const CommandArgs&)
        {
            auto& e = song->getEdit();
            auto markers = e.getMarkerManager().getMarkers();
            if (markers.isEmpty()) return CommandResult::done ("No markers.");
            std::vector<MarkerClip*> sorted;
            for (auto* m : markers) sorted.push_back (m);
            std::sort (sorted.begin(), sorted.end(), [] (auto* a, auto* b) { return a->getPosition().getStart() < b->getPosition().getStart(); });
            juce::StringArray parts;
            for (auto* m : sorted) parts.add (m->getName() + " at " + placeText (e, m->getPosition().getStart()));
            return CommandResult::done (juce::String ((int) sorted.size()) + (sorted.size() == 1 ? " marker. " : " markers. ") + parts.joinIntoString (". ") + ".");
        };
        add (c);
    }

    //==========================================================================
    // Tracks
    {
        Command c; c.id = "tracks.report"; c.name = "About this track"; c.category = "Tracks"; c.isReport = true;
        c.description = "Says the selected track's clips, volume, pan and whether it's muted or soloed. Changes nothing.";
        c.tags = { "status", "info", "level" };
        c.available = needTrack;
        c.run = [this] (const CommandArgs&) { return CommandResult::done (trackSentence (*selectedAudioTrack())); };
        add (c);
    }
    struct AllTracks { const char* id; const char* name; const char* description; bool mute; bool report; };
    for (auto& a : std::vector<AllTracks> {
             { "tracks.unmute-all",     "Unmute all tracks",     "Takes the mute off every track.",           true,  false },
             { "tracks.unsolo-all",     "Unsolo all tracks",     "Takes the solo off every track.",           false, false },
             { "tracks.report-muted",   "Which tracks are muted",  "Says every muted track. Changes nothing.",  true,  true },
             { "tracks.report-soloed",  "Which tracks are soloed", "Says every soloed track. Changes nothing.", false, true } })
    {
        Command c; c.id = a.id; c.name = a.name; c.category = "Tracks"; c.description = a.description;
        c.isReport = a.report; c.undoable = ! a.report;
        c.available = needSong;
        c.run = [this, songChanged, mute = a.mute, report = a.report] (const CommandArgs&)
        {
            juce::StringArray names;
            for (auto* t : contentTracks (song->getEdit()))
                if (mute ? t->isMuted (false) : t->isSolo (false))
                {
                    names.add (t->getName());
                    if (! report) { if (mute) t->setMute (false); else t->setSolo (false); }
                }
            const juce::String word = mute ? "muted" : "soloed";
            if (report) return CommandResult::done (names.isEmpty() ? "No tracks are " + word + "." : juce::String (names.size()) + " " + word + ": " + names.joinIntoString (", ") + ".");
            songChanged();
            return CommandResult::done (names.isEmpty() ? "Nothing was " + word + "." : (mute ? "Unmuted " : "Unsoloed ") + names.joinIntoString (", ") + ".");
        };
        add (c);
    }
    {
        Command c; c.id = "tracks.rename"; c.name = "Rename track"; c.category = "Tracks";
        c.description = "Gives the selected track a new name.";
        c.tags = { "name", "label" };
        ArgSpec a; a.name = "name"; a.type = ArgSpec::Type::text; a.defaultValue = "";
        a.currentValue = [this]() -> juce::var { auto* t = selectedAudioTrack(); return t != nullptr ? t->getName() : juce::String(); };
        c.args = { a };
        c.askForSettings = true;
        c.undoable = true;
        c.available = needTrack;
        c.run = [this, songChanged] (const CommandArgs& args)
        {
            const auto name = args["name"].toString().trim();
            if (name.isEmpty()) return CommandResult::failed ("A track needs a name.");
            selectedAudioTrack()->setName (name);
            songChanged();
            return CommandResult::done ("Renamed to " + name + ".");
        };
        add (c);
    }
    {
        Command c; c.id = "tracks.delete"; c.name = "Delete track"; c.category = "Tracks";
        c.description = "Deletes the selected track and its clips. Undo brings it back.";
        c.tags = { "remove" };
        c.undoable = true;
        c.available = needTrack;
        c.run = [this, songChanged] (const CommandArgs&)
        {
            auto& e = song->getEdit();
            auto* t = selectedAudioTrack();
            const auto name = t->getName();
            e.deleteTrack (t);
            selectedTrack = juce::jlimit (0, std::max (0, contentTracks (e).size() - 1), selectedTrack);
            songChanged();
            return CommandResult::done ("Deleted " + name + ". Undo brings it back.");
        };
        add (c);
    }
    auto moveInList = [this, songChanged] (int direction)
    {
        auto& e = song->getEdit();
        auto tracks = contentTracks (e);
        const int i = tracks.indexOf (selectedAudioTrack());
        const int to = i + direction;
        if (to < 0 || to >= tracks.size())
            return CommandResult::done (direction < 0 ? "It's already the first track." : "It's already the last track.");
        // The track it should come after. Moving to the top goes after whatever
        // sits above the first track, so the song's own hidden tracks stay put.
        Track* after = direction > 0 ? tracks[to] : (to > 0 ? tracks[to - 1] : tracks[0]->getSiblingTrack (-1, false));
        Track::Ptr moving (tracks[i]);
        e.moveTrack (moving, TrackInsertPoint (nullptr, after));
        selectedTrack = contentTracks (e).indexOf (dynamic_cast<AudioTrack*> (moving.get()));
        songChanged();
        return CommandResult::done (moving->getName() + ", " + juce::String (selectedTrack + 1) + " of " + juce::String (tracks.size()) + ".");
    };
    {
        Command c; c.id = "tracks.move-up"; c.name = "Move track up"; c.category = "Tracks";
        c.description = "Moves the selected track one place up the list.";
        c.tags = { "reorder" };
        c.undoable = true;
        c.available = needTrack;
        c.run = [moveInList] (const CommandArgs&) { return moveInList (-1); };
        add (c);
    }
    {
        Command c; c.id = "tracks.move-down"; c.name = "Move track down"; c.category = "Tracks";
        c.description = "Moves the selected track one place down the list.";
        c.tags = { "reorder" };
        c.undoable = true;
        c.available = needTrack;
        c.run = [moveInList] (const CommandArgs&) { return moveInList (1); };
        add (c);
    }
    {
        Command c; c.id = "tracks.mute"; c.name = "Mute"; c.category = "Tracks";
        c.description = "Mutes or unmutes the selected track.";
        c.tags = { "silence" };
        c.onWord = "Muted"; c.offWord = "Unmuted"; c.sayNameWithState = false;
        c.undoable = true;
        c.available = needTrack;
        c.toggleState = [this] { auto* t = selectedAudioTrack(); return t != nullptr ? std::optional<bool> (t->isMuted (false)) : std::optional<bool>(); };
        c.run = [this, songChanged] (const CommandArgs&)
        {
            auto* t = selectedAudioTrack();
            t->setMute (! t->isMuted (false));
            songChanged();
            return CommandResult::done (t->getName() + ".");
        };
        add (c);
    }
    {
        Command c; c.id = "tracks.solo"; c.name = "Solo"; c.category = "Tracks";
        c.description = "Solos the selected track, or takes the solo off.";
        c.onWord = "Soloed"; c.offWord = "Solo off"; c.sayNameWithState = false;
        c.undoable = true;
        c.available = needTrack;
        c.toggleState = [this] { auto* t = selectedAudioTrack(); return t != nullptr ? std::optional<bool> (t->isSolo (false)) : std::optional<bool>(); };
        c.run = [this, songChanged] (const CommandArgs&)
        {
            auto* t = selectedAudioTrack();
            t->setSolo (! t->isSolo (false));
            songChanged();
            return CommandResult::done (t->getName() + ".");
        };
        add (c);
    }
    auto setVolume = [this, songChanged] (double db)
    {
        auto* t = selectedAudioTrack();
        auto* v = t->getVolumePlugin();
        if (v == nullptr) return CommandResult::failed ("This track has no volume control.");
        v->setVolumeDb ((float) juce::jlimit (-100.0, 12.0, db));
        songChanged();
        return CommandResult::done (dbWords (v->getVolumeDb()) + ".", dbWords (v->getVolumeDb()) + ".");
    };
    auto setPan = [this, songChanged] (double pan)
    {
        auto* t = selectedAudioTrack();
        auto* v = t->getVolumePlugin();
        if (v == nullptr) return CommandResult::failed ("This track has no pan control.");
        v->setPan ((float) juce::jlimit (-1.0, 1.0, pan));
        songChanged();
        const auto words = panWords (v->getPan());
        return CommandResult::done (words.substring (0, 1).toUpperCase() + words.substring (1) + ".");
    };
    auto currentDb = [this] { auto* t = selectedAudioTrack(); auto* v = t != nullptr ? t->getVolumePlugin() : nullptr; return v != nullptr ? (double) v->getVolumeDb() : 0.0; };
    auto currentPan = [this] { auto* t = selectedAudioTrack(); auto* v = t != nullptr ? t->getVolumePlugin() : nullptr; return v != nullptr ? (double) v->getPan() : 0.0; };
    {
        Command c; c.id = "tracks.volume"; c.name = "Track volume"; c.category = "Tracks";
        c.description = "Sets the selected track's volume in decibels.";
        c.tags = { "level", "gain", "fader" };
        ArgSpec a; a.name = "volume"; a.unit = "dB"; a.defaultValue = 0.0; a.minimum = -100.0; a.maximum = 12.0;
        a.currentValue = [currentDb]() -> juce::var { return std::round (currentDb() * 10.0) / 10.0; };
        c.args = { a };
        c.askForSettings = true; c.undoable = true; c.available = needTrack;
        c.run = [setVolume] (const CommandArgs& args) { return setVolume ((double) args["volume"]); };
        add (c);
    }
    {
        Command c; c.id = "tracks.pan"; c.name = "Track pan"; c.category = "Tracks";
        c.description = "Sets the selected track's pan: -100 is fully left, 0 is the centre, 100 is fully right.";
        c.tags = { "balance", "stereo" };
        ArgSpec a; a.name = "pan"; a.defaultValue = 0.0; a.minimum = -100.0; a.maximum = 100.0;
        a.currentValue = [currentPan]() -> juce::var { return juce::roundToInt (currentPan() * 100.0); };
        c.args = { a };
        c.askForSettings = true; c.undoable = true; c.available = needTrack;
        c.run = [setPan] (const CommandArgs& args) { return setPan ((double) args["pan"] / 100.0); };
        add (c);
    }
    struct Nudge { const char* id; const char* name; const char* description; std::function<CommandResult()> go; };
    for (auto& n : std::vector<Nudge> {
             { "tracks.volume-up",   "Volume up",   "Raises the selected track by 1 dB.",        [setVolume, currentDb] { return setVolume (currentDb() + 1.0); } },
             { "tracks.volume-down", "Volume down", "Lowers the selected track by 1 dB.",        [setVolume, currentDb] { return setVolume (currentDb() - 1.0); } },
             { "tracks.pan-left",    "Pan left",    "Moves the selected track 5 percent left.",  [setPan, currentPan]   { return setPan (currentPan() - 0.05); } },
             { "tracks.pan-right",   "Pan right",   "Moves the selected track 5 percent right.", [setPan, currentPan]   { return setPan (currentPan() + 0.05); } } })
    {
        Command c; c.id = n.id; c.name = n.name; c.category = "Tracks"; c.description = n.description;
        c.tags = { "level", "balance", "nudge" };
        c.undoable = true; c.available = needTrack;
        c.run = [go = n.go] (const CommandArgs&) { return go(); };
        add (c);
    }
    {
        Command c; c.id = "tracks.input"; c.name = "Track input"; c.category = "Tracks";
        c.description = "Chooses the input you hear through the selected track, with its volume, pan and mute. Capture records every input either way.";
        c.tags = { "monitor", "listen", "source" };
        ArgSpec a; a.name = "input"; a.type = ArgSpec::Type::choice; a.defaultValue = "";
        a.liveChoices = [this]
        {
            juce::StringArray names ("No input");
            if (song != nullptr)
                for (auto* in : song->getEdit().getAllInputDevices())
                    if (in->owner.getDeviceType() == InputDevice::waveDevice) names.add (in->owner.getName());
            return names;
        };
        a.currentValue = [this]() -> juce::var
        {
            auto* t = selectedAudioTrack();
            if (t != nullptr)
                for (auto* in : song->getEdit().getAllInputDevices())
                    if (in->getTargets().contains (t->itemID)) return in->owner.getName();
            return "No input";
        };
        c.args = { a };
        c.askForSettings = true; c.undoable = true; c.available = needTrack;
        c.run = [this, songChanged] (const CommandArgs& args)
        {
            auto& e = song->getEdit();
            auto* t = selectedAudioTrack();
            const auto choice = args["input"].toString();
            for (auto* in : e.getAllInputDevices())
                if (in->getTargets().contains (t->itemID))
                    (void) in->removeTarget (t->itemID, &e.getUndoManager());
            if (choice == "No input")
            {
                songChanged();
                return CommandResult::done (t->getName() + " has no input.");
            }
            for (auto* in : e.getAllInputDevices())
                if (in->owner.getName() == choice)
                {
                    auto result = in->setTarget (t->itemID, false, &e.getUndoManager(), 0);
                    if (! result) return CommandResult::failed ("That input couldn't be used: " + result.error());
                    in->owner.setMonitorMode (InputDevice::MonitorMode::on);
                    songChanged();
                    return CommandResult::done ("You'll hear " + choice + " through " + t->getName() + ".");
                }
            return CommandResult::failed ("There's no input called " + choice + ".");
        };
        add (c);
    }
}

//==============================================================================
// Bringing audio in, rendering it out.
juce::String AppCore::importAudio (const juce::Array<juce::File>& files)
{
    if (song == nullptr) return "No song is open.";
    if (files.isEmpty()) return {};
    auto& e = song->getEdit();
    auto folder = song->getFolder().getChildFile ("Audio");
    folder.createDirectory();
    const auto at = e.getTransport().getPosition();
    int placed = 0;
    juce::String first, trouble;
    auto tracks = contentTracks (e);
    int trackIndex = juce::jlimit (0, std::max (0, tracks.size() - 1), selectedTrack);
    e.getUndoManager().beginNewTransaction ("Import audio");
    for (auto& f : files)
    {
        // Songs own their audio: a copy, so moving the original never breaks them.
        auto copy = folder.getChildFile (f.getFileName());
        if (copy != f && copy.existsAsFile() && copy.getSize() != f.getSize()) copy = copy.getNonexistentSibling();
        if (copy != f && ! copy.existsAsFile() && ! f.copyFileTo (copy)) { trouble = "couldn't copy " + f.getFileName(); continue; }
        tracktion::AudioFile audio (engine, copy);
        if (! audio.isValid() || audio.getLength() <= 0.0) { copy.deleteFile(); trouble = f.getFileName() + " isn't audio Ear First can read"; continue; }
        while (contentTracks (e).size() <= trackIndex) ensureContentTracks (e, trackIndex + 1);
        auto* track = contentTracks (e)[trackIndex];
        auto clip = track->insertWaveClip (f.getFileNameWithoutExtension(), copy,
                                           { { at, at + TimeDuration::fromSeconds (audio.getLength()) }, {} }, false);
        if (clip == nullptr) { trouble = "couldn't place " + f.getFileName(); continue; }
        if (placed++ == 0) first = f.getFileNameWithoutExtension() + " on " + track->getName();
        ++trackIndex;   // several files: one track each, all starting together
    }
    if (onSongChanged) onSongChanged();
    if (placed == 0) return "Nothing was imported: " + trouble + ".";
    juce::String s = placed == 1 ? "Imported " + first : "Imported " + juce::String (placed) + " files, starting with " + first;
    s << " at " << placeText (e, at) << ".";
    if (trouble.isNotEmpty()) s << " But " << trouble << ".";
    return s;
}

void AppCore::registerClipAndFileCommands()
{
    auto add = [this] (Command c)
    {
        auto r = commandList.add (std::move (c));
        jassert (r.wasOk());
        juce::ignoreUnused (r);
    };
    auto needSong = [this] { return song == nullptr ? Availability::no ("No song is open. Press Command-N for a new one.") : Availability::yes(); };
    auto songChanged = [this] { if (onSongChanged) onSongChanged(); };

    {
        Command c; c.id = "file.import-audio"; c.name = "Import audio"; c.category = "File";
        c.description = "Copies audio files into the song and puts them at the playhead, starting on the selected track, one track each.";
        c.tags = { "add file", "load", "wav", "drag" };
        ArgSpec a; a.name = "file"; a.type = ArgSpec::Type::text; a.defaultValue = "";
        c.args = { a };
        c.available = needSong;
        c.run = [this] (const CommandArgs& args)
        {
            const auto path = args["file"].toString();
            if (path.isEmpty())
            {
                if (! onImportAudioRequested) return CommandResult::failed ("There's no window to choose from.");
                onImportAudioRequested();
                return CommandResult::done();
            }
            auto s = importAudio ({ juce::File (path) });
            return s.startsWith ("Imported") ? CommandResult::done (s) : CommandResult::failed (s);
        };
        add (c);
    }
    {
        Command c; c.id = "file.render"; c.name = "Render song"; c.category = "File";
        c.description = "Writes the whole song to one audio file in the song's Renders folder. The click is left out. It runs in the background.";
        c.tags = { "export", "bounce", "mixdown", "save as", "wav", "flac" };
        ArgSpec name; name.name = "name"; name.type = ArgSpec::Type::text; name.defaultValue = "";
        name.currentValue = [this]() -> juce::var { return song ? song->getName() : juce::String(); };
        ArgSpec format; format.name = "format"; format.type = ArgSpec::Type::choice;
        format.choices = { "WAV 24-bit", "WAV 32-bit float", "WAV 16-bit", "FLAC 24-bit" };
        format.defaultValue = "WAV 24-bit";
        c.args = { name, format };
        c.askForSettings = true;
        c.available = [this, needSong]
        {
            if (auto ok = needSong(); ! ok.enabled) return ok;
            if (renderJob != nullptr) return Availability::no ("A render is already going.");
            return song->getEdit().getLength() <= TimeDuration() ? Availability::no ("There's nothing in the song to render yet.") : Availability::yes();
        };
        c.run = [this] (const CommandArgs& args)
        {
            auto& e = song->getEdit();
            const auto format = args["format"].toString();
            auto& formats = engine.getAudioFileFormatManager();
            const bool flac = format.startsWith ("FLAC");
            auto base = juce::File::createLegalFileName (args["name"].toString().trim());
            if (base.isEmpty()) base = song->getName();
            auto folder = song->getFolder().getChildFile ("Renders");
            folder.createDirectory();
            auto file = folder.getChildFile (base + (flac ? ".flac" : ".wav")).getNonexistentSibling();

            juce::Array<Track*> toDo;
            for (auto* t : contentTracks (e)) toDo.add (t);
            Renderer::Parameters p (e);
            p.tracksToDo = toBitSet (toDo);
            p.destFile = file;
            p.audioFormat = flac ? formats.getFlacFormat() : formats.getWavFormat();
            p.bitDepth = format.contains ("32") ? 32 : format.contains ("16") ? 16 : 24;
            p.sampleRateForAudio = 48000.0;
            if (auto* d = engine.getDeviceManager().deviceManager.getCurrentAudioDevice()) p.sampleRateForAudio = d->getCurrentSampleRate();
            p.time = { TimePosition(), TimePosition() + e.getLength() };
            p.usePlugins = true;
            p.useMasterPlugins = true;
            p.canRenderInMono = false;

            lastRender = juce::File();
            auto flag = alive;
            renderJob = EditRenderer::render (p, [this, flag, name = file.getFileName(), file] (auto result)
            {
                juce::MessageManager::callAsync ([this, flag, name, file, result]
                {
                    if (! *flag) return;
                    renderJob.reset();
                    if (result) { lastRender = file; say ("Rendered " + name + ". It's in the song's Renders folder.", "Render"); }
                    else say ("The render didn't finish: " + juce::String (result.error()) + ".", "Errors", {}, Speech::Priority::urgent);
                });
            });
            return CommandResult::done ("Rendering " + file.getFileName() + ". You can keep working; I'll say when it's done.", "Rendering.");
        };
        add (c);
    }
    {
        Command c; c.id = "file.show-renders"; c.name = "Show renders folder"; c.category = "File";
        c.description = "Opens the song's Renders folder.";
        c.tags = { "reveal", "finder", "explorer" };
        c.available = needSong;
        c.run = [this] (const CommandArgs&)
        {
            auto folder = song->getFolder().getChildFile ("Renders");
            folder.createDirectory();
            folder.revealToUser();
            return CommandResult::done();
        };
        add (c);
    }

    //==========================================================================
    // Levels
    {
        Command c; c.id = "levels.report"; c.name = "What's the level"; c.category = "Audio"; c.isReport = true;
        c.description = "Says the peak of every input and output with sound on it over the last few seconds. Changes nothing.";
        c.tags = { "meter", "peak", "gain", "clipping", "loud" };
        c.run = [this] (const CommandArgs&)
        {
            if (capture == nullptr) return CommandResult::failed ("No audio interface is open.");
            const auto say_ = [] (const juce::StringArray& names, const std::vector<float>& held)
            {
                juce::StringArray parts;
                for (int i = 0; i < names.size() && i < (int) held.size(); ++i)
                    if (held[(size_t) i] > 0.001f)
                        parts.add (names[i] + " " + juce::String (juce::roundToInt (juce::Decibels::gainToDecibels (held[(size_t) i]))) + " dB");
                return parts.joinIntoString (", ");
            };
            auto in = say_ (capture->getInputNames(), meters.inputHeld);
            auto out = say_ (capture->getOutputNames(), meters.outputHeld);
            if (in.isEmpty() && out.isEmpty()) return CommandResult::done ("Nothing's coming in or going out.");
            juce::String s;
            if (in.isNotEmpty()) s << "In: " << in << ". ";
            if (out.isNotEmpty()) s << "Out: " << out << ".";
            return CommandResult::done (s.trimEnd());
        };
        add (c);
    }
    {
        Command c; c.id = "levels.clip-warnings"; c.name = "Clipping warnings"; c.category = "Audio";
        c.description = "Says so when an input or output clips.";
        c.tags = { "overload", "distortion", "alert" };
        c.toggleState = [this] { return std::optional<bool> (clipWarnings); };
        c.run = [this] (const CommandArgs&) { clipWarnings = ! clipWarnings; saveAppSettings(); return CommandResult::done(); };
        add (c);
    }

    //==========================================================================
    // Clips on the selected track
    auto clipHere = [this] () -> Clip*
    {
        auto* t = selectedAudioTrack();
        if (t == nullptr) return nullptr;
        const auto now = song->getEdit().getTransport().getPosition();
        for (auto* clip : t->getClips())
            if (clip->getPosition().time.contains (now) || clip->getPosition().getStart() == now) return clip;
        return nullptr;
    };
    auto needClip = [this, clipHere, needSong]
    {
        if (auto ok = needSong(); ! ok.enabled) return ok;
        return clipHere() == nullptr ? Availability::no ("There's no clip at the playhead on this track.") : Availability::yes();
    };
    auto needClips = [this, needSong]
    {
        if (auto ok = needSong(); ! ok.enabled) return ok;
        auto* t = selectedAudioTrack();
        return t == nullptr || t->getClips().isEmpty() ? Availability::no ("This track has no clips.") : Availability::yes();
    };
    auto clipWords = [this] (Clip& clip)
    {
        auto& e = song->getEdit();
        return clip.getName() + ", " + placeText (e, clip.getPosition().getStart()) + ", "
             + juce::String (clip.getPosition().getLength().inSeconds(), 1) + " seconds";
    };
    {
        Command c; c.id = "clips.report"; c.name = "About this clip"; c.category = "Clips"; c.isReport = true;
        c.description = "Says the clip at the playhead: its name, where it starts and how long it is. Changes nothing.";
        c.available = needClip;
        c.run = [clipHere, clipWords] (const CommandArgs&) { return CommandResult::done (clipWords (*clipHere()) + "."); };
        add (c);
    }
    auto toClip = [this, clipWords] (int direction)
    {
        auto* t = selectedAudioTrack();
        auto& transport = song->getEdit().getTransport();
        const auto now = transport.getPosition().inSeconds();
        Clip* best = nullptr;
        for (auto* clip : t->getClips())
        {
            const auto at = clip->getPosition().getStart().inSeconds();
            if ((direction > 0 ? at > now + nearEnough : at < now - nearEnough)
                && (best == nullptr || (direction > 0 ? at < best->getPosition().getStart().inSeconds() : at > best->getPosition().getStart().inSeconds())))
                best = clip;
        }
        if (best == nullptr) return CommandResult::done (direction > 0 ? "No more clips on this track." : "No clips before this.");
        transport.setPosition (best->getPosition().getStart());
        return CommandResult::done (clipWords (*best) + ".", best->getName() + ".");
    };
    {
        Command c; c.id = "clips.next"; c.name = "Next clip"; c.category = "Clips";
        c.description = "Moves the playhead to the start of the next clip on this track and says what it is.";
        c.available = needClips;
        c.run = [toClip] (const CommandArgs&) { return toClip (1); };
        add (c);
    }
    {
        Command c; c.id = "clips.previous"; c.name = "Previous clip"; c.category = "Clips";
        c.description = "Moves the playhead to the start of the previous clip on this track and says what it is.";
        c.available = needClips;
        c.run = [toClip] (const CommandArgs&) { return toClip (-1); };
        add (c);
    }
    {
        Command c; c.id = "clips.split"; c.name = "Split clip"; c.category = "Clips";
        c.description = "Cuts the clip in two at the playhead.";
        c.tags = { "cut", "razor", "blade", "divide" };
        c.undoable = true; c.available = needClip;
        c.run = [this, clipHere, songChanged] (const CommandArgs&)
        {
            auto* clip = clipHere();
            const auto now = song->getEdit().getTransport().getPosition();
            if (clip->getPosition().getStart() == now) return CommandResult::failed ("The playhead is on the very start of the clip, so there's nothing to split.");
            if (tracktion::split (*clip, now) == nullptr) return CommandResult::failed ("The clip couldn't be split here.");
            songChanged();
            return CommandResult::done ("Split at " + placeText (song->getEdit(), now) + ".", "Split.");
        };
        add (c);
    }
    {
        Command c; c.id = "clips.trim-start"; c.name = "Trim clip start to here"; c.category = "Clips";
        c.description = "Removes the part of the clip before the playhead. Undo brings it back.";
        c.tags = { "cut", "head", "crop" };
        c.undoable = true; c.available = needClip;
        c.run = [this, clipHere, songChanged] (const CommandArgs&)
        {
            auto* clip = clipHere();
            clip->setStart (song->getEdit().getTransport().getPosition(), true, false);
            songChanged();
            return CommandResult::done ("Trimmed the start. " + juce::String (clip->getPosition().getLength().inSeconds(), 1) + " seconds left.", "Trimmed.");
        };
        add (c);
    }
    {
        Command c; c.id = "clips.trim-end"; c.name = "Trim clip end to here"; c.category = "Clips";
        c.description = "Removes the part of the clip after the playhead. Undo brings it back.";
        c.tags = { "cut", "tail", "crop" };
        c.undoable = true; c.available = needClip;
        c.run = [this, clipHere, songChanged] (const CommandArgs&)
        {
            auto* clip = clipHere();
            clip->setEnd (song->getEdit().getTransport().getPosition(), true);
            songChanged();
            return CommandResult::done ("Trimmed the end. " + juce::String (clip->getPosition().getLength().inSeconds(), 1) + " seconds left.", "Trimmed.");
        };
        add (c);
    }
    {
        Command c; c.id = "clips.delete"; c.name = "Delete clip"; c.category = "Clips";
        c.description = "Deletes the clip at the playhead. Undo brings it back.";
        c.tags = { "remove" };
        c.undoable = true; c.available = needClip;
        c.run = [clipHere, songChanged] (const CommandArgs&)
        {
            auto* clip = clipHere();
            const auto name = clip->getName();
            clip->removeFromParent();
            songChanged();
            return CommandResult::done ("Deleted " + name + ".");
        };
        add (c);
    }
    {
        Command c; c.id = "clips.move"; c.name = "Move clip"; c.category = "Clips";
        c.description = "Moves the clip at the playhead by a number of beats: positive is later, negative is earlier.";
        c.tags = { "nudge", "shift", "slide" };
        ArgSpec a; a.name = "beats"; a.defaultValue = 1.0; a.unit = "beats";
        c.args = { a };
        c.askForSettings = true; c.undoable = true; c.available = needClip;
        c.run = [this, clipHere, clipWords, songChanged] (const CommandArgs& args)
        {
            auto& e = song->getEdit();
            auto* clip = clipHere();
            const auto beat = e.tempoSequence.toBeats (clip->getPosition().getStart()).inBeats() + (double) args["beats"];
            const auto to = e.tempoSequence.toTime (BeatPosition::fromBeats (std::max (0.0, beat)));
            clip->setStart (to, false, true);
            e.getTransport().setPosition (to);       // the playhead goes with it, so it's still "this clip"
            songChanged();
            return CommandResult::done ("Moved to " + placeText (e, to) + ".");
        };
        add (c);
    }
    {
        Command c; c.id = "clips.move-to-playhead"; c.name = "Move clip to bar"; c.category = "Clips";
        c.description = "Moves the clip at the playhead so it starts on the bar you name.";
        c.tags = { "place", "position" };
        ArgSpec bar; bar.name = "bar"; bar.type = ArgSpec::Type::integer; bar.defaultValue = 1; bar.minimum = 1;
        c.args = { bar };
        c.askForSettings = true; c.undoable = true; c.available = needClip;
        c.run = [this, clipHere, songChanged] (const CommandArgs& args)
        {
            auto& e = song->getEdit();
            auto* clip = clipHere();
            tempo::BarsAndBeats where; where.bars = (int) args["bar"] - 1;
            const auto to = e.tempoSequence.toTime (where);
            clip->setStart (to, false, true);
            e.getTransport().setPosition (to);
            songChanged();
            return CommandResult::done ("Moved to " + placeText (e, to) + ".");
        };
        add (c);
    }
}

//==============================================================================
// Moving by any unit at all.
namespace
{
enum class UnitKind { seconds, samples, frames, beats, bars };
struct Unit { const char* name; UnitKind kind; double size; const char* plural; };

// Finest first, so finer and coarser walk this list.
const std::vector<Unit>& units()
{
    static const std::vector<Unit> u {
        { "Sample",              UnitKind::samples, 1,        "samples" },
        { "Tick",                UnitKind::beats,   1.0 / 960, "ticks" },
        { "Millisecond",         UnitKind::seconds, 0.001,    "milliseconds" },
        { "10 milliseconds",     UnitKind::seconds, 0.01,     "lots of 10 milliseconds" },
        { "Frame",               UnitKind::frames,  1,        "frames" },
        { "Sixteenth of a beat", UnitKind::beats,   1.0 / 16, "sixteenths of a beat" },
        { "Eighth of a beat",    UnitKind::beats,   1.0 / 8,  "eighths of a beat" },
        { "100 milliseconds",    UnitKind::seconds, 0.1,      "lots of 100 milliseconds" },
        { "Sixth of a beat",     UnitKind::beats,   1.0 / 6,  "sixths of a beat" },
        { "Quarter of a beat",   UnitKind::beats,   0.25,     "quarters of a beat" },
        { "Third of a beat",     UnitKind::beats,   1.0 / 3,  "thirds of a beat" },
        { "Half a beat",         UnitKind::beats,   0.5,      "half beats" },
        { "Beat",                UnitKind::beats,   1,        "beats" },
        { "Second",              UnitKind::seconds, 1,        "seconds" },
        { "Bar",                 UnitKind::bars,    1,        "bars" },
        { "2 bars",              UnitKind::bars,    2,        "lots of 2 bars" },
        { "5 seconds",           UnitKind::seconds, 5,        "lots of 5 seconds" },
        { "4 bars",              UnitKind::bars,    4,        "lots of 4 bars" },
        { "10 seconds",          UnitKind::seconds, 10,       "lots of 10 seconds" },
        { "8 bars",              UnitKind::bars,    8,        "lots of 8 bars" },
        { "30 seconds",          UnitKind::seconds, 30,       "lots of 30 seconds" },
        { "16 bars",             UnitKind::bars,    16,       "lots of 16 bars" },
        { "Minute",              UnitKind::seconds, 60,       "minutes" },
    };
    return u;
}

const Unit* findUnit (const juce::String& name)
{
    for (auto& u : units()) if (name.equalsIgnoreCase (u.name)) return &u;
    return nullptr;
}

// Steps of `size` along a line, snapping to the grid first: from between two
// lines, the first step lands on the nearer line in that direction.
double stepOnGrid (double at, double size, double count)
{
    if (count == 0) return at;
    const double eps = 1.0e-9 * std::max (1.0, std::abs (at / size));
    const double index = at / size;
    const double whole = count > 0 ? std::floor (index + eps) + count : std::ceil (index - eps) + count;
    return whole * size;
}
} // namespace

juce::StringArray AppCore::stepUnitNames()
{
    juce::StringArray s;
    for (auto& u : units()) s.add (u.name);
    return s;
}

double AppCore::currentSampleRate() const
{
    if (auto* d = const_cast<tracktion::Engine&> (engine).getDeviceManager().deviceManager.getCurrentAudioDevice())
        return d->getCurrentSampleRate();
    return 48000.0;
}

TimePosition AppCore::moveBy (TimePosition from, const juce::String& unitName, double count) const
{
    const auto* u = findUnit (unitName);
    if (u == nullptr || song == nullptr) return from;
    auto& tempo = song->getEdit().tempoSequence;
    switch (u->kind)
    {
        case UnitKind::seconds: return TimePosition::fromSeconds (std::max (0.0, stepOnGrid (from.inSeconds(), u->size, count)));
        case UnitKind::samples:
        {
            const double rate = currentSampleRate();
            return TimePosition::fromSeconds (std::max (0.0, stepOnGrid (from.inSeconds() * rate, 1.0, count) / rate));
        }
        case UnitKind::frames:  return TimePosition::fromSeconds (std::max (0.0, stepOnGrid (from.inSeconds(), 1.0 / frameRate, count)));
        case UnitKind::beats:   return tempo.toTime (BeatPosition::fromBeats (std::max (0.0, stepOnGrid (tempo.toBeats (from).inBeats(), u->size, count))));
        case UnitKind::bars:
        {
            // Bars can change length with the meter, so they're counted, not measured.
            const auto here = tempo.toBarsAndBeats (from);
            const int n = juce::roundToInt (u->size * count);
            const bool onLine = here.beats.inBeats() < 1.0e-6 && here.bars % juce::roundToInt (u->size) == 0;
            int bar = here.bars - (here.bars % juce::roundToInt (u->size));
            if (n > 0) bar += n;
            else if (onLine) bar += n;
            else bar += n + juce::roundToInt (u->size);
            tempo::BarsAndBeats to; to.bars = std::max (0, bar);
            return tempo.toTime (to);
        }
    }
    return from;
}

double AppCore::stepSeconds() const
{
    if (song == nullptr) return 1.0;
    const auto now = song->getEdit().getTransport().getPosition();
    const auto next = moveBy (now, stepUnit, 1.0);
    return std::max (1.0 / currentSampleRate(), (next - now).inSeconds());
}

juce::String AppCore::positionInEveryUnit() const
{
    if (song == nullptr) return "No song is open.";
    auto& e = song->getEdit();
    const auto t = e.getTransport().getPosition();
    const auto bb = e.tempoSequence.toBarsAndBeats (t);
    const double beatInBar = bb.beats.inBeats();
    const int ticks = juce::roundToInt ((beatInBar - std::floor (beatInBar)) * 960.0);
    const double secs = t.inSeconds();
    const int h = (int) (secs / 3600), m = (int) (secs / 60) % 60;
    const double s = secs - h * 3600 - m * 60;
    const auto frames = (int) std::floor ((secs - std::floor (secs)) * frameRate + 1.0e-6);
    juce::String clock = (h > 0 ? juce::String (h) + ":" + juce::String (m).paddedLeft ('0', 2) : juce::String (m)) + ":" + juce::String (s, 3).paddedLeft ('0', 6);
    juce::String tc = juce::String (h).paddedLeft ('0', 2) + ":" + juce::String (m).paddedLeft ('0', 2) + ":" + juce::String ((int) s).paddedLeft ('0', 2) + ":" + juce::String (frames).paddedLeft ('0', 2);
    return "Bar " + juce::String (bb.bars + 1) + ", beat " + juce::String ((int) std::floor (beatInBar) + 1) + ", tick " + juce::String (ticks) + " of 960. "
         + clock + ". Sample " + juce::String ((juce::int64) std::llround (secs * currentSampleRate())) + " at " + juce::String ((int) currentSampleRate()) + " hertz. "
         + "Timecode " + tc + " at " + juce::String (frameRate, 3).trimCharactersAtEnd ("0").trimCharactersAtEnd (".") + " frames.";
}

// "9", "9.3", "bar 9 beat 3", "1:23.5", "1:02:03", "83.2s", "250ms", "48000 samples", "00:01:02:12" (timecode).
std::optional<TimePosition> AppCore::parsePlace (const juce::String& typed) const
{
    if (song == nullptr) return {};
    auto& tempo = song->getEdit().tempoSequence;
    auto text = typed.trim().toLowerCase();
    if (text.isEmpty()) return {};
    const auto number = [] (const juce::String& s) -> std::optional<double>
    {
        auto t = s.trim();
        if (t.isEmpty() || ! t.containsOnly ("0123456789.-")) return {};
        return t.getDoubleValue();
    };
    if (text.endsWith ("samples") || text.endsWith ("sample"))
        if (auto n = number (text.upToFirstOccurrenceOf ("sample", false, false))) return TimePosition::fromSeconds (std::max (0.0, *n / currentSampleRate()));
    if (text.endsWith ("ms"))
        if (auto n = number (text.dropLastCharacters (2))) return TimePosition::fromSeconds (std::max (0.0, *n / 1000.0));
    for (auto suffix : { "seconds", "second", "secs", "sec", "s" })
        if (text.endsWith (suffix))
            if (auto n = number (text.dropLastCharacters ((int) strlen (suffix)))) return TimePosition::fromSeconds (std::max (0.0, *n));
    if (text.containsChar (':'))
    {
        auto parts = juce::StringArray::fromTokens (text, ":", "");
        for (auto& p : parts) if (! number (p)) return {};
        double secs = 0;
        if (parts.size() == 4)   // timecode
            secs = parts[0].getDoubleValue() * 3600 + parts[1].getDoubleValue() * 60 + parts[2].getDoubleValue() + parts[3].getDoubleValue() / frameRate;
        else
            for (auto& p : parts) secs = secs * 60 + p.getDoubleValue();
        return TimePosition::fromSeconds (std::max (0.0, secs));
    }
    // Bars and beats: "bar 9 beat 3", "9 3", "9.3", or just "9".
    auto words = text.replace ("bar", " ").replace ("beat", " ").replace (",", " ").replace (".", " ");
    auto nums = juce::StringArray::fromTokens (words, " ", "");
    nums.removeEmptyStrings();
    if (nums.isEmpty() || nums.size() > 2) return {};
    for (auto& n : nums) if (! n.containsOnly ("0123456789")) return {};
    tempo::BarsAndBeats to;
    to.bars = std::max (0, nums[0].getIntValue() - 1);
    to.beats = BeatDuration::fromBeats (nums.size() > 1 ? std::max (0, nums[1].getIntValue() - 1) : 0);
    return tempo.toTime (to);
}

void AppCore::registerMovementCommands()
{
    auto add = [this] (Command c)
    {
        auto r = commandList.add (std::move (c));
        jassert (r.wasOk());
        juce::ignoreUnused (r);
    };
    auto needSong = [this] { return song == nullptr ? Availability::no ("No song is open. Press Command-N for a new one.") : Availability::yes(); };
    auto arrive = [this] (TimePosition t, juce::String prefix = {})
    {
        song->getEdit().getTransport().setPosition (std::max (TimePosition(), t));
        auto s = positionSentence();
        return prefix.isEmpty() ? CommandResult::done (s) : CommandResult::done (prefix + " " + s, prefix);
    };

    auto unitArg = [] (const char* name, juce::String def)
    {
        ArgSpec a; a.name = name; a.type = ArgSpec::Type::choice; a.choices = AppCore::stepUnitNames(); a.defaultValue = def;
        return a;
    };
    {
        Command c; c.id = "transport.step-forward"; c.name = "Step forward"; c.category = "Transport";
        c.description = "Moves the playhead forward one step. The step can be anything from a sample to a minute or 16 bars: Finer step and Coarser step change it.";
        c.tags = { "move", "nudge", "right", "navigate" };
        c.available = needSong;
        c.run = [this, arrive] (const CommandArgs&) { return arrive (moveBy (song->getEdit().getTransport().getPosition(), stepUnit, 1)); };
        add (c);
    }
    {
        Command c; c.id = "transport.step-back"; c.name = "Step back"; c.category = "Transport";
        c.description = "Moves the playhead back one step.";
        c.tags = { "move", "nudge", "left", "navigate" };
        c.available = needSong;
        c.run = [this, arrive] (const CommandArgs&) { return arrive (moveBy (song->getEdit().getTransport().getPosition(), stepUnit, -1)); };
        add (c);
    }
    auto changeStep = [this] (int direction)
    {
        auto names = stepUnitNames();
        const int i = juce::jlimit (0, names.size() - 1, names.indexOf (stepUnit) + direction);
        if (names[i] == stepUnit) return CommandResult::done ((direction < 0 ? "Finest step already: " : "Coarsest step already: ") + stepUnit + ".");
        stepUnit = names[i];
        saveAppSettings();
        if (onViewChanged) onViewChanged();
        return CommandResult::done ("Step: " + stepUnit + ".", stepUnit + ".");
    };
    {
        Command c; c.id = "transport.finer-step"; c.name = "Finer step"; c.category = "Transport";
        c.description = "Makes each step smaller, down to one sample, and zooms the timeline in.";
        c.tags = { "zoom in", "resolution", "smaller", "precision" };
        c.run = [changeStep] (const CommandArgs&) { return changeStep (-1); };
        add (c);
    }
    {
        Command c; c.id = "transport.coarser-step"; c.name = "Coarser step"; c.category = "Transport";
        c.description = "Makes each step bigger, up to a minute or 16 bars, and zooms the timeline out.";
        c.tags = { "zoom out", "resolution", "bigger" };
        c.run = [changeStep] (const CommandArgs&) { return changeStep (1); };
        add (c);
    }
    {
        Command c; c.id = "transport.step-size"; c.name = "Step size"; c.category = "Transport";
        c.description = "Chooses how far each step moves: samples, ticks, milliseconds, frames, parts of a beat, beats, seconds, bars or minutes.";
        c.tags = { "grid", "resolution", "nudge", "unit" };
        auto a = unitArg ("step", "Beat");
        a.currentValue = [this]() -> juce::var { return stepUnit; };
        c.args = { a };
        c.askForSettings = true;
        c.run = [this] (const CommandArgs& args)
        {
            stepUnit = args["step"].toString();
            saveAppSettings();
            if (onViewChanged) onViewChanged();
            return CommandResult::done ("Step: " + stepUnit + ".");
        };
        add (c);
    }
    {
        Command c; c.id = "transport.move"; c.name = "Move by"; c.category = "Transport";
        c.description = "Moves the playhead by any amount of any unit: 3 bars, -250 milliseconds, 12 frames, 480 samples.";
        c.tags = { "nudge", "offset", "jump", "relative" };
        ArgSpec amount; amount.name = "amount"; amount.defaultValue = 1.0;
        auto unit = unitArg ("unit", "Bar");
        unit.currentValue = [this]() -> juce::var { return stepUnit; };
        c.args = { amount, unit };
        c.askForSettings = true;
        c.available = needSong;
        c.run = [this, arrive] (const CommandArgs& args)
        {
            const double amount = args["amount"];
            const auto unit = args["unit"].toString();
            const auto* u = findUnit (unit);
            if (u == nullptr) return CommandResult::failed ("There's no unit called " + unit + ".");
            auto at = song->getEdit().getTransport().getPosition();
            // Whole steps snap to the grid; a fraction is an exact offset.
            if (amount == std::floor (amount)) at = moveBy (at, unit, amount);
            else
            {
                const auto start = moveBy (at, unit, 0);
                const auto one = moveBy (at, unit, amount >= 0 ? 1 : -1);
                at = at + (one - start) * std::abs (amount);
            }
            return arrive (at);
        };
        add (c);
    }
    {
        Command c; c.id = "transport.go-to"; c.name = "Go to"; c.category = "Transport";
        c.description = "Moves the playhead to a place written any way: 9 or 9.3 for a bar and beat, 1:23.5 for minutes and seconds, 83s, 250ms, 48000 samples, or timecode like 00:01:02:12.";
        c.tags = { "jump", "locate", "time", "timecode", "samples", "position" };
        ArgSpec a; a.name = "place"; a.type = ArgSpec::Type::text; a.defaultValue = "";
        a.currentValue = [this]() -> juce::var
        {
            if (song == nullptr) return "";
            auto bb = song->getEdit().tempoSequence.toBarsAndBeats (song->getEdit().getTransport().getPosition());
            return juce::String (bb.bars + 1) + "." + juce::String (bb.getWholeBeats() + 1);
        };
        c.args = { a };
        c.askForSettings = true;
        c.available = needSong;
        c.run = [this, arrive] (const CommandArgs& args)
        {
            auto where = parsePlace (args["place"].toString());
            if (! where) return CommandResult::failed ("I couldn't read \"" + args["place"].toString() + "\" as a place. Try 9.3, 1:23.5, 83s, 250ms, 48000 samples or 00:01:02:12.");
            return arrive (*where);
        };
        add (c);
    }
    {
        Command c; c.id = "report.position-detail"; c.name = "Where am I, every way"; c.category = "Reports"; c.isReport = true;
        c.description = "Says the playhead's place as bar, beat and tick, minutes and seconds, the sample, and timecode. Changes nothing.";
        c.tags = { "position", "samples", "timecode", "ticks", "exact" };
        c.available = needSong;
        c.run = [this] (const CommandArgs&) { return CommandResult::done (positionInEveryUnit()); };
        add (c);
    }
    {
        Command c; c.id = "transport.frame-rate"; c.name = "Frame rate"; c.category = "Transport";
        c.description = "The frame rate for stepping by frames and for timecode, to match a video.";
        c.tags = { "video", "fps", "timecode", "film" };
        ArgSpec a; a.name = "frames per second"; a.type = ArgSpec::Type::choice;
        a.choices = { "23.976", "24", "25", "29.97", "30", "48", "50", "59.94", "60" };
        a.defaultValue = "30";
        a.currentValue = [this]() -> juce::var { return juce::String (frameRate, 3).trimCharactersAtEnd ("0").trimCharactersAtEnd ("."); };
        c.args = { a };
        c.askForSettings = true;
        c.run = [this] (const CommandArgs& args)
        {
            frameRate = args["frames per second"].toString().getDoubleValue();
            if (frameRate <= 0) frameRate = 30.0;
            saveAppSettings();
            return CommandResult::done (args["frames per second"].toString() + " frames a second.");
        };
        add (c);
    }

    // Edges: anything with a start or end, on any track, and every marker,
    // loop point and tempo or meter change, nearest first.
    auto edges = [this] (bool changesOnly)
    {
        std::vector<std::pair<double, juce::String>> out;
        auto& e = song->getEdit();
        auto& tempo = e.tempoSequence;
        if (! changesOnly)
        {
            for (auto* t : contentTracks (e))
                for (auto* clip : t->getClips())
                {
                    out.push_back ({ clip->getPosition().getStart().inSeconds(), "Start of " + clip->getName() + " on " + t->getName() });
                    out.push_back ({ clip->getPosition().getEnd().inSeconds(), "End of " + clip->getName() + " on " + t->getName() });
                }
            for (auto* m : e.getMarkerManager().getMarkers())
                out.push_back ({ m->getPosition().getStart().inSeconds(), m->getName() });
            if (e.getTransport().looping.get())
            {
                auto lr = e.getTransport().getLoopRange();
                out.push_back ({ lr.getStart().inSeconds(), "Loop start" });
                out.push_back ({ lr.getEnd().inSeconds(), "Loop end" });
            }
            out.push_back ({ 0.0, "Start of the song" });
            out.push_back ({ e.getLength().inSeconds(), "End of the song" });
        }
        for (int i = 1; i < tempo.getNumTempos(); ++i)
            if (auto* t = tempo.getTempo (i))
                out.push_back ({ t->getStartTime().inSeconds(), "Tempo " + juce::String (t->getBpm(), 1).trimCharactersAtEnd ("0").trimCharactersAtEnd (".") });
        for (int i = 1; i < tempo.getNumTimeSigs(); ++i)
            if (auto* ts = tempo.getTimeSig (i))
                out.push_back ({ tempo.toTime (ts->getStartBeat()).inSeconds(), "Time signature " + juce::String (ts->numerator.get()) + "/" + juce::String (ts->denominator.get()) });
        return out;
    };
    auto toEdge = [this, edges, arrive] (int direction, bool changesOnly)
    {
        const auto now = song->getEdit().getTransport().getPosition().inSeconds();
        auto list = edges (changesOnly);
        double best = direction > 0 ? 1.0e300 : -1.0;
        juce::StringArray names;
        for (auto& [at, name] : list)
        {
            const bool ahead = direction > 0 ? at > now + 1.0e-6 : at < now - 1.0e-6;
            if (! ahead) continue;
            if (std::abs (at - best) < 1.0e-6) names.addIfNotAlreadyThere (name);
            else if (direction > 0 ? at < best : at > best) { best = at; names = { name }; }
        }
        if (names.isEmpty())
            return CommandResult::done (changesOnly ? (direction > 0 ? "No more tempo or meter changes." : "No tempo or meter changes before this.")
                                                    : (direction > 0 ? "Nothing more after this." : "Nothing before this."));
        return arrive (TimePosition::fromSeconds (best), names.joinIntoString (", ") + ".");
    };
    {
        Command c; c.id = "transport.next-edge"; c.name = "Next edge"; c.category = "Transport";
        c.description = "Moves to the next place something starts or ends: a clip on any track, a marker, the loop, a tempo or meter change, and says what it is.";
        c.tags = { "transient", "boundary", "item", "region", "tab" };
        c.available = needSong;
        c.run = [toEdge] (const CommandArgs&) { return toEdge (1, false); };
        add (c);
    }
    {
        Command c; c.id = "transport.previous-edge"; c.name = "Previous edge"; c.category = "Transport";
        c.description = "Moves to the previous place something starts or ends, and says what it is.";
        c.tags = { "transient", "boundary", "item", "region" };
        c.available = needSong;
        c.run = [toEdge] (const CommandArgs&) { return toEdge (-1, false); };
        add (c);
    }
    {
        Command c; c.id = "transport.next-change"; c.name = "Next tempo or meter change"; c.category = "Transport";
        c.description = "Moves to the next tempo or time signature change.";
        c.tags = { "tempo", "time signature", "meter" };
        c.available = needSong;
        c.run = [toEdge] (const CommandArgs&) { return toEdge (1, true); };
        add (c);
    }
    {
        Command c; c.id = "transport.previous-change"; c.name = "Previous tempo or meter change"; c.category = "Transport";
        c.description = "Moves to the previous tempo or time signature change.";
        c.tags = { "tempo", "time signature", "meter" };
        c.available = needSong;
        c.run = [toEdge] (const CommandArgs&) { return toEdge (-1, true); };
        add (c);
    }
}

} // namespace earfirst
