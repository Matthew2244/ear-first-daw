#include "capture/EngineCapture.h"
#include "session/Tracks.h"

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
    // A keyboard plugged in later is picked up once the engine has rebuilt its inputs.
    dm.addChangeListener (this);
    // Swapping stops and restarts the engine's callback, and the engine
    // finishes a restart on the message thread. Finish it now, so the engine
    // is never left paused waiting for it.
    dm.dispatchPendingUpdates();
}

EngineCapture::~EngineCapture()
{
    stopTimer();
    engine.getDeviceManager().removeChangeListener (this);
    detachMidiTaps();
    midi.reset();
    auto& jdm = engine.getDeviceManager().deviceManager;
    jdm.removeAudioCallback (this);
    jdm.addAudioCallback (engineCallback);
    engine.getDeviceManager().dispatchPendingUpdates();
    if (recorder != nullptr)
        recorder->stop();
}

void EngineCapture::detachMidiTaps()
{
    // When a device is unplugged the engine rebuilds its inputs and deletes
    // the old ones, our listeners with them. Only unhook from inputs that
    // still exist (unplugging the test keyboard crashed it, 6 November 2026).
    juce::Array<InputDeviceInstance*> live;
    if (auto* ctx = context.load())
        live = ctx->getAllInputs();
    for (auto& [instance, tap] : midiTaps)
        if (live.contains (instance))
            instance->removeConsumer (tap.get());
    midiTaps.clear();
}

void EngineCapture::attachEdit (Edit* e)
{
    detachMidiTaps();
    edit = e;
    EditPlaybackContext* ctx = nullptr;
    if (e != nullptr)
    {
        e->getTransport().ensureContextAllocated();
        ctx = e->getTransport().getCurrentPlaybackContext();
    }
    // The callback reads this under the device's lock, so once this returns
    // no block is still using the old one.
    {
        const juce::ScopedLock sl (engine.getDeviceManager().deviceManager.getAudioCallbackLock());
        context.store (ctx);
    }

    attachMidiTaps();
}

void EngineCapture::attachMidiTaps()
{
    // Listen to every MIDI input of this song, armed or not. Called again when
    // devices change, since the engine rebuilds its inputs then.
    detachMidiTaps();
    auto* ctx = context.load();
    if (ctx == nullptr) return;
    for (auto& mi : engine.getDeviceManager().getMidiInDevices())
        if (! mi->isEnabled()) mi->setEnabled (true);
    for (auto* input : ctx->getAllInputs())
        if (input->owner.isMidi())
        {
            // The engine's "All MIDI Ins" merges every device; listening to it
            // too would keep every note twice (heard 6 November 2026).
            if (auto* v = dynamic_cast<VirtualMidiInputDevice*> (&input->owner); v != nullptr && v->useAllInputs)
                continue;
            auto tap = std::make_unique<MidiTap> (*this, input->owner.getName());
            input->addConsumer (tap.get());
            midiTaps.push_back ({ input, std::move (tap) });
        }
}

