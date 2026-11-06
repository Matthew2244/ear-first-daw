#include "CaptureRecorder.h"
#if JUCE_MAC
 #include <pthread/qos.h>
#endif

namespace earfirst
{

//==============================================================================
bool CaptureInfo::save (const juce::File& folder) const
{
    auto obj = std::make_unique<juce::DynamicObject>();
    obj->setProperty ("sampleRate", sampleRate);
    obj->setProperty ("segmentSamples", (juce::int64) segmentSamples);
    obj->setProperty ("started", started.toISO8601 (true));
    obj->setProperty ("bitsPerSample", 24);
    juce::Array<juce::var> names;
    for (auto& n : inputNames) names.add (n);
    obj->setProperty ("inputs", names);
    return folder.getChildFile ("capture.json")
                 .replaceWithText (juce::JSON::toString (juce::var (obj.release())));
}

std::optional<CaptureInfo> CaptureInfo::load (const juce::File& folder)
{
    auto v = juce::JSON::parse (folder.getChildFile ("capture.json"));
    if (! v.isObject()) return std::nullopt;

    CaptureInfo i;
    i.sampleRate = v["sampleRate"];
    i.segmentSamples = (juce::int64) v["segmentSamples"];
    i.started = juce::Time::fromISO8601 (v["started"].toString());
    if (auto* arr = v["inputs"].getArray())
        for (auto& n : *arr) i.inputNames.add (n.toString());

    if (i.sampleRate <= 0 || i.segmentSamples <= 0 || i.inputNames.isEmpty()) return std::nullopt;
    return i;
}

//==============================================================================
CaptureStore::CaptureStore (juce::File f, CaptureInfo i) : folder (std::move (f)), info (std::move (i)) {}

juce::File CaptureStore::inputFolder (int input) const
{
    return folder.getChildFile ("inputs").getChildFile (juce::String (input + 1).paddedLeft ('0', 3));
}

juce::File CaptureStore::segmentFile (int input, int64_t segmentIndex) const
{
    return inputFolder (input).getChildFile ("seg-" + juce::String (segmentIndex).paddedLeft ('0', 8) + ".pcm");
}

bool CaptureStore::readRange (int input, int64_t start, int64_t num, int* dest) const
{
    std::vector<uint8_t> bytes;
    auto seg = info.segmentSamples;

    while (num > 0)
    {
        auto index = start / seg;
        auto offset = start - index * seg;
        auto n = std::min (num, seg - offset);

        juce::FileInputStream in (segmentFile (input, index));
        if (! in.openedOk() || ! in.setPosition (offset * bytesPerSample)) return false;

        bytes.resize ((size_t) (n * bytesPerSample));
        if (in.read (bytes.data(), (int) bytes.size()) != (int) bytes.size()) return false;

        for (int64_t i = 0; i < n; ++i)
        {
            auto* b = bytes.data() + i * bytesPerSample;
            auto v = (int32_t) ((uint32_t) b[0] << 8 | (uint32_t) b[1] << 16 | (uint32_t) b[2] << 24);
            dest[i] = v;   // left-justified, which is what JUCE's integer writers expect
        }

        dest += n; start += n; num -= n;
    }
    return true;
}

static int64_t segmentIndexOf (const juce::File& f)
{
    return f.getFileNameWithoutExtension().fromFirstOccurrenceOf ("seg-", false, false).getLargeIntValue();
}

int64_t CaptureStore::firstSampleOnDisk() const
{
    auto files = inputFolder (0).findChildFiles (juce::File::findFiles, false, "seg-*.pcm");
    if (files.isEmpty()) return 0;
    int64_t lowest = std::numeric_limits<int64_t>::max();
    for (auto& f : files) lowest = std::min (lowest, segmentIndexOf (f));
    return lowest * info.segmentSamples;
}

int64_t CaptureStore::endSampleOnDisk() const
{
    int64_t end = std::numeric_limits<int64_t>::max();
    for (int input = 0; input < info.numInputs(); ++input)
    {
        auto files = inputFolder (input).findChildFiles (juce::File::findFiles, false, "seg-*.pcm");
        int64_t inputEnd = 0;
        for (auto& f : files)
            inputEnd = std::max (inputEnd, segmentIndexOf (f) * info.segmentSamples + f.getSize() / bytesPerSample);
        end = std::min (end, inputEnd);
    }
    return end == std::numeric_limits<int64_t>::max() ? 0 : end;
}

TakeResult CaptureStore::exportTake (int64_t start, int64_t end, const juce::Array<int>& inputs,
                                     const juce::File& destFolder, const juce::String& takeName) const
{
    TakeResult r;
    r.start = start; r.end = end;
    if (end <= start) { r.error = "There is nothing in that stretch of time."; return r; }
    destFolder.createDirectory();

    juce::WavAudioFormat wav;
    auto bwav = juce::WavAudioFormat::createBWAVMetadata ("Kept from always-on capture", "Ear First",
                                                          "", juce::Time::getCurrentTime(), start, "");
    std::unordered_map<juce::String, juce::String> meta;
    for (auto& key : bwav.getAllKeys()) meta[key] = bwav[key];

    constexpr int chunk = 65536;
    std::vector<int> data (chunk);

    for (auto input : inputs)
    {
        if (! juce::isPositiveAndBelow (input, info.numInputs()))
        {
            r.error = "There is no input " + juce::String (input + 1) + ".";
            return r;
        }

        auto name = juce::File::createLegalFileName (takeName + " " + info.inputNames[input]);
        auto file = destFolder.getNonexistentChildFile (name, ".wav", false);

        std::unique_ptr<juce::OutputStream> out = std::make_unique<juce::FileOutputStream> (file);
        auto writer = wav.createWriterFor (out, juce::AudioFormatWriterOptions{}
                                                    .withSampleRate (info.sampleRate)
                                                    .withNumChannels (1)
                                                    .withBitsPerSample (24)
                                                    .withMetadataValues (meta));
        if (writer == nullptr) { r.error = "Could not create " + file.getFileName() + "."; return r; }

        for (int64_t pos = start; pos < end; pos += chunk)
        {
            auto n = (int) std::min<int64_t> (chunk, end - pos);
            if (! readRange (input, pos, n, data.data()))
            {
                r.error = "Part of " + info.inputNames[input] + " is no longer on disk.";
                writer.reset();
                file.deleteFile();
                return r;
            }
            const int* chans[] = { data.data(), nullptr };
            if (! writer->write (chans, n)) { r.error = "Writing " + file.getFileName() + " failed."; return r; }
        }

        writer.reset();
        r.files.add (file);
    }
    return r;
}

//==============================================================================
struct CaptureRecorder::ChannelWriter
{
    ChannelWriter (const CaptureStore& s, int in) : store (s), input (in)
    {
        store.inputFolder (input).createDirectory();
    }

