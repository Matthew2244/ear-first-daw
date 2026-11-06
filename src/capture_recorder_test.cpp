// capture-recorder-test: the disk-backed, whole-session capture recorder.
//
// Feeds a known signal (different on every input, different at every sample)
// and checks every kept sample comes back bit for bit, at the right time,
// on the right input. No microphone needed.
//
// Output is one plain sentence per line, written for a screen reader.

#include <JuceHeader.h>
#include "capture/CaptureRecorder.h"
#include <sys/resource.h>
#include <iostream>

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

double peakMemoryMB()
{
    rusage ru {};
    getrusage (RUSAGE_SELF, &ru);
    return (double) ru.ru_maxrss / (1024.0 * 1024.0);
}

constexpr double sr = 48000.0;

// A different, repeatable value for every input at every sample.
float signal (int ch, int64_t pos)
{
    auto h = (uint32_t) (pos * 2654435761u) ^ (uint32_t) ((ch + 1) * 40503u);
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    return ((float) (h % 16777215u) - 8388607.0f) / 8388607.0f * 0.5f;
}

juce::StringArray names (int n)
{
    juce::StringArray a;
    for (int i = 0; i < n; ++i) a.add ("Input " + juce::String (i + 1));
    return a;
}

// Feeds `seconds` of signal in blocks. speed > 0 paces it at that many times
// real time; 0 means as fast as possible. Returns, per block, whether it was kept.
struct FeedResult { std::vector<bool> accepted; double maxPushMicros = 0; double wallSeconds = 0; };

FeedResult feed (CaptureRecorder& rec, int numInputs, double seconds, int block, double speed)
{
    FeedResult fr;
    juce::AudioBuffer<float> buf (numInputs, block);
    auto total = (int64_t) std::llround (seconds * sr);
    auto t0 = juce::Time::getMillisecondCounterHiRes();

    for (int64_t pos = rec.getPushedSamples(), end = pos + total; pos < end; pos += block)
    {
        auto n = (int) std::min<int64_t> (block, end - pos);
        for (int ch = 0; ch < numInputs; ++ch)
        {
            auto* d = buf.getWritePointer (ch);
            for (int i = 0; i < n; ++i) d[i] = signal (ch, pos + i);
        }

        auto p0 = juce::Time::getMillisecondCounterHiRes();
        fr.accepted.push_back (rec.push (buf.getArrayOfReadPointers(), n));
        fr.maxPushMicros = std::max (fr.maxPushMicros, (juce::Time::getMillisecondCounterHiRes() - p0) * 1000.0);

        if (speed > 0)
        {
            auto due = (double) (pos + n) / sr / speed * 1000.0;
            while (juce::Time::getMillisecondCounterHiRes() - t0 < due)
                std::this_thread::sleep_for (std::chrono::microseconds (200));
        }
    }
    fr.wallSeconds = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
    return fr;
}

// Every sample of a kept file against what was played. `blockAccepted`
// (optional) says which blocks were dropped and should be silence.
struct Compare { bool lengthOk = false, stampOk = false; int64_t mismatches = 0; };

Compare compareTake (const juce::File& f, int ch, int64_t start, int64_t end,
                     const std::vector<bool>* blockAccepted = nullptr, int block = 0)
{
    Compare c;
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> r (wav.createReaderFor (new juce::FileInputStream (f), true));
    if (r == nullptr) { c.mismatches = -1; return c; }

    c.lengthOk = r->lengthInSamples == end - start && r->bitsPerSample == 24;
    c.stampOk = r->metadataValues[juce::WavAudioFormat::bwavTimeReference] == juce::String (start);

    std::vector<int> data (65536);
    for (int64_t pos = 0; pos < r->lengthInSamples; pos += (int64_t) data.size())
    {
        auto n = (int) std::min<int64_t> ((int64_t) data.size(), r->lengthInSamples - pos);
        int* chans[] = { data.data() };
        r->read (chans, 1, pos, n, false);
        for (int i = 0; i < n; ++i)
        {
            auto abs = start + pos + i;
            bool dropped = blockAccepted != nullptr && ! (*blockAccepted)[(size_t) (abs / block)];
            auto expected = dropped ? 0 : CaptureRecorder::toInt24 (signal (ch, abs));
            if (data[(size_t) i] != expected * 256) ++c.mismatches;
        }
    }
    return c;
}