void EngineCapture::midiArrived (const juce::String& device, const juce::MidiMessage& m)
{
    auto* mc = midi.get();
    if (mc == nullptr) return;
    // Read the block clock consistently (it changes once a block).
    int64_t pos; double stream; uint32_t v1, v2;
    do
    {
        v1 = clockVersion.load (std::memory_order_acquire);
        pos = clockSample.load (std::memory_order_relaxed);
        stream = clockStreamTime.load (std::memory_order_relaxed);
        v2 = clockVersion.load (std::memory_order_acquire);
    } while (v1 != v2 || (v1 & 1u) != 0);
    const auto sample = pos + (int64_t) std::llround ((m.getTimeStamp() - stream) * sampleRate);
    mc->add (device, sample, m);
    if (onMidi) onMidi (device, m);
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

    // Same device, same inputs: carry on with the capture we have.
    if ((recorder != nullptr || midi != nullptr) && rate == sampleRate && newNames == inputNames)
        return;

    if (recorder != nullptr)
    {
        // Never lose audio on a switch: keep it on disk as recovered audio.
        const auto had = (double) recorder->getPushedSamples() / sampleRate;
        auto folder = recorder->getStore().getFolder();
        if (had > 0.5) { recorder->keepFilesOnClose(); if (midi) midi->keepFilesOnClose(); }
        midi.reset();
        recorder->stop();
        recorder.reset();
        if (had > 0.5 && onCaptureSetAside) onCaptureSetAside (folder, had);
    }
    midi.reset();
    blockStart = 0;
    sampleRate = rate;
    numInputs = newNames.size();
    inputNames = newNames;
    deviceBlock = device->getCurrentBufferSizeSamples();
    const auto runFolder = captureFolder.getChildFile (juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S"));
    if (numInputs == 0)
    {
        // No audio inputs: MIDI is still kept, on the device's clock.
        midi = std::make_unique<MidiCapture> (runFolder.getChildFile ("midi"), rate);
        return;
    }

    CaptureRecorder::Options o;
    o.folder = runFolder;
    o.sampleRate = rate;
    o.inputNames = inputNames;
    o.maxBlockSize = maxBlock = std::max (4096, device->getCurrentBufferSizeSamples());
    chunk.assign ((size_t) numInputs, nullptr);
    recorder = std::make_unique<CaptureRecorder> (o);
    midi = std::make_unique<MidiCapture> (o.folder.getChildFile ("midi"), rate);
}

void EngineCapture::audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut,
                                                      int n, const juce::AudioIODeviceCallbackContext& deviceContext)
{
    // The engine hasn't run this block yet: its clock reads the block's start.
    // MIDI timestamps are mapped onto the capture clock through this pair.
    clockVersion.fetch_add (1, std::memory_order_acq_rel);
    clockSample.store (blockStart.load (std::memory_order_relaxed), std::memory_order_relaxed);
    clockStreamTime.store (engine.getDeviceManager().getCurrentStreamTime(), std::memory_order_relaxed);
    clockVersion.fetch_add (1, std::memory_order_acq_rel);

    // Inputs first, before the engine can touch the buffers.
    capture (in, numIn, n);
    engineCallback->audioDeviceIOCallbackWithContext (in, numIn, out, numOut, n, deviceContext);
    renderEarcon (out, numOut, n);
    // The clock runs whenever the device does, inputs or not, so MIDI is
    // captured and placed even with no audio inputs open.
    if (sampleRate > 0)
        noteTimeline (n);
}

void EngineCapture::playEarcon (float f1, float f2)
{
    int s1, n1, s2, n2;
    earconFifo.prepareToWrite (1, s1, n1, s2, n2);
    if (n1 > 0) { earconQueue[(size_t) s1] = { f1, f2 }; earconFifo.finishedWrite (1); }
}

