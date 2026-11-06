#include "capture/EngineCapture.h"

namespace earfirst
{
using namespace tracktion;

EngineCapture::EngineCapture (Engine& e, juce::File folder)
    : engine (e), captureFolder (std::move (folder))
{
    // We wrap the engine's own audio callback: each block we read the inputs
    // first, untouched, then hand the very same buffers to the engine. Some
    // devices give the first callback output buffers that share memory with
    // the inputs, so anything that ran after the engine could find inputs
    // already overwritten, and anything that ran before it and wrote output
    // would overwrite the engine's inputs. Wrapping avoids both.
    // The engine's DeviceManager is a callback through a private base; a
    // C-style cast is the one cast the language allows to reach it.
    auto& dm = engine.getDeviceManager();
    engineCallback = (juce::AudioIODeviceCallback*) &dm;
    dm.deviceManager.removeAudioCallback (engineCallback);
    dm.deviceManager.addAudioCallback (this);
    // Swapping stops and restarts the engine's callback, and the engine
    // finishes a restart on the message thread. Finish it now, so the engine
    // is never left paused waiting for it.
    dm.dispatchPendingUpdates();
}

EngineCapture::~EngineCapture()
{
    auto& jdm = engine.getDeviceManager().deviceManager;
    jdm.removeAudioCallback (this);
    jdm.addAudioCallback (engineCallback);
    engine.getDeviceManager().dispatchPendingUpdates();
    if (recorder != nullptr)
        recorder->stop();
}

void EngineCapture::attachEdit (Edit* e)
{
    edit = e;
    EditPlaybackContext* ctx = nullptr;
    if (e != nullptr)
    {
        e->getTransport().ensureContextAllocated();
        ctx = e->getTransport().getCurrentPlaybackContext();
    }
    // The callback reads this under the device's lock, so once this returns
    // no block is still using the old one.
    const juce::ScopedLock sl (engine.getDeviceManager().deviceManager.getAudioCallbackLock());
    context.store (ctx);
}

void EngineCapture::audioDeviceStopped()
{
    engineCallback->audioDeviceStopped();
}

void EngineCapture::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    engineCallback->audioDeviceAboutToStart (device);
    if (device == nullptr) return;

    auto names = device->getInputChannelNames();
    auto active = device->getActiveInputChannels();
    juce::StringArray newNames;
    for (int i = 0; i < names.size(); ++i)
        if (active[i]) newNames.add (names[i].isNotEmpty() ? names[i] : "Input " + juce::String (i + 1));
    const double rate = device->getCurrentSampleRate();

    // Same device shape: carry on with the capture we have.
    if (recorder != nullptr && rate == sampleRate && newNames.size() == numInputs)
        return;

    if (recorder != nullptr)
        recorder->stop();
    recorder.reset();
    blockStart = 0;
    sampleRate = rate;
    numInputs = newNames.size();
    inputNames = newNames;
    if (numInputs == 0) return;

    CaptureRecorder::Options o;
    o.folder = captureFolder.getChildFile (juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S"));
    o.sampleRate = rate;
    o.inputNames = inputNames;
    o.maxBlockSize = maxBlock = std::max (4096, device->getCurrentBufferSizeSamples());
    chunk.assign ((size_t) numInputs, nullptr);
    recorder = std::make_unique<CaptureRecorder> (o);
}

void EngineCapture::audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut,
                                                      int n, const juce::AudioIODeviceCallbackContext& deviceContext)
{
    // Inputs first, before the engine can touch the buffers.
    const bool captured = capture (in, numIn, n);
    engineCallback->audioDeviceIOCallbackWithContext (in, numIn, out, numOut, n, deviceContext);
    if (captured)
        noteTimeline (n);
}

bool EngineCapture::capture (const float* const* in, int numIn, int n) noexcept
{
    auto* rec = recorder.get();
    if (rec == nullptr || numIn != numInputs) return false;

    for (int done = 0; done < n;)
    {
        const int len = std::min (n - done, maxBlock);
        for (int c = 0; c < numIn; ++c) chunk[(size_t) c] = in[c] + done;
        rec->push (chunk.data(), len);
        done += len;
    }
    return true;
}

