#include "session/Session.h"
#include "capture/CaptureRecorder.h"
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#else
 #include <signal.h>
 #include <unistd.h>
#endif

namespace earfirst
{
using namespace tracktion;

namespace
{
juce::String countText (int n, const char* unit)
{
    return juce::String (n) + " " + unit + (n == 1 ? "" : "s");
}

juce::String lengthText (double secs)
{
    secs = std::max (0.0, secs);
    if (secs < 90) return countText (juce::roundToInt (secs), "second");
    if (secs < 5400) return countText (juce::roundToInt (secs / 60.0), "minute");
    return juce::String (secs / 3600.0, 1) + " hours";
}

juce::String agoText (juce::RelativeTime t)    { return lengthText (t.inSeconds()); }

bool processIsAlive (int pid)
{
    if (pid <= 0) return false;
   #if JUCE_WINDOWS
    auto h = OpenProcess (PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD) pid);
    if (h == nullptr) return GetLastError() == ERROR_ACCESS_DENIED;
    DWORD code = 0;
    const bool alive = GetExitCodeProcess (h, &code) && code == STILL_ACTIVE;
    CloseHandle (h);
    return alive;
   #else
    return ::kill (pid, 0) == 0 || errno == EPERM;
   #endif
}

int thisProcessId()
{
   #if JUCE_WINDOWS
    return (int) GetCurrentProcessId();
   #else
    return (int) ::getpid();
   #endif
}
} // namespace

Session::Session (Engine& e, juce::File f) : engine (e), folder (std::move (f)) {}

std::unique_ptr<Session> Session::create (Engine& engine, const juce::File& folder, juce::String& error)
{
    if (folder.exists() && folder.getNumberOfChildFiles (juce::File::findFilesAndDirectories) > 0)
    {
        error = "There's already something in " + folder.getFullPathName() + ".";
        return {};
    }
    if (! folder.createDirectory())
    {
        error = "I couldn't make the folder " + folder.getFullPathName() + ".";
        return {};
    }
    std::unique_ptr<Session> s (new Session (engine, folder));
    s->audioFolder().createDirectory();
    s->captureFolder().createDirectory();
    s->edit = createEmptyEdit (engine, s->songFile());
    s->writeMarker();
    if (! s->writeSong (s->songFile()))
    {
        error = "I couldn't write the song file.";
        return {};
    }
    s->edit->resetChangedStatus();
    s->lastSavedState = s->currentState();
    return s;
}

std::unique_ptr<Session> Session::open (Engine& engine, const juce::File& folder, OpenReport& report, juce::String& error)
{
    std::unique_ptr<Session> s (new Session (engine, folder));
    if (! s->songFile().existsAsFile())
    {
        error = "There's no song in " + folder.getFullPathName() + ".";
        return {};
    }

    // Is it open somewhere else, or was it left open by an app that died?
    if (s->markerFile().existsAsFile())
    {
        auto lines = juce::StringArray::fromLines (s->markerFile().loadFileAsString());
        const int pid = lines[0].getIntValue();
        if (pid != thisProcessId() && processIsAlive (pid) && lines[1] == juce::SystemStats::getComputerName())
        {
            error = "This song is already open in another copy of the app.";
            return {};
        }
        report.closedBadly = true;
    }

    auto fileToLoad = s->songFile();
    if (report.closedBadly && s->autosaveFile().existsAsFile()
        && s->autosaveFile().getLastModificationTime() > s->songFile().getLastModificationTime())
    {
        fileToLoad = s->autosaveFile();
        report.usedAutosave = true;
    }

    s->edit = loadEditFromFile (engine, fileToLoad);
    if (s->edit == nullptr && report.usedAutosave)
    {
        // A damaged autosave never stops the song opening.
        report.usedAutosave = false;
        s->edit = loadEditFromFile (engine, s->songFile());
    }
    if (s->edit == nullptr)
    {
        error = "The song file couldn't be read.";
        return {};
    }
    // Keep saving to the song file, whichever file we opened.
    s->edit->editFileRetriever = [f = s->songFile()] { return f; };

    // Audio captured but never kept: any capture run still on disk now
    // belongs to a session that didn't close cleanly.
    for (auto& run : s->captureFolder().findChildFiles (juce::File::findDirectories, false))
        if (auto info = CaptureInfo::load (run))
        {
            CaptureStore store (run, *info);
            const auto len = (double) (store.endSampleOnDisk() - store.firstSampleOnDisk()) / info->sampleRate;
            if (len <= 0) { run.deleteRecursively(); continue; }
            report.leftoverCaptures.add (run);
            report.leftoverSeconds = std::max (report.leftoverSeconds, len);
            report.leftoverInputs = std::max (report.leftoverInputs, info->numInputs());
        }

    juce::StringArray parts;
    if (report.closedBadly)
    {
        if (report.usedAutosave)
            parts.add ("This song didn't close properly last time. I opened the autosave, which is "
                       + agoText (s->autosaveFile().getLastModificationTime() - s->songFile().getLastModificationTime())
                       + " newer than your last save.");
        else
            parts.add ("This song didn't close properly last time. Nothing was lost since your last save.");
    }
    if (! report.leftoverCaptures.isEmpty())
        parts.add ("There's " + lengthText (report.leftoverSeconds) + " of audio from " + juce::String (report.leftoverInputs)
                   + (report.leftoverInputs == 1 ? " input" : " inputs") + " that was captured but never kept. You can keep it or throw it away.");
    report.sentence = parts.joinIntoString (" ");

    s->writeMarker();
    s->edit->resetChangedStatus();
    s->lastSavedState = s->currentState();
    s->openedFromAutosave = report.usedAutosave;   // differs from the song file, so it counts as unsaved
    s->audioFolder().createDirectory();
    s->captureFolder().createDirectory();
    return s;
}

