// session-test: saving, autosave, and coming back after a crash.
// Output is one plain sentence per line, written for a screen reader.

#include <JuceHeader.h>
#include "session/Session.h"
#include "capture/CaptureRecorder.h"
#include <iostream>
#include <thread>

using namespace tracktion;
using namespace earfirst;

namespace
{
void say (const juce::String& s)    { std::cout << s << std::endl; }

int failures = 0;
void check (bool ok, const juce::String& what)
{
    if (! ok) ++failures;
    say ((ok ? "Pass: " : "FAIL: ") + what);
}

struct QuietEngineBehaviour : public EngineBehaviour
{
    bool autoInitialiseDeviceManager() override    { return false; }
};

juce::File writeTone (const juce::File& f, double seconds)
{
    f.getParentDirectory().createDirectory();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> os (new juce::FileOutputStream (f));
    auto w = wav.createWriterFor (os, juce::AudioFormatWriterOptions{}.withSampleRate (48000).withNumChannels (1).withBitsPerSample (24));
    juce::AudioBuffer<float> b (1, (int) (seconds * 48000));
    for (int i = 0; i < b.getNumSamples(); ++i) b.setSample (0, i, 0.25f * std::sin (i * 0.05f));
    w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
    return f;
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    Engine engine (std::make_unique<PropertyStorage> ("EarFirstSessionTest"),
                   std::make_unique<UIBehaviour>(), std::make_unique<QuietEngineBehaviour>());
    say ("Session test: saving, autosave and crash recovery.");

    auto root = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("EarFirstSessionTest");
    root.deleteRecursively();
    root.createDirectory();
    auto folder = root.getChildFile ("Morning Song");
    juce::String error;

    say ("");
    say ("A new song");
    {
        auto s = Session::create (engine, folder, error);
        check (s != nullptr, "a new song is made. " + error);
        if (s == nullptr) return 1;
        check (s->songFile().existsAsFile() && s->audioFolder().isDirectory() && s->captureFolder().isDirectory(),
               "it has its song file, an Audio folder and a Capture folder.");
        check (s->markerFile().existsAsFile(), "while it's open, the open marker is there.");

        auto& edit = s->getEdit();
        edit.ensureNumberOfAudioTracks (2);
        auto tracks = getAudioTracks (edit);
        tracks[0]->setName ("Hammond");
        tracks[1]->setName ("Vocal");
        auto tone = writeTone (s->audioFolder().getChildFile ("Hammond take.wav"), 3.0);
        tracks[0]->insertWaveClip ("Hammond take", tone, { { TimePosition::fromSeconds (2.0), TimeDuration::fromSeconds (3.0) } }, false);
        check (s->hasUnsavedChanges(), "after adding tracks and a clip, it knows there are unsaved changes.");
        check (s->save() == "Saved." && ! s->hasUnsavedChanges(), "saving says Saved, and nothing is unsaved after.");
        check (! s->autosaveIfNeeded(), "with nothing changed, autosave writes nothing.");
        edit.getTransport().setPosition (TimePosition::fromSeconds (12.0));
        check (! s->hasUnsavedChanges(), "moving the playhead doesn't count as an unsaved change.");
    }
    check (! folder.getChildFile (".open").exists(), "closing cleanly removes the open marker.");

    say ("");
    say ("Opening it again");
    {
        Session::OpenReport report;
        auto s = Session::open (engine, folder, report, error);
        check (s != nullptr, "it opens. " + error);
        if (s == nullptr) return 1;
        check (report.sentence.isEmpty(), "after a clean close there's nothing to report.");
        auto tracks = getAudioTracks (s->getEdit());
        check (tracks.size() == 2 && tracks[0]->getName() == "Hammond" && tracks[1]->getName() == "Vocal", "both tracks came back with their names.");
        auto clips = tracks[0]->getClips();
        auto* wave = clips.isEmpty() ? nullptr : dynamic_cast<WaveAudioClip*> (clips[0]);
        check (wave != nullptr && std::abs (wave->getPosition().getStart().inSeconds() - 2.0) < 1e-9, "the clip is back at 2 seconds.");
        check (wave != nullptr && wave->getOriginalFile().existsAsFile(), "and its audio file is found: " + (wave ? wave->getOriginalFile().getFileName() : juce::String ("none")) + ".");

        // A change, then an autosave, then the app dies.
        tracks[1]->setName ("Lead vocal");
        std::this_thread::sleep_for (std::chrono::milliseconds (1100));   // file times have one-second steps on some disks
        check (s->autosaveIfNeeded(), "after a change, autosave writes the autosave file.");
        check (! s->autosaveIfNeeded(), "and doesn't write it again until something else changes.");

        // Capture runs, then never gets kept.
        CaptureRecorder::Options o;
        o.folder = s->captureFolder().getChildFile ("run 1");
        o.sampleRate = 48000; o.inputNames = { "Hammond L", "Hammond R" }; o.deleteOnClose = false;
        {
            CaptureRecorder rec (o);
            juce::AudioBuffer<float> b (2, 480);
            for (int i = 0; i < 480; ++i) { b.setSample (0, i, 0.1f); b.setSample (1, i, -0.1f); }
            for (int n = 0; n < 300; ++n) rec.push (b.getArrayOfReadPointers(), 480);   // 3 seconds
            rec.stop();
        }
        s->abandonWithoutClosing();
    }
    check (folder.getChildFile (".open").exists(), "after a crash the open marker is left behind.");

    say ("");
    say ("Coming back after the crash");
    {
        Session::OpenReport report;
        auto s = Session::open (engine, folder, report, error);
        check (s != nullptr, "it opens. " + error);
        if (s == nullptr) return 1;
        check (report.closedBadly && report.usedAutosave, "it knows it didn't close properly, and opened the newer autosave.");
        auto tracks = getAudioTracks (s->getEdit());
        check (tracks.size() == 2 && tracks[1]->getName() == "Lead vocal", "the change made after the last save is there.");
        check (s->hasUnsavedChanges(), "and it counts as unsaved until you save.");
        check (report.leftoverCaptures.size() == 1 && std::abs (report.leftoverSeconds - 3.0) < 0.01 && report.leftoverInputs == 2,
               "it found the 3 seconds on 2 inputs that were captured but never kept.");
        say ("It says: " + report.sentence);
        check (report.sentence.contains ("didn't close properly") && report.sentence.contains ("3 seconds of audio from 2 inputs"),
               "one sentence covers both.");
        auto kept = s->keepLeftoverCapture (report.leftoverCaptures[0]);
        say ("Keeping it says: " + kept);
        auto recovered = s->audioFolder().getChildFile ("Recovered").findChildFiles (juce::File::findFiles, true, "*.wav");
        check (recovered.size() == 2 && ! report.leftoverCaptures[0].exists(), "keeping it makes 2 files in Audio, Recovered, and clears the raw capture.");
        check (s->save() == "Saved." && ! s->hasUnsavedChanges() && ! s->autosaveFile().exists(), "saving clears the unsaved state and the autosave.");
    }

    say ("");
    say ("A damaged autosave never stops the song opening");
    {
        Session::simulateCrashMarker (folder);
        std::this_thread::sleep_for (std::chrono::milliseconds (1100));
        folder.getChildFile ("Morning Song.autosave").replaceWithText ("this is not a song");
        Session::OpenReport report;
        auto s = Session::open (engine, folder, report, error);
        check (s != nullptr && ! report.usedAutosave, "it opens the last save instead. " + error);
        if (s != nullptr)
            check (getAudioTracks (s->getEdit()).size() == 2, "with both tracks.");
    }

    say ("");
    say ("Open twice");
    {
        // Pretend a live copy of the app on this computer has it open (a process that is always running).
       #if JUCE_WINDOWS
        const juce::String alwaysRunning = "4";   // the Windows System process
       #else
        const juce::String alwaysRunning = "1";   // launchd
       #endif
        folder.getChildFile (".open").replaceWithText (alwaysRunning + "\n" + juce::SystemStats::getComputerName() + "\n");
        Session::OpenReport report;
        auto s = Session::open (engine, folder, report, error);
        check (s == nullptr && error.contains ("already open"), "a song open in another copy of the app isn't opened twice: \"" + error + "\"");
        folder.getChildFile (".open").deleteFile();
    }

    say ("");
    say ("Making a song where one already is");
    {
        auto s = Session::create (engine, folder, error);
        check (s == nullptr && error.contains ("already something"), "it refuses rather than overwrite: \"" + error + "\"");
    }

    root.deleteRecursively();
    say ("");
    say (failures == 0 ? "All checks passed." : juce::String (failures) + " checks failed.");
    return failures == 0 ? 0 : 1;
}