juce::String mb (double bytes) { return juce::String (juce::roundToInt (bytes / (1024.0 * 1024.0))) + " MB"; }

//==============================================================================
void test1 (const juce::File& root)
{
    say ("");
    say ("Test 1: one input, 30 seconds, kept across two segment joins.");
    CaptureRecorder::Options o;
    o.folder = root.getChildFile ("t1");
    o.sampleRate = sr; o.inputNames = names (1); o.segmentSeconds = 10;

    CaptureRecorder rec (o);
    feed (rec, 1, 30, 256, 20);

    auto start = (int64_t) (8.3 * sr) + 17, end = (int64_t) (27.9 * sr) + 5;
    auto r = rec.keep (start, end, { 0 }, root.getChildFile ("kept1"), "Take 1");
    check (r.ok() && r.files.size() == 1, "kept one file. " + r.error);
    if (! r.ok() || r.files.isEmpty()) return;

    auto c = compareTake (r.files[0], 0, start, end);
    check (c.lengthOk, "the take is exactly the length asked for, at 24 bit.");
    check (c.stampOk, "the take is stamped with its position, so it lines up in any DAW.");
    check (c.mismatches == 0, "every sample matches what was played, bit for bit ("
                              + juce::String (c.mismatches) + " differ).");
}

void test2 (const juce::File& root)
{
    say ("");
    say ("Test 2: 44 inputs, 3 minutes, at ten times real speed, then keep the last minute of all 44.");
    auto mem0 = peakMemoryMB();

    CaptureRecorder::Options o;
    o.folder = root.getChildFile ("t2");
    o.sampleRate = sr; o.inputNames = names (44); o.segmentSeconds = 60;

    CaptureRecorder rec (o);
    say ("Using " + juce::String (rec.getWriterThreadCount()) + " writer threads and a "
         + mb ((double) rec.getBufferBytes()) + " buffer.");

    auto fr = feed (rec, 44, 180, 256, 10);
    check (rec.getDroppedSamples() == 0, "nothing dropped (" + juce::String (rec.getDroppedSamples()) + " samples).");
    say ("The slowest hand-off from the audio thread took " + juce::String (fr.maxPushMicros, 0)
         + " microseconds. A 256-sample block at 48 kHz allows 5333.");

    auto t0 = juce::Time::getMillisecondCounterHiRes();
    auto r = rec.keepLast (60, {}, root.getChildFile ("kept2"), "Take 1");
    auto keepSecs = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
    check (r.ok() && r.files.size() == 44, "kept 44 files in " + juce::String (keepSecs, 1) + " seconds. " + r.error);

    int64_t bad = 0; bool lengths = r.files.size() == 44, stamps = true;
    for (int ch = 0; ch < r.files.size(); ++ch)
    {
        auto c = compareTake (r.files[ch], ch, r.start, r.end);
        bad += c.mismatches; lengths &= c.lengthOk; stamps &= c.stampOk;
    }
    check (lengths && stamps, "all 44 takes are the right length and stamped at the same position.");
    check (bad == 0, "all 44 inputs match bit for bit, so no input slipped against another ("
                     + juce::String (bad) + " samples differ).");

    say ("Memory grew by " + juce::String (juce::roundToInt (peakMemoryMB() - mem0)) + " MB, and stays there however long it records.");
    say ("Status would say: " + rec.status());
}

