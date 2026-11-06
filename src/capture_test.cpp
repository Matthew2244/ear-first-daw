// capture-test: does Tracktion Engine's always-on capture (retrospective record)
// keep audio from every input, playing or stopped, and put it in the right place?
//
//   capture-test selftest     feeds a known click into the engine, no microphone
//   capture-test live         uses the real audio interface: type keep, status, quit
//
// Output is one plain sentence per line, written for a screen reader.

#include <JuceHeader.h>
#include <tracktion_engine/testing/tracktion_EnginePlayer.h>
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <psapi.h>
#else
 #include <sys/resource.h>
#endif
#include <iostream>

using namespace tracktion;
using namespace std::chrono_literals;

namespace
{
void say (const juce::String& s)    { std::cout << s << std::endl; }

int failures = 0;
void check (bool ok, const juce::String& what)
{
    if (! ok) ++failures;
    say ((ok ? "Pass: " : "FAIL: ") + what);
}

double peakMemoryMB()
{
   #if JUCE_WINDOWS
    PROCESS_MEMORY_COUNTERS pmc {};
    GetProcessMemoryInfo (GetCurrentProcess(), &pmc, sizeof (pmc));
    return (double) pmc.PeakWorkingSetSize / (1024.0 * 1024.0);
   #else
    rusage ru {};
    getrusage (RUSAGE_SELF, &ru);
   #if JUCE_MAC
    return (double) ru.ru_maxrss / (1024.0 * 1024.0);   // bytes on macOS
   #else
    return (double) ru.ru_maxrss / 1024.0;              // kilobytes on Linux
   #endif
   #endif
}

struct QuietEngineBehaviour : public EngineBehaviour
{
    bool autoInitialiseDeviceManager() override    { return false; }
};

// Sample index of the first loud sample in a file, or -1.
int64_t findClick (Engine& engine, const juce::File& f)
{
    std::unique_ptr<juce::AudioFormatReader> r (AudioFileUtils::createReaderFor (engine, f));
    if (r == nullptr) return -1;

    juce::AudioBuffer<float> buf ((int) r->numChannels, 4096);
    for (int64_t pos = 0; pos < r->lengthInSamples; pos += 4096)
    {
        auto n = (int) std::min<int64_t> (4096, r->lengthInSamples - pos);
        r->read (&buf, 0, n, pos, true, true);
        for (int i = 0; i < n; ++i)
            if (std::abs (buf.getSample (0, i)) > 0.5f)
                return pos + i;
    }
    return -1;
}

// Where on the timeline does the click inside this clip land, in seconds?
double clickTimeOnTimeline (Engine& engine, WaveAudioClip& clip, double sampleRate)
{
    auto idx = findClick (engine, clip.getOriginalFile());
    if (idx < 0) return -1.0;
    auto pos = clip.getPosition();
    return pos.getStart().inSeconds() + (double) idx / sampleRate - pos.getOffset().inSeconds();
}

//==============================================================================
struct Rig
{
    Rig (Engine& e, int numInputs, double sr, int block, double bufferSeconds)
        : engine (e)
    {
        params.sampleRate = sr;
        params.blockSize = block;
        params.inputChannels = numInputs;
        params.outputChannels = 2;

        player = std::make_unique<test_utilities::EnginePlayer> (engine, params);
        for (auto wi : engine.getDeviceManager().getWaveInputDevices())
            wi->setEnabled (true);

        folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getNonexistentChildFile ("ear-first-capture", {});
        folder.createDirectory();
        tempProject = std::make_unique<ProjectManager::TempProject> (engine.getProjectManager(),
                                                                     folder.getChildFile ("test.tracktion"), true);
        auto item = tempProject->project->createNewEdit();
        edit = createEmptyEdit (engine, item->getSourceFile());
        edit->setProjectItemRef (item->getProjectItemRef());
        edit->ensureNumberOfAudioTracks (numInputs);

        auto& transport = edit->getTransport();
        transport.ensureContextAllocated();
        auto context = transport.getCurrentPlaybackContext();
        auto tracks = getAudioTracks (*edit);
        auto inputs = context->getAllInputs();
        inputs.removeIf ([] (auto i) { return i->owner.isMidi(); });

        for (int i = 0; i < std::min (inputs.size(), tracks.size()); ++i)
        {
            [[maybe_unused]] auto r = inputs[i]->setTarget (tracks[i]->itemID, true, nullptr, 0);
            inputs[i]->setRecordingEnabled (tracks[i]->itemID, true);
        }

        for (auto wi : engine.getDeviceManager().getWaveInputDevices())
            wi->updateRetrospectiveBufferLength (bufferSeconds);

        edit->dispatchPendingUpdatesSynchronously();
        numInputsAttached = inputs.size();
    }

