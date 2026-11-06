#pragma once
// EngineCapture: the always-on disk recorder, inside the Tracktion engine.
//
// It listens to the same audio device the engine uses, so it hears every input
// whether or not a track is armed and whether or not the song is playing. Keep
// turns the last stretch of time into clips on the song's timeline, one per
// input, at the moment they were played.
//
//   EngineCapture cap (engine, captureFolder);   // starts with the audio device
//   cap.attachEdit (edit.get());                 // which song to line up with
//   auto k = cap.keepLast (30, takesFolder);     // clips land on tracks; k.sentence says what happened
//   cap.attachEdit (nullptr);                    // before that edit's playback context goes away

#include <JuceHeader.h>
#include "capture/CaptureRecorder.h"
#include "capture/TimelineMap.h"

namespace earfirst
{

class EngineCapture : private juce::AudioIODeviceCallback
{
public:
    EngineCapture (tracktion::Engine&, juce::File captureFolder);
    ~EngineCapture() override;

    // Message thread. Pass nullptr before the edit or its playback context goes away.
    void attachEdit (tracktion::Edit*);

    struct Kept
    {
        TakeResult take;
        juce::Array<tracktion::WaveAudioClip*> clips;
        tracktion::TimePosition timelineStart;
        bool linedUpWithSong = false;   // false: the song wasn't playing, so it went at the playhead
        double secondsBeforeSongStart = 0;   // played before bar 1: kept in the file, hidden at the clip's start

        // When the song was looping, every pass around the loop. All of them
        // are in the take's files; `chosenPass` is the one on the tracks.
        struct Pass { tracktion::TimePosition start; tracktion::TimeDuration length, offsetInFile; };
        std::vector<Pass> passes;
        int chosenPass = -1;
        juce::String sentence;
    };

    // Message thread. Keeps the last `seconds` of every input and puts each on
    // its own track (made if needed, named after the input).
    Kept keepLast (double seconds, const juce::File& takesFolder);

    CaptureRecorder* getRecorder()          { return recorder.get(); }

    // The interface changed: what was captured on the old one is kept on
    // disk, never thrown away, and handed over here (folder, seconds).
    std::function<void (const juce::File&, double)> onCaptureSetAside;
    TimelineMap& getTimelineMap()           { return map; }

private:
    void audioDeviceIOCallbackWithContext (const float* const*, int, float* const*, int, int,
                                           const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override;
    bool capture (const float* const*, int numIn, int n) noexcept;
    void noteTimeline (int n) noexcept;
    juce::AudioIODeviceCallback* engineCallback = nullptr;   // the engine's own; we call it each block

    tracktion::Engine& engine;
    juce::File captureFolder;
    std::unique_ptr<CaptureRecorder> recorder;
    TimelineMap map;
    double sampleRate = 0;
    int numInputs = 0;
    juce::StringArray inputNames;

    tracktion::Edit* edit = nullptr;
    std::atomic<tracktion::EditPlaybackContext*> context { nullptr };
    std::atomic<int64_t> blockStart { 0 };   // capture sample where the current block begins
    int takeNumber = 0;
    int maxBlock = 4096;
    int deviceBlock = 512;
    std::vector<const float*> chunk;   // sized before audio starts
};

} // namespace earfirst
