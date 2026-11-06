#pragma once
// MidiCapture: every message from every MIDI input, kept all the time, on the
// same sample clock as the audio capture, so a keep gets audio and MIDI
// together, lined up.
//
// Messages arrive on MIDI threads and go into memory under a short lock; a
// writer thread appends them to one file per device four times a second, so
// at most a quarter second is at risk in a crash. MIDI is small: an hour of
// dense playing is a few megabytes, so nothing is ever thrown away.
//
// File per device: a run of records, each an 8-byte little-endian sample
// position, a 2-byte length, then the message bytes. midi.json lists the
// devices and the sample rate.

#include <juce_audio_basics/juce_audio_basics.h>
#include <mutex>
#include <thread>
#include <condition_variable>

namespace earfirst
{

class MidiCapture
{
public:
    struct Event { int64_t sample = 0; juce::MidiMessage message; };

    MidiCapture (juce::File folder, double sampleRate, bool deleteOnClose = true);
    ~MidiCapture();

    // Any thread. The device name keeps each keyboard or controller apart.
    void add (const juce::String& device, int64_t sample, const juce::MidiMessage&);

    juce::StringArray devices() const;
    // Everything from one device in [start, end), in order.
    std::vector<Event> eventsIn (const juce::String& device, int64_t start, int64_t end) const;
    // The last value of each controller (sustain pedal and the like) on each
    // channel before `before`, so a kept take starts with the pedal where it was.
    std::vector<juce::MidiMessage> controllerStateAt (const juce::String& device, int64_t before) const;
    int64_t numEvents() const;

    void flushNow();
    void keepFilesOnClose()             { deleteOnClose = false; }
    const juce::File& getFolder() const { return folder; }

    // Read a folder left behind (after a crash).
    static std::unique_ptr<MidiCapture> openExisting (const juce::File& folder);

private:
    MidiCapture (juce::File folder);   // for openExisting
    void writerLoop();
    void writeIndex() const;
    juce::File fileFor (const juce::String& device) const;
    std::vector<Event> readAll (const juce::String& device) const;

    juce::File folder;
    double sampleRate = 48000;
    bool deleteOnClose = true;

    mutable std::mutex lock;
    juce::StringArray deviceNames;
    std::vector<std::pair<int, Event>> pending;     // device index, event: not yet on disk
    std::map<int, std::vector<Event>> written;      // also kept in memory, for fast keeps
    std::atomic<int64_t> count { 0 };

    std::mutex wakeLock, flushLock;
    std::condition_variable wake;
    std::atomic<bool> running { true };
    std::thread writer;
};

} // namespace earfirst