Session::~Session()
{
    edit.reset();
    if (crashed) return;
    autosaveFile().deleteFile();
    markerFile().deleteFile();
}

void Session::writeMarker()
{
    markerFile().replaceWithText (juce::String (thisProcessId()) + "\n" + juce::SystemStats::getComputerName() + "\n");
}

void Session::simulateCrashMarker (const juce::File& f)
{
    // A process id that can't be running.
    f.getChildFile (".open").replaceWithText ("999999\n" + juce::SystemStats::getComputerName() + "\n");
}

bool Session::writeSong (const juce::File& dest)
{
    // Write beside the real file, then swap it in, so a crash mid-save can
    // never leave a half-written song.
    juce::TemporaryFile temp (dest);
    edit->flushState();
    auto xml = edit->state.createXml();
    if (xml == nullptr || ! xml->writeTo (temp.getFile()))
        return false;
    return temp.overwriteTargetFileWithTemporary();
}

juce::String Session::currentState() const
{
    edit->flushState();
    return edit->state.toXmlString();
}

bool Session::hasUnsavedChanges() const
{
    // The engine marks changes a moment after they happen, so also compare
    // the song itself with what was last saved: a change made just before a
    // save, a close or a crash always counts.
    return openedFromAutosave || edit->hasChangedSinceSaved() || currentState() != lastSavedState;
}

juce::String Session::save()
{
    if (! writeSong (songFile()))
        return "Couldn't save. The disk may be full or the folder may have moved.";
    edit->resetChangedStatus();
    openedFromAutosave = false;
    lastSavedState = currentState();
    autosaveFile().deleteFile();
    lastAutosavedState.clear();
    return "Saved.";
}

bool Session::autosaveIfNeeded()
{
    auto now = currentState();
    if (! openedFromAutosave && now == lastSavedState)
        return false;
    if (now == lastAutosavedState)
        return false;
    if (! writeSong (autosaveFile()))
        return false;
    lastAutosavedState = now;
    return true;
}

juce::String Session::keepLeftoverCapture (const juce::File& run)
{
    auto info = CaptureInfo::load (run);
    if (! info) return "That audio can't be read any more.";
    CaptureStore store (run, *info);
    juce::Array<int> all;
    for (int i = 0; i < info->numInputs(); ++i) all.add (i);
    auto dest = audioFolder().getChildFile ("Recovered").getChildFile (info->started.formatted ("%Y-%m-%d %H-%M-%S"));
    auto r = store.exportTake (store.firstSampleOnDisk(), store.endSampleOnDisk(), all, dest, "Recovered");
    if (! r.ok()) return "Couldn't keep it: " + r.error;
    run.deleteRecursively();
    return "Kept " + lengthText ((double) (r.end - r.start) / info->sampleRate) + " on " + juce::String (r.files.size())
           + (r.files.size() == 1 ? " input" : " inputs") + ", in Audio, Recovered.";
}

juce::String Session::discardLeftoverCapture (const juce::File& run)
{
    return run.deleteRecursively() ? "Thrown away." : "Couldn't throw it away.";
}

} // namespace earfirst