    bool write (const float* src, int64_t n)
    {
        while (n > 0)
        {
            if (! ensureSegment()) return false;
            auto room = segLen() - (pos - currentIndex * segLen());
            auto todo = (int) std::min<int64_t> ({ n, room, (int64_t) scratchSamples });

            for (int i = 0; i < todo; ++i)
            {
                auto v = CaptureRecorder::toInt24 (src[i]);
                auto* b = scratch + i * 3;
                b[0] = (uint8_t) (v & 0xff);
                b[1] = (uint8_t) ((v >> 8) & 0xff);
                b[2] = (uint8_t) ((v >> 16) & 0xff);
            }

            if (! out->write (scratch, (size_t) todo * 3)) return false;
            src += todo; n -= todo; pos += todo;
        }
        return true;
    }

    bool writeSilence (int64_t n)
    {
        while (n > 0)
        {
            if (! ensureSegment()) return false;
            auto room = segLen() - (pos - currentIndex * segLen());
            auto todo = (int) std::min<int64_t> ({ n, room, (int64_t) scratchSamples });
            std::memset (scratch, 0, (size_t) todo * 3);
            if (! out->write (scratch, (size_t) todo * 3)) return false;
            n -= todo; pos += todo;
        }
        return true;
    }

    bool flush()
    {
        if (out == nullptr) return true;
        out->flush();
        return out->getStatus().wasOk();
    }

    void close()    { out.reset(); }

private:
    int64_t segLen() const  { return store.getInfo().segmentSamples; }

    bool ensureSegment()
    {
        auto index = pos / segLen();
        if (out != nullptr && index == currentIndex) return true;

        out.reset();
        auto file = store.segmentFile (input, index);
        file.deleteFile();
        out = std::make_unique<juce::FileOutputStream> (file, 1 << 16);
        currentIndex = index;
        return out->openedOk();
    }

