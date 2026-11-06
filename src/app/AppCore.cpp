#include "app/AppCore.h"
#include "session/Tracks.h"
#include "click/ClickPlugin.h"

namespace earfirst
{
using namespace tracktion;

juce::File AppCore::appDataFolder()
{
    // JUCE's "application data" is ~/Library on the Mac; apps belong in
    // ~/Library/Application Support. On Windows it's already AppData\Roaming.
    auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
   #if JUCE_MAC
    base = base.getChildFile ("Application Support");
   #endif
    return base.getChildFile ("Ear First");
}

namespace
{
// The engine's own settings and cache go in the app's folder too.
struct AppPropertyStorage : public PropertyStorage
{
    AppPropertyStorage() : PropertyStorage ("Ear First") {}
    juce::File getAppCacheFolder() override  { return AppCore::appDataFolder().getChildFile ("Cache"); }
    juce::File getAppPrefsFolder() override  { return AppCore::appDataFolder(); }
};

struct AppEngineBehaviour : public EngineBehaviour
{
    explicit AppEngineBehaviour (bool device) : openDevice (device) {}
    // Never: the app checks the interface answers first, then opens it itself.
    bool autoInitialiseDeviceManager() override    { return false; }
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
    : engine (std::make_unique<AppPropertyStorage>(), std::make_unique<UIBehaviour>(),
              std::make_unique<AppEngineBehaviour> (o.openAudioDevice)),
      options (std::move (o)),
      speaker (out)
{
    engine.getPluginManager().createBuiltInType<ClickPlugin>();

    if (options.songsFolder == juce::File())
        options.songsFolder = juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("Ear First Songs");

    if (options.openAudioDevice)
    {
        // Open every input the interface has; capture listens to all of them.
        // Capture belongs to the app, not a song: it runs from launch, across
        // songs. Anything left from a run that didn't close cleanly is found
        // first, before a new run starts.
        auto captureRoot = appDataFolder().getChildFile ("Capture");
        captureRoot.createDirectory();
        for (auto& run : captureRoot.findChildFiles (juce::File::findDirectories, false))
            if (CaptureInfo::load (run)) lastOpenReport.leftoverCaptures.add (run);
            else run.deleteRecursively();
        // An interface that has stopped answering would freeze the app before
        // it could say a word (it happened with a StudioLive 64S, 6 November
        // 2026). So try the saved one in the background first; if it doesn't
        // answer in a few seconds, start on the system's own audio and say why.
        if (auto stuck = savedInterfaceThatWontAnswer(); stuck.isNotEmpty())
            startupProblem = stuck + " isn't answering, so Ear First is using other audio for now. "
                             "Turn the interface off and on, then choose it again with Record from.";
        engine.getDeviceManager().initialise (1024, 2);
        capture = std::make_unique<EngineCapture> (engine, captureRoot);
        capture->describePlace = [this] (const Edit& e, TimePosition t) { return placeText (e, t); };
        capture->onCaptureSetAside = [this] (const juce::File& run, double secs)
        {
            lastOpenReport.leftoverCaptures.addIfNotAlreadyThere (run);
            setAsideNote = "The " + juce::String (secs < 90 ? juce::roundToInt (secs) : juce::roundToInt (secs / 60.0))
                           + (secs < 90 ? " seconds" : " minutes") + " captured before the switch are kept. Keep recovered audio saves them.";
        };
    }

    updates = Updater::createForThisPlatform ({});
    updates->isSafeToInstall = [this] { return ! isRecordingOrCapturing() && (song == nullptr || ! song->hasUnsavedChanges()); };
    updates->onUpdateFound = [this] (const juce::String& v)
    {
        // Once per version, and only while the app is in front.
        if (v == announcedVersion || ! juce::Process::isForegroundProcess()) return;
        announcedVersion = v;
        saveAppSettings();
        say ("Ear First " + v + " is out. Check for Updates in the Help menu when you're ready.", "Updates", "Version " + v + " is out.", Speech::Priority::normal, "update");
    };

    registerCommands();
    setDefaultKeys();
    if (options.openAudioDevice) loadAppSettings();   // the self-test starts from defaults

    juce::StringArray moved;
    if (auto f = keymapFile(); options.saveSettings && f.existsAsFile())
        if (auto r = keys.loadUserChanges (juce::JSON::parse (f), moved); r.failed())
            say (r.getErrorMessage(), "Errors", {}, Speech::Priority::urgent);
    for (auto& m : moved) say (m, "Startup");
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

namespace
{
// Opens a device and asks it a question on a thread of its own, waiting up to
// `seconds`. A wedged driver can block that thread forever; it's left behind.
bool answers (const juce::String& type, const juce::String& out, const juce::String& in, double seconds)
{
    auto done = std::make_shared<std::atomic<bool>> (false);
    std::thread ([done, type, out, in]
    {
        juce::AudioDeviceManager scratch;
        juce::OwnedArray<juce::AudioIODeviceType> types;
        scratch.createAudioDeviceTypes (types);
        for (auto* t : types)
            if (type.isEmpty() || t->getTypeName() == type)
            {
                t->scanForDevices();
                if (std::unique_ptr<juce::AudioIODevice> d { t->createDevice (out, in) })
                    juce::ignoreUnused (d->getAvailableBufferSizes(), d->getAvailableSampleRates());
                break;
            }
        *done = true;
    }).detach();
    for (int i = 0; i < (int) (seconds * 20) && ! *done; ++i)
        juce::Thread::sleep (50);
    return *done;
}

// Tries every device at once and returns the first in the list that answers,
// so a stuck interface costs one short wait, not one per device.
juce::String firstThatAnswers (const juce::String& type, const juce::StringArray& names, bool inputs,
                               const juce::String& skip, double seconds)
{
    std::vector<std::shared_ptr<std::atomic<bool>>> done;
    for (auto& n : names)
    {
        auto flag = std::make_shared<std::atomic<bool>> (n == skip);
        done.push_back (flag);
        if (n == skip) { *flag = false; continue; }
        std::thread ([flag, type, n, inputs]
        {
            juce::AudioDeviceManager scratch;
            juce::OwnedArray<juce::AudioIODeviceType> types;
            scratch.createAudioDeviceTypes (types);
            for (auto* t : types)
                if (type.isEmpty() || t->getTypeName() == type)
                {
                    t->scanForDevices();
                    if (std::unique_ptr<juce::AudioIODevice> d { inputs ? t->createDevice ({}, n) : t->createDevice (n, {}) })
                        juce::ignoreUnused (d->getAvailableBufferSizes());
                    break;
                }
            *flag = true;
        }).detach();
    }
    for (int i = 0; i < (int) (seconds * 20); ++i)
    {
        juce::Thread::sleep (50);
        for (size_t k = 0; k < done.size(); ++k)
        {
            if (! *done[k]) break;          // keep list order: wait for earlier ones
            if (names[(int) k] != skip) return names[(int) k];
        }
    }
    for (size_t k = 0; k < done.size(); ++k)
        if (*done[k] && names[(int) k] != skip) return names[(int) k];
    return {};
}
} // namespace

juce::String AppCore::savedInterfaceThatWontAnswer()
{
    // What would be opened: the saved choice, or the system's defaults.
    juce::AudioDeviceManager scratch;
    juce::OwnedArray<juce::AudioIODeviceType> types;
    scratch.createAudioDeviceTypes (types);
    if (types.isEmpty()) return {};
    auto* type = types[0];
    type->scanForDevices();
    auto outs = type->getDeviceNames (false), ins = type->getDeviceNames (true);

    juce::String in, out;
    if (auto xml = engine.getPropertyStorage().getXmlProperty (SettingID::audio_device_setup))
    {
        in = xml->getStringAttribute ("audioInputDeviceName");
        out = xml->getStringAttribute ("audioOutputDeviceName");
    }
    if (in.isEmpty())  in  = ins[juce::jmax (0, type->getDefaultDeviceIndex (true))];
    if (out.isEmpty()) out = outs[juce::jmax (0, type->getDefaultDeviceIndex (false))];

    const bool outOk = out.isEmpty() || answers (type->getTypeName(), out, {}, 3.0);
    const bool inOk  = in.isEmpty() || in == out ? outOk : answers (type->getTypeName(), {}, in, 3.0);
    if (outOk && inOk) return {};

    // Something that answers instead: the first output that does, and the
    // first input that does (or none, rather than freeze).
    juce::String newOut = outOk ? out : firstThatAnswers (type->getTypeName(), outs, false, out, 2.0);
    juce::String newIn  = inOk  ? in  : firstThatAnswers (type->getTypeName(), ins, true, in, 2.0);

    juce::XmlElement setup ("DEVICESETUP");
    setup.setAttribute ("deviceType", type->getTypeName());
    setup.setAttribute ("audioOutputDeviceName", newOut);
    setup.setAttribute ("audioInputDeviceName", newIn);
    engine.getPropertyStorage().setXmlProperty (SettingID::audio_device_setup, setup);
    return ! outOk ? out : in;
}

juce::String AppCore::startupSentence() const
{
    if (startupProblem.isNotEmpty()) return startupProblem + (lastOpenReport.leftoverCaptures.isEmpty() ? juce::String() : " " + leftoverSentence());
    return leftoverSentence();
}

juce::String AppCore::leftoverSentence() const
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
    return appDataFolder().getChildFile ("Keys.json");
}

void AppCore::saveAppSettings()
{
    if (! options.saveSettings) return;
    auto* o = new juce::DynamicObject();
    o->setProperty ("textScale", textScale);
    o->setProperty ("waitForGaps", speaker.settings.waitForGaps);
    o->setProperty ("announcedVersion", announcedVersion);
    o->setProperty ("speech", speechPrefs.toVar());
    auto f = appDataFolder().getChildFile ("App.json");
    f.getParentDirectory().createDirectory();
    f.replaceWithText (juce::JSON::toString (juce::var (o)));
}

void AppCore::loadAppSettings()
{
    auto v = juce::JSON::parse (appDataFolder().getChildFile ("App.json"));
    if (! v.isObject()) return;
    textScale = juce::jlimit (0.75f, 3.0f, (float) (double) v.getProperty ("textScale", 1.0));
    speaker.settings.waitForGaps = (bool) v.getProperty ("waitForGaps", true);
    announcedVersion = v.getProperty ("announcedVersion", {}).toString();
    speechPrefs.fromVar (v.getProperty ("speech", {}));
}

void AppCore::saveKeymap()
{
    if (! options.saveSettings) return;
    auto f = keymapFile();
    f.getParentDirectory().createDirectory();
    f.replaceWithText (juce::JSON::toString (keys.userChangesToJson()));
}

void AppCore::speak (const Message& m)
{
    // Your choices decide how much is said; the history always keeps it all.
    lastKind = m.answer ? juce::String ("Answers") : m.kind;
    const auto text = speechPrefs.choose (m);
    speaker.say (text, m.urgent ? Speech::Priority::urgent : Speech::Priority::normal, m.topic, m.full);
}

void AppCore::say (const juce::String& full, const juce::String& kind, const juce::String& brief,
                   Speech::Priority priority, const juce::String& topic)
{
    Message m;
    m.full = full; m.brief = brief; m.kind = kind; m.topic = topic;
    m.urgent = priority == Speech::Priority::urgent;
    lastKind = kind;
    const auto text = speechPrefs.choose (m);
    speaker.say (text, priority, topic, full);
}

juce::String AppCore::placeText (const tracktion::Edit& edit, TimePosition t) const
{
    auto bb = edit.tempoSequence.toBarsAndBeats (t);
    const auto bars = "bar " + juce::String (bb.bars + 1) + " beat " + juce::String (bb.getWholeBeats() + 1);
    const auto secs = t.inSeconds();
    const auto time = secs >= 60.0 ? juce::String ((int) (secs / 60.0)) + " minutes " + juce::String (std::fmod (secs, 60.0), 1) + " seconds"
                                   : juce::String (secs, 1) + " seconds";
    switch (speechPrefs.places)
    {
        case SpeechPrefs::Places::bars: return bars;
        case SpeechPrefs::Places::time: return time;
        case SpeechPrefs::Places::barsAndTime: break;
    }
    return bars + ", " + time;
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

    // A quiet look for updates a minute after launch, then every four hours.
    if (options.openAudioDevice && now - lastUpdateCheck > (lastUpdateCheck == 0 ? 60.0 : 4 * 3600.0))
    {
        // With a test feed set, the first look comes after 5 seconds, not a minute.
        const double firstWait = juce::SystemStats::getEnvironmentVariable ("EARFIRST_TEST_FEED", {}).isNotEmpty() ? 5.0 : 60.0;
        if (lastUpdateCheck == 0) lastUpdateCheck = now - 4 * 3600.0 + firstWait;
        else { lastUpdateCheck = now; updates->checkQuietly(); }
    }

    if (song != nullptr && now - lastAutosave > 30.0)
    {
        lastAutosave = now;
        song->autosaveIfNeeded();
    }
}

//==============================================================================
CommandResult AppCore::runFromUser (const juce::String& id, CommandList::Source source)
{
    if (auto* c = commandList.find (id); c != nullptr && c->askForSettings && ! c->args.empty() && onSettingsNeeded)
    {
        if (c->available)
            if (auto a = c->available(); ! a.enabled)
                return commandList.run (id, {}, source);   // says why it can't run
        onSettingsNeeded (*c);
        return CommandResult::done();
    }
    return commandList.run (id, {}, source);
}

//==============================================================================
juce::StringArray AppCore::inputDeviceNames() const
{
    if (auto* t = const_cast<tracktion::Engine&> (engine).getDeviceManager().deviceManager.getCurrentDeviceTypeObject())
    {
        t->scanForDevices();
        return t->getDeviceNames (true);
    }
    return {};
}

juce::StringArray AppCore::outputDeviceNames() const
{
    if (auto* t = const_cast<tracktion::Engine&> (engine).getDeviceManager().deviceManager.getCurrentDeviceTypeObject())
    {
        t->scanForDevices();
        return t->getDeviceNames (false);
    }
    return {};
}

juce::String AppCore::audioSentence() const
{
    auto& jdm = const_cast<tracktion::Engine&> (engine).getDeviceManager().deviceManager;
    auto* dev = jdm.getCurrentAudioDevice();
    if (dev == nullptr) return "No audio interface is open.";
    juce::AudioDeviceManager::AudioDeviceSetup setup;
    jdm.getAudioDeviceSetup (setup);
    const int ins = dev->getActiveInputChannels().countNumberOfSetBits();
    const int outs = dev->getActiveOutputChannels().countNumberOfSetBits();
    const auto rate = dev->getCurrentSampleRate();
    const int buf = dev->getCurrentBufferSizeSamples();
    juce::String s;
    if (setup.inputDeviceName.isNotEmpty())
        s << "Recording from " << setup.inputDeviceName << ", " << ins << (ins == 1 ? " input" : " inputs") << ". ";
    else
        s << "No input is open. ";
    s << "Playing through " << (setup.outputDeviceName.isNotEmpty() ? setup.outputDeviceName : juce::String ("nothing"))
      << (outs > 0 ? ", " + juce::String (outs) + " outputs" : juce::String()) << ". "
      << juce::String (rate / 1000.0, rate == std::floor (rate / 1000.0) * 1000.0 ? 0 : 1) << " kilohertz, buffer "
      << buf << " samples, about " << juce::String (buf * 1000.0 / rate, 1) << " milliseconds.";
    return s;
}

juce::String AppCore::setAudio (const juce::String& input, const juce::String& output, double rate, int buffer)
{
    auto& jdm = engine.getDeviceManager().deviceManager;
    if (jdm.getCurrentDeviceTypeObject() == nullptr)
        return "There's no audio system to set up.";
    juce::AudioDeviceManager::AudioDeviceSetup setup;
    jdm.getAudioDeviceSetup (setup);
    if (input.isNotEmpty())
    {
        setup.inputDeviceName = input;
        setup.useDefaultInputChannels = false;
        setup.inputChannels.clear();
        setup.inputChannels.setRange (0, 1024, true);   // every input the interface has
    }
    if (output.isNotEmpty())
    {
        setup.outputDeviceName = output;
        setup.useDefaultOutputChannels = true;
    }
    if (rate > 0) setup.sampleRate = rate;
    if (buffer > 0) setup.bufferSize = buffer;
    setAsideNote.clear();
    auto err = jdm.setAudioDeviceSetup (setup, true);
    engine.getDeviceManager().dispatchPendingUpdates();
    if (err.isNotEmpty())
        return "That didn't work: " + err.trimCharactersAtEnd (".") + ".";
    auto s = audioSentence();
    if (setAsideNote.isNotEmpty()) s << " " << setAsideNote;
    return s;
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
    ensureContentTracks (song->getEdit(), 1);
    ensureClickTrack();
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
    ensureClickTrack();
    syncCountIn();
    song->getEdit().resetChangedStatus();
    if (capture != nullptr) capture->attachEdit (&song->getEdit());
    selectedTrack = 0;
    if (onSongChanged) onSongChanged();
    auto tracks = contentTracks (song->getEdit()).size();
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

//==============================================================================
// The click: its own track, muted when off, never part of a mix.
ClickPlugin* AppCore::clickPlugin() const
{
    if (song == nullptr) return nullptr;
    for (auto* t : getAudioTracks (song->getEdit()))
        if (isClickTrack (*t))
            for (auto* p : t->pluginList)
                if (auto* c = dynamic_cast<ClickPlugin*> (p)) return c;
    return nullptr;
}

AudioTrack* AppCore::clickTrack() const
{
    if (song == nullptr) return nullptr;
    for (auto* t : getAudioTracks (song->getEdit()))
        if (isClickTrack (*t)) return t;
    return nullptr;
}

void AppCore::ensureClickTrack()
{
    if (song == nullptr || clickPlugin() != nullptr) return;
    auto& e = song->getEdit();
    auto* t = clickTrack();
    if (t == nullptr)
    {
        auto all = getAudioTracks (e);
        auto added = e.insertNewAudioTrack (TrackInsertPoint (nullptr, all.isEmpty() ? nullptr : all.getLast()), nullptr, false);
        if (added == nullptr) return;
        t = added.get();
        t->state.setProperty ("earFirstRole", "click", nullptr);
        t->setName ("Click");
        t->setMute (true);   // off until you turn it on
    }
    t->pluginList.insertPlugin (e.getPluginCache().createNewPlugin (ClickPlugin::xmlTypeName, {}), 0, nullptr);
}

bool AppCore::clickIsOn() const
{
    auto* t = clickTrack();
    return t != nullptr && ! t->isMuted (false);
}

juce::String AppCore::tempoSentence() const
{
    if (song == nullptr) return "No song is open.";
    auto& ts = song->getEdit().tempoSequence;
    auto pos = song->getEdit().getTransport().getPosition();
    auto& sig = ts.getTimeSigAt (pos);
    const double bpm = ts.getTempoAt (pos).getBpm();
    return juce::String (bpm, bpm == std::floor (bpm) ? 0 : 1) + " beats a minute, " + juce::String (sig.numerator.get())
           + " " + juce::String (sig.denominator.get()) + " time.";
}

juce::String AppCore::clickSentence() const
{
    auto* c = clickPlugin();
    if (c == nullptr) return "No song is open.";
    auto d = c->getSettings().describe();
    return juce::String (clickIsOn() ? "Click on. " : "Click off. ") + tempoSentence().upToFirstOccurrenceOf (" time.", true, false)
           + " " + d.substring (0, 1).toUpperCase() + d.substring (1) + ".";
}

TimePosition AppCore::countInStart (TimePosition from, int bars) const
{
    auto& ts = song->getEdit().tempoSequence;
    const auto beatsPerBar = (double) ts.getTimeSigAt (from).numerator.get();
    return ts.toTime (ts.toBeats (from) - BeatDuration::fromBeats (beatsPerBar * bars));
}

// The engine lets playback start before zero only as far as the song's own
// count-in setting, so keep that in step with the click's.
void AppCore::syncCountIn()
{
    if (song == nullptr) return;
    auto* c = clickPlugin();
    const int bars = c != nullptr ? c->countInBars.get() : 0;
    song->getEdit().setCountInMode (bars >= 2 ? Edit::CountIn::twoBar : bars == 1 ? Edit::CountIn::oneBar : Edit::CountIn::none);
}

void AppCore::playFromHere()
{
    auto& t = song->getEdit().getTransport();
    auto* c = clickPlugin();
    const int bars = c != nullptr ? c->countInBars.get() : 0;
    const auto from = t.getPosition();
    if (bars > 0 && c != nullptr)
    {
        // Start the given number of bars early; the click knows where the count-in ends.
        c->countInEndsAt = from.inSeconds();
        t.setPosition (countInStart (from, bars));
    }
    else if (c != nullptr)
        c->countInEndsAt = -1.0e9;
    t.play (false);
}

juce::String AppCore::transportLine() const
{
    if (song == nullptr) return "No song open";
    auto& t = song->getEdit().getTransport();
    auto pos = t.getPosition();
    auto bb = song->getEdit().tempoSequence.toBarsAndBeats (pos);
    const int secs = (int) pos.inSeconds();
    return juce::String (t.isPlaying() ? "Playing" : "Stopped") + "   Bar " + juce::String (bb.bars + 1) + ", beat "
           + juce::String (bb.getWholeBeats() + 1) + "   " + juce::String (secs / 60) + ":" + juce::String (secs % 60).paddedLeft ('0', 2)
           + (t.looping.get() ? "   Loop on" : "") + (song->getEdit().clickTrackEnabled.get() ? "   Click on" : "");
}

juce::String AppCore::captureLine() const
{
    if (capture == nullptr || capture->getRecorder() == nullptr) return "Not capturing: no inputs open";
    auto* r = capture->getRecorder();
    const auto secs = (double) r->getPushedSamples() / std::max (1.0, r->getStore().getInfo().sampleRate);
    const int n = r->getStore().getInfo().numInputs();
    juce::String len = secs < 60 ? juce::String ((int) secs) + " s" : juce::String ((int) (secs / 60)) + " min";
    return "Capturing " + juce::String (n) + (n == 1 ? " input" : " inputs") + ", " + len
           + (r->getDroppedSamples() > 0 ? ", some audio dropped" : ", nothing dropped");
}

juce::String AppCore::positionSentence() const
{
    if (song == nullptr) return "No song is open.";
    auto t = song->getEdit().getTransport().getPosition();
    auto p = placeText (song->getEdit(), t);
    return p.substring (0, 1).toUpperCase() + p.substring (1) + ".";
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
        c.run = [this] (const CommandArgs& a)
        {
            auto s = newSong (a["name"].toString());
            return song ? CommandResult::done (s, "New song, " + song->getName() + ".") : CommandResult::failed (s);
        };
        add (c);

        // The same, asking for the name first. Command-N stays instant.
        c.id = "file.new-song-named"; c.name = "New Song with a name";
        c.description = "Asks for the song's name, then makes it in its own folder.";
        c.askForSettings = true;
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
                return song ? CommandResult::done (s, "Opened " + song->getName() + ".") : CommandResult::failed (s);
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
        c.description = "Closes the song. If it has unsaved changes, asks whether to save first.";
        c.available = needSong;
        c.run = [this] (const CommandArgs&)
        {
            if (song->hasUnsavedChanges())
            {
                if (onUnsavedClose)
                {
                    onUnsavedClose ([this] { say (closeSong(), "File"); });
                    return CommandResult::done();
                }
                return CommandResult::failed (song->getName() + " has unsaved changes. Save first, with Command-S.");
            }
            return CommandResult::done (closeSong());
        };
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
            if (t.isPlaying()) t.stop (false, false); else playFromHere();
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
        Command c; c.id = "transport.metronome"; c.name = "Metronome"; c.category = "Click";
        c.description = "Turns the click on or off.";
        c.tags = { "metronome", "click", "count" };
        c.available = needSong;
        c.name = "Click";
        c.toggleState = [this] { return song ? std::optional<bool> (clickIsOn()) : std::optional<bool>(); };
        c.run = [this] (const CommandArgs&)
        {
            ensureClickTrack();
            if (auto* t = clickTrack()) t->setMute (! t->isMuted (false));
            return CommandResult::done();
        };
        add (c);
    }
    {
        Command c; c.id = "click.status"; c.name = "About the click"; c.category = "Click"; c.isReport = true;
        c.description = "Says whether the click is on, the tempo and meter, and how it's set up.";
        c.tags = { "metronome", "tempo", "status" };
        c.run = [this] (const CommandArgs&)
        {
            if (! song) return CommandResult::done ("No song is open.");
            auto& ts = song->getEdit().tempoSequence;
            auto pos = song->getEdit().getTransport().getPosition();
            auto& sig = ts.getTimeSigAt (pos);
            return CommandResult::done (clickSentence(), juce::String (clickIsOn() ? "Click on, " : "Click off, ")
                                        + juce::String (juce::roundToInt (ts.getTempoAt (pos).getBpm())) + ", "
                                        + juce::String (sig.numerator.get()) + " " + juce::String (sig.denominator.get()) + ".");
        };
        add (c);
    }
    {
        Command c; c.id = "click.settings"; c.name = "Click settings"; c.category = "Click";
        c.description = "Sounds, subdivisions, swing, accents, count-in and level for the click.";
        c.tags = { "metronome", "count-in", "accent", "subdivision", "swing", "sound" };
        c.askForSettings = true;
        c.available = needSong;
        auto soundArg = [this] (const char* name, std::function<juce::String (ClickPlugin&)> current)
        {
            ArgSpec a; a.name = name; a.type = ArgSpec::Type::choice; a.choices = ClickSettings::sounds(); a.defaultValue = "Woodblock";
            a.currentValue = [this, current] { auto* p = clickPlugin(); return p ? juce::var (current (*p)) : juce::var(); };
            return a;
        };
        ArgSpec sub; sub.name = "subdivision"; sub.type = ArgSpec::Type::choice;
        sub.choices = { "beats", "eighths", "triplets", "sixteenths" }; sub.defaultValue = "beats";
        sub.currentValue = [this] { auto* p = clickPlugin(); return p ? juce::var (juce::StringArray { "beats", "eighths", "triplets", "sixteenths" }[juce::jlimit (1, 4, p->subdivision.get()) - 1]) : juce::var(); };
        ArgSpec swing = numberArg ("swing", 0, 0, 75, "percent");
        swing.currentValue = [this] { auto* p = clickPlugin(); return p ? juce::var (juce::roundToInt (p->swing.get() * 100)) : juce::var(); };
        ArgSpec accents = textArg ("accents", "");
        accents.description = "One letter per beat: L loud, N normal, S soft, 0 silent. Empty accents the one.";
        accents.currentValue = [this] { auto* p = clickPlugin(); return p ? juce::var (p->accents.get()) : juce::var(); };
        ArgSpec countIn = numberArg ("count-in bars", 0, 0, 2, "bars"); countIn.type = ArgSpec::Type::integer;
        countIn.currentValue = [this] { auto* p = clickPlugin(); return p ? juce::var (p->countInBars.get()) : juce::var(); };
        ArgSpec only; only.name = "click only during count-in"; only.type = ArgSpec::Type::boolean; only.defaultValue = false;
        only.currentValue = [this] { auto* p = clickPlugin(); return p ? juce::var (p->countInOnly.get()) : juce::var(); };
        ArgSpec lvl = numberArg ("level", 80, 0, 100, "percent");
        lvl.currentValue = [this] { auto* p = clickPlugin(); return p ? juce::var (juce::roundToInt (p->level.get() * 100)) : juce::var(); };
        c.args = { soundArg ("one sound", [] (ClickPlugin& p) { return p.barSound.get(); }),
                   soundArg ("beat sound", [] (ClickPlugin& p) { return p.beatSound.get(); }),
                   soundArg ("subdivision sound", [] (ClickPlugin& p) { return p.subdivisionSound.get(); }),
                   sub, swing, accents, countIn, only, lvl };
        c.undoable = true;
        c.run = [this] (const CommandArgs& a)
        {
            ensureClickTrack();
            auto* p = clickPlugin();
            if (p == nullptr) return CommandResult::failed ("The click couldn't be set up.");
            auto acc = a["accents"].toString().toUpperCase().removeCharacters (" ,");
            if (! acc.containsOnly ("LNS0")) return CommandResult::failed ("Accents use L, N, S and 0 only, one per beat.");
            ClickSettings s;
            s.barSound = a["one sound"].toString(); s.beatSound = a["beat sound"].toString(); s.subdivisionSound = a["subdivision sound"].toString();
            s.subdivision = juce::StringArray { "beats", "eighths", "triplets", "sixteenths" }.indexOf (a["subdivision"].toString()) + 1;
            s.swing = (float) (double) a["swing"] / 100.0f; s.accents = acc;
            s.countInBars = (int) a["count-in bars"]; s.countInOnly = (bool) a["click only during count-in"];
            s.level = (float) (double) a["level"] / 100.0f;
            p->setSettings (s);
            syncCountIn();
            return CommandResult::done ("Click: " + s.describe() + ".");
        };
        add (c);
    }
    {
        Command c; c.id = "song.tempo"; c.name = "Tempo"; c.category = "Song";
        c.description = "Sets the tempo where the playhead is.";
        c.tags = { "bpm", "speed", "metronome" };
        c.askForSettings = true;
        c.available = needSong;
        c.undoable = true;
        ArgSpec bpm = numberArg ("beats a minute", 120, 20, 400, "");
        bpm.currentValue = [this] { return song ? juce::var (song->getEdit().tempoSequence.getTempoAt (song->getEdit().getTransport().getPosition()).getBpm()) : juce::var(); };
        c.args = { bpm };
        c.run = [this] (const CommandArgs& a)
        {
            auto& e = song->getEdit();
            e.tempoSequence.getTempoAt (e.getTransport().getPosition()).setBpm ((double) a["beats a minute"]);
            return CommandResult::done (tempoSentence());
        };
        add (c);
    }
    {
        Command c; c.id = "song.time-signature"; c.name = "Time signature"; c.category = "Song";
        c.description = "Sets the time signature where the playhead is, written like 7/8.";
        c.tags = { "meter", "bar", "metronome" };
        c.askForSettings = true;
        c.available = needSong;
        c.undoable = true;
        ArgSpec sig = textArg ("time signature", "4/4");
        sig.currentValue = [this]
        {
            if (! song) return juce::var();
            auto& s = song->getEdit().tempoSequence.getTimeSigAt (song->getEdit().getTransport().getPosition());
            return juce::var (juce::String (s.numerator.get()) + "/" + juce::String (s.denominator.get()));
        };
        c.args = { sig };
        c.run = [this] (const CommandArgs& a)
        {
            auto text = a["time signature"].toString().removeCharacters (" ");
            const int num = text.upToFirstOccurrenceOf ("/", false, false).getIntValue();
            const int den = text.fromFirstOccurrenceOf ("/", false, false).getIntValue();
            if (num < 1 || num > 32 || ! juce::isPowerOfTwo (den) || den < 1 || den > 32)
                return CommandResult::failed ("Write it like 4/4 or 7/8: up to 32 beats, over 1, 2, 4, 8, 16 or 32.");
            auto& e = song->getEdit();
            e.tempoSequence.getTimeSigAt (e.getTransport().getPosition()).setStringTimeSig (juce::String (num) + "/" + juce::String (den));
            return CommandResult::done (tempoSentence());
        };
        add (c);
    }
    {
        Command c; c.id = "song.tempo-report"; c.name = "Tempo and meter"; c.category = "Reports"; c.isReport = true;
        c.description = "Says the tempo and time signature where the playhead is.";
        c.tags = { "bpm", "time signature" };
        c.run = [this] (const CommandArgs&) { return CommandResult::done (tempoSentence()); };
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
            return k.take.ok() ? CommandResult::done (k.sentence, k.brief) : CommandResult::failed (k.sentence);
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
            auto* r = capture->getRecorder();
            const auto secs = (double) r->getPushedSamples() / std::max (1.0, r->getStore().getInfo().sampleRate);
            return CommandResult::done (r->status(), juce::String (r->getStore().getInfo().numInputs()) + " inputs, "
                                        + (secs < 90 ? juce::String ((int) secs) + " seconds" : juce::String ((int) (secs / 60)) + " minutes")
                                        + (r->getDroppedSamples() > 0 ? ", something dropped." : ", nothing dropped."));
        };
        add (c);
    }
    {
        Command c; c.id = "capture.keep-recovered"; c.name = "Keep recovered audio"; c.category = "Capture";
        c.description = "Keeps audio that was captured but never kept: from before the app closed unexpectedly, or before you switched interfaces.";
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
        c.description = "Throws away audio that was captured but never kept: from before the app closed unexpectedly, or before you switched interfaces.";
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
            auto tracks = contentTracks (e);
            auto* after = tracks.isEmpty() ? nullptr : tracks[juce::jlimit (0, tracks.size() - 1, selectedTrack)];
            auto t = e.insertNewAudioTrack (TrackInsertPoint (nullptr, after), nullptr);
            selectedTrack = contentTracks (e).indexOf (t.get());
            if (onSongChanged) onSongChanged();
            return CommandResult::done ("Added " + (t ? t->getName() : juce::String ("a track")) + ".");
        };
        add (c);
    }
    auto moveTrack = [this] (int delta)
    {
        auto tracks = contentTracks (song->getEdit());
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
            auto n = contentTracks (song->getEdit()).size();
            return CommandResult::done (song->getName() + ", " + juce::String (n) + (n == 1 ? " track, " : " tracks, ")
                                        + (song->hasUnsavedChanges() ? "with unsaved changes." : "saved."));
        };
        add (c);
    }

