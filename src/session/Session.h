#pragma once
// Session: one song on disk, saved safely and brought back after a crash.
//
// A song is a folder:
//
//   My Song/
//     My Song.earfirst          the song (Tracktion's edit format)
//     My Song.autosave          written every half minute while there are unsaved changes
//     Audio/                    kept takes and recovered audio
//     Capture/                  the always-on recorder's raw audio
//     .open                     present while the song is open; says which app has it
//
// Saving writes a new file beside the old one and swaps it in, so a crash
// mid-save can never leave a half-written song. If the app dies, the next
// open finds the .open marker left behind, loads the autosave if it's newer
// than the last save, finds any audio that was captured but never kept, and
// says what it found in one sentence.

#include <JuceHeader.h>

namespace earfirst
{

class Session
{
public:
    struct OpenReport
    {
        bool closedBadly = false;         // the app didn't close the song last time
        bool usedAutosave = false;        // and the autosave was newer, so that's what opened
        juce::Array<juce::File> leftoverCaptures;   // captured audio never kept
        double leftoverSeconds = 0;       // longest of those, in seconds
        int leftoverInputs = 0;
        juce::String sentence;            // what to say about all of the above; empty if nothing happened
    };

    // A new, empty song in a new folder.
    static std::unique_ptr<Session> create (tracktion::Engine&, const juce::File& folder, juce::String& error);
    // An existing song.
    static std::unique_ptr<Session> open (tracktion::Engine&, const juce::File& folder, OpenReport&, juce::String& error);

    ~Session();   // a clean close: removes the marker and the autosave

    tracktion::Edit& getEdit()          { return *edit; }
    juce::String getName() const        { return folder.getFileName(); }
    juce::File getFolder() const        { return folder; }
    juce::File songFile() const         { return folder.getChildFile (getName() + ".earfirst"); }
    juce::File autosaveFile() const     { return folder.getChildFile (getName() + ".autosave"); }
    juce::File audioFolder() const      { return folder.getChildFile ("Audio"); }
    juce::File captureFolder() const    { return folder.getChildFile ("Capture"); }
    juce::File markerFile() const       { return folder.getChildFile (".open"); }

    bool hasUnsavedChanges() const;
    // Cheap enough for a screen that updates ten times a second; may lag a change by a moment.
    bool looksUnsaved() const          { return openedFromAutosave || edit->hasChangedSinceSaved(); }

    // Saves now. Returns what to say: "Saved." or why it couldn't.
    juce::String save();
    // Called every half minute or so: writes the autosave if anything changed
    // since the last save or autosave. True if it wrote one.
    bool autosaveIfNeeded();

    // Audio captured before a crash and never kept: keep all of it as takes
    // in Audio/Recovered, or throw it away. Each returns what to say.
    juce::String keepLeftoverCapture (const juce::File& captureRun);
    juce::String discardLeftoverCapture (const juce::File& captureRun);

    // For tests: pretend this song was left open by an app that's gone, or
    // drop the session without the clean-close tidy-up, as a crash would.
    static void simulateCrashMarker (const juce::File& folder);
    void abandonWithoutClosing()        { crashed = true; }

private:
    Session (tracktion::Engine&, juce::File folder);
    bool writeSong (const juce::File& dest);
    void writeMarker();

    tracktion::Engine& engine;
    juce::File folder;
    std::unique_ptr<tracktion::Edit> edit;
    juce::String lastAutosavedState, lastSavedState;
    juce::String currentState() const;
    bool openedFromAutosave = false, crashed = false;
};

} // namespace earfirst