    ~Rig()
    {
        edit.reset();
        tempProject.reset();
        player.reset();
        auto& dm = engine.getDeviceManager();
        dm.closeDevices();
        dm.removeHostedAudioDeviceInterface();
        dm.deviceManager.closeAudioDevice();
        folder.deleteRecursively();
    }

    // Feed `seconds` of silence, with a click on every input at `clickAt` seconds
    // from the start of this call (negative = no click).
    void feed (double seconds, double clickAt = -1.0)
    {
        auto total = (int64_t) std::llround (seconds * params.sampleRate);
        auto clickSample = clickAt < 0 ? -1 : (int64_t) std::llround (clickAt * params.sampleRate);
        const int chunk = params.blockSize * 64;

        juce::AudioBuffer<float> in (params.inputChannels, chunk);
        for (int64_t done = 0; done < total;)
        {
            auto n = (int) std::min<int64_t> (chunk, total - done);
            in.setSize (params.inputChannels, n, false, false, true);
            in.clear();
            if (clickSample >= done && clickSample < done + n)
                for (int c = 0; c < params.inputChannels; ++c)
                    in.setSample (c, (int) (clickSample - done), 1.0f);
            player->process (in);
            done += n;
        }
    }

    juce::Array<WaveAudioClip*> clips()
    {
        juce::Array<WaveAudioClip*> out;
        for (auto t : getAudioTracks (*edit))
            for (auto c : t->getClips())
                if (auto w = dynamic_cast<WaveAudioClip*> (c))
                    out.add (w);
        return out;
    }

