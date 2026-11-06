#pragma once
// AppCore: everything the app is, without the window. The engine, the song,
// the always-on capture, the command list, the keymap and the speaking layer,
// wired together. The window, menus, palette and self-test all drive this.

#include <JuceHeader.h>
#include "commands/Commands.h"
#include "commands/Keymap.h"
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
    };

    AppCore (SpeechOutput&, Options);
    ~AppCore() override;

    // CommandHost
    void speak (const juce::String&, bool urgent) override;
    void beginUndoStep (const juce::String&) override;
    void endUndoStep() override;

    CommandList& commands()             { return commandList; }
    Keymap& keymap()                    { return keys; }
    Speech& speech()                    { return speaker; }
    tracktion::Engine& getEngine()      { return engine; }
    Session* session()                  { return song.get(); }
    Updater& updater()                  { return *updates; }
    // Said once the window is up: anything found from a run that didn't close cleanly.
    juce::String startupSentence() const;

    // Songs.
    juce::String newSong (const juce::String& name);
    juce::String openSong (const juce::File& folder);
    juce::String closeSong();
    std::function<void()> onSongChanged;        // the window refreshes its lists
    std::function<void()> onOpenSongRequested;  // the window shows a folder chooser

    // Tracks, for the window's list.
    int selectedTrack = 0;

    // The user's keys live here.
    juce::File keymapFile() const;
    static juce::File appDataFolder();   // ~/Library/Application Support/Ear First on the Mac
    void saveKeymap();

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
    Speech speaker;
    std::unique_ptr<Session> song;
    std::unique_ptr<EngineCapture> capture;
    std::unique_ptr<Updater> updates;
    Session::OpenReport lastOpenReport;
    double lastAutosave = 0;
    bool wasPlaying = false;
};

} // namespace earfirst
