#include "app/AppCore.h"

namespace earfirst
{
using namespace tracktion;

namespace
{
struct AppEngineBehaviour : public EngineBehaviour
{
    explicit AppEngineBehaviour (bool device) : openDevice (device) {}
    bool autoInitialiseDeviceManager() override    { return openDevice; }
    bool openDevice;
};

ArgSpec numberArg (const char* name, double def, double lo, double hi, const char* unit)
{
    ArgSpec a; a.name = name; a.defaultValue = def; a.minimum = lo; a.maximum = hi; a.unit = unit;
    return a;
}

ArgSpec textArg (const char* name, const char* def)
{
    ArgSpec a; a.name = name; a.type = ArgSpec::Type::text; a.defaultValue = def;
    return a;
}
} // namespace

AppCore::AppCore (SpeechOutput& out, Options o)
    : engine (std::make_unique<PropertyStorage> ("Ear First"), std::make_unique<UIBehaviour>(),
              std::make_unique<AppEngineBehaviour> (o.openAudioDevice)),
      options (std::move (o)),
      speaker (out)
{
    if (options.songsFolder == juce::File())
        options.songsFolder = juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("Ear First Songs");

    if (options.openAudioDevice)
    {
        // Open every input the interface has; capture listens to all of them.
        // Capture belongs to the app, not a song: it runs from launch, across
        // songs. Anything left from a run that didn't close cleanly is found
        // first, before a new run starts.
        auto captureRoot = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                               .getChildFile ("Ear First").getChildFile ("Capture");
        captureRoot.createDirectory();
        for (auto& run : captureRoot.findChildFiles (juce::File::findDirectories, false))
            if (CaptureInfo::load (run)) lastOpenReport.leftoverCaptures.add (run);
            else run.deleteRecursively();
        engine.getDeviceManager().initialise (1024, 2);
        capture = std::make_unique<EngineCapture> (engine, captureRoot);
    }

    updates = Updater::createForThisPlatform ({});
    updates->isSafeToInstall = [this] { return ! isRecordingOrCapturing() && (song == nullptr || ! song->hasUnsavedChanges()); };
    updates->onUpdateFound = [this] (const juce::String& v)
    {
        speaker.say ("Ear First " + v + " is out. Check for Updates when you're ready.", Speech::Priority::normal, "update");
    };

    registerCommands();
    setDefaultKeys();

    juce::StringArray moved;
    if (auto f = keymapFile(); f.existsAsFile())
        if (auto r = keys.loadUserChanges (juce::JSON::parse (f), moved); r.failed())
            speaker.say (r.getErrorMessage(), Speech::Priority::urgent);
    for (auto& m : moved) speaker.say (m);
    if (! moved.isEmpty()) saveKeymap();

    startTimerHz (20);
}

AppCore::~AppCore()
{
    stopTimer();
    if (capture != nullptr) capture->attachEdit (nullptr);
    capture.reset();
    song.reset();
}

juce::String AppCore::startupSentence() const
{
    if (lastOpenReport.leftoverCaptures.isEmpty()) return {};
    double secs = 0;
    for (auto& run : lastOpenReport.leftoverCaptures)
        if (auto info = CaptureInfo::load (run))
        {
            CaptureStore store (run, *info);
            secs += (double) (store.endSampleOnDisk() - store.firstSampleOnDisk()) / info->sampleRate;
        }
    auto mins = juce::roundToInt (secs / 60.0);
    return "The app didn't close properly last time. "
           + (secs < 90 ? juce::String (juce::roundToInt (secs)) + " seconds" : juce::String (mins) + " minutes")
           + " of captured audio was never kept. Use Keep recovered audio, or Throw away recovered audio.";
}

juce::File AppCore::keymapFile() const
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("Ear First").getChildFile ("Keys.json");
}

void AppCore::saveKeymap()
{
    auto f = keymapFile();
    f.getParentDirectory().createDirectory();
    f.replaceWithText (juce::JSON::toString (keys.userChangesToJson()));
}

void AppCore::speak (const juce::String& s, bool urgent)
{
    speaker.say (s, urgent ? Speech::Priority::urgent : Speech::Priority::normal);
}

void AppCore::beginUndoStep (const juce::String& name)
{
    if (song != nullptr) song->getEdit().getUndoManager().beginNewTransaction (name);
}