    Engine& engine;
    HostedAudioDeviceInterface::Parameters params;
    std::unique_ptr<test_utilities::EnginePlayer> player;
    juce::File folder;
    std::unique_ptr<ProjectManager::TempProject> tempProject;
    std::unique_ptr<Edit> edit;
    int numInputsAttached = 0;
};

//==============================================================================
// Checks every clip: one per input, click present, all at the same timeline spot.
void checkClips (Rig& rig, double expectedClickTime, double expectedLength, const juce::String& label,
                 bool timelineMatters = true)
{
    auto clips = rig.clips();
    check (clips.size() == rig.params.inputChannels,
           label + ": " + juce::String (clips.size()) + " takes kept from "
               + juce::String (rig.params.inputChannels) + " inputs.");
    if (clips.isEmpty()) return;

    double worstMs = 0.0, first = -1.0;
    bool allFound = true, allSame = true;
    for (auto c : clips)
    {
        auto t = clickTimeOnTimeline (rig.engine, *c, rig.params.sampleRate);
        if (t < 0) { allFound = false; continue; }
        if (first < 0) first = t;
        if (std::abs (t - first) > 0.5 / rig.params.sampleRate) allSame = false;
        worstMs = std::max (worstMs, std::abs (t - expectedClickTime) * 1000.0);
    }

    auto len = clips[0]->getPosition().getLength().inSeconds();
    check (std::abs (len - expectedLength) < 0.05,
           label + ": each take is " + juce::String (len, 2) + " seconds long, expected "
               + juce::String (expectedLength, 2) + ".");
    check (allFound, label + ": the click is in every take.");
    check (allSame, label + ": every input's click lands on the same sample.");

    auto samplesOff = worstMs * rig.params.sampleRate / 1000.0;
    say (label + ": click lands at " + juce::String (first, 4) + " seconds on the timeline, expected "
         + juce::String (expectedClickTime, 4) + ". Off by " + juce::String (samplesOff, 1)
         + " samples (" + juce::String (worstMs, 2) + " ms).");
    if (timelineMatters)
        check (samplesOff < 0.5, label + ": click lands on the exact sample.");
    else
        say (label + ": nothing was playing, so there is no timeline position to match; the take just goes before the playhead.");
}

//==============================================================================
int runSelfTest (Engine& engine)
{
    const double sr = 48000.0;
    const int block = 256;

    say ("Self test. No microphone is used; the engine is fed a known click.");

    // 1. Simplest setup: one input, transport never started.
    {
        say ("");
        say ("Test 1: one input, stopped, nothing playing. Click at 4 seconds into 10 seconds of input.");
        Rig rig (engine, 1, sr, block, 30.0);
        rig.feed (10.0, 4.0);
        auto res = rig.edit->getTransport().applyRetrospectiveRecord (false);
        check (res.wasOk(), "Test 1: capture accepted. " + res.getErrorMessage());
        // Stopped at 0 and never played: the take is placed starting at 0.
        checkClips (rig, 4.0, 10.0, "Test 1");
    }

    // 2. Reference: a normal record pass, click 6 seconds in.
    double recordedClickTime = -1.0;
    {
        say ("");
        say ("Test 2: normal recording on 8 inputs, for reference. Click 6 seconds in.");
        Rig rig (engine, 8, sr, block, 30.0);
        auto& tc = rig.edit->getTransport();
        tc.record (false, false);
        rig.edit->dispatchPendingUpdatesSynchronously();
        rig.feed (10.0, 6.0);
        tc.stop (false, true);
        auto clips = rig.clips();
        check (clips.size() == 8, "Test 2: " + juce::String (clips.size()) + " normal takes recorded.");
        if (! clips.isEmpty())
        {
            recordedClickTime = clickTimeOnTimeline (engine, *clips[0], sr);
            say ("Test 2: normal recording puts the click at " + juce::String (recordedClickTime, 4) + " seconds.");
        }
    }

    // 3. Playing along: capture while the song plays, same click, compare to normal record.
    {
        say ("");
        say ("Test 3: 8 inputs, song playing, not recording. Click 6 seconds in, then keep it.");
        Rig rig (engine, 8, sr, block, 30.0);
        auto& tc = rig.edit->getTransport();
        tc.play (false);
        rig.edit->dispatchPendingUpdatesSynchronously();
        rig.feed (10.0, 6.0);
        auto res = tc.applyRetrospectiveRecord (false);
        check (res.wasOk(), "Test 3: capture accepted. " + res.getErrorMessage());
        tc.stop (false, true);
        auto expected = recordedClickTime > 0 ? recordedClickTime : 6.0;
        say ("Test 3: comparing against where normal recording put the same click.");
        checkClips (rig, expected, 10.0, "Test 3");
    }

    // 4. Most demanding: a full room of inputs and a long buffer.
    {
        const int inputs = 44;
        const double seconds = 120.0;
        say ("");
        say ("Test 4: " + juce::String (inputs) + " inputs, " + juce::String (seconds, 0)
             + " second buffer, stopped. Click 100 seconds in.");
        auto before = peakMemoryMB();
        Rig rig (engine, inputs, sr, block, seconds);
        auto t0 = juce::Time::getMillisecondCounterHiRes();
        rig.feed (seconds, 100.0);
        auto t1 = juce::Time::getMillisecondCounterHiRes();
        auto res = rig.edit->getTransport().applyRetrospectiveRecord (false);
        auto t2 = juce::Time::getMillisecondCounterHiRes();
        check (res.wasOk(), "Test 4: capture accepted. " + res.getErrorMessage());
        checkClips (rig, 100.0, seconds, "Test 4", false);
        auto expectedMB = inputs * seconds * sr * 4.0 / (1024.0 * 1024.0);
        say ("Test 4: memory grew by about " + juce::String (peakMemoryMB() - before, 0)
             + " MB. The buffer itself should be " + juce::String (expectedMB, 0) + " MB.");
        say ("Test 4: feeding two minutes of audio took " + juce::String ((t1 - t0) / 1000.0, 1)
             + " seconds; saving the takes took " + juce::String ((t2 - t1) / 1000.0, 1) + " seconds.");
        say ("Test 4: so a 10 minute buffer on 44 inputs at 48 kHz would hold about "
             + juce::String (expectedMB * 5.0 / 1024.0, 1) + " GB in memory.");
    }

    // 5. Demanding and playing: buffer overflows, timing must still be exact.
    {
        const int inputs = 44;
        say ("");
        say ("Test 5: " + juce::String (inputs) + " inputs, 60 second buffer, song playing for 90 seconds so the buffer overflows. Click 80 seconds in.");
        Rig rig (engine, inputs, sr, block, 60.0);
        auto& tc = rig.edit->getTransport();
        tc.play (false);
        rig.edit->dispatchPendingUpdatesSynchronously();
        rig.feed (90.0, 80.0);
        auto res = tc.applyRetrospectiveRecord (false);
        check (res.wasOk(), "Test 5: capture accepted. " + res.getErrorMessage());
        tc.stop (false, true);
        checkClips (rig, 80.0, 60.0, "Test 5");
    }

    say ("");
    say (failures == 0 ? "All checks passed." : juce::String (failures) + " checks failed.");
    return failures == 0 ? 0 : 1;
}

//==============================================================================
int runLive (Engine& engine)
{
    auto& dm = engine.getDeviceManager();
    dm.initialise (256, 2);
    dm.dispatchPendingUpdates();

    auto inputs = dm.getWaveInputDevices();
    if (inputs.empty())
    {
        say ("No audio inputs found. Check the interface is connected and microphone access is allowed.");
        return 1;
    }
    for (auto wi : inputs)
        wi->setEnabled (true);
    dm.dispatchPendingUpdates();
    inputs = dm.getWaveInputDevices();

    auto folder = juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                      .getChildFile ("Ear First Capture Test");
    folder.createDirectory();
    ProjectManager::TempProject project (engine.getProjectManager(),
                                         folder.getNonexistentChildFile ("session", ".tracktion"), true);
    auto item = project.project->createNewEdit();
    auto edit = createEmptyEdit (engine, item->getSourceFile());
    edit->setProjectItemRef (item->getProjectItemRef());
    edit->ensureNumberOfAudioTracks ((int) inputs.size());

    auto& tc = edit->getTransport();
    tc.ensureContextAllocated();
    auto ctx = tc.getCurrentPlaybackContext();
    auto instances = ctx->getAllInputs();
    instances.removeIf ([] (auto i) { return i->owner.isMidi(); });
    auto tracks = getAudioTracks (*edit);
    for (int i = 0; i < std::min (instances.size(), tracks.size()); ++i)
        [[maybe_unused]] auto r = instances[i]->setTarget (tracks[i]->itemID, true, nullptr, 0);

    auto dev = dm.deviceManager.getCurrentAudioDevice();
    say ("Listening on " + (dev ? dev->getName() : juce::String ("no device")) + ", "
         + juce::String (instances.size()) + " inputs, keeping the last 30 seconds of each.");
    say ("Type keep and press Return to keep what you just played. Type status, or quit.");

    for (std::string line; std::getline (std::cin, line);)
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
        auto cmd = juce::String (line).trim().toLowerCase();
        if (cmd == "quit" || cmd == "q") break;
        if (cmd == "status")
        {
            say (juce::String (instances.size()) + " inputs listening. Takes are saved in " + folder.getFullPathName() + ".");
            continue;
        }
        if (cmd == "keep" || cmd == "k")
        {
            auto files = tc.getRetrospectiveRecordAsAudioFiles();
            if (files.isEmpty())
                say ("Nothing to keep yet.");
            for (auto& f : files)
                say ("Kept " + f.getFileName() + ", "
                     + juce::String (AudioFile (engine, f).getLength(), 1) + " seconds.");
            continue;
        }
        say ("Commands are keep, status and quit.");
    }
    say ("Done. Takes are in " + folder.getFullPathName() + ".");
    return 0;
}
} // namespace

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI init;
    Engine engine (std::make_unique<PropertyStorage> ("EarFirstCaptureTest"),
                   std::make_unique<UIBehaviour>(),
                   std::make_unique<QuietEngineBehaviour>());

    juce::String mode = argc > 1 ? argv[1] : "selftest";
    if (mode == "live")
        return runLive (engine);
    return runSelfTest (engine);
}
