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
        captureFolderRoot = captureRoot;
        // The interface itself opens once the window is up: startAudio.
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

    // Your click sounds: beside your settings, or a temporary folder when testing.
    ClickSamples::get().setFolder (options.saveSettings ? appDataFolder().getChildFile ("Click Sounds")
                                                        : juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("Ear First Test Click Sounds"));
    registerCommands();
    setDefaultKeys();
    if (options.openAudioDevice) loadAppSettings();   // the self-test starts from defaults

    juce::StringArray moved;
    if (auto f = keymapFile(); options.saveSettings && f.existsAsFile())
        if (auto parsed = juce::JSON::parse (f); (controls.fromVar (parsed["midiControls"]), true))
        if (auto r = keys.loadUserChanges (parsed, moved); r.failed())
            say (r.getErrorMessage(), "Errors", {}, Speech::Priority::urgent);
    for (auto& m : moved) say (m, "Startup");
    if (! moved.isEmpty()) saveKeymap();

    startTimerHz (20);
}

void AppCore::updateMeters (double now)
{
    if (capture == nullptr) return;
    const auto update = [&] (std::vector<float>& level, std::vector<float>& held, std::vector<double>& heldAt, std::vector<float>& fresh,
                             const juce::StringArray& names, const juce::StringArray& ignoreForClips, bool& clippedFlag)
    {
        level.resize (fresh.size(), 0.0f); held.resize (fresh.size(), 0.0f); heldAt.resize (fresh.size(), 0.0);
        for (size_t i = 0; i < fresh.size(); ++i)
        {
            level[i] = std::max (fresh[i], level[i] * 0.82f);
            if (fresh[i] >= held[i] || now - heldAt[i] > 3.0) { held[i] = fresh[i]; heldAt[i] = now; }
            if (fresh[i] >= 0.9886f && (int) i < names.size() && ! ignoreForClips.contains (names[(int) i]))
            {
                clippedFlag = true;
                auto& last = lastClipWarning[names[(int) i]];
                if (clipWarnings && now - last > 5.0)
                {
                    last = now;
                    say (names[(int) i] + " is clipping.", "Levels", "Clip.", Speech::Priority::urgent, "clip");
                }
            }
        }
    };
    std::vector<float> freshIn, freshOut;
    capture->takeLevels (freshIn, freshOut);
    auto iface = capture->getInterfaceName();
    update (meters.input, meters.inputHeld, meters.inputHeldAt, freshIn, capture->getInputNames(), computerInputsOn (iface), meters.inputClipped);
    update (meters.output, meters.outputHeld, meters.outputHeldAt, freshOut, capture->getOutputNames(), {}, meters.outputClipped);
}

AppCore::~AppCore()
{
    *alive = false;
    stopTimer();
    renderJob.reset();
    if (capture != nullptr) capture->attachEdit (nullptr);
    capture.reset();
    song.reset();
    idleEdit.reset();
}

