#pragma once
// AppCore: everything the app is, without the window. The engine, the song,
// the always-on capture, the command list, the keymap and the speaking layer,
// wired together. The window, menus, palette and self-test all drive this.

#include <JuceHeader.h>
#include "commands/Commands.h"
#include "commands/Keymap.h"
#include "commands/MidiControls.h"
#include "speech/Speech.h"
#include "session/Session.h"
#include "capture/EngineCapture.h"
#include "app/Updater.h"

namespace earfirst
{

class AppCore : public CommandHost, private juce::Timer
{
public:
    struct Options
    {
        bool openAudioDevice = true;      // false for the self-test: no microphone, no device
        juce::File songsFolder;           // where new songs go
        bool saveSettings = true;         // false for the self-test: never touch your settings or keys
    };

    AppCore (SpeechOutput&, Options);
    ~AppCore() override;

    // CommandHost
    void speak (const Message&) override;
    // Say something that isn't a command's result, through your speech choices.
    void say (const juce::String& full, const juce::String& kind, const juce::String& brief = {},
              Speech::Priority = Speech::Priority::normal, const juce::String& topic = {});
    SpeechPrefs speechPrefs;
    juce::String lastKind;           // the kind of the last message, for "don't say that kind"
    std::pair<float, float> lastEarcon { 0, 0 };   // the last tone played instead of words (for the self-test)
    // A place on the timeline, the way you've chosen: bars, time, or both.
    juce::String placeText (const tracktion::Edit&, tracktion::TimePosition) const;
    void beginUndoStep (const juce::String&) override;
    void endUndoStep() override;

    CommandList& commands()             { return commandList; }
    Keymap& keymap()                    { return keys; }
    MidiControls& midiControls()        { return controls; }
    // A MIDI message from any device: learning a control, or running one. Message thread.
    void handleMidiControl (const juce::String& device, const juce::MidiMessage&);
    Speech& speech()                    { return speaker; }
    tracktion::Engine& getEngine()      { return engine; }
    Session* session()                  { return song.get(); }
    Updater& updater()                  { return *updates; }
    // Said once the window is up: anything found from a run that didn't close cleanly.
    juce::String startupSentence() const;
    juce::String leftoverSentence() const;
    juce::String startupProblemSentence() const   { return startupProblem; }

    // Songs.
    juce::String newSong (const juce::String& name);
    juce::String openSong (const juce::File& folder);
    juce::String closeSong();
    std::function<void()> onSongChanged;        // the window refreshes its lists
    std::function<void()> onOpenSongRequested;  // the window shows a folder chooser
    std::function<void()> onShowPalette;        // the window shows Find a command
    bool keyHelpOn = false;                     // key help: keys say what they do instead of doing it
    std::function<void (std::function<void()>)> onUnsavedClose;   // ask save / don't save / cancel, then run it

    // Run a command the way a person asked for it: from a menu or the
    // palette, a command that needs settings shows its form first.
    CommandResult runFromUser (const juce::String& id, CommandList::Source);
    std::function<void (const Command&)> onSettingsNeeded;   // the window shows the form

    // The audio interface.
    juce::String audioSentence() const;
    // The click and the song's tempo.
    class ClickPlugin* clickPlugin() const;
    tracktion::AudioTrack* clickTrack() const;
    void ensureClickTrack();
    bool clickIsOn() const;
    juce::String clickSentence() const;
    juce::String tempoSentence() const;
    void playFromHere();   // with the click's count-in, if it has one
    void syncCountIn();
    tracktion::TimePosition countInStart (tracktion::TimePosition from, int bars) const;
    // Short lines for the screen: what's playing and what capture is doing.
    juce::String transportLine() const;
    juce::String captureLine() const;
    // Text size for everyone who reads the screen, kept between launches.
    float textScale = 1.0f;
    juce::StringArray inputDeviceNames() const;
    juce::StringArray outputDeviceNames() const;
    juce::String setAudio (const juce::String& input, const juce::String& output, double rate, int buffer);

    // Tracks, for the window's list.
    int selectedTrack = 0;

    // The user's keys live here.
    juce::File keymapFile() const;
    static juce::File appDataFolder();   // ~/Library/Application Support/Ear First on the Mac
    void saveKeymap();
    void saveAppSettings();
    void loadAppSettings();

    static constexpr int keymapDefaultsVersion = 1;

private:
    void timerCallback() override;
    void registerCommands();
    void setDefaultKeys();
    juce::String positionSentence() const;
    bool isRecordingOrCapturing() const;

    tracktion::Engine engine;
    Options options;
    CommandList commandList { *this };
    Keymap keys { commandList };
    MidiControls controls { commandList };
    juce::String learningCommand;    // waiting for a control to press for this command
    double learningSince = 0;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);
    Speech speaker;
    std::unique_ptr<Session> song;
    std::unique_ptr<tracktion::Edit> idleEdit;   // listens to MIDI while no song is open
    void attachIdleEdit();
    std::unique_ptr<EngineCapture> capture;
    std::unique_ptr<Updater> updates;
    Session::OpenReport lastOpenReport;
    double lastAutosave = 0, lastUpdateCheck = 0, lastSpaceCheck = 0;
    juce::String announcedVersion;
    juce::String setAsideNote;
    // Per interface, the inputs that carry the computer's own sound.
    std::map<juce::String, juce::StringArray> computerInputs;
    mutable std::mutex computerInputsLock;
    juce::StringArray computerInputsOn (const juce::String& interfaceName) const;
    juce::String preferredInput, preferredOutput;   // the interface you chose; tried first at every launch
    juce::String startupProblem;   // said at startup: an interface that wouldn't answer
    juce::String savedInterfaceThatWontAnswer();   // said after a device switch kept the old capture
    bool wasPlaying = false;
};

} // namespace earfirst