void test3 (const juce::File& root)
{
    say ("");
    say ("Test 3: the disk can't keep up. 8 inputs, a tiny buffer, fed at a hundred times real speed.");
    CaptureRecorder::Options o;
    o.folder = root.getChildFile ("t3");
    o.sampleRate = sr; o.inputNames = names (8); o.segmentSeconds = 5;
    o.bufferSeconds = 0.03; o.maxBlockSize = 256; o.writerThreads = 1;

    CaptureRecorder rec (o);
    auto fr = feed (rec, 8, 20, 256, 100);
    // End on a dropped block on purpose: the case that used to hang a keep.
    {
        juce::AudioBuffer<float> next (8, 256);
        for (;;)
        {
            auto pos = rec.getPushedSamples();
            for (int ch = 0; ch < 8; ++ch)
                for (int i = 0; i < 256; ++i) next.setSample (ch, i, signal (ch, pos + i));
            bool kept = rec.push (next.getArrayOfReadPointers(), 256);
            fr.accepted.push_back (kept);
            if (! kept) break;
        }
    }
    auto dropped = rec.getDroppedSamples();
    check (dropped > 0, "some audio was dropped, as this test intends ("
                        + juce::String ((double) dropped / sr, 1) + " seconds in " + juce::String (rec.getGapCount()) + " gaps).");

    auto r = rec.keep (0, rec.getPushedSamples(), {}, root.getChildFile ("kept3"), "Take 1");
    check (r.ok() && r.files.size() == 8, "kept all 8 inputs. " + r.error);

    int64_t bad = 0; bool lengths = r.files.size() == 8;
    for (int ch = 0; ch < r.files.size(); ++ch)
    {
        auto c = compareTake (r.files[ch], ch, r.start, r.end, &fr.accepted, 256);
        bad += c.mismatches; lengths &= c.lengthOk;
    }
    check (lengths, "every take is the full 20 seconds, so the gaps took up their real time.");
    check (bad == 0, "dropped blocks are silence and everything else is exact, so nothing slipped out of line ("
                     + juce::String (bad) + " samples differ).");
    say ("Status would say: " + rec.status());
}

void test4 (const juce::File& root)
{
    say ("");
    say ("Test 4: the disk runs low partway through. Recording has to carry on.");
    CaptureRecorder::Options o;
    o.folder = root.getChildFile ("t4");
    o.sampleRate = sr; o.inputNames = names (4); o.segmentSeconds = 5;
    o.diskReserveBytes = 1000;

    std::atomic<int64_t> fakeFree { 1LL << 40 };
    o.freeBytes = [&] { return fakeFree.load(); };

    CaptureRecorder rec (o);
    feed (rec, 4, 22, 256, 20);
    fakeFree = 10;                                       // the disk is suddenly almost full
    std::this_thread::sleep_for (std::chrono::milliseconds (600));
    fakeFree = 1LL << 40;                                // and then there's room again
    feed (rec, 4, 10, 256, 20);

    check (rec.getEvictedSegments() > 0, "it cleared " + juce::String (rec.getEvictedSegments())
                                          + " old segments of unkept audio to make room.");
    check (rec.getDroppedSamples() == 0, "nothing new was dropped while it did.");

    auto r = rec.keepLast (8, {}, root.getChildFile ("kept4"), "Take 1");
    int64_t bad = 0;
    for (int ch = 0; ch < r.files.size(); ++ch) bad += compareTake (r.files[ch], ch, r.start, r.end).mismatches;
    check (r.ok() && r.files.size() == 4 && bad == 0, "the last 8 seconds are all there and exact. " + r.error);

    auto early = rec.keep (0, (int64_t) (3 * sr), { 0 }, root.getChildFile ("kept4"), "Early");
    check (! early.ok() || early.note.isNotEmpty() || early.files.isEmpty(),
           "asking for cleared audio says so rather than pretending: "
           + (early.error.isNotEmpty() ? early.error : early.note));
    say ("Status would say: " + rec.status());
}

void test5 (const juce::File& root)
{
    say ("");
    say ("Test 5: the app goes away without saving. Is the audio still there?");
    auto folder = root.getChildFile ("t5");
    {
        CaptureRecorder::Options o;
        o.folder = folder;
        o.sampleRate = sr; o.inputNames = names (2); o.segmentSeconds = 4;
        o.deleteOnClose = false;
        CaptureRecorder rec (o);
        feed (rec, 2, 12, 256, 20);
    }   // nothing kept, nothing saved

    auto info = CaptureInfo::load (folder);
    check (info.has_value() && info->numInputs() == 2, "a fresh start finds the capture and both inputs.");
    if (! info) return;

    CaptureStore store (folder, *info);
    auto end = store.endSampleOnDisk();
    check (end == (int64_t) (12 * sr), "all 12 seconds are on disk.");

    auto r = store.exportTake (end - (int64_t) (5 * sr), end, { 0, 1 }, root.getChildFile ("kept5"), "Recovered");
    int64_t bad = 0;
    for (int ch = 0; ch < r.files.size(); ++ch) bad += compareTake (r.files[ch], ch, r.start, r.end).mismatches;
    check (r.ok() && r.files.size() == 2 && bad == 0, "the last 5 seconds can still be kept, exact. " + r.error);
}
} // namespace