    // Audio interface
    {
        Command c; c.id = "audio.report"; c.name = "Audio interface"; c.category = "Audio"; c.isReport = true;
        c.description = "Says which interface you're recording from and playing through, the sample rate and the buffer.";
        c.tags = { "device", "sound card", "sample rate", "buffer", "latency" };
        c.run = [this] (const CommandArgs&) { return CommandResult::done (audioSentence()); };
        add (c);
    }
    {
        Command c; c.id = "audio.choose-input"; c.name = "Record from"; c.category = "Audio";
        c.description = "Chooses the interface to record from. Every input on it is captured.";
        c.tags = { "input", "device", "interface", "sound card", "microphone" };
        ArgSpec a; a.name = "interface"; a.type = ArgSpec::Type::choice;
        a.liveChoices = [this] { return inputDeviceNames(); };
        a.currentValue = [this] { juce::AudioDeviceManager::AudioDeviceSetup s; engine.getDeviceManager().deviceManager.getAudioDeviceSetup (s); return juce::var (s.inputDeviceName); };
        a.defaultValue = "";
        c.args = { a };
        c.askForSettings = true;
        c.available = [this] { return inputDeviceNames().isEmpty() ? Availability::no ("No audio interfaces are available.") : Availability::yes(); };
        c.run = [this] (const CommandArgs& args)
        {
            auto s = setAudio (args["interface"].toString(), {}, 0, 0);
            return s.startsWith ("That didn't") ? CommandResult::failed (s) : CommandResult::done (s);
        };
        add (c);
    }
    {
        Command c; c.id = "audio.choose-output"; c.name = "Play through"; c.category = "Audio";
        c.description = "Chooses the interface or speakers you hear the song through.";
        c.tags = { "output", "device", "speakers", "headphones", "sound card" };
        ArgSpec a; a.name = "interface"; a.type = ArgSpec::Type::choice;
        a.liveChoices = [this] { return outputDeviceNames(); };
        a.currentValue = [this] { juce::AudioDeviceManager::AudioDeviceSetup s; engine.getDeviceManager().deviceManager.getAudioDeviceSetup (s); return juce::var (s.outputDeviceName); };
        a.defaultValue = "";
        c.args = { a };
        c.askForSettings = true;
        c.available = [this] { return outputDeviceNames().isEmpty() ? Availability::no ("No audio outputs are available.") : Availability::yes(); };
        c.run = [this] (const CommandArgs& args)
        {
            auto s = setAudio ({}, args["interface"].toString(), 0, 0);
            return s.startsWith ("That didn't") ? CommandResult::failed (s) : CommandResult::done (s);
        };
        add (c);
    }
    {
        Command c; c.id = "audio.sample-rate"; c.name = "Sample rate"; c.category = "Audio";
        c.description = "Sets the sample rate. Capture starts a new run at the new rate; nothing captured is lost.";
        c.tags = { "khz", "hertz", "quality" };
        ArgSpec a; a.name = "rate"; a.type = ArgSpec::Type::choice;
        a.liveChoices = [this]
        {
            juce::StringArray out;
            if (auto* d = engine.getDeviceManager().deviceManager.getCurrentAudioDevice())
                for (auto r : d->getAvailableSampleRates()) out.add (juce::String ((int) r));
            return out;
        };
        a.defaultValue = "48000"; a.unit = "hertz";
        a.currentValue = [this] { auto* d = engine.getDeviceManager().deviceManager.getCurrentAudioDevice(); return d ? juce::var (juce::String ((int) d->getCurrentSampleRate())) : juce::var(); };
        c.args = { a };
        c.askForSettings = true;
        c.available = [this] { return engine.getDeviceManager().deviceManager.getCurrentAudioDevice() == nullptr ? Availability::no ("No audio interface is open.") : Availability::yes(); };
        c.run = [this] (const CommandArgs& args)
        {
            auto s = setAudio ({}, {}, args["rate"].toString().getDoubleValue(), 0);
            return s.startsWith ("That didn't") ? CommandResult::failed (s) : CommandResult::done (s);
        };
        add (c);
    }
    {
        Command c; c.id = "audio.buffer-size"; c.name = "Buffer size"; c.category = "Audio";
        c.description = "Sets the buffer. Smaller is quicker to hear yourself; larger is safer with heavy sessions.";
        c.tags = { "latency", "delay", "block size" };
        ArgSpec a; a.name = "samples"; a.type = ArgSpec::Type::choice;
        a.liveChoices = [this]
        {
            juce::StringArray out;
            if (auto* d = engine.getDeviceManager().deviceManager.getCurrentAudioDevice())
                for (auto b : d->getAvailableBufferSizes()) out.add (juce::String (b));
            return out;
        };
        a.defaultValue = "256";
        a.currentValue = [this] { auto* d = engine.getDeviceManager().deviceManager.getCurrentAudioDevice(); return d ? juce::var (juce::String (d->getCurrentBufferSizeSamples())) : juce::var(); };
        c.args = { a };
        c.askForSettings = true;
        c.available = [this] { return engine.getDeviceManager().deviceManager.getCurrentAudioDevice() == nullptr ? Availability::no ("No audio interface is open.") : Availability::yes(); };
        c.run = [this] (const CommandArgs& args)
        {
            auto s = setAudio ({}, {}, 0, args["samples"].toString().getIntValue());
            return s.startsWith ("That didn't") ? CommandResult::failed (s) : CommandResult::done (s);
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
        Command c; c.id = "speech.settings"; c.name = "Speech settings"; c.category = "Speech";
        c.description = "Chooses, for each kind of message, whether it's said in full, said short, or not said; and how places are said.";
        c.tags = { "verbosity", "voiceover", "screen reader", "announcements", "quiet", "chatty" };
        c.askForSettings = true;
        for (auto& k : SpeechPrefs::kinds())
        {
            ArgSpec a; a.name = k.id; a.type = ArgSpec::Type::choice; a.description = k.description;
            a.choices = k.canBeOff ? SpeechPrefs::detailNames() : juce::StringArray { "Full", "Short" };
            a.defaultValue = "Full";
            auto id = k.id;
            a.currentValue = [this, id] { return juce::var (SpeechPrefs::detailNames()[(int) speechPrefs.detailFor (id)]); };
            c.args.push_back (a);
        }
        ArgSpec places; places.name = "say places as"; places.type = ArgSpec::Type::choice;
        places.choices = SpeechPrefs::placeNames(); places.defaultValue = places.choices[0];
        places.currentValue = [this] { return juce::var (SpeechPrefs::placeNames()[(int) speechPrefs.places]); };
        c.args.push_back (places);
        c.run = [this] (const CommandArgs& a)
        {
            for (auto& k : SpeechPrefs::kinds())
                speechPrefs.setDetail (k.id, (SpeechPrefs::Detail) juce::jmax (0, SpeechPrefs::detailNames().indexOf (a[k.id].toString())));
            speechPrefs.places = (SpeechPrefs::Places) juce::jmax (0, SpeechPrefs::placeNames().indexOf (a["say places as"].toString()));
            saveAppSettings();
            return CommandResult::done ("Speech settings saved.");
        };
        add (c);
    }
    {
        Command c; c.id = "speech.mute-last-kind"; c.name = "Don't say that kind"; c.category = "Speech";
        c.description = "Turns off the kind of message just said. Errors and answers you ask for go short instead. Speech settings brings any kind back.";
        c.tags = { "quiet", "mute", "stop saying", "less" };
        c.run = [this] (const CommandArgs&)
        {
            const auto kind = lastKind;
            if (kind.isEmpty() || kind == "Speech") return CommandResult::failed ("Nothing has been said yet to turn off.");
            speechPrefs.setDetail (kind, SpeechPrefs::Detail::off);
            saveAppSettings();
            if (speechPrefs.detailFor (kind) == SpeechPrefs::Detail::brief)
                return CommandResult::done (kind + " messages are short now. They can't be turned all the way off.");
            return CommandResult::done (kind + " messages are off. Speech settings brings them back.", kind + " off.");
        };
        add (c);
    }
    {
        Command c; c.id = "speech.wait-for-gaps"; c.name = "Wait for gaps"; c.category = "Speech";
        c.description = "While the song plays, holds messages until a quiet moment in the music. Urgent ones always cut in.";
        c.toggleState = [this] { return std::optional<bool> (speaker.settings.waitForGaps); };
        c.run = [this] (const CommandArgs&) { speaker.settings.waitForGaps = ! speaker.settings.waitForGaps; saveAppSettings(); return CommandResult::done(); };
        add (c);
    }

    // View
    {
        Command c; c.id = "view.bigger-text"; c.name = "Bigger text"; c.category = "View";
        c.description = "Makes everything on screen larger.";
        c.tags = { "zoom", "font", "size", "low vision" };
        c.run = [this] (const CommandArgs&)
        {
            textScale = juce::jmin (3.0f, textScale + 0.25f);
            saveAppSettings();
            if (onSongChanged) onSongChanged();
            return CommandResult::done ("Text " + juce::String (juce::roundToInt (textScale * 100)) + " percent.");
        };
        add (c);
        c.id = "view.smaller-text"; c.name = "Smaller text";
        c.description = "Makes everything on screen smaller.";
        c.run = [this] (const CommandArgs&)
        {
            textScale = juce::jmax (0.75f, textScale - 0.25f);
            saveAppSettings();
            if (onSongChanged) onSongChanged();
            return CommandResult::done ("Text " + juce::String (juce::roundToInt (textScale * 100)) + " percent.");
        };
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
        k ("Shift+C", "click.status", "main"),
        k ("Primary+Shift+K", "click.settings"),
        k ("Primary+Shift+T", "song.tempo"),
        k ("Shift+T", "song.tempo-report", "main"),
        k ("L", "transport.loop", "main"),
        k ("K", "capture.keep", "main"),
        k ("Shift+K", "capture.keep", "main", keep2min),
        k ("Primary+Shift+C", "capture.status"),
        k ("Primary+T", "tracks.add"),
        // Up and Down in the track list are the list's own: it moves and the
        // screen reader reads each row. tracks.next/previous stay as
        // commands for control surfaces and the palette.
        k ("Primary+Shift+L", "report.position"),
        k ("Primary+Shift+I", "report.song"),
        k ("Primary+Shift+A", "audio.report"),
        k ("Primary+Comma", "audio.choose-input"),
        k ("Primary+=", "view.bigger-text"),
        k ("Primary+-", "view.smaller-text"),
        k ("Primary+Shift+R", "speech.say-again"),
        k ("Primary+Shift+Comma", "speech.previous-message"),
        k ("Primary+Shift+Period", "speech.next-message"),
        k ("Primary+Shift+W", "speech.read-waiting"),
        k ("Escape", "speech.stop", "main"),
        k ("Primary+Shift+V", "speech.settings"),
        k ("Primary+Shift+X", "speech.mute-last-kind"),
    }, keymapDefaultsVersion);
}

} // namespace earfirst