void EngineCapture::renderEarcon (float* const* out, int numOut, int n) noexcept
{
    if (! earcon.active && earconFifo.getNumReady() > 0)
    {
        int s1, n1, s2, n2;
        earconFifo.prepareToRead (1, s1, n1, s2, n2);
        if (n1 > 0) { auto r = earconQueue[(size_t) s1]; earcon = { r.f1, r.f2, 0.0, 0, true }; }
        earconFifo.finishedRead (n1 + n2);
    }
    if (! earcon.active || numOut == 0 || sampleRate <= 0) return;
    const int noteLen = (int) (0.07 * sampleRate);
    for (int i = 0; i < n && earcon.active; ++i, ++earcon.age)
    {
        const int note = earcon.age / noteLen;
        if (note >= 2) { earcon.active = false; break; }
        const int pos = earcon.age % noteLen;
        const float f = note == 0 ? earcon.f1 : earcon.f2;
        earcon.phase += juce::MathConstants<double>::twoPi * f / sampleRate;
        const float env = std::min (1.0f, pos / 96.0f) * std::exp (-4.0f * (float) pos / (float) noteLen);
        const float v = 0.2f * env * (float) std::sin (earcon.phase);
        for (int c = 0; c < std::min (numOut, 2); ++c)
            if (out[c] != nullptr) out[c][i] += v;
    }
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

    // A loop can wrap in the middle of a block. Note the exact sample where
    // it does, so every pass lines up to the sample, whatever the block size.
    if (playing)
        if (auto* ctx = context.load (std::memory_order_acquire))
            if (auto* ph = ctx->getNodePlayHead(); ph != nullptr && ph->isLooping() && ! ph->isRollingIntoLoop())
            {
                const auto loop = ph->getLoopRange();
                if (! loop.isEmpty() && editSample < loop.getEnd() && editSample + n > loop.getEnd())
                    map.note (pos + (loop.getEnd() - editSample), true, loop.getStart());
            }

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

void EngineCapture::placeMidi (Kept& k, TimePosition at, TimeDuration length, int64_t captureStart, const juce::String& takeName)
{
    if (midi == nullptr) return;
    midi->flushNow();
    const auto captureEnd = captureStart + toSamples (TimePosition() + length, sampleRate);
    for (auto& device : midi->devices())
    {
        auto events = midi->eventsIn (device, captureStart, captureEnd);
        bool anyNotes = false;
        for (auto& e : events) anyNotes |= e.message.isNoteOn();
        if (events.empty() || ! anyNotes) continue;

        // Times on the song's timeline, in seconds, the way the engine imports them.
        juce::MidiMessageSequence seq;
        for (auto& cc : midi->controllerStateAt (device, captureStart))   // the pedal where it was
            seq.addEvent (juce::MidiMessage (cc, at.inSeconds()));
        for (auto& e : events)
            seq.addEvent (juce::MidiMessage (e.message, at.inSeconds() + (double) (e.sample - captureStart) / sampleRate));
        seq.updateMatchedPairs();
        // A note still held when the take ends gets its end there.
        const double endSecs = at.inSeconds() + length.inSeconds();
        juce::Array<juce::MidiMessage> ends;
        for (int i = 0; i < seq.getNumEvents(); ++i)
            if (auto* ev = seq.getEventPointer (i); ev->message.isNoteOn() && ev->noteOffObject == nullptr)
                ends.add (juce::MidiMessage::noteOff (ev->message.getChannel(), ev->message.getNoteNumber(), (juce::uint8) 0)
                              .withTimeStamp (endSecs));
        for (auto& e : ends) seq.addEvent (e);
        seq.updateMatchedPairs();

        // The device's own track, made the first time it plays.
        AudioTrack* track = nullptr;
        for (auto* t : contentTracks (*edit))
            if (t->getName() == device) track = t;
        if (track == nullptr)
        {
            auto all = contentTracks (*edit);
            auto t = edit->insertNewAudioTrack (TrackInsertPoint (nullptr, all.isEmpty() ? nullptr : all.getLast()), nullptr);
            if (t == nullptr) continue;
            t->setName (device);
            track = t.get();
        }
        if (auto clip = track->insertMIDIClip (device + " " + takeName, TimeRange (at, length), nullptr))
        {
            clip->getSequence().importMidiSequence (seq, edit, at, nullptr);
            k.midiClips.add (clip.get());
        }
    }
}

EngineCapture::Kept EngineCapture::keepLast (double seconds, const juce::File& takesFolder)
{
    Kept k;
    if ((recorder == nullptr && midi == nullptr) || edit == nullptr)
    {
        k.take.error = edit == nullptr ? "There's no song open to put the take in."
                                       : "Nothing is being captured. No audio interface is open.";
        k.sentence = k.take.error;
        return k;
    }

    auto name = "Take " + juce::String (++takeNumber);
    if (recorder != nullptr)
    {
        juce::Array<int> all;
        for (int i = 0; i < numInputs; ++i) all.add (i);
        k.take = recorder->keepLast (seconds, all, takesFolder, name);
        if (! k.take.ok()) { k.sentence = k.take.error; return k; }
    }
    else
    {
        // MIDI only: the same stretch of time on the device's clock.
        k.take.end = blockStart.load();
        k.take.start = std::max<int64_t> (0, k.take.end - (int64_t) std::llround (seconds * sampleRate));
        if (k.take.end <= k.take.start) { k.take.error = "There's nothing in that stretch of time."; k.sentence = k.take.error; return k; }
    }

    const auto latency = engine.getDeviceManager().getRecordAdjustmentSamples();
    ensureContentTracks (*edit, numInputs);
    auto tracks = contentTracks (*edit);
    for (int i = 0; i < numInputs && i < tracks.size(); ++i)
        if (tracks[i]->getName().startsWith ("Track ") || tracks[i]->getName().isEmpty())
            tracks[i]->setName (inputNames[i]);

    auto placeOnTracks = [&] (TimePosition at, TimeDuration length, TimeDuration offset)
    {
        // A take that starts before the song's beginning is trimmed to start at zero.
        if (at < TimePosition())
        {
            k.secondsBeforeSongStart = -at.inSeconds();
            offset = offset + TimeDuration::fromSeconds (-at.inSeconds());
            length = length - TimeDuration::fromSeconds (-at.inSeconds());
            at = TimePosition();
        }
        if (length <= TimeDuration()) return;
        for (int i = 0; i < k.take.files.size() && i < tracks.size(); ++i)
            if (auto clip = tracks[i]->insertWaveClip (inputNames[i] + " " + name, k.take.files[i],
                                                       ClipPosition { { at, length }, offset }, false))
                k.clips.add (clip.get());
        k.timelineStart = at;
        placeMidi (k, at, length, k.take.start + toSamples (TimePosition() + offset, sampleRate), name);
    };

    // Was the song looping? Passes are steady playing stretches (the single
    // block right after pressing play is a transition, not a pass), and a
    // loop is a pass that goes back over the timeline the one before covered.
    std::vector<TimelineMap::Stretch> passes;
    for (auto& st : map.stretches (k.take.start, k.take.end))
        if (st.playing && st.length() > deviceBlock)
            passes.push_back (st);
    bool looped = false;
    for (size_t i = 1; i < passes.size(); ++i)
        if (passes[i].editStart < passes[i - 1].editStart + passes[i - 1].length() - deviceBlock)
            looped = true;

    if (looped)
    {
        // Every pass is kept. The fullest one goes on the tracks, the latest
        // if several are equally full; the rest wait in the take for comping.
        size_t best = 0;
        for (size_t i = 0; i < passes.size(); ++i)
        {
            const auto& p = passes[i];
            k.passes.push_back ({ TimePosition::fromSamples (p.editStart - latency, sampleRate),
                                  TimeDuration::fromSamples (p.length(), sampleRate),
                                  TimeDuration::fromSamples (p.captureStart - k.take.start, sampleRate) });
            if (p.length() >= passes[best].length() - deviceBlock)
                best = i;
        }
        k.chosenPass = (int) best;
        k.linedUpWithSong = true;
        auto& chosen = k.passes[best];
        placeOnTracks (chosen.start, chosen.length, chosen.offsetInFile);
    }
    else
    {
        // Line it up with the song if the song was playing, allowing for the
        // interface's latency the same way normal recording does.
        TimePosition at;
        if (auto start = map.timelineStartFor (k.take.start, k.take.end))
        {
            at = TimePosition::fromSamples (*start - latency, sampleRate);
            k.linedUpWithSong = true;
        }
        else
        {
            at = edit->getTransport().getPosition();
        }
        placeOnTracks (at, TimeDuration::fromSamples (k.take.end - k.take.start, sampleRate), {});
    }

    auto took = (double) (k.take.end - k.take.start) / sampleRate;
    if (k.take.files.isEmpty())
    {
        // MIDI only.
        if (k.midiClips.isEmpty())
        {
            k.take.error = "Nothing was played on a MIDI device in the last " + secondsText (took) + ".";
            k.sentence = k.take.error;
            return k;
        }
        k.sentence = "Kept " + secondsText (took) + " of MIDI from " + juce::String (k.midiClips.size())
                     + (k.midiClips.size() == 1 ? " device, " : " devices, ")
                     + (k.linedUpWithSong ? "lined up where you played it, at " : "placed at the playhead, ")
                     + (describePlace ? describePlace (*edit, k.timelineStart) : whereText (*edit, k.timelineStart)) + ".";
        k.brief = "Kept " + secondsText (took) + " of MIDI.";
        return k;
    }
    const auto midiWords = k.midiClips.isEmpty() ? juce::String()
                         : " and " + juce::String (k.midiClips.size()) + (k.midiClips.size() == 1 ? " MIDI device" : " MIDI devices");
    if (looped)
    {
        const int others = (int) k.passes.size() - 1;
        k.sentence = "Kept " + juce::String ((int) k.passes.size()) + " passes of the loop on "
                     + juce::String (k.take.files.size()) + (k.take.files.size() == 1 ? " input" : " inputs") + midiWords
                     + ". Pass " + juce::String (k.chosenPass + 1) + " is on the tracks at " + (describePlace ? describePlace (*edit, k.timelineStart) : whereText (*edit, k.timelineStart))
                     + (others == 1 ? ". The other pass is saved" : ". The other " + juce::String (others) + " are saved")
                     + " with the take for comping.";
        if (k.take.note.isNotEmpty()) k.sentence += " " + k.take.note;
        k.brief = "Kept " + juce::String ((int) k.passes.size()) + " passes, pass " + juce::String (k.chosenPass + 1) + " on the tracks.";
        return k;
    }
    k.sentence = "Kept " + secondsText (took) + " on " + juce::String (k.take.files.size())
                 + (k.take.files.size() == 1 ? " input" : " inputs") + midiWords + ", "
                 + (k.linedUpWithSong ? "lined up where you played it, at " : "placed at the playhead, ")
                 + (describePlace ? describePlace (*edit, k.timelineStart) : whereText (*edit, k.timelineStart)) + ".";
    k.brief = "Kept " + secondsText (took) + ", " + juce::String (k.take.files.size()) + (k.take.files.size() == 1 ? " input" : " inputs")
              + (k.midiClips.isEmpty() ? juce::String() : " and " + juce::String (k.midiClips.size()) + " MIDI") + ".";
    if (k.secondsBeforeSongStart >= 0.5)
        k.sentence += " The first " + secondsText (k.secondsBeforeSongStart)
                      + " came before the song's start, so they're tucked in at the start of the clip.";
    if (k.take.note.isNotEmpty()) k.sentence += " " + k.take.note;
    if (map.hasOverflowed()) k.sentence += " The timing record overflowed, so placement may be off.";
    return k;
}

} // namespace earfirst
