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
#include "capture/MidiCapture.h"
#include "capture/ReturnFinder.h"
#include "capture/RouteMatrix.h"
#include <map>
#include <array>

namespace earfirst
{

class EngineCapture : private juce::AudioIODeviceCallback, private juce::ChangeListener, private juce::Timer
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
        juce::Array<tracktion::MidiClip*> midiClips;     // one per MIDI device that played
        tracktion::TimePosition timelineStart;
        bool linedUpWithSong = false;   // false: the song wasn't playing, so it went at the playhead
        double secondsBeforeSongStart = 0;   // played before bar 1: kept in the file, hidden at the clip's start

        // When the song was looping, every pass around the loop. All of them
        // are in the take's files; `chosenPass` is the one on the tracks.
        struct Pass { tracktion::TimePosition start; tracktion::TimeDuration length, offsetInFile; };
        std::vector<Pass> passes;
        int chosenPass = -1;
        juce::String sentence;
        juce::String brief;      // the short version: "Kept 30 seconds, 8 inputs."
    };

    // Message thread. Keeps the last `seconds` of every input and puts each on
    // its own track (made if needed, named after the input).
    Kept keepLast (double seconds, const juce::File& takesFolder);

    // The same, with the export (the slow part) on a thread of its own, so a
    // long keep never freezes the window. `done` runs on the message thread
    // once the clips are on the timeline.
    void keepLastInBackground (double seconds, const juce::File& takesFolder, std::function<void (const Kept&)> done);
    // Roughly how long exporting would take, in seconds of audio times inputs.
    double keepSize (double seconds) const   { return seconds * juce::jmax (1, numInputs); }

    CaptureRecorder* getRecorder()          { return recorder.get(); }
    MidiCapture* getMidi()                  { return midi.get(); }
    bool keepOnQuit = true;                 // leave unkept capture on disk at a clean quit, offered next time
    // Since the last keep: did any input go from quiet to loud (someone
    // started playing), and was a note played? Quitting only keeps capture
    // that had something in it. An input that's loud all along (a return of
    // the computer's own audio, a hum) never counts.
    std::vector<char> inputWasQuiet;            // per input, audio thread only; sized before audio starts
    std::array<std::atomic<bool>, 512> computerInput {};   // per input: the computer's own sound
    std::array<std::atomic<bool>, 512> playingStarted {};   // per input, since the last keep
    bool playingStartedSinceKeep() const;   // on any input not marked as the computer's own sound
    std::atomic<bool> notePlayedSinceKeep { false };
    int numMidiListeners() const            { return (int) midiTaps.size(); }   // for tests

    // Inputs that carry the computer's own sound (a return of what it plays,
    // like channels 63 and 64 on a StudioLive). They're still captured, but
    // sound on them never counts as you playing. The app answers this for
    // each interface by name, whenever one starts. Any thread.
    std::function<juce::StringArray (const juce::String& interfaceName)> computerInputsFor;
    void refreshComputerInputs();             // message thread: after you change the list

    // Finding them by itself (see ReturnFinder): while Ear First plays something
    // with sound across the range (the song, the click), each input is compared
    // with what went out. Inputs that match twice are reported here, on the
    // message thread, with the interface's name.
    std::function<void (const juce::String& interfaceName, const juce::StringArray& inputs)> onComputerInputsFound;
    // On purpose: plays a soft sweep, then reports which inputs carried it back.
    // `problem` says why it couldn't tell, if it couldn't. Message thread.
    void findComputerInputsNow (std::function<void (const juce::StringArray& found, const juce::String& problem)> done);
    void playSweep()                          { sweepRequested = true; }   // just the sound (tests)
    juce::StringArray getInputNames() const   { std::lock_guard<std::mutex> g (namesLock); return inputNames; }
    juce::String getInterfaceName() const     { std::lock_guard<std::mutex> g (namesLock); return interfaceName; }
    juce::StringArray getOutputNames() const  { std::lock_guard<std::mutex> g (namesLock); return outputNames; }

    // Any input to any output, by channel name, live whether or not a song is
    // open. It changes what you hear and nothing that's captured. The app
    // answers this for each interface. A name that isn't on the interface is
    // ignored, and so is an input carrying the computer's own sound: sending
    // that to an output would feed back.
    struct NamedRoute { juce::String input, output; float gain = 1.0f; };
    std::function<std::vector<NamedRoute> (const juce::String& interfaceName)> routesFor;
    void refreshRoutes();                     // after you change the routes

    // A short tone mixed into the outputs (never the inputs, so never into a
    // recording): two notes, each about 70 ms. Message thread.
    void playEarcon (float firstHz, float secondHz);

    // The loudest output sample each block, so speech can wait for a gap in
    // the music. Called on the audio thread; must not block or allocate.
    std::function<void (float)> onOutputLevel;

    // Every MIDI message, as it arrives (on a MIDI thread), for controls.
    std::function<void (const juce::String& device, const juce::MidiMessage&)> onMidi;

    // How to say a place on the timeline; the app sets it from your speech choices.
    std::function<juce::String (const tracktion::Edit&, tracktion::TimePosition)> describePlace;

    // The interface changed: what was captured on the old one is kept on
    // disk, never thrown away, and handed over here (folder, seconds).
    std::function<void (const juce::File&, double)> onCaptureSetAside;
    TimelineMap& getTimelineMap()           { return map; }

    // The loudest sample on each channel since the last call. Message thread.
    void takeLevels (std::vector<float>& inputs, std::vector<float>& outputs);

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
    std::shared_ptr<CaptureRecorder> recorder;   // shared: a keep exporting in the background holds it
    TimelineMap map;
    double sampleRate = 0;
    int numInputs = 0;
    juce::StringArray inputNames, outputNames;
    juce::String interfaceName;
    RouteMatrix routes;
    std::atomic<int> routeCapacity { 8192 };    // the most samples a block can route
    mutable std::mutex namesLock;               // inputNames and interfaceName, read from the message thread

    std::atomic<tracktion::Edit*> edit { nullptr };   // read on the audio thread too
    std::atomic<tracktion::EditPlaybackContext*> context { nullptr };
    std::atomic<int64_t> blockStart { 0 };   // capture sample where the current block begins
    int takeNumber = 0;
    // MIDI: a listener on each MIDI input of the attached song.
    struct MidiTap : tracktion::InputDeviceInstance::Consumer
    {
        MidiTap (EngineCapture& o, juce::String d) : owner (o), device (std::move (d)) {}
        void handleIncomingMidiMessage (const juce::MidiMessage& m, tracktion::MPESourceID) override { owner.midiArrived (device, m); }
        EngineCapture& owner;
        juce::String device;
    };
    // The engine can throw its playback context away and make a new one
    // (a device plugged in, a render). It says so only afterwards, so the
    // stored pointer is switched at once, and nothing that unhooks listeners
    // trusts the stored one: it asks the transport for the current context.
    struct TransportWatch : tracktion::TransportControl::Listener
    {
        explicit TransportWatch (EngineCapture& o) : owner (o) {}
        void playbackContextChanged() override  { owner.contextChanged(); }
        EngineCapture& owner;
    };
    TransportWatch transportWatch { *this };
    void contextChanged();
    tracktion::EditPlaybackContext* currentContext() const;

    void midiArrived (const juce::String& device, const juce::MidiMessage&);
    void attachMidiTaps();
    void changeListenerCallback (juce::ChangeBroadcaster*) override   { startTimer (700); }   // devices changed
    void timerCallback() override                                      { stopTimer(); attachMidiTaps(); }
    void placeMidi (Kept&, tracktion::TimePosition at, tracktion::TimeDuration length, int64_t captureStart, const juce::String& takeName);
    Kept finishKeep (Kept, const juce::String& name);   // placement and the sentence, message thread
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);
    std::vector<std::pair<std::thread, std::shared_ptr<std::atomic<bool>>>> keepThreads;   // and whether each has finished
    void detachMidiTaps();
    std::vector<std::pair<tracktion::InputDeviceInstance*, std::unique_ptr<MidiTap>>> midiTaps;
    std::unique_ptr<MidiCapture> midi;

    // The engine's clock at the start of the current block, paired with the
    // capture sample there, so MIDI timestamps map onto the capture clock.
    std::atomic<uint32_t> clockVersion { 0 };
    std::atomic<int64_t> clockSample { 0 };
    std::atomic<double> clockStreamTime { 0 };

    struct EarconRequest { float f1 = 0, f2 = 0; };
    juce::AbstractFifo earconFifo { 16 };
    std::array<EarconRequest, 16> earconQueue;
    struct EarconVoice { float f1 = 0, f2 = 0; double phase = 0; int age = 0; bool active = false; };
    EarconVoice earcon;
    void renderEarcon (float* const* out, int numOut, int n) noexcept;

    // What went out, summed to one channel, on the capture clock (audio thread writes).
    static constexpr int outRingSize = 1 << 19;
    std::vector<float> outRing = std::vector<float> ((size_t) outRingSize, 0.0f);
    std::atomic<int64_t> outWritten { 0 };
    // The sweep: made when the device starts, played when asked.
    std::vector<float> sweep;
    std::atomic<bool> sweepRequested { false };
    std::atomic<int64_t> sweepStartedAt { -1 };
    int64_t sweepPos = -1;                       // audio thread
    void renderSweep (float* const* out, int numOut, int n, int64_t blockPos) noexcept;

    // Message thread: a check every second, on a thread of its own.
    struct ReturnTimer : juce::Timer
    {
        explicit ReturnTimer (EngineCapture& o) : owner (o) {}
        void timerCallback() override   { owner.checkForReturns(); }
        EngineCapture& owner;
    };
    ReturnTimer returnTimer { *this };
    void checkForReturns();
    juce::ThreadPool returnPool { 1 };
    std::atomic<bool> returnCheckBusy { false };
    int64_t returnCheckedUpTo = 0;
    std::map<juce::String, int> returnHits, notReturnCount;   // per input, this run
    std::function<void (const juce::StringArray&, const juce::String&)> sweepDone;
    juce::uint32 sweepAskedAt = 0;
    std::atomic<bool> returnStateStale { false };   // set when the interface changes; the timer starts afresh
    std::atomic<int> runNumber { 0 }, sweepRun { -1 };   // which capture run is going, and which one the sweep played in
    std::atomic<int> roundTripSamples { 8192 };     // output to input through the interface, with room to spare

    std::array<std::atomic<float>, 1024> inputLevel {}, outputLevel {};   // loudest since last read; audio thread raises them
    std::atomic<int> levelInputs { 0 }, levelOutputs { 0 };
    int maxBlock = 4096;
    int deviceBlock = 512;
    std::vector<const float*> chunk;   // sized before audio starts
};

} // namespace earfirst
