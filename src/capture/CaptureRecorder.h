// CaptureRecorder: every input, recorded to disk for the whole session.
//
// The audio thread only copies samples into memory and returns. Writer
// threads (one group of inputs each, spread across the cores) move them to
// disk as raw 24-bit files cut into segments. Raw files need no header, so if
// the app crashes everything played up to that moment is still on disk and
// can be kept afterwards.
//
// "Keep" copies any stretch of time, on any inputs, into broadcast WAV files
// stamped with their position, so they line up in any DAW.
//
// If the buffer ever fills (the disk stalls for seconds), audio is dropped
// for that block on every input together and the gap is written as silence,
// so the inputs never slip out of line with each other or with time.
// If the disk runs low, the oldest unkept audio is cleared so recording
// carries on.

#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace earfirst
{

struct CaptureInfo
{
    double sampleRate = 48000.0;
    juce::StringArray inputNames;
    int64_t segmentSamples = 0;
    juce::Time started;

    int numInputs() const   { return inputNames.size(); }
    bool save (const juce::File& folder) const;
    static std::optional<CaptureInfo> load (const juce::File& folder);
};

struct TakeResult
{
    juce::Array<juce::File> files;
    int64_t start = 0, end = 0;
    juce::String error, note;
    bool ok() const     { return error.isEmpty(); }
};

// Reads what is on disk. Used by the recorder, and on its own to recover
// audio after a crash.
class CaptureStore
{
public:
    CaptureStore (juce::File folder, CaptureInfo);

    const CaptureInfo& getInfo() const      { return info; }
    const juce::File& getFolder() const     { return folder; }

    juce::File inputFolder (int input) const;
    juce::File segmentFile (int input, int64_t segmentIndex) const;

    // Fills dest with 32-bit left-justified samples. False if any part is missing.
    bool readRange (int input, int64_t start, int64_t num, int* dest) const;

    int64_t firstSampleOnDisk() const;
    int64_t endSampleOnDisk() const;

    TakeResult exportTake (int64_t start, int64_t end, const juce::Array<int>& inputs,
                           const juce::File& destFolder, const juce::String& takeName) const;

    static constexpr int bytesPerSample = 3;

private:
    juce::File folder;
    CaptureInfo info;
};

class CaptureRecorder
{
public:
    struct Options
    {
        juce::File folder;
        double sampleRate = 48000.0;
        juce::StringArray inputNames;
        double segmentSeconds = 600.0;
        double bufferSeconds = 8.0;
        // Big input counts shorten the buffer so memory stays sane, never below 2 seconds.
        int64_t bufferBytesMax = 512LL * 1024 * 1024;
        int maxBlockSize = 4096;
        int writerThreads = 0;                          // 0 = choose from the core count
        int64_t diskReserveBytes = 2LL * 1024 * 1024 * 1024;
        std::function<int64_t()> freeBytes;             // for tests; default asks the volume
        bool deleteOnClose = true;
    };

    explicit CaptureRecorder (Options);
    ~CaptureRecorder();

    // Audio thread. No locks, no allocation. False if this block was dropped.
    bool push (const float* const* channels, int numSamples) noexcept;

    // Waits for everything pushed so far to reach disk, then stops the writers.
    void stop();
    // Keep the raw capture on disk when this recorder goes away (it becomes
    // recovered audio), instead of deleting it.
    void keepFilesOnClose()             { options.deleteOnClose = false; }

    int64_t getPushedSamples() const    { return pushedPos.load(); }
    int64_t getWrittenSamples() const   { return writtenPos.load(); }
    int64_t getDroppedSamples() const   { return droppedSamples.load(); }
    int getGapCount() const             { return gapCount.load(); }
    int getEvictedSegments() const      { return evictedSegments.load(); }
    int64_t getFirstAvailableSample() const { return firstAvailable.load(); }
    bool isDiskFull() const             { return diskFull.load(); }
    int getWriterThreadCount() const    { return numGroups; }
    size_t getBufferBytes() const       { return (size_t) fifoCapacity * (size_t) numInputs * sizeof (float); }
    double getBytesPerSecond() const    { return info.sampleRate * numInputs * CaptureStore::bytesPerSample; }
    const CaptureStore& getStore() const { return store; }

    TakeResult keep (int64_t start, int64_t end, juce::Array<int> inputs,
                     const juce::File& destFolder, const juce::String& takeName, int timeoutMs = 10000);
    TakeResult keepLast (double seconds, juce::Array<int> inputs,
                         const juce::File& destFolder, const juce::String& takeName);

    double hoursOfRoomLeft() const;
    juce::String status() const;

    static int toInt24 (float x) noexcept
    {
        return juce::roundToInt (juce::jlimit (-1.0f, 1.0f, x) * 8388607.0f);
    }

private:
    struct Block { int64_t start; int length; };
    struct Op { bool gap; int64_t length; };   // gap: silence; else: frames from the buffer
    struct ChannelWriter;

    void drainLoop();
    bool drainOnce();
    void checkDisk();
    int64_t freeBytesNow() const;

    Options options;
    CaptureInfo info;
    CaptureStore store;
    int numInputs = 0;

    // Shared between the audio thread and the drain thread.
    int fifoCapacity = 0;
    juce::AbstractFifo sampleFifo { 1 };
    juce::AbstractFifo blockFifo { 1 };
    std::vector<std::vector<float>> sampleBuffer;
    std::vector<Block> blockBuffer;

    std::atomic<int64_t> pushedPos { 0 }, writtenPos { 0 }, droppedSamples { 0 }, firstAvailable { 0 };
    std::atomic<int> gapCount { 0 }, evictedSegments { 0 };
    std::atomic<bool> running { true }, diskFull { false }, writeError { false };
    int64_t audioThreadPos = 0;

    // Drain thread only.
    std::vector<std::unique_ptr<ChannelWriter>> writers;
    std::vector<std::vector<int>> groups;
    int numGroups = 1;
    std::unique_ptr<juce::ThreadPool> pool;
    std::vector<Op> ops;
    int64_t expectedPos = 0;
    juce::uint32 lastDiskCheck = 0;

    mutable std::mutex storeMutex;      // keeps an export and an eviction apart
    std::thread drainThread;
    bool stopped = false;
};

} // namespace earfirst