void EngineCapture::noteTimeline (int n) noexcept
{
    // Asked after the engine has run this block, the engine's clock reads the
    // block's end, and mapping that reading gives the timeline position the
    // engine's own recording gives this block's first sample. Checked against
    // the engine's recording at block sizes from 64 to 4096 by capture-engine-test.
    const auto pos = blockStart.load (std::memory_order_relaxed);
    bool playing = false;
    int64_t editSample = 0;
    if (auto* ctx = context.load (std::memory_order_acquire))
    {
        playing = ctx->isPlaying();
        if (playing)
            editSample = toSamples (ctx->globalStreamTimeToEditTime (engine.getDeviceManager().getCurrentStreamTime()), sampleRate);
    }
    map.note (pos, playing, editSample);
    blockStart.store (pos + n, std::memory_order_relaxed);
}

namespace
{
juce::String secondsText (double s)
{
    auto r = juce::roundToInt (s * 10.0) / 10.0;
    return juce::String (r, r == std::floor (r) ? 0 : 1) + (r == 1.0 ? " second" : " seconds");
}

juce::String whereText (Edit& edit, TimePosition t)
{
    auto bb = edit.tempoSequence.toBarsAndBeats (t);
    auto secs = t.inSeconds();
    juce::String time = secs >= 60.0 ? juce::String ((int) (secs / 60.0)) + " minutes " + secondsText (std::fmod (secs, 60.0))
                                     : secondsText (secs);
    return "bar " + juce::String (bb.bars + 1) + " beat " + juce::String (bb.getWholeBeats() + 1) + ", " + time + " in";
}
} // namespace

EngineCapture::Kept EngineCapture::keepLast (double seconds, const juce::File& takesFolder)
{
    Kept k;
    if (recorder == nullptr || edit == nullptr)
    {
        k.take.error = recorder == nullptr ? "Nothing is being captured. The audio device has no inputs open."
                                           : "There's no song open to put the take in.";
        k.sentence = k.take.error;
        return k;
    }

    juce::Array<int> all;
    for (int i = 0; i < numInputs; ++i) all.add (i);
    auto name = "Take " + juce::String (++takeNumber);
    k.take = recorder->keepLast (seconds, all, takesFolder, name);
    if (! k.take.ok()) { k.sentence = k.take.error; return k; }

    // Where it goes. Line it up with the song if the song was playing,
    // allowing for the interface's latency the same way normal recording does.
    auto start = map.timelineStartFor (k.take.start, k.take.end);
    if (start.has_value())
    {
        const auto latency = engine.getDeviceManager().getRecordAdjustmentSamples();
        k.timelineStart = TimePosition::fromSamples (*start - latency, sampleRate);
        k.linedUpWithSong = true;
    }
    else
    {
        k.timelineStart = edit->getTransport().getPosition();
    }

    const auto length = TimeDuration::fromSamples (k.take.end - k.take.start, sampleRate);
    // A take that starts before the song's beginning is trimmed to start at zero.
    TimeDuration offset;
    if (k.timelineStart < TimePosition())
    {
        offset = TimeDuration::fromSeconds (-k.timelineStart.inSeconds());
        k.timelineStart = TimePosition();
    }

    edit->ensureNumberOfAudioTracks (numInputs);
    auto tracks = getAudioTracks (*edit);
    for (int i = 0; i < k.take.files.size() && i < tracks.size(); ++i)
    {
        auto* t = tracks[i];
        if (t->getName().startsWith ("Track ") || t->getName().isEmpty())
            t->setName (inputNames[i]);
        if (offset >= length) continue;
        ClipPosition pos { { k.timelineStart, length - offset }, offset };
        if (auto clip = t->insertWaveClip (inputNames[i] + " " + name, k.take.files[i], pos, false))
            k.clips.add (clip.get());
    }

    auto took = (double) (k.take.end - k.take.start) / sampleRate;
    k.sentence = "Kept " + secondsText (took) + " on " + juce::String (k.clips.size())
                 + (k.clips.size() == 1 ? " input" : " inputs") + ", "
                 + (k.linedUpWithSong ? "lined up where you played it, at " : "placed at the playhead, ")
                 + whereText (*edit, k.timelineStart) + ".";
    if (k.take.note.isNotEmpty()) k.sentence += " " + k.take.note;
    if (map.hasOverflowed()) k.sentence += " The timing record overflowed, so placement may be off.";
    return k;
}

} // namespace earfirst