void AppCore::endUndoStep()
{
    if (song != nullptr) song->getEdit().getUndoManager().beginNewTransaction();
}

bool AppCore::isRecordingOrCapturing() const
{
    return song != nullptr && song->getEdit().getTransport().isRecording();
}

void AppCore::timerCallback()
{
    const double now = juce::Time::getMillisecondCounterHiRes() / 1000.0;
    const bool playing = song != nullptr && song->getEdit().getTransport().isPlaying();
    if (playing != wasPlaying) { speaker.setPlaying (playing); wasPlaying = playing; }
    speaker.tick (now);

    if (song != nullptr && now - lastAutosave > 30.0)
    {
        lastAutosave = now;
        song->autosaveIfNeeded();
    }
}

//==============================================================================
juce::String AppCore::newSong (const juce::String& name)
{
    if (song != nullptr && song->hasUnsavedChanges())
        return "Save or close " + song->getName() + " first. It has unsaved changes.";
    auto clean = juce::File::createLegalFileName (name.trim().isEmpty() ? juce::String ("Untitled") : name.trim());
    auto folder = options.songsFolder.getChildFile (clean);
    for (int n = 2; folder.exists(); ++n) folder = options.songsFolder.getChildFile (clean + " " + juce::String (n));
    options.songsFolder.createDirectory();

    closeSong();
    juce::String error;
    song = Session::create (engine, folder, error);
    if (song == nullptr) return error;
    song->getEdit().ensureNumberOfAudioTracks (1);
    song->save();
    if (capture != nullptr) { capture->attachEdit (&song->getEdit()); }
    selectedTrack = 0;
    if (onSongChanged) onSongChanged();
    return "New song, " + folder.getFileName() + ". Everything you play is being kept in the background; press K to keep the last 30 seconds.";
}

juce::String AppCore::openSong (const juce::File& folder)
{
    if (song != nullptr && song->hasUnsavedChanges())
        return "Save or close " + song->getName() + " first. It has unsaved changes.";
    closeSong();
    juce::String error;
    Session::OpenReport report;
    song = Session::open (engine, folder, report, error);
    lastOpenReport.leftoverCaptures.addArray (report.leftoverCaptures);
    lastOpenReport.sentence = report.sentence;
    if (song == nullptr) return error;
    if (capture != nullptr) capture->attachEdit (&song->getEdit());
    selectedTrack = 0;
    if (onSongChanged) onSongChanged();
    auto tracks = getAudioTracks (song->getEdit()).size();
    juce::String s = "Opened " + song->getName() + ", " + juce::String (tracks) + (tracks == 1 ? " track." : " tracks.");
    if (report.sentence.isNotEmpty()) s << " " << report.sentence;
    return s;
}

juce::String AppCore::closeSong()
{
    if (song == nullptr) return "No song is open.";
    auto name = song->getName();
    if (capture != nullptr) capture->attachEdit (nullptr);
    song.reset();
    if (onSongChanged) onSongChanged();
    return "Closed " + name + ".";
}

juce::String AppCore::positionSentence() const
{
    if (song == nullptr) return "No song is open.";
    auto& edit = song->getEdit();
    auto t = edit.getTransport().getPosition();
    auto bb = edit.tempoSequence.toBarsAndBeats (t);
    return "Bar " + juce::String (bb.bars + 1) + ", beat " + juce::String (bb.getWholeBeats() + 1) + ", "
           + juce::String (t.inSeconds(), 1) + " seconds.";
}