    static constexpr int scratchSamples = 8192;
    const CaptureStore& store;
    int input;
    int64_t pos = 0, currentIndex = -1;
    std::unique_ptr<juce::FileOutputStream> out;
    uint8_t scratch[scratchSamples * 3];
};

//==============================================================================
static CaptureInfo makeInfo (const CaptureRecorder::Options& o)
{
    CaptureInfo i;
    i.sampleRate = o.sampleRate;
    i.inputNames = o.inputNames;
    i.segmentSamples = std::max<int64_t> (1, (int64_t) std::llround (o.segmentSeconds * o.sampleRate));
    i.started = juce::Time::getCurrentTime();
    return i;
}

CaptureRecorder::CaptureRecorder (Options o)
    : options (std::move (o)), info (makeInfo (options)), store (options.folder, info)
{
    numInputs = info.numInputs();
    jassert (numInputs > 0);

    options.folder.createDirectory();
    info.save (options.folder);

    {
        double secs = options.bufferSeconds;
        const double bytesPerSecond = info.sampleRate * (double) std::max (1, info.numInputs()) * sizeof (float);
        if (options.bufferBytesMax > 0 && secs * bytesPerSecond > (double) options.bufferBytesMax)
            secs = std::max (std::min (2.0, options.bufferSeconds), (double) options.bufferBytesMax / bytesPerSecond);
        fifoCapacity = std::max (options.maxBlockSize * 4, (int) std::llround (secs * info.sampleRate));
    }
    sampleFifo.setTotalSize (fifoCapacity + 1);   // AbstractFifo holds one less than its size
    sampleBuffer.assign ((size_t) numInputs, std::vector<float> ((size_t) fifoCapacity + 1));

    auto maxBlocks = fifoCapacity / 16 + 64;      // even tiny blocks fit
    blockFifo.setTotalSize (maxBlocks + 1);
    blockBuffer.resize ((size_t) maxBlocks + 1);

    for (int i = 0; i < numInputs; ++i)
        writers.push_back (std::make_unique<ChannelWriter> (store, i));

    auto cores = (int) std::max (1u, std::thread::hardware_concurrency());
    numGroups = options.writerThreads > 0 ? options.writerThreads : juce::jlimit (1, 8, cores / 2);
    numGroups = std::min (numGroups, numInputs);
    groups.resize ((size_t) numGroups);
    for (int i = 0; i < numInputs; ++i)
        groups[(size_t) (i % numGroups)].push_back (i);

    if (numGroups > 1)
        pool = std::make_unique<juce::ThreadPool> (juce::ThreadPoolOptions{}
                                                       .withThreadName ("Capture writer")
                                                       .withNumberOfThreads (numGroups - 1)
                                                       .withDesiredThreadPriority (juce::Thread::Priority::low));

    // Touch every page of the buffer now, so the audio thread never pays for
    // the system handing memory over the first time it writes there.
    for (auto& ch : sampleBuffer)
        for (size_t i = 0; i < ch.size(); i += 1024)
            reinterpret_cast<volatile float&> (ch[i]) = 0.0f;

    // Disk work runs below the audio thread and below the app's own threads:
    // it has seconds of buffer to catch up in, the audio thread has
    // milliseconds.
    drainThread = std::thread ([this]
    {
       #if JUCE_MAC
        pthread_set_qos_class_self_np (QOS_CLASS_UTILITY, 0);
       #endif
        drainLoop();
    });
}

CaptureRecorder::~CaptureRecorder()
{
    stop();
    if (options.deleteOnClose)
        options.folder.deleteRecursively();
}

bool CaptureRecorder::push (const float* const* channels, int numSamples) noexcept
{
    auto start = audioThreadPos;
    audioThreadPos += numSamples;

    if (numSamples <= 0) return true;

    if (sampleFifo.getFreeSpace() < numSamples || blockFifo.getFreeSpace() < 1 || numSamples > options.maxBlockSize)
    {
        droppedSamples.fetch_add (numSamples, std::memory_order_relaxed);
        pushedPos.store (audioThreadPos, std::memory_order_release);
        return false;
    }

    {
        auto w = sampleFifo.write (numSamples);
        for (int ch = 0; ch < numInputs; ++ch)
        {
            auto* dst = sampleBuffer[(size_t) ch].data();
            if (channels[ch] != nullptr)
            {
                if (w.blockSize1 > 0) std::memcpy (dst + w.startIndex1, channels[ch], (size_t) w.blockSize1 * sizeof (float));
                if (w.blockSize2 > 0) std::memcpy (dst + w.startIndex2, channels[ch] + w.blockSize1, (size_t) w.blockSize2 * sizeof (float));
            }
            else
            {
                if (w.blockSize1 > 0) std::memset (dst + w.startIndex1, 0, (size_t) w.blockSize1 * sizeof (float));
                if (w.blockSize2 > 0) std::memset (dst + w.startIndex2, 0, (size_t) w.blockSize2 * sizeof (float));
            }
        }
    }   // samples are published before the block that describes them

    {
        auto w = blockFifo.write (1);
        blockBuffer[(size_t) (w.blockSize1 > 0 ? w.startIndex1 : w.startIndex2)] = { start, numSamples };
    }

    pushedPos.store (audioThreadPos, std::memory_order_release);
    return true;
}

void CaptureRecorder::drainLoop()
{
    while (running.load())
    {
        if (! drainOnce())
            std::this_thread::sleep_for (std::chrono::milliseconds (5));
        checkDisk();
    }
    while (drainOnce()) {}
}

bool CaptureRecorder::drainOnce()
{
    // Read the pushed position before looking for blocks. A kept block is
    // published before the position moves past it, so if none are waiting,
    // everything up to here was dropped and is written as silence.
    auto pushed = pushedPos.load (std::memory_order_acquire);
    auto numBlocks = blockFifo.getNumReady();

    if (numBlocks == 0)
    {
        if (pushed <= expectedPos) return false;
        bool ok = true;
        for (auto& w : writers) { ok &= w->writeSilence (pushed - expectedPos); ok &= w->flush(); }
        if (! ok) writeError.store (true);
        gapCount.fetch_add (1);
        expectedPos = pushed;
        writtenPos.store (expectedPos, std::memory_order_release);
        return true;
    }

    ops.clear();
    int64_t frames = 0;
    {
        auto r = blockFifo.read (numBlocks);
        auto add = [&] (int index)
        {
            auto& b = blockBuffer[(size_t) index];
            if (b.start > expectedPos)
            {
                ops.push_back ({ true, b.start - expectedPos });
                gapCount.fetch_add (1);
            }
            ops.push_back ({ false, b.length });
            expectedPos = b.start + b.length;
            frames += b.length;
        };
        r.forEach (add);
    }

    int s1, n1, s2, n2;
    sampleFifo.prepareToRead ((int) frames, s1, n1, s2, n2);
    jassert (n1 + n2 == frames);

    auto writeGroup = [&] (const std::vector<int>& group)
    {
        bool ok = true;
        for (auto ch : group)
        {
            auto& w = *writers[(size_t) ch];
            auto* buf = sampleBuffer[(size_t) ch].data();
            int64_t k = 0;   // frames consumed from the buffer so far

            for (auto& op : ops)
            {
                if (op.gap) { ok &= w.writeSilence (op.length); continue; }

                auto remaining = op.length;
                while (remaining > 0)
                {
                    const float* src;
                    int64_t avail;
                    if (k < n1) { src = buf + s1 + k; avail = n1 - k; }
                    else        { src = buf + s2 + (k - n1); avail = n2 - (k - n1); }
                    auto n = std::min (remaining, avail);
                    ok &= w.write (src, n);
                    k += n; remaining -= n;
                }
            }
            ok &= w.flush();
        }
        if (! ok) writeError.store (true);
    };

    if (pool != nullptr)
    {
        std::atomic<int> remaining { numGroups - 1 };
        juce::WaitableEvent done;
        for (int g = 1; g < numGroups; ++g)
            pool->addJob ([&, g]
            {
                writeGroup (groups[(size_t) g]);
                if (--remaining == 0) done.signal();
            });
        writeGroup (groups[0]);
        done.wait();
    }
    else
    {
        writeGroup (groups[0]);
    }

    sampleFifo.finishedRead ((int) frames);
    writtenPos.store (expectedPos, std::memory_order_release);
    return true;
}

int64_t CaptureRecorder::freeBytesNow() const
{
    return options.freeBytes ? options.freeBytes() : options.folder.getBytesFreeOnVolume();
}

void CaptureRecorder::checkDisk()
{
    auto now = juce::Time::getMillisecondCounter();
    if (now - lastDiskCheck < 250) return;
    lastDiskCheck = now;

    if (freeBytesNow() >= options.diskReserveBytes) { diskFull.store (false); return; }

    // A keep may be exporting under this lock; never wait for it here, or the
    // writers stall and live audio drops. Try again next time round.
    std::unique_lock<std::mutex> lock (storeMutex, std::try_to_lock);
    if (! lock.owns_lock()) return;
    auto seg = info.segmentSamples;
    auto oldest = firstAvailable.load() / seg;
    auto current = writtenPos.load() / seg;

    if (oldest >= current) { diskFull.store (true); return; }

    for (int i = 0; i < numInputs; ++i)
        store.segmentFile (i, oldest).deleteFile();

    firstAvailable.store ((oldest + 1) * seg);
    evictedSegments.fetch_add (1);
}

void CaptureRecorder::stop()
{
    if (stopped) return;
    stopped = true;
    running.store (false);
    if (drainThread.joinable()) drainThread.join();
    for (auto& w : writers) { w->flush(); w->close(); }
    pool.reset();
}

TakeResult CaptureRecorder::keep (int64_t start, int64_t end, juce::Array<int> inputs,
                                  const juce::File& destFolder, const juce::String& takeName, int timeoutMs)
{
    juce::String note;
    end = std::min (end, pushedPos.load());
    auto first = firstAvailable.load();
    if (start < first)
    {
        note = "The start was cleared earlier to make room, so this take begins "
             + juce::String ((first - start) / info.sampleRate, 1) + " seconds later than asked.";
        start = first;
    }
    if (end <= start)
    {
        TakeResult r;
        r.error = note.isNotEmpty() ? "That audio was cleared earlier to make room on the disk."
                                    : "There is nothing in that stretch of time.";
        return r;
    }
    if (inputs.isEmpty())
        for (int i = 0; i < numInputs; ++i) inputs.add (i);

    auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
    while (writtenPos.load() < end)
    {
        if (juce::Time::getMillisecondCounter() > deadline)
        {
            TakeResult r;
            r.error = "The disk is behind, so that audio isn't saved yet. Try again in a moment.";
            return r;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (2));
    }

    std::lock_guard<std::mutex> lock (storeMutex);
    auto r = store.exportTake (start, end, inputs, destFolder, takeName);
    r.note = note;
    return r;
}

TakeResult CaptureRecorder::keepLast (double seconds, juce::Array<int> inputs,
                                      const juce::File& destFolder, const juce::String& takeName)
{
    auto end = pushedPos.load();
    auto start = std::max<int64_t> (0, end - (int64_t) std::llround (seconds * info.sampleRate));
    return keep (start, end, std::move (inputs), destFolder, takeName);
}

double CaptureRecorder::hoursOfRoomLeft() const
{
    auto usable = (double) (freeBytesNow() - options.diskReserveBytes);
    return std::max (0.0, usable / getBytesPerSecond() / 3600.0);
}

static juce::String describeDuration (double seconds)
{
    if (seconds < 90)   return juce::String (juce::roundToInt (seconds)) + (juce::roundToInt (seconds) == 1 ? " second" : " seconds");
    if (seconds < 5400) return juce::String (juce::roundToInt (seconds / 60)) + " minutes";
    const double hours = seconds / 3600;
    if (hours >= 10)   // "about 4749 hours", not "4749.0"
        return juce::String (juce::roundToInt (hours)) + " hours";
    return juce::String (hours, 1) + " hours";
}

juce::String CaptureRecorder::status() const
{
    juce::StringArray s;
    s.add ("Capturing " + juce::String (numInputs) + (numInputs == 1 ? " input, " : " inputs, ")
           + describeDuration ((double) pushedPos.load() / info.sampleRate) + " so far.");

    if (diskFull.load())
        s.add ("The disk is full and there's no unkept audio left to clear. Capture can't continue.");
    else
        s.add ("Room on the disk for about " + describeDuration (hoursOfRoomLeft() * 3600) + " more.");

    if (auto dropped = droppedSamples.load(); dropped > 0)
        s.add (describeDuration ((double) dropped / info.sampleRate) + " was dropped in "
               + juce::String (gapCount.load()) + (gapCount.load() == 1 ? " place" : " places")
               + " because the disk fell behind.");
    else
        s.add ("Nothing dropped.");

    if (auto ev = evictedSegments.load(); ev > 0)
        s.add ("The disk got low, so the oldest " + describeDuration ((double) ev * info.segmentSamples / info.sampleRate)
               + " of unkept audio was cleared to keep recording.");

    if (writeError.load())
        s.add ("A write to the disk failed.");

    return s.joinIntoString (" ");
}

} // namespace earfirst