// Big channel counts at true real time: does anything drop, and is every input exact?
void scaleTest (const juce::File& root, int numInputs, double seconds)
{
    say ("");
    say ("Scale: " + juce::String (numInputs) + " inputs, " + juce::String (juce::roundToInt (seconds))
         + " seconds at real speed, then keep the last 20 seconds of all of them.");
    auto mem0 = peakMemoryMB();
    CaptureRecorder::Options o;
    o.folder = root.getChildFile ("scale" + juce::String (numInputs));
    o.sampleRate = sr; o.inputNames = names (numInputs); o.segmentSeconds = 60;
    CaptureRecorder rec (o);
    say ("Using " + juce::String (rec.getWriterThreadCount()) + " writer threads and a "
         + mb ((double) rec.getBufferBytes()) + " buffer. Writing "
         + mb (rec.getBytesPerSecond()) + " a second.");

    // Feed from a real-time thread, as a real audio interface would.
    FeedResult fr;
    {
        struct Feeder : juce::Thread
        {
            Feeder (CaptureRecorder& r, int n, double s) : juce::Thread ("Test audio thread"), rec (r), num (n), secs (s) {}
            void run() override { result = feed (rec, num, secs, 256, 1); }
            CaptureRecorder& rec; int num; double secs; FeedResult result;
        } feeder (rec, numInputs, seconds);
        feeder.startRealtimeThread (juce::Thread::RealtimeOptions{}.withPeriodMs (256.0 / 48.0));
        while (feeder.isThreadRunning()) juce::Thread::sleep (50);
        fr = feeder.result;
    }
    check (rec.getDroppedSamples() == 0, juce::String (numInputs) + " inputs: nothing dropped ("
           + juce::String (rec.getDroppedSamples()) + " samples).");
    check (fr.maxPushMicros < 1000.0, "the slowest hand-off from the audio thread took " + juce::String (fr.maxPushMicros, 0)
           + " microseconds, well inside the 5333 a 256-sample block allows.");

    auto t0 = juce::Time::getMillisecondCounterHiRes();
    auto r = rec.keepLast (20, {}, root.getChildFile ("keptscale" + juce::String (numInputs)), "Take");
    auto keepSecs = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
    check (r.ok() && r.files.size() == numInputs, "kept " + juce::String (r.files.size()) + " files in "
           + juce::String (keepSecs, 1) + " seconds. " + r.error);
    int64_t bad = 0; bool lengths = r.files.size() == numInputs, stamps = true;
    for (int ch = 0; ch < r.files.size(); ++ch)
    {
        auto c = compareTake (r.files[ch], ch, r.start, r.end);
        bad += c.mismatches; lengths &= c.lengthOk; stamps &= c.stampOk;
    }
    check (lengths && stamps && bad == 0, "all " + juce::String (numInputs) + " match bit for bit, right length, same stamp ("
           + juce::String (bad) + " samples differ).");
    say ("Memory grew by " + juce::String (juce::roundToInt (peakMemoryMB() - mem0)) + " MB.");
    rec.stop();
    o.folder.deleteRecursively();
    root.getChildFile ("keptscale" + juce::String (numInputs)).deleteRecursively();
}

int main (int argc, char** argv)
{
    if (argc > 1 && juce::String (argv[1]) == "scale")
    {
        auto root = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("EarFirstScaleTest");
        root.deleteRecursively(); root.createDirectory();
        say ("Channel count test, at true real time. No microphone is used.");
        double secs = argc > 2 ? juce::String (argv[2]).getDoubleValue() : 60.0;
        for (int n : { 128, 256, 512, 1024 })
            scaleTest (root, n, secs);
        root.deleteRecursively();
        say ("");
        say (failures == 0 ? "All checks passed." : juce::String (failures) + " checks failed.");
        return failures == 0 ? 0 : 1;
    }

    auto root = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("EarFirstCaptureRecorderTest");
    root.deleteRecursively();
    root.createDirectory();

    say ("Capture recorder self-test. No microphone is used.");
    test1 (root);
    test2 (root);
    test3 (root);
    test4 (root);
    test5 (root);

    root.deleteRecursively();
    say ("");
    say (failures == 0 ? "All checks passed." : juce::String (failures) + " checks failed.");
    return failures == 0 ? 0 : 1;
}