//==============================================================================
void AppCore::registerCommands()
{
    auto needSong = [this] { return song == nullptr ? Availability::no ("No song is open. Press Command-N for a new one.") : Availability::yes(); };
    auto add = [this] (Command c)
    {
        auto r = commandList.add (std::move (c));
        jassert (r.wasOk());
        juce::ignoreUnused (r);
    };

    // File
    {
        Command c; c.id = "file.new-song"; c.name = "New Song"; c.category = "File";
        c.description = "Makes a new song in its own folder, ready to record.";
        c.args = { textArg ("name", "Untitled") };
        c.tags = { "create", "project", "session" };
        c.run = [this] (const CommandArgs& a) { auto s = newSong (a["name"].toString()); return song ? CommandResult::done (s) : CommandResult::failed (s); };
        add (c);
    }
    {
        Command c; c.id = "file.open-song"; c.name = "Open Song"; c.category = "File";
        c.description = "Opens a song folder.";
        c.tags = { "project", "session", "load" };
        c.args = { textArg ("folder", "") };
        c.run = [this] (const CommandArgs& a)
        {
            if (a["folder"].toString().isNotEmpty())
            {
                auto s = openSong (juce::File (a["folder"].toString()));
                return song ? CommandResult::done (s) : CommandResult::failed (s);
            }
            if (onOpenSongRequested) onOpenSongRequested();
            return CommandResult::done();
        };
        add (c);
    }
    {
        Command c; c.id = "file.save"; c.name = "Save"; c.category = "File";
        c.description = "Saves the song.";
        c.available = needSong;
        c.run = [this] (const CommandArgs&) { auto s = song->save(); return s == "Saved." ? CommandResult::done (s) : CommandResult::failed (s); };
        add (c);
    }
    {
        Command c; c.id = "file.close-song"; c.name = "Close Song"; c.category = "File";
        c.description = "Closes the song. If it has unsaved changes, save first.";
        c.available = [this, needSong]
        {
            if (song == nullptr) return needSong();
            return song->hasUnsavedChanges() ? Availability::no (song->getName() + " has unsaved changes. Save first, with Command-S.") : Availability::yes();
        };
        c.run = [this] (const CommandArgs&) { return CommandResult::done (closeSong()); };
        add (c);
    }

    // Edit
    {
        Command c; c.id = "edit.undo"; c.name = "Undo"; c.category = "Edit";
        c.description = "Undoes the last change.";
        c.available = [this, needSong]
        {
            if (song == nullptr) return needSong();
            return song->getEdit().getUndoManager().canUndo() ? Availability::yes() : Availability::no ("Nothing to undo.");
        };
        c.run = [this] (const CommandArgs&)
        {
            auto& um = song->getEdit().getUndoManager();
            auto what = um.getUndoDescription();
            um.undo();
            if (onSongChanged) onSongChanged();
            return CommandResult::done ("Undid " + (what.isNotEmpty() ? what : juce::String ("the last change")) + ".");
        };
        add (c);
    }
    {
        Command c; c.id = "edit.redo"; c.name = "Redo"; c.category = "Edit";
        c.description = "Does again what you just undid.";
        c.available = [this, needSong]
        {
            if (song == nullptr) return needSong();
            return song->getEdit().getUndoManager().canRedo() ? Availability::yes() : Availability::no ("Nothing to redo.");
        };
        c.run = [this] (const CommandArgs&)
        {
            auto& um = song->getEdit().getUndoManager();
            auto what = um.getRedoDescription();
            um.redo();
            if (onSongChanged) onSongChanged();
            return CommandResult::done ("Redid " + (what.isNotEmpty() ? what : juce::String ("the change")) + ".");
        };
        add (c);
    }

    // Transport
    {
        Command c; c.id = "transport.play-stop"; c.name = "Play"; c.category = "Transport";
        c.description = "Starts the song from the playhead, or stops it.";
        c.tags = { "stop", "start", "pause" };
        c.onWord = "Playing"; c.offWord = "Stopped"; c.sayNameWithState = false;
        c.available = needSong;
        c.toggleState = [this] { return song ? std::optional<bool> (song->getEdit().getTransport().isPlaying()) : std::optional<bool>(); };
        c.run = [this] (const CommandArgs&)
        {
            auto& t = song->getEdit().getTransport();
            if (t.isPlaying()) t.stop (false, false); else t.play (false);
            return CommandResult::done();
        };
        c.name = "Play or stop";
        add (c);
    }
    {
        Command c; c.id = "transport.go-to-start"; c.name = "Go to start"; c.category = "Transport";
        c.description = "Moves the playhead to the beginning of the song.";
        c.tags = { "home", "beginning", "rewind" };
        c.available = needSong;
        c.run = [this] (const CommandArgs&) { song->getEdit().getTransport().setPosition (TimePosition()); return CommandResult::done ("Start."); };
        add (c);
    }
    {
        Command c; c.id = "transport.metronome"; c.name = "Metronome"; c.category = "Transport";
        c.description = "Turns the click on or off.";
        c.tags = { "click", "count" };
        c.available = needSong;
        c.toggleState = [this] { return song ? std::optional<bool> (song->getEdit().clickTrackEnabled.get()) : std::optional<bool>(); };
        c.run = [this] (const CommandArgs&) { auto& e = song->getEdit(); e.clickTrackEnabled = ! e.clickTrackEnabled.get(); return CommandResult::done(); };
        add (c);
    }
    {
        Command c; c.id = "transport.loop"; c.name = "Loop"; c.category = "Transport";
        c.description = "Turns looping on or off.";
        c.tags = { "cycle", "repeat" };
        c.available = needSong;
        c.toggleState = [this] { return song ? std::optional<bool> (song->getEdit().getTransport().looping.get()) : std::optional<bool>(); };
        c.run = [this] (const CommandArgs&) { auto& t = song->getEdit().getTransport(); t.looping = ! t.looping.get(); return CommandResult::done(); };
        add (c);
    }

    // Capture
    {
        Command c; c.id = "capture.keep"; c.name = "Keep"; c.category = "Capture";
        c.description = "Keeps what you just played, on every input, lined up where you played it.";
        c.tags = { "record", "take", "capture", "save what i played" };
        c.args = { numberArg ("seconds", 30, 1, 24 * 3600, "seconds") };
        c.available = [this, needSong]
        {
            if (song == nullptr) return needSong();
            if (capture == nullptr || capture->getRecorder() == nullptr)
                return Availability::no ("Nothing is being captured. No audio inputs are open.");
            return Availability::yes();
        };
        c.undoable = true;
        c.run = [this] (const CommandArgs& a)
        {
            auto k = capture->keepLast ((double) a["seconds"], song->audioFolder());
            if (onSongChanged) onSongChanged();
            return k.take.ok() ? CommandResult::done (k.sentence) : CommandResult::failed (k.sentence);
        };
        add (c);
    }
    {
        Command c; c.id = "capture.status"; c.name = "Capture status"; c.category = "Capture"; c.isReport = true;
        c.description = "Says how long capture has been running, how much room is left, and whether anything dropped.";
        c.run = [this] (const CommandArgs&)
        {
            if (capture == nullptr || capture->getRecorder() == nullptr)
                return CommandResult::done ("Nothing is being captured. No audio inputs are open.");
            return CommandResult::done (capture->getRecorder()->status());
        };
        add (c);
    }
    {
        Command c; c.id = "capture.keep-recovered"; c.name = "Keep recovered audio"; c.category = "Capture";
        c.description = "Keeps audio that was captured before the app closed unexpectedly.";
        c.available = [this] { return lastOpenReport.leftoverCaptures.isEmpty() ? Availability::no ("There's no recovered audio waiting.") : Availability::yes(); };
        c.run = [this] (const CommandArgs&)
        {
            // Into the open song's Audio folder, or Recovered Audio beside your songs.
            juce::StringArray said;
            auto dest = song != nullptr ? song->audioFolder().getChildFile ("Recovered")
                                        : options.songsFolder.getChildFile ("Recovered Audio");
            for (auto& run : lastOpenReport.leftoverCaptures)
            {
                auto info = CaptureInfo::load (run);
                if (! info) continue;
                CaptureStore store (run, *info);
                juce::Array<int> all;
                for (int i = 0; i < info->numInputs(); ++i) all.add (i);
                auto r = store.exportTake (store.firstSampleOnDisk(), store.endSampleOnDisk(), all,
                                           dest.getChildFile (info->started.formatted ("%Y-%m-%d %H-%M-%S")), "Recovered");
                if (r.ok()) { run.deleteRecursively(); said.add ("Kept " + juce::String (r.files.size()) + " files."); }
                else said.add (r.error);
            }
            lastOpenReport.leftoverCaptures.clear();
            return CommandResult::done (said.joinIntoString (" ") + " They're in " + dest.getFileName() + ".");
        };
        add (c);
    }
    {
        Command c; c.id = "capture.discard-recovered"; c.name = "Throw away recovered audio"; c.category = "Capture";
        c.description = "Throws away audio that was captured before the app closed unexpectedly.";
        c.available = [this] { return lastOpenReport.leftoverCaptures.isEmpty() ? Availability::no ("There's no recovered audio waiting.") : Availability::yes(); };
        c.run = [this] (const CommandArgs&)
        {
            for (auto& run : lastOpenReport.leftoverCaptures) run.deleteRecursively();
            lastOpenReport.leftoverCaptures.clear();
            return CommandResult::done ("Thrown away.");
        };
        add (c);
    }

    // Tracks
    {
        Command c; c.id = "tracks.add"; c.name = "Add track"; c.category = "Tracks";
        c.description = "Adds an audio track after the selected one.";
        c.tags = { "new track", "insert" };
        c.available = needSong;
        c.undoable = true;
        c.run = [this] (const CommandArgs&)
        {
            auto& e = song->getEdit();
            auto tracks = getAudioTracks (e);
            auto* after = tracks.isEmpty() ? nullptr : tracks[juce::jlimit (0, tracks.size() - 1, selectedTrack)];
            auto t = e.insertNewAudioTrack (TrackInsertPoint (nullptr, after), nullptr);
            selectedTrack = getAudioTracks (e).indexOf (t.get());
            if (onSongChanged) onSongChanged();
            return CommandResult::done ("Added " + (t ? t->getName() : juce::String ("a track")) + ".");
        };
        add (c);
    }
    auto moveTrack = [this] (int delta)
    {
        auto tracks = getAudioTracks (song->getEdit());
        if (tracks.isEmpty()) return CommandResult::failed ("There are no tracks.");
        const int next = selectedTrack + delta;
        if (next < 0 || next >= tracks.size())
            return CommandResult::done (delta < 0 ? "First track. " + tracks[0]->getName() : "Last track. " + tracks.getLast()->getName());
        selectedTrack = next;
        if (onSongChanged) onSongChanged();
        return CommandResult::done (tracks[next]->getName() + ", " + juce::String (next + 1) + " of " + juce::String (tracks.size()) + ".");
    };
    {
        Command c; c.id = "tracks.next"; c.name = "Next track"; c.category = "Tracks";
        c.description = "Moves to the next track and says its name.";
        c.available = needSong;
        c.run = [moveTrack] (const CommandArgs&) { return moveTrack (1); };
        add (c);
    }
    {
        Command c; c.id = "tracks.previous"; c.name = "Previous track"; c.category = "Tracks";
        c.description = "Moves to the previous track and says its name.";
        c.available = needSong;
        c.run = [moveTrack] (const CommandArgs&) { return moveTrack (-1); };
        add (c);
    }

    // Reports
    {
        Command c; c.id = "report.position"; c.name = "Where am I"; c.category = "Reports"; c.isReport = true;
        c.description = "Says where the playhead is: bar, beat and seconds. Changes nothing.";
        c.tags = { "position", "time", "playhead", "location" };
        c.run = [this] (const CommandArgs&) { return CommandResult::done (positionSentence()); };
        add (c);
    }
    {
        Command c; c.id = "report.song"; c.name = "About this song"; c.category = "Reports"; c.isReport = true;
        c.description = "Says the song's name, how many tracks it has, and whether it's saved. Changes nothing.";
        c.tags = { "info", "summary", "session" };
        c.run = [this] (const CommandArgs&)
        {
            if (song == nullptr) return CommandResult::done ("No song is open.");
            auto n = getAudioTracks (song->getEdit()).size();
            return CommandResult::done (song->getName() + ", " + juce::String (n) + (n == 1 ? " track, " : " tracks, ")
                                        + (song->hasUnsavedChanges() ? "with unsaved changes." : "saved."));
        };
        add (c);
    }

    // Speech
    {
        Command c; c.id = "speech.say-again"; c.name = "Say that again"; c.category = "Speech"; c.isReport = true;
        c.description = "Repeats the last thing the app said.";
        c.tags = { "repeat", "what" };
        c.run = [this] (const CommandArgs&) { speaker.repeatLast(); return CommandResult::done(); };
        add (c);
    }
    {
        Command c; c.id = "speech.previous-message"; c.name = "Previous message"; c.category = "Speech"; c.isReport = true;
        c.description = "Steps back through what the app has said.";
        c.tags = { "history" };
        c.run = [this] (const CommandArgs&) { speaker.historyBack(); return CommandResult::done(); };
        add (c);
    }
    {
        Command c; c.id = "speech.next-message"; c.name = "Next message"; c.category = "Speech"; c.isReport = true;
        c.description = "Steps forward through what the app has said.";
        c.tags = { "history" };
        c.run = [this] (const CommandArgs&) { speaker.historyForward(); return CommandResult::done(); };
        add (c);
    }
    {
        Command c; c.id = "speech.read-waiting"; c.name = "Read waiting messages"; c.category = "Speech"; c.isReport = true;
        c.description = "Says everything that's waiting for a gap in the music, now.";
        c.run = [this] (const CommandArgs&) { speaker.readWaiting(); return CommandResult::done(); };
        add (c);
    }
    {
        Command c; c.id = "speech.stop"; c.name = "Stop speaking"; c.category = "Speech"; c.isReport = true;
        c.description = "Stops the current message and drops anything waiting.";
        c.tags = { "quiet", "silence", "shut up" };
        c.run = [this] (const CommandArgs&) { speaker.stopSpeaking(); return CommandResult::done(); };
        add (c);
    }
    {
        Command c; c.id = "speech.wait-for-gaps"; c.name = "Wait for gaps"; c.category = "Speech";
        c.description = "While the song plays, holds messages until a quiet moment in the music. Urgent ones always cut in.";
        c.toggleState = [this] { return std::optional<bool> (speaker.settings.waitForGaps); };
        c.run = [this] (const CommandArgs&) { speaker.settings.waitForGaps = ! speaker.settings.waitForGaps; return CommandResult::done(); };
        add (c);
    }

    // Help (key help and the palette live in the window; their commands are registered there)
    {
        Command c; c.id = "help.export-keys"; c.name = "Save list of keys"; c.category = "Help";
        c.description = "Saves every key and what it does, by category, as a text file in your Documents folder.";
        c.tags = { "shortcuts", "keyboard", "keymap", "cheat sheet" };
        c.run = [this] (const CommandArgs&)
        {
            auto f = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("Ear First keys.txt");
            f.replaceWithText (keys.exportReadable());
            return CommandResult::done ("Saved the list of keys to Documents, Ear First keys.");
        };
        add (c);
    }
    {
        Command c; c.id = "app.check-for-updates"; c.name = "Check for Updates"; c.category = "Help";
        c.description = "Looks for a newer version of the app.";
        c.tags = { "update", "new version", "upgrade" };
        c.run = [this] (const CommandArgs&) { return CommandResult::done (updates->checkWithWindow()); };
        add (c);
    }
}