// With no song open, an empty song stays loaded out of sight, so MIDI inputs
// are still listened to: a footswitch can run New Song, and what you played
// before the song existed can be kept once it does.
void AppCore::attachIdleEdit()
{
    if (capture == nullptr) return;
    if (idleEdit == nullptr)
        idleEdit = Edit::createSingleTrackEdit (engine);
    capture->attachEdit (idleEdit.get());
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

AppCore::InterfaceCheck AppCore::checkSavedInterface (std::unique_ptr<juce::XmlElement> saved, juce::String prefIn, juce::String prefOut)
{
    // Any thread: it opens nothing the app keeps and touches nothing shared.
    InterfaceCheck result;
    juce::AudioDeviceManager scratch;
    juce::OwnedArray<juce::AudioIODeviceType> types;
    scratch.createAudioDeviceTypes (types);
    if (types.isEmpty()) return result;
    auto* type = types[0];
    type->scanForDevices();
    auto outs = type->getDeviceNames (false), ins = type->getDeviceNames (true);

    // What would be opened: the saved choice, or the system's defaults.
    juce::String in, out;
    if (saved != nullptr)
    {
        in = saved->getStringAttribute ("audioInputDeviceName");
        out = saved->getStringAttribute ("audioOutputDeviceName");
    }
    // The interface you chose wins over whatever the last session fell back
    // to, so after a hiccup it comes back by itself.
    if (prefIn.isNotEmpty() && ins.contains (prefIn)) in = prefIn;
    if (prefOut.isNotEmpty() && outs.contains (prefOut)) out = prefOut;
    if (in.isEmpty())  in  = ins[juce::jmax (0, type->getDefaultDeviceIndex (true))];
    if (out.isEmpty()) out = outs[juce::jmax (0, type->getDefaultDeviceIndex (false))];

    const bool outOk = out.isEmpty() || answers (type->getTypeName(), out, {}, 3.0);
    const bool inOk  = in.isEmpty() || in == out ? outOk : answers (type->getTypeName(), {}, in, 3.0);
    result.setup = std::make_unique<juce::XmlElement> ("DEVICESETUP");
    if (outOk && inOk)
    {
        // Open what was asked for, even if the last session had fallen back.
        if (saved != nullptr) *result.setup = *saved;
        result.setup->setAttribute ("deviceType", type->getTypeName());
        result.setup->setAttribute ("audioOutputDeviceName", out);
        result.setup->setAttribute ("audioInputDeviceName", in);
        return result;
    }

    // Something that answers instead: the first output that does, and the
    // first input that does (or none, rather than freeze).
    juce::String newOut = outOk ? out : firstThatAnswers (type->getTypeName(), outs, false, out, 2.0);
    juce::String newIn  = inOk  ? in  : firstThatAnswers (type->getTypeName(), ins, true, in, 2.0);
    result.setup->setAttribute ("deviceType", type->getTypeName());
    result.setup->setAttribute ("audioOutputDeviceName", newOut);
    result.setup->setAttribute ("audioInputDeviceName", newIn);
    result.stuck = ! outOk ? out : in;
    return result;
}

void AppCore::openAudioNow()
{
    auto captureRoot = captureFolderRoot;
        engine.getDeviceManager().initialise (1024, 1024);   // every input and output the interface has: routing can reach any of them
        capture = std::make_unique<EngineCapture> (engine, captureRoot);
        capture->describePlace = [this] (const Edit& e, TimePosition t) { return placeText (e, t); };
        capture->onOutputLevel = [this] (float peak) { speaker.setOutputLevel (peak); };
        capture->computerInputsFor = [this] (const juce::String& iface) { return computerInputsOn (iface); };
        capture->routesFor = [this] (const juce::String& iface) { return routesOn (iface); };
        capture->onComputerInputsFound = [this] (const juce::String& iface, const juce::StringArray& found)
        {
            markComputerInputs (iface, found, false, true);
        };
        capture->refreshComputerInputs();
        capture->onMidi = [this, flag = alive] (const juce::String& device, const juce::MidiMessage& m)
        {
            if (m.isNoteOff() || m.isActiveSense() || m.isMidiClock()) return;
            juce::MessageManager::callAsync ([this, flag, device, m] { if (*flag) handleMidiControl (device, m); });
        };
        attachIdleEdit();
        capture->onCaptureSetAside = [this] (const juce::File& run, double secs)
        {
            lastOpenReport.leftoverCaptures.addIfNotAlreadyThere (run);
            setAsideNote = "The " + juce::String (secs < 90 ? juce::roundToInt (secs) : juce::roundToInt (secs / 60.0))
                           + (secs < 90 ? " seconds" : " minutes") + " captured before the switch are kept. Keep recovered audio saves them.";
        };
    capture->refreshComputerInputs();     // the marks were loaded with your settings
}

void AppCore::startAudio (std::function<void()> whenOpen)
{
    if (! options.openAudioDevice || capture != nullptr || audioStarting) return;
    audioStarting = true;
    // The slow part, asking the interface whether it answers, goes on a thread
    // of its own, so the window is up and talking while it happens.
    auto saved = engine.getPropertyStorage().getXmlProperty (SettingID::audio_device_setup);
    auto app = juce::JSON::parse (appDataFolder().getChildFile ("App.json"));
    auto prefIn = app.getProperty ("preferredInput", {}).toString(), prefOut = app.getProperty ("preferredOutput", {}).toString();
    auto flag = alive;
    std::thread ([this, flag, saved = std::shared_ptr<juce::XmlElement> (std::move (saved)), prefIn, prefOut, whenOpen]
    {
        auto check = std::make_shared<InterfaceCheck> (checkSavedInterface (saved != nullptr ? std::make_unique<juce::XmlElement> (*saved) : nullptr, prefIn, prefOut));
        juce::MessageManager::callAsync ([this, flag, check, whenOpen]
        {
            if (! *flag) return;
            if (check->setup != nullptr) engine.getPropertyStorage().setXmlProperty (SettingID::audio_device_setup, *check->setup);
            if (check->stuck.isNotEmpty())
                startupProblem = check->stuck + " isn't answering, so Ear First is using other audio for now. "
                                 "Turn the interface off and on, then choose it again with Record from.";
            openAudioNow();
            audioStarting = false;
            if (whenOpen) whenOpen();
        });
    }).detach();
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
    int64_t midiEvents = 0;
    for (auto& run : lastOpenReport.leftoverCaptures)
        if (auto info = CaptureInfo::load (run))
        {
            if (info->numInputs() > 0)
            {
                CaptureStore store (run, *info);
                secs += (double) (store.endSampleOnDisk() - store.firstSampleOnDisk()) / info->sampleRate;
            }
            if (auto m = MidiCapture::openExisting (run.getChildFile ("midi")))
                for (auto& d : m->devices())
                    midiEvents += (int64_t) m->eventsIn (d, std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max()).size();
        }
    if (secs < 0.5 && midiEvents == 0) return {};
    bool allClean = true;
    for (auto& run : lastOpenReport.leftoverCaptures) allClean &= run.getChildFile ("closed cleanly").existsAsFile();
    auto mins = juce::roundToInt (secs / 60.0);
    juce::StringArray what;
    if (secs >= 0.5) what.add ((secs < 90 ? juce::String (juce::roundToInt (secs)) + " seconds" : juce::String (mins) + " minutes") + " of audio");
    if (midiEvents > 0) what.add ("some MIDI");
    return (allClean ? "From last time: " + what.joinIntoString (" and ") + " you didn't keep. "
                     : "The app didn't close properly last time. " + what.joinIntoString (" and ") + " captured then was never kept. ")
           + "Use Keep recovered audio, or Throw away recovered audio.";
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
    o->setProperty ("theme", theme);
    o->setProperty ("beatFlash", beatFlash);
    o->setProperty ("welcomeSeen", welcomeSeen);
    o->setProperty ("clipWarnings", clipWarnings);
    o->setProperty ("waitForGaps", speaker.settings.waitForGaps);
    o->setProperty ("announcedVersion", announcedVersion);
    o->setProperty ("speech", speechPrefs.toVar());
    o->setProperty ("preferredInput", preferredInput);
    o->setProperty ("preferredOutput", preferredOutput);
    {
        std::lock_guard<std::mutex> g (computerInputsLock);
        auto* ci = new juce::DynamicObject();
        for (auto& [iface, names] : computerInputs)
            if (! names.isEmpty())
            {
                juce::Array<juce::var> list;
                for (auto& n : names) list.add (n);
                ci->setProperty (iface, list);
            }
        o->setProperty ("computerInputs", juce::var (ci));
        auto* nci = new juce::DynamicObject();
        for (auto& [iface, names] : notComputerInputs)
            if (! names.isEmpty())
            {
                juce::Array<juce::var> list;
                for (auto& n : names) list.add (n);
                nci->setProperty (iface, list);
            }
        o->setProperty ("notComputerInputs", juce::var (nci));
        juce::Array<juce::var> checked;
        for (auto& n : checkedInterfaces) checked.add (n);
        o->setProperty ("checkedInterfaces", checked);
        auto* routed = new juce::DynamicObject();
        for (auto& [iface, list] : routeSettings)
        {
            juce::Array<juce::var> items;
            for (auto& r : list)
            {
                auto* item = new juce::DynamicObject();
                item->setProperty ("input", r.input);
                item->setProperty ("output", r.output);
                item->setProperty ("db", r.db);
                items.add (juce::var (item));
            }
            if (! items.isEmpty()) routed->setProperty (iface, items);
        }
        o->setProperty ("routes", juce::var (routed));
    }
    auto f = appDataFolder().getChildFile ("App.json");
    f.getParentDirectory().createDirectory();
    f.replaceWithText (juce::JSON::toString (juce::var (o)));
}

void AppCore::loadAppSettings()
{
    auto v = juce::JSON::parse (appDataFolder().getChildFile ("App.json"));
    if (! v.isObject()) return;
    textScale = juce::jlimit (0.75f, 3.0f, (float) (double) v.getProperty ("textScale", 1.0));
    theme = v.getProperty ("theme", "Follow the system").toString();
    beatFlash = (bool) v.getProperty ("beatFlash", false);
    welcomeSeen = (bool) v.getProperty ("welcomeSeen", false);
    clipWarnings = (bool) v.getProperty ("clipWarnings", true);
    speaker.settings.waitForGaps = (bool) v.getProperty ("waitForGaps", true);
    announcedVersion = v.getProperty ("announcedVersion", {}).toString();
    speechPrefs.fromVar (v.getProperty ("speech", {}));
    preferredInput = v.getProperty ("preferredInput", {}).toString();
    preferredOutput = v.getProperty ("preferredOutput", {}).toString();
    std::lock_guard<std::mutex> g (computerInputsLock);
    computerInputs.clear();
    if (auto* ci = v.getProperty ("computerInputs", {}).getDynamicObject())
        for (auto& p : ci->getProperties())
            if (auto* arr = p.value.getArray())
                for (auto& n : *arr) computerInputs[p.name.toString()].add (n.toString());
    checkedInterfaces.clear();
    if (auto* arr = v.getProperty ("checkedInterfaces", {}).getArray())
        for (auto& n : *arr) checkedInterfaces.add (n.toString());
    notComputerInputs.clear();
    if (auto* nci = v.getProperty ("notComputerInputs", {}).getDynamicObject())
        for (auto& p : nci->getProperties())
            if (auto* arr = p.value.getArray())
                for (auto& n : *arr) notComputerInputs[p.name.toString()].add (n.toString());
    routeSettings.clear();
    if (auto* routed = v.getProperty ("routes", {}).getDynamicObject())
        for (auto& p : routed->getProperties())
            if (auto* arr = p.value.getArray())
                for (auto& r : *arr)
                    routeSettings[p.name.toString()].push_back ({ r.getProperty ("input", {}).toString(), r.getProperty ("output", {}).toString(),
                                                                  (double) r.getProperty ("db", 0.0) });
}

//==============================================================================
// Routing. A choice is one channel, or a neighbouring pair (1 and 2, 3 and 4).
juce::StringArray AppCore::channelChoices (const juce::StringArray& names)
{
    juce::StringArray out (names);
    for (int i = 0; i + 1 < names.size(); i += 2)
        out.add (names[i] + " and " + names[i + 1]);
    return out;
}

juce::StringArray AppCore::channelsIn (const juce::String& choice, const juce::StringArray& names)
{
    if (names.contains (choice)) return juce::StringArray (choice);
    for (int i = 0; i + 1 < names.size(); i += 2)
        if (choice == names[i] + " and " + names[i + 1])
            return juce::StringArray (names[i], names[i + 1]);
    return {};
}

std::vector<EngineCapture::NamedRoute> AppCore::routesOn (const juce::String& iface) const
{
    if (capture == nullptr) return {};
    const auto inputs = capture->getInputNames(), outputs = capture->getOutputNames();
    std::vector<RouteSetting> settings;
    {
        std::lock_guard<std::mutex> g (computerInputsLock);
        if (auto it = routeSettings.find (iface); it != routeSettings.end()) settings = it->second;
    }
    // The same channel pair named twice counts once: the later setting wins.
    std::map<std::pair<juce::String, juce::String>, float> gains;
    for (auto& r : settings)
    {
        const auto in = channelsIn (r.input, inputs), out = channelsIn (r.output, outputs);
        const auto gain = juce::Decibels::decibelsToGain ((float) r.db);
        if (in.size() == 2 && out.size() == 2)
        {
            gains[{ in[0], out[0] }] = gain;      // left to left, right to right
            gains[{ in[1], out[1] }] = gain;
        }
        else
            for (auto& a : in)
                for (auto& b : out)
                    gains[{ a, b }] = in.size() == 2 ? gain * 0.7079f : gain;   // a pair folded to one output, 3 dB down each
    }
    std::vector<EngineCapture::NamedRoute> result;
    for (auto& [key, gain] : gains) result.push_back ({ key.first, key.second, gain });
    return result;
}

juce::String AppCore::describeRoute (const RouteSetting& r)
{
    return r.input + " to " + r.output + " at " + juce::String (r.db, r.db == std::floor (r.db) ? 0 : 1) + " dB";
}

juce::String AppCore::routesSentence() const
{
    if (capture == nullptr) return "No audio is open.";
    const auto iface = capture->getInterfaceName();
    std::vector<RouteSetting> settings;
    {
        std::lock_guard<std::mutex> g (computerInputsLock);
        if (auto it = routeSettings.find (iface); it != routeSettings.end()) settings = it->second;
    }
    if (settings.empty()) return "Nothing is routed on " + iface + ". Every input is still being captured.";
    const auto inputs = capture->getInputNames(), outputs = capture->getOutputNames(), marked = computerInputsOn (iface);
    juce::String s (settings.size() == 1 ? "One route. " : juce::String ((int) settings.size()) + " routes. ");
    for (auto& r : settings)
    {
        s << describeRoute (r);
        const auto in = channelsIn (r.input, inputs);
        if (in.isEmpty() || channelsIn (r.output, outputs).isEmpty()) s << ", silent, that channel isn't open";
        else
            for (auto& n : in)
                if (marked.contains (n)) { s << ", silent, it's the computer's own sound"; break; }
        s << ". ";
    }
    return s.trimEnd();
}

void AppCore::findComputerInputs (bool firstTime)
{
    if (capture == nullptr || capture->getInputNames().isEmpty()) return;
    const auto iface = capture->getInterfaceName();
    if (firstTime)
    {
        // Once per interface: the first time it's used, with words before the sound.
        if (checkedInterfaces.contains (iface) || ! options.openAudioDevice) return;
        checkedInterfaces.add (iface);
        saveAppSettings();
        say ("First time with " + iface + ". Listen for a short, soft sweep: it checks which inputs carry the computer's own sound.",
             "Audio", "Listen for a sweep.", Speech::Priority::normal, "find-return");
    }
    else
        say ("Listen for a short, soft sweep. It checks which inputs carry the computer's own sound.",
             "Audio", "Listen for a sweep.", Speech::Priority::normal, "find-return");

    juce::Timer::callAfterDelay (firstTime ? 4000 : 2500, [this, flag = alive, firstTime]
    {
        if (! *flag || capture == nullptr) return;
        capture->findComputerInputsNow ([this, flag, firstTime] (const juce::StringArray& found, const juce::String& problem)
        {
            if (! *flag || capture == nullptr) return;
            juce::AudioDeviceManager::AudioDeviceSetup s;
            engine.getDeviceManager().deviceManager.getAudioDeviceSetup (s);
            const auto iface = capture->getInterfaceName();
            // The first-time check leaves alone any input you unmarked; asking leaves nothing out.
            markComputerInputs (iface, found, ! firstTime, false);
            const auto marked = computerInputsOn (iface);
            juce::StringArray shown, leftAlone;
            for (auto& n : found) (marked.contains (n) ? shown : leftAlone).add (n);
            if (! found.isEmpty())
            {
                juce::String t;
                if (! shown.isEmpty())
                {
                    const bool one = shown.size() == 1;
                    t << shown.joinIntoString (" and ") << (one ? " carries" : " carry") << " the computer's own sound. "
                      << (one ? "It's" : "They're") << " marked, so sound on " << (one ? "it" : "them") << " won't count as you playing.";
                    if (firstTime) t << " Computer's own sound, in the Audio menu, changes that.";
                }
                if (! leftAlone.isEmpty())
                    t << (t.isEmpty() ? "" : " ") << leftAlone.joinIntoString (" and ") << " sounded like the computer's own sound, but you unmarked "
                      << (leftAlone.size() == 1 ? "it" : "them") << ", so " << (leftAlone.size() == 1 ? "it's" : "they're") << " left as you set.";
                say (t, "Audio", "Found " + found.joinIntoString (" and ") + ".", Speech::Priority::normal, "find-return");
                return;
            }
            if (firstTime && problem.isNotEmpty())
            {
                checkedInterfaces.removeString (iface);   // it didn't get a fair check: try again next time
                saveAppSettings();
            }
            juce::String s2 = problem.isNotEmpty() ? problem + " " : juce::String();
            s2 << "No input on " << iface << " carries the computer's own sound.";
            if (s.inputDeviceName != s.outputDeviceName)
                s2 << " You're playing through " << s.outputDeviceName << ", so its sound can only come back there.";
            say (s2, "Audio", "None found.", Speech::Priority::normal, "find-return");
        });
    });
}

void AppCore::importClickSounds (const juce::Array<juce::File>& files)
{
    if (files.isEmpty()) return;
    juce::StringArray problems;
    auto names = ClickSamples::get().import (files, problems);
    if (auto* p = clickPlugin()) p->refreshNow();
    juce::String s;
    if (! names.isEmpty())
        s << "Imported " << names.joinIntoString (", ") << ". "
          << (names.size() == 1 ? "It's" : "They're") << " in Click settings with the built-in sounds.";
    if (! problems.isEmpty()) s << (s.isEmpty() ? "" : " ") << problems.joinIntoString (" ");
    say (s, names.isEmpty() ? "Errors" : "Click", names.isEmpty() ? juce::String() : "Imported " + juce::String (names.size()) + ".");
}

juce::StringArray AppCore::markComputerInputs (const juce::String& iface, const juce::StringArray& inputs, bool evenIfUnmarked, bool speak)
{
    juce::StringArray added;
    {
        std::lock_guard<std::mutex> g (computerInputsLock);
        auto& list = computerInputs[iface];
        auto& not_ = notComputerInputs[iface];
        for (auto& n : inputs)
        {
            if (list.contains (n)) continue;
            if (not_.contains (n)) { if (! evenIfUnmarked) continue; not_.removeString (n); }
            list.add (n); added.add (n);
        }
    }
    if (added.isEmpty()) return added;
    if (capture) capture->refreshComputerInputs();
    saveAppSettings();
    if (! speak) return added;   // the caller says it
    const bool one = added.size() == 1;
    say (added.joinIntoString (" and ") + (one ? " carries" : " carry") + " the computer's own sound, so sound on "
         + (one ? "it" : "them") + " won't count as you playing. Computer's own sound, in the Audio menu, changes that.",
         "Audio", added.joinIntoString (" and ") + " marked as the computer's own sound.");
    return added;
}

juce::StringArray AppCore::computerInputsOn (const juce::String& iface) const
{
    std::lock_guard<std::mutex> g (computerInputsLock);
    auto it = computerInputs.find (iface);
    return it != computerInputs.end() ? it->second : juce::StringArray();
}

void AppCore::saveKeymap()
{
    if (! options.saveSettings) return;
    auto f = keymapFile();
    f.getParentDirectory().createDirectory();
    auto v = keys.userChangesToJson();
    if (auto* o = v.getDynamicObject()) o->setProperty ("midiControls", controls.toVar());
    f.replaceWithText (juce::JSON::toString (v));
}

void AppCore::speak (const Message& m)
{
    // Your choices decide how much is said; the history always keeps it all.
    lastKind = m.answer ? juce::String ("Answers") : m.kind;
    if (speechPrefs.wantsSound (m))
    {
        lastEarcon = SpeechPrefs::earconFor (m);
        if (capture != nullptr) capture->playEarcon (lastEarcon.first, lastEarcon.second);
    }
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
    if (speechPrefs.wantsSound (m))
    {
        lastEarcon = SpeechPrefs::earconFor (m);
        if (capture != nullptr) capture->playEarcon (lastEarcon.first, lastEarcon.second);
    }
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
    updateMeters (now);
    if (learningCommand.isNotEmpty() && now - learningSince > 20.0)
    {
        learningCommand.clear();
        say ("Nothing was pressed, so nothing was assigned.", "Help");
    }

    // A quiet look for updates a minute after launch, then every four hours.
    if (options.openAudioDevice && now - lastUpdateCheck > (lastUpdateCheck == 0 ? 60.0 : 4 * 3600.0))
    {
        // With a test feed set, the first look comes after 5 seconds, not a minute.
        const double firstWait = juce::SystemStats::getEnvironmentVariable ("EARFIRST_TEST_FEED", {}).isNotEmpty() ? 5.0 : 60.0;
        if (lastUpdateCheck == 0) lastUpdateCheck = now - 4 * 3600.0 + firstWait;
        else { lastUpdateCheck = now; updates->checkQuietly(); }
    }

    // Space running low: recovered audio goes first, oldest first, so live
    // capture never has to give anything up for it. Said each time.
    if (capture != nullptr && now - lastSpaceCheck > 30.0 && ! lastOpenReport.leftoverCaptures.isEmpty())
    {
        lastSpaceCheck = now;
        auto root = appDataFolder().getChildFile ("Capture");
        if (root.getBytesFreeOnVolume() < (int64_t) 6 * 1024 * 1024 * 1024)
        {
            auto& runs = lastOpenReport.leftoverCaptures;
            std::sort (runs.begin(), runs.end(), [] (auto& a, auto& b) { return a.getFileName() < b.getFileName(); });
            auto oldest = runs.removeAndReturn (0);
            const auto name = oldest.getFileName();
            oldest.deleteRecursively();
            say ("The disk is nearly full, so the oldest recovered audio, from " + name + ", was cleared to keep capture going.", "Capture");
        }
    }

    if (song != nullptr && now - lastAutosave > 30.0)
    {
        lastAutosave = now;
        song->autosaveIfNeeded();
    }
}

//==============================================================================
void AppCore::handleMidiControl (const juce::String& device, const juce::MidiMessage& m)
{
    if (learningCommand.isNotEmpty())
    {
        auto control = MidiControls::controlFor (device, m);
        if (! control) { controls.remember (device, m); return; }   // a release or a small move: keep waiting
        control->commandId = learningCommand;
        learningCommand.clear();
        controls.add (*control);
        controls.remember (device, m);
        saveKeymap();
        auto* c = commandList.find (control->commandId);
        auto name = c ? c->name : control->commandId;
        juce::String s = control->describe() + " now runs " + name + ".";
        if (control->type == MidiControl::Type::controller && control->number == 64)
            s << " That's the sustain pedal, so it will also hold notes while you play.";
        say (s, "Help", name + " assigned.");
        return;
    }
    if (auto* b = controls.match (device, m))
        commandList.run (b->commandId, b->args, CommandList::Source::midi);
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
    if (input.isNotEmpty()) preferredInput = input;
    if (output.isNotEmpty()) preferredOutput = output;
    saveAppSettings();
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
    attachIdleEdit();
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
        Command c; c.id = "click.import-sounds"; c.name = "Import click sounds"; c.category = "Click";
        c.description = "Brings in audio files of your own to use as click sounds, for the one, the beats or the subdivisions. "
                        "They're copied into Ear First, so moving the originals never breaks the click.";
        c.tags = { "metronome", "sample", "custom", "my own sound", "wav" };
        c.run = [this] (const CommandArgs&)
        {
            if (onImportClickSoundsRequested) onImportClickSoundsRequested();
            return CommandResult::done();
        };
        add (c);
    }
    {
        Command c; c.id = "click.remove-sound"; c.name = "Remove a click sound"; c.category = "Click";
        c.description = "Removes a click sound you imported. A song that used it clicks with the default sounds instead.";
        ArgSpec a; a.name = "sound"; a.type = ArgSpec::Type::choice; a.liveChoices = [] { return ClickSamples::get().names(); }; a.defaultValue = "";
        c.args = { a };
        c.askForSettings = true;
        c.available = [] { return ClickSamples::get().names().isEmpty() ? Availability::no ("You haven't imported any click sounds.") : Availability::yes(); };
        c.run = [this] (const CommandArgs& args)
        {
            const auto name = args["sound"].toString();
            if (! ClickSamples::get().remove (name)) return CommandResult::failed ("There's no click sound called " + name + ".");
            if (auto* p = clickPlugin()) p->refreshNow();
            return CommandResult::done ("Removed " + name + ".", "Removed.");
        };
        add (c);
    }
    {
        Command c; c.id = "click.show-sounds-folder"; c.name = "Show click sounds folder"; c.category = "Click";
        c.description = "Opens the folder your imported click sounds are kept in. Audio files dropped there show up in Click settings too.";
        c.run = [] (const CommandArgs&)
        {
            auto f = ClickSamples::get().getFolder();
            f.createDirectory();
            f.startAsProcess();
            return CommandResult::done ("Opened the Click Sounds folder.", "Opened.");
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
            ArgSpec a; a.name = name; a.type = ArgSpec::Type::choice; a.liveChoices = [] { return ClickSettings::sounds(); }; a.defaultValue = "Clave";
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
            if (capture == nullptr || (capture->getRecorder() == nullptr && capture->getMidi() == nullptr))
                return Availability::no ("Nothing is being captured. No audio interface is open.");
            return Availability::yes();
        };
        c.undoable = true;
        c.run = [this] (const CommandArgs& a)
        {
            const double secs = (double) a["seconds"];
            // A big keep (hours, or many inputs) exports in the background, so the
            // window and the screen reader never freeze; it says when it's done.
            if (capture->keepSize (secs) > 20000.0)
            {
                capture->keepLastInBackground (secs, song->audioFolder(), [this] (const EngineCapture::Kept& k)
                {
                    if (onSongChanged) onSongChanged();
                    if (k.take.ok()) say (k.sentence, "Capture", k.brief);
                    else say (k.sentence, "Errors", {}, Speech::Priority::urgent);
                });
                const auto mins = juce::roundToInt (secs / 60.0);
                return CommandResult::done ("Keeping " + (secs < 90 ? juce::String (juce::roundToInt (secs)) + " seconds" : juce::String (mins) + " minutes")
                                            + ". I'll say when it's done; you can keep working.", "Keeping.");
            }
            auto k = capture->keepLast (secs, song->audioFolder());
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
            if (capture == nullptr || (capture->getRecorder() == nullptr && capture->getMidi() == nullptr))
                return CommandResult::done ("Nothing is being captured. No audio interface is open.");
            if (capture->getRecorder() == nullptr)
            {
                const int n = capture->getMidi()->devices().size();
                return CommandResult::done ("No audio inputs are open, so only MIDI is being kept"
                                            + (n > 0 ? ", from " + juce::String (n) + (n == 1 ? " device." : " devices.") : juce::String (".")),
                                            "MIDI only.");
            }
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
                auto runDest = dest.getChildFile (info->started.formatted ("%Y-%m-%d %H-%M-%S"));
                int files = 0;
                bool ok = true;
                if (info->numInputs() > 0)
                {
                    CaptureStore store (run, *info);
                    juce::Array<int> all;
                    for (int i = 0; i < info->numInputs(); ++i) all.add (i);
                    auto r = store.exportTake (store.firstSampleOnDisk(), store.endSampleOnDisk(), all, runDest, "Recovered");
                    if (r.ok()) files += r.files.size(); else { ok = false; said.add (r.error); }
                }
                // MIDI too, as a standard MIDI file per device, timed in milliseconds.
                if (auto midiRun = MidiCapture::openExisting (run.getChildFile ("midi")))
                    for (auto& device : midiRun->devices())
                    {
                        auto events = midiRun->eventsIn (device, std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max());
                        if (events.empty()) continue;
                        const auto first = events.front().sample;
                        juce::MidiMessageSequence seq;
                        for (auto& e : events)
                            seq.addEvent (juce::MidiMessage (e.message, (double) (e.sample - first) / info->sampleRate * 1000.0));
                        seq.updateMatchedPairs();
                        juce::MidiFile mf;
                        mf.setSmpteTimeFormat (25, 40);   // 1000 ticks a second
                        mf.addTrack (seq);
                        runDest.createDirectory();
                        auto f = runDest.getChildFile (juce::File::createLegalFileName (device) + " Recovered.mid");
                        juce::FileOutputStream os (f);
                        if (os.openedOk() && mf.writeTo (os)) ++files; else { ok = false; said.add ("Couldn't save the MIDI from " + device + "."); }
                    }
                // Only once everything is safely out is the raw capture removed.
                if (ok) { run.deleteRecursively(); said.add ("Kept " + juce::String (files) + (files == 1 ? " file." : " files.")); }
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

    {
        Command c; c.id = "tracks.output"; c.name = "Play track through"; c.category = "Tracks";
        c.description = "Chooses which outputs the selected track plays through. The default is the interface's main outputs.";
        c.tags = { "routing", "output", "send to", "headphones", "cue" };
        ArgSpec a; a.name = "output"; a.type = ArgSpec::Type::choice;
        a.liveChoices = [this]
        {
            juce::StringArray out ("Main outputs");
            for (auto* wo : engine.getDeviceManager().getWaveOutputDevices()) out.add (wo->getName());
            return out;
        };
        a.currentValue = [this] () -> juce::var
        {
            if (song == nullptr) return "Main outputs";
            auto tracks = contentTracks (song->getEdit());
            auto* t = tracks.isEmpty() ? nullptr : tracks[juce::jlimit (0, tracks.size() - 1, selectedTrack)];
            auto* out = t != nullptr ? t->getOutput().getOutputDevice (false) : nullptr;
            return out != nullptr && ! t->getOutput().usesDefaultAudioOut() ? juce::var (out->getName()) : juce::var ("Main outputs");
        };
        a.defaultValue = "";
        c.args = { a };
        c.askForSettings = true;
        c.undoable = true;
        c.available = [this, needSong]
        {
            if (auto ok = needSong(); ! ok.enabled) return ok;
            return contentTracks (song->getEdit()).isEmpty() ? Availability::no ("There are no tracks.") : Availability::yes();
        };
        c.run = [this] (const CommandArgs& args)
        {
            auto tracks = contentTracks (song->getEdit());
            auto* t = tracks[juce::jlimit (0, tracks.size() - 1, selectedTrack)];
            const auto choice = args["output"].toString();
            if (choice == "Main outputs")
            {
                t->getOutput().setOutputToDefaultDevice (false);
                if (onSongChanged) onSongChanged();
                return CommandResult::done (t->getName() + " plays through the main outputs.");
            }
            auto& dm = engine.getDeviceManager();
            for (auto* wo : dm.getWaveOutputDevices())
                if (wo->getName() == choice)
                {
                    const auto id = wo->getDeviceID();
                    if (! wo->isEnabled()) dm.setDeviceEnabled (*wo, true);   // rebuilds the list: wo is gone after this
                    t->getOutput().setOutputToDeviceID (id);
                    if (onSongChanged) onSongChanged();
                    return CommandResult::done (t->getName() + " plays through " + choice + ".");
                }
            return CommandResult::failed ("There's no output called " + choice + ".");
        };
        add (c);
    }

    registerSongCommands();
    registerClipAndFileCommands();

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
            if (s.startsWith ("That didn't")) return CommandResult::failed (s);
            juce::Timer::callAfterDelay (3000, [this, flag = alive] { if (*flag) findComputerInputs (true); });   // a new one gets checked once
            return CommandResult::done (s);
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
        Command c; c.id = "audio.computer-input"; c.name = "Computer's own sound"; c.category = "Audio";
        c.description = "Marks an input as carrying the computer's own sound, like the last two channels on a StudioLive, "
                        "or unmarks it. It's still captured, but sound on it never counts as you playing, "
                        "so quitting doesn't keep a session where only the computer made noise.";
        c.tags = { "return", "loopback", "computer audio", "daw return", "not a source" };
        ArgSpec a; a.name = "input"; a.type = ArgSpec::Type::choice;
        a.liveChoices = [this] { return capture ? capture->getInputNames() : juce::StringArray(); };
        a.defaultValue = "";
        c.args = { a };
        c.askForSettings = true;
        c.available = [this] { return capture == nullptr || capture->getInputNames().isEmpty() ? Availability::no ("No audio inputs are open.") : Availability::yes(); };
        c.run = [this] (const CommandArgs& args)
        {
            const auto input = args["input"].toString();
            const auto iface = capture->getInterfaceName();
            if (! capture->getInputNames().contains (input)) return CommandResult::failed ("There's no input called " + input + ".");
            bool nowMarked;
            juce::StringArray all;
            {
                std::lock_guard<std::mutex> g (computerInputsLock);
                auto& list = computerInputs[iface];
                nowMarked = ! list.contains (input);
                if (nowMarked) list.add (input); else list.removeString (input);
                auto& not_ = notComputerInputs[iface];   // unmarked by you: never marked again by itself
                if (nowMarked) not_.removeString (input); else not_.addIfNotAlreadyThere (input);
                all = list;
            }
            capture->refreshComputerInputs();
            saveAppSettings();
            juce::String s = input + (nowMarked ? " is marked as the computer's own sound." : " is no longer marked as the computer's own sound.");
            s << (all.isEmpty() ? juce::String (" No inputs on ") + iface + " are marked."
                                : " Marked on " + iface + ": " + all.joinIntoString (", ") + ".");
            return CommandResult::done (s, input + (nowMarked ? " marked." : " unmarked."));
        };
        add (c);
    }
    {
        Command c; c.id = "audio.find-computer-input"; c.name = "Find the computer's own sound"; c.category = "Audio";
        c.description = "Plays a short, soft sweep and listens for it on every input. Inputs that carry it straight back "
                        "are the computer's own sound, and get marked. Ear First also notices by itself while the song or click plays.";
        c.tags = { "return", "loopback", "detect", "computer audio" };
        c.available = [this] { return capture == nullptr || capture->getInputNames().isEmpty() ? Availability::no ("No audio inputs are open.") : Availability::yes(); };
        c.run = [this] (const CommandArgs&)
        {
            findComputerInputs (false);
            return CommandResult::done();
        };
        add (c);
    }
    // Routing: any input to any output, live, song or no song.
    {
        Command c; c.id = "routing.send"; c.name = "Send input to output"; c.category = "Audio";
        c.description = "Sends an input straight to an output so you hear it live, with or without a song open. "
                        "It changes what you hear, never what's captured. Choosing the same input and output again changes its level. "
                        "A mic near speakers can feed back, so start with headphones.";
        c.tags = { "routing", "route", "monitor", "monitoring", "matrix", "patch", "headphones", "cue", "hear myself" };
        ArgSpec in; in.name = "input"; in.type = ArgSpec::Type::choice;
        in.liveChoices = [this] { return capture ? channelChoices (capture->getInputNames()) : juce::StringArray(); };
        in.defaultValue = "";
        ArgSpec out; out.name = "output"; out.type = ArgSpec::Type::choice;
        out.liveChoices = [this] { return capture ? channelChoices (capture->getOutputNames()) : juce::StringArray(); };
        out.defaultValue = "";
        ArgSpec level; level.name = "level"; level.type = ArgSpec::Type::number; level.defaultValue = 0.0;
        level.minimum = -60.0; level.maximum = 12.0; level.unit = "dB";
        c.args = { in, out, level };
        c.askForSettings = true;
        c.available = [this]
        {
            if (capture == nullptr || capture->getInputNames().isEmpty()) return Availability::no ("No audio inputs are open.");
            return capture->getOutputNames().isEmpty() ? Availability::no ("No audio outputs are open.") : Availability::yes();
        };
        c.run = [this] (const CommandArgs& args)
        {
            const RouteSetting r { args["input"].toString(), args["output"].toString(), (double) args["level"] };
            const auto iface = capture->getInterfaceName();
            const auto channels = channelsIn (r.input, capture->getInputNames());
            if (channels.isEmpty()) return CommandResult::failed ("There's no input called " + r.input + ".");
            if (channelsIn (r.output, capture->getOutputNames()).isEmpty()) return CommandResult::failed ("There's no output called " + r.output + ".");
            const auto marked = computerInputsOn (iface);
            for (auto& n : channels)
                if (marked.contains (n))
                    return CommandResult::failed (n + " carries the computer's own sound, so sending it to an output would feed back. "
                                                  "If that's wrong, unmark it with Computer's own sound.");
            bool changed = false;
            {
                std::lock_guard<std::mutex> g (computerInputsLock);
                auto& list = routeSettings[iface];
                for (auto& existing : list)
                    if (existing.input == r.input && existing.output == r.output) { existing = r; changed = true; }
                if (! changed) list.push_back (r);
            }
            capture->refreshRoutes();
            saveAppSettings();
            return CommandResult::done (describeRoute (r) + (changed ? ", changed." : ", on."), describeRoute (r) + ".");
        };
        add (c);
    }
    {
        Command c; c.id = "routing.remove"; c.name = "Remove a route"; c.category = "Audio";
        c.description = "Stops one input going to an output. Capture carries on.";
        c.tags = { "routing", "route", "unroute", "monitor off", "disconnect" };
        ArgSpec a; a.name = "route"; a.type = ArgSpec::Type::choice;
        a.liveChoices = [this]
        {
            juce::StringArray out;
            if (capture == nullptr) return out;
            std::lock_guard<std::mutex> g (computerInputsLock);
            if (auto it = routeSettings.find (capture->getInterfaceName()); it != routeSettings.end())
                for (auto& r : it->second) out.add (describeRoute (r));
            return out;
        };
        a.defaultValue = "";
        c.args = { a };
        c.askForSettings = true;
        c.available = [this, liveChoices = a.liveChoices] { return liveChoices().isEmpty() ? Availability::no ("Nothing is routed.") : Availability::yes(); };
        c.run = [this] (const CommandArgs& args)
        {
            const auto wanted = args["route"].toString();
            bool removed = false;
            {
                std::lock_guard<std::mutex> g (computerInputsLock);
                auto& list = routeSettings[capture->getInterfaceName()];
                for (auto it = list.begin(); it != list.end(); ++it)
                    if (describeRoute (*it) == wanted) { list.erase (it); removed = true; break; }
            }
            if (! removed) return CommandResult::failed ("That route isn't there any more.");
            capture->refreshRoutes();
            saveAppSettings();
            return CommandResult::done ("Removed " + wanted + ".");
        };
        add (c);
    }
    {
        Command c; c.id = "routing.clear"; c.name = "Remove every route"; c.category = "Audio";
        c.description = "Stops every input going to the outputs on this interface. Capture carries on.";
        c.tags = { "routing", "panic", "silence", "monitor off", "feedback" };
        c.available = [this]
        {
            if (capture == nullptr) return Availability::no ("Nothing is routed.");
            std::lock_guard<std::mutex> g (computerInputsLock);
            auto it = routeSettings.find (capture->getInterfaceName());
            return it == routeSettings.end() || it->second.empty() ? Availability::no ("Nothing is routed.") : Availability::yes();
        };
        c.run = [this] (const CommandArgs&)
        {
            {
                std::lock_guard<std::mutex> g (computerInputsLock);
                routeSettings[capture->getInterfaceName()].clear();
            }
            capture->refreshRoutes();
            saveAppSettings();
            return CommandResult::done ("Every route removed.");
        };
        add (c);
    }
    {
        Command c; c.id = "routing.report"; c.name = "What's routed"; c.category = "Audio"; c.isReport = true;
        c.description = "Says which inputs are going to which outputs on this interface. Changes nothing.";
        c.tags = { "routing", "routes", "matrix", "monitoring" };
        c.run = [this] (const CommandArgs&) { return CommandResult::done (routesSentence()); };
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
        c.run = [this] (const CommandArgs&)
        {
            if (learningCommand.isNotEmpty())
            {
                learningCommand.clear();
                return CommandResult::done ("Cancelled. Nothing was assigned.");
            }
            speaker.stopSpeaking();
            return CommandResult::done();
        };
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
            a.choices = k.canBeOff ? SpeechPrefs::detailNames()
                      : k.id == "Errors" ? juce::StringArray { "Full", "Short", "Sound" } : juce::StringArray { "Full", "Short" };
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
            if (onViewChanged) onViewChanged();
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
            if (onViewChanged) onViewChanged();
            return CommandResult::done ("Text " + juce::String (juce::roundToInt (textScale * 100)) + " percent.");
        };
        add (c);
    }

    {
        Command c; c.id = "view.theme"; c.name = "Colours"; c.category = "View";
        c.description = "Light, dark, high contrast, or whatever the system is set to.";
        c.tags = { "theme", "dark mode", "high contrast", "appearance", "low vision" };
        ArgSpec a; a.name = "colours"; a.type = ArgSpec::Type::choice;
        a.choices = { "Follow the system", "Light", "Dark", "High contrast" };
        a.defaultValue = "Follow the system";
        a.currentValue = [this]() -> juce::var { return theme; };
        c.args = { a };
        c.askForSettings = true;
        c.run = [this] (const CommandArgs& args)
        {
            theme = args["colours"].toString();
            saveAppSettings();
            if (onViewChanged) onViewChanged();
            return CommandResult::done ("Colours: " + theme + ".");
        };
        add (c);
    }
    {
        Command c; c.id = "view.beat-flash"; c.name = "Flash on the beat"; c.category = "View";
        c.description = "Lights the edge of the window on every beat while the song plays, for anyone who can't hear the click.";
        c.tags = { "visual click", "metronome", "deaf", "hard of hearing", "light" };
        c.toggleState = [this] { return std::optional<bool> (beatFlash); };
        c.run = [this] (const CommandArgs&) { beatFlash = ! beatFlash; saveAppSettings(); if (onViewChanged) onViewChanged(); return CommandResult::done(); };
        add (c);
    }
    {
        Command c; c.id = "view.messages"; c.name = "Go to messages"; c.category = "View";
        c.description = "Moves to the list of everything Ear First has said, newest at the bottom.";
        c.tags = { "history", "log", "missed", "what did it say" };
        c.run = [this] (const CommandArgs&)
        {
            if (! onShowMessages) return CommandResult::failed ("There's no window to show them in.");
            onShowMessages();
            return CommandResult::done();
        };
        add (c);
    }
    {
        Command c; c.id = "help.welcome"; c.name = "Welcome"; c.category = "Help";
        c.description = "Shows the welcome again: what Ear First is and how to start.";
        c.tags = { "start", "introduction", "getting started", "tour" };
        c.run = [this] (const CommandArgs&)
        {
            if (! onShowWelcome) return CommandResult::failed ("There's no window to show it in.");
            onShowWelcome();
            return CommandResult::done();
        };
        add (c);
    }

    // Key help and Find a command (the window shows the palette)
    {
        Command c; c.id = "help.key-help"; c.name = "Key help"; c.category = "Help";
        c.description = "While on, pressing a key says what it does instead of doing it. Press Escape or this key again to turn it off.";
        c.tags = { "what does this key do", "describe key", "learn" };
        c.toggleState = [this] { return std::optional<bool> (keyHelpOn); };
        c.run = [this] (const CommandArgs&) { keyHelpOn = ! keyHelpOn; return CommandResult::done(); };
        add (c);
    }
    {
        Command c; c.id = "help.find-command"; c.name = "Find a command"; c.category = "Help";
        c.description = "Opens a search of every command. Type part of a name or what it does, and press Return to run it.";
        c.tags = { "palette", "search", "action list", "everything" };
        c.run = [this] (const CommandArgs&)
        {
            if (! onShowPalette) return CommandResult::failed ("There's no window to show it in.");
            onShowPalette();
            return CommandResult::done();
        };
        add (c);
    }

    // MIDI controls
    {
        Command c; c.id = "controls.learn"; c.name = "Assign a MIDI control"; c.category = "Help";
        c.description = "Chooses a command, then the next pedal, footswitch, pad, key or button you press on a MIDI device runs it.";
        c.tags = { "midi learn", "footswitch", "pedal", "pad", "controller", "control surface", "foot" };
        c.askForSettings = true;
        ArgSpec a; a.name = "command"; a.type = ArgSpec::Type::choice; a.defaultValue = "Keep";
        a.liveChoices = [this]
        {
            juce::StringArray names;
            for (auto& id : commandList.allIds())
                if (auto* cmd = commandList.find (id); cmd != nullptr && ! id.startsWith ("controls."))
                    names.addIfNotAlreadyThere (cmd->name);
            names.sort (true);
            return names;
        };
        c.args = { a };
        c.run = [this] (const CommandArgs& args)
        {
            const auto name = args["command"].toString();
            for (auto& id : commandList.allIds())
                if (auto* cmd = commandList.find (id); cmd != nullptr && cmd->name == name)
                {
                    learningCommand = id;
                    learningSince = juce::Time::getMillisecondCounterHiRes() / 1000.0;
                    return CommandResult::done ("Press the pedal, pad, key or button you want for " + name
                                                + ". Escape cancels.", "Press a control for " + name + ".");
                }
            return CommandResult::failed ("There's no command called " + name + ".");
        };
        add (c);
    }
    {
        Command c; c.id = "controls.list"; c.name = "MIDI controls"; c.category = "Help"; c.isReport = true;
        c.description = "Says every MIDI control that's assigned and what it runs.";
        c.tags = { "midi learn", "footswitch", "pedal", "assigned" };
        c.run = [this] (const CommandArgs&) { return CommandResult::done (controls.listSentence()); };
        add (c);
    }
    {
        Command c; c.id = "controls.remove"; c.name = "Remove a MIDI control"; c.category = "Help";
        c.description = "Removes one assigned MIDI control.";
        c.askForSettings = true;
        c.available = [this] { return controls.all().empty() ? Availability::no ("No MIDI controls are assigned.") : Availability::yes(); };
        ArgSpec a; a.name = "control"; a.type = ArgSpec::Type::choice; a.defaultValue = "";
        a.liveChoices = [this]
        {
            juce::StringArray out;
            for (auto& b : controls.all())
                if (auto* cmd = commandList.find (b.commandId)) out.add (b.describe() + ", " + cmd->name);
            return out;
        };
        c.args = { a };
        c.run = [this] (const CommandArgs& args)
        {
            auto choices = juce::StringArray();
            for (auto& b : controls.all())
                if (auto* cmd = commandList.find (b.commandId)) choices.add (b.describe() + ", " + cmd->name);
            const int i = choices.indexOf (args["control"].toString());
            if (! controls.remove (i)) return CommandResult::failed ("That control isn't assigned.");
            saveKeymap();
            return CommandResult::done ("Removed.");
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
        k ("Primary+Shift+O", "routing.send"),
        k ("Shift+O", "routing.report", "main"),
        k ("Primary+Shift+Backspace", "routing.remove"),
        k ("Primary+Shift+H", "tracks.output"),
        k ("Primary+Shift+B", "view.beat-flash"),
        k ("Primary+Shift+G", "view.messages"),
        k ("Primary+I", "file.import-audio"),
        k ("Primary+Shift+E", "file.render"),
        k ("V", "levels.report", "main"),
        k ("N", "clips.next", "main"),
        k ("P", "clips.previous", "main"),
        k ("B", "clips.split", "main"),
        k ("Primary+[", "clips.trim-start"),
        k ("Primary+]", "clips.trim-end"),
        k ("Backspace", "clips.delete", "main"),
        k ("Delete", "clips.delete", "main"),
        k ("Shift+I", "clips.report", "main"),
        k ("Right", "transport.next-bar", "main"),
        k ("Left", "transport.previous-bar", "main"),
        k ("Shift+Right", "transport.next-beat", "main"),
        k ("Shift+Left", "transport.previous-beat", "main"),
        k ("Period", "transport.fast-forward", "main"),
        k ("Comma", "transport.rewind", "main"),
        k ("End", "transport.go-to-end", "main"),
        k ("Primary+G", "transport.go-to-bar"),
        k ("Shift+M", "markers.add", "main"),
        k ("]", "markers.next", "main"),
        k ("[", "markers.previous", "main"),
        k ("Primary+Shift+M", "markers.list"),
        k ("M", "tracks.mute", "main"),
        k ("S", "tracks.solo", "main"),
        k ("I", "tracks.report", "main"),
        k ("Primary+R", "tracks.rename"),
        k ("Primary+Backspace", "tracks.delete"),
        k ("Shift+Up", "tracks.volume-up", "main"),
        k ("Shift+Down", "tracks.volume-down", "main"),
        k ("Primary+Left", "tracks.pan-left", "main"),
        k ("Primary+Right", "tracks.pan-right", "main"),
        k ("Primary+Shift+Up", "tracks.move-up"),
        k ("Primary+Shift+Down", "tracks.move-down"),
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
        k ("Primary+/", "help.key-help"),
        k ("Primary+Shift+P", "help.find-command"),
        k ("Primary+Shift+V", "speech.settings"),
        k ("Primary+Shift+X", "speech.mute-last-kind"),
    }, keymapDefaultsVersion);
}

} // namespace earfirst
