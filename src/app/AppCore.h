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
    // Opens the audio interface after the window is up; the check for a stuck
    // interface runs on a thread. `whenOpen` runs on the message thread after.
    void startAudio (std::function<void()> whenOpen = {});
    bool audioIsOpen() const                      { return capture != nullptr; }

    // Songs.
    juce::String newSong (const juce::String& name);
    juce::String openSong (const juce::File& folder);
    juce::String closeSong();
    std::function<void()> onSongChanged;        // the window refreshes its lists
    std::function<void()> onOpenSongRequested;  // the window shows a folder chooser
    std::function<void()> onImportClickSoundsRequested;   // the window shows a file chooser, then calls importClickSounds
    // Copies audio files in as click sounds and says what happened.
    void importClickSounds (const juce::Array<juce::File>&);
    std::function<void()> onImportAudioRequested;   // the window shows a file chooser, then calls importAudio
    juce::String importAudio (const juce::Array<juce::File>&);   // says what happened
    juce::File lastRender;                      // for tests and Show renders
    bool isRendering() const                    { return renderJob != nullptr; }
    // Levels, 20 times a second: now (decaying) and held for a few seconds, per channel.
    struct Meters { std::vector<float> input, output, inputHeld, outputHeld; std::vector<double> inputHeldAt, outputHeldAt; bool inputClipped = false, outputClipped = false; } meters;
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
    // How the window looks: a Theme name ("Follow the system", "Light", "Dark", "High contrast").
    juce::String theme = "Follow the system";
    bool beatFlash = false;                     // the whole window's edge lights on each beat
    bool welcomeSeen = false;                   // the welcome window has been shown once
    std::function<void()> onViewChanged;        // theme, flash or text size changed: the window redraws
    std::function<void()> onShowWelcome, onShowMessages;
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

    static constexpr int keymapDefaultsVersion = 2;

private:
    void timerCallback() override;
    void registerCommands();
    void registerSongCommands();           // moving around, markers, track basics (SongCommands.cpp)
    void registerClipAndFileCommands();    // import, render, levels, clips (SongCommands.cpp)
    tracktion::AudioTrack* selectedAudioTrack() const;
    std::shared_ptr<tracktion::EditRenderer::Handle> renderJob;
    bool clipWarnings = true;
    std::map<juce::String, double> lastClipWarning;
    void updateMeters (double now);
    juce::String trackSentence (tracktion::AudioTrack&) const;
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
    std::map<juce::String, juce::StringArray> notComputerInputs;   // ones you unmarked: never marked again by itself
    // Marks them; returns the ones newly marked. Says so only when `speak`.
    juce::StringArray markComputerInputs (const juce::String& interfaceName, const juce::StringArray& inputs, bool evenIfUnmarked, bool speak);
public:
    // Plays the sweep and marks what comes back. firstTime: only if this
    // interface has never been checked, and says it's the first time.
    void findComputerInputs (bool firstTime);
private:
    juce::StringArray checkedInterfaces;
    mutable std::mutex computerInputsLock;
    juce::StringArray computerInputsOn (const juce::String& interfaceName) const;

    // Routing, per interface, by channel name. Guarded by computerInputsLock.
    struct RouteSetting { juce::String input, output; double db = 0; };
    std::map<juce::String, std::vector<RouteSetting>> routeSettings;
    std::vector<EngineCapture::NamedRoute> routesOn (const juce::String& interfaceName) const;
    juce::String routesSentence() const;
    static juce::String describeRoute (const RouteSetting&);
    static juce::StringArray channelChoices (const juce::StringArray& names);
    static juce::StringArray channelsIn (const juce::String& choice, const juce::StringArray& names);
    juce::String preferredInput, preferredOutput;   // the interface you chose; tried first at every launch
    juce::String startupProblem;   // said at startup: an interface that wouldn't answer
    struct InterfaceCheck { std::unique_ptr<juce::XmlElement> setup; juce::String stuck; };
    static InterfaceCheck checkSavedInterface (std::unique_ptr<juce::XmlElement> saved, juce::String prefIn, juce::String prefOut);
    void openAudioNow();
    bool audioStarting = false;
    juce::File captureFolderRoot;
    bool wasPlaying = false;
};

} // namespace earfirst