void AppCore::setDefaultKeys()
{
    keys.addScope ("main", "global", "");                     // the main window: no label needed
    keys.addScope ("tracks", "main", "in the track list");

    auto k = [] (const char* keysText, const char* id, const char* scope = "global", CommandArgs args = {})
    {
        return KeyBinding { keysText, id, scope, std::move (args) };
    };
    CommandArgs keep2min; keep2min.set ("seconds", 120);

    keys.setDefaults ({
        k ("Primary+N", "file.new-song"),
        k ("Primary+O", "file.open-song"),
        k ("Primary+S", "file.save"),
        k ("Primary+W", "file.close-song"),
        k ("Primary+Z", "edit.undo"),
        k ("Primary+Shift+Z", "edit.redo"),
        k ("Space", "transport.play-stop", "main"),
        k ("Home", "transport.go-to-start", "main"),
        k ("W", "transport.go-to-start", "main"),
        k ("C", "transport.metronome", "main"),
        k ("L", "transport.loop", "main"),
        k ("K", "capture.keep", "main"),
        k ("Shift+K", "capture.keep", "main", keep2min),
        k ("Primary+Shift+C", "capture.status"),
        k ("Primary+T", "tracks.add"),
        k ("Down", "tracks.next", "tracks"),
        k ("Up", "tracks.previous", "tracks"),
        k ("Primary+Shift+L", "report.position"),
        k ("Primary+Shift+I", "report.song"),
        k ("Primary+Shift+R", "speech.say-again"),
        k ("Primary+Shift+Comma", "speech.previous-message"),
        k ("Primary+Shift+Period", "speech.next-message"),
        k ("Primary+Shift+W", "speech.read-waiting"),
        k ("Escape", "speech.stop", "main"),
    }, keymapDefaultsVersion);
}

} // namespace earfirst
