// capture-engine-test: does a kept take land on the song timeline where it was played?
//
// No microphone. A stand-in audio device feeds a click into every input at a
// known moment, and each test checks where that click lands on the timeline.
// The yardstick is the engine's own normal recording of the same click.
//
// Output is one plain sentence per line, written for a screen reader.

#include <JuceHeader.h>
#include <tracktion_engine/testing/tracktion_EnginePlayer.h>
#include "capture/EngineCapture.h"
#include <iostream>
#include <thread>

using namespace tracktion;

namespace
{
void say (const juce::String& s)    { std::cout << s << std::endl; }

int failures = 0;
void check (bool ok, const juce::String& what)
{
    if (! ok) ++failures;
    say ((ok ? "Pass: " : "FAIL: ") + what);
}

constexpr double sr = 48000.0;
int block = 256;

struct QuietEngineBehaviour : public EngineBehaviour
{
    bool autoInitialiseDeviceManager() override    { return false; }
};

int64_t findClick (Engine& engine, const juce::File& f)
{
    std::unique_ptr<juce::AudioFormatReader> r (AudioFileUtils::createReaderFor (engine, f));
    if (r == nullptr) return -1;
    juce::AudioBuffer<float> buf ((int) r->numChannels, 4096);
    for (int64_t pos = 0; pos < r->lengthInSamples; pos += 4096)
    {
        auto n = (int) std::min<int64_t> (4096, r->lengthInSamples - pos);
        r->read (&buf, 0, n, pos, true, true);
        for (int i = 0; i < n; ++i)
            if (std::abs (buf.getSample (0, i)) > 0.5f)
                return pos + i;
    }
    return -1;
}

double clickTimeOnTimeline (Engine& engine, WaveAudioClip& clip)
{
    auto idx = findClick (engine, clip.getOriginalFile());
    if (idx < 0) return -1.0;
    auto pos = clip.getPosition();
    return pos.getStart().inSeconds() + (double) idx / sr - pos.getOffset().inSeconds();
}

struct Rig
{
    Rig (Engine& e, int numInputs, bool armForNormalRecording)
        : engine (e)
    {
        params.sampleRate = sr;
        params.blockSize = block;
        params.inputChannels = numInputs;
        params.outputChannels = 2;
        player = std::make_unique<test_utilities::EnginePlayer> (engine, params);
        for (auto wi : engine.getDeviceManager().getWaveInputDevices())
            wi->setEnabled (armForNormalRecording);
        for (auto mi : engine.getDeviceManager().getMidiInDevices())
            mi->setEnabled (true);

        folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("ear-first-engine", {});
        folder.createDirectory();
        tempProject = std::make_unique<ProjectManager::TempProject> (engine.getProjectManager(),
                                                                     folder.getChildFile ("test.tracktion"), true);
        auto item = tempProject->project->createNewEdit();
        edit = createEmptyEdit (engine, item->getSourceFile());
        edit->setProjectItemRef (item->getProjectItemRef());
        edit->ensureNumberOfAudioTracks (armForNormalRecording ? numInputs : 1);

        auto& transport = edit->getTransport();
        transport.ensureContextAllocated();
        if (armForNormalRecording)
        {
            auto context = transport.getCurrentPlaybackContext();
            auto tracks = getAudioTracks (*edit);
            auto inputs = context->getAllInputs();
            inputs.removeIf ([] (auto i) { return i->owner.isMidi(); });
            for (int i = 0; i < std::min (inputs.size(), tracks.size()); ++i)
            {
                [[maybe_unused]] auto r = inputs[i]->setTarget (tracks[i]->itemID, true, nullptr, 0);
                inputs[i]->setRecordingEnabled (tracks[i]->itemID, true);
            }
        }
        edit->dispatchPendingUpdatesSynchronously();

        capture = std::make_unique<earfirst::EngineCapture> (engine, folder.getChildFile ("capture"));
        capture->attachEdit (edit.get());
    }

    ~Rig()
    {
        capture->attachEdit (nullptr);
        capture.reset();
        edit.reset();
        tempProject.reset();
        player.reset();
        auto& dm = engine.getDeviceManager();
        dm.closeDevices();
        dm.removeHostedAudioDeviceInterface();
        dm.deviceManager.closeAudioDevice();
        folder.deleteRecursively();
    }

    // `seconds` of silence, with a click on every input `clickAt` seconds in (negative: none).
    void feed (double seconds, double clickAt = -1.0)
    {
        auto total = (int64_t) std::llround (seconds * sr);
        auto clickSample = clickAt < 0 ? -1 : (int64_t) std::llround (clickAt * sr);
        const int chunk = block * 64;
        juce::AudioBuffer<float> in (params.inputChannels, chunk);
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        for (int64_t done = 0; done < total;)
        {
            auto n = (int) std::min<int64_t> (chunk, total - done);
            in.setSize (params.inputChannels, n, false, false, true);
            in.clear();
            if (clickSample >= done && clickSample < done + n)
                for (int c = 0; c < params.inputChannels; ++c)
                    in.setSample (c, (int) (clickSample - done), 1.0f);
            player->process (in);
            done += n;
            // A real device delivers audio in real time. Feed at four times
            // that, so the disk is tested hard but not asked the impossible.
            auto due = (double) done / sr / 4.0 * 1000.0;
            while (juce::Time::getMillisecondCounterHiRes() - t0 < due)
                std::this_thread::sleep_for (std::chrono::milliseconds (1));
        }
    }

    // `seconds` of silence with MIDI messages at the given times (seconds from
    // the start of this call), one device block at a time, as a real device would.
    void feedMidi (double seconds, std::vector<std::pair<double, juce::MidiMessage>> events)
    {
        auto total = (int64_t) std::llround (seconds * sr);
        juce::AudioBuffer<float> in (params.inputChannels, block);
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        for (int64_t done = 0; done < total; done += block)
        {
            in.clear();
            juce::MidiBuffer midi;
            for (auto& [t, m] : events)
            {
                auto s = (int64_t) std::llround (t * sr);
                if (s >= done && s < done + block) midi.addEvent (m, (int) (s - done));
            }
            player->process (in, midi);
            auto due = (double) (done + block) / sr / 4.0 * 1000.0;
            while (juce::Time::getMillisecondCounterHiRes() - t0 < due)
                std::this_thread::sleep_for (std::chrono::milliseconds (1));
        }
    }

    void armMidiForNormalRecording()
    {
        auto context = edit->getTransport().getCurrentPlaybackContext();
        auto tracks = getAudioTracks (*edit);
        for (auto* i : context->getAllInputs())
            if (i->owner.isMidi())
            {
                [[maybe_unused]] auto r = i->setTarget (tracks[0]->itemID, true, nullptr, 0);
                i->setRecordingEnabled (tracks[0]->itemID, true);
            }
        edit->dispatchPendingUpdatesSynchronously();
    }

    juce::Array<MidiClip*> midiClips()
    {
        juce::Array<MidiClip*> out;
        for (auto t : getAudioTracks (*edit))
            for (auto c : t->getClips())
                if (auto m = dynamic_cast<MidiClip*> (c))
                    out.add (m);
        return out;
    }

    juce::Array<WaveAudioClip*> clips()
    {
        juce::Array<WaveAudioClip*> out;
        for (auto t : getAudioTracks (*edit))
            for (auto c : t->getClips())
                if (auto w = dynamic_cast<WaveAudioClip*> (c))
                    out.add (w);
        return out;
    }

    juce::File takes() const    { return folder.getChildFile ("takes"); }

    Engine& engine;
    HostedAudioDeviceInterface::Parameters params;
    std::unique_ptr<test_utilities::EnginePlayer> player;
    juce::File folder;
    std::unique_ptr<ProjectManager::TempProject> tempProject;
    std::unique_ptr<Edit> edit;
    std::unique_ptr<earfirst::EngineCapture> capture;
};

// One clip per input, every click at the same spot, and that spot where expected.
void checkPlacement (Engine& engine, earfirst::EngineCapture::Kept& k, int numInputs, double expected,
                     const juce::String& label, bool expectLinedUp)
{
    say (label + " says: " + k.sentence);
    check (k.take.ok(), label + ": the keep worked. " + k.take.error);
    check (k.clips.size() == numInputs, label + ": " + juce::String (k.clips.size()) + " clips, one per input.");
    check (k.linedUpWithSong == expectLinedUp,
           label + (expectLinedUp ? ": lined up with the song." : ": went to the playhead, since the song wasn't playing."));

    double worst = 0, first = -1;
    bool found = ! k.clips.isEmpty();
    int missing = 0;
    for (auto* c : k.clips)
    {
        auto t = clickTimeOnTimeline (engine, *c);
        if (t < 0) { found = false; ++missing; say ("  no click in " + c->getName() + ", file " + c->getOriginalFile().getFileName() + ", " + juce::String (c->getOriginalFile().getSize()) + " bytes"); continue; }
        if (first < 0) first = t;
        worst = std::max (worst, std::abs (t - expected));
    }
    check (found, label + ": the click is in every clip (" + juce::String (missing) + " missing).");
    const double samplesOff = worst * sr;
    check (samplesOff < 0.5, label + ": the click lands at " + juce::String (first, 4) + " seconds on every track, expected "
           + juce::String (expected, 4) + ". Off by " + juce::String (samplesOff, 1) + " samples at worst.");
}
} // namespace

int main (int argc, char** argv)
{
    if (argc > 1) block = juce::String (argv[1]).getIntValue();
    juce::ScopedJuceInitialiser_GUI init;
    Engine engine (std::make_unique<PropertyStorage> ("EarFirstEngineCaptureTest"),
                   std::make_unique<UIBehaviour>(),
                   std::make_unique<QuietEngineBehaviour>());

    say ("Engine capture test, audio blocks of " + juce::String (block) + " samples. No microphone is used.");

    // The yardstick: where the engine's normal recording puts a click 6 seconds in.
    double reference = -1;
    {
        Rig rig (engine, 8, true);
        auto& tc = rig.edit->getTransport();
        tc.record (false, false);
        rig.edit->dispatchPendingUpdatesSynchronously();
        rig.feed (10.0, 6.0);
        tc.stop (false, true);
        auto clips = rig.clips();
        if (! clips.isEmpty()) reference = clickTimeOnTimeline (engine, *clips[0]);
        say ("Normal recording puts a click played 6 seconds in at " + juce::String (reference, 4) + " seconds.");
        check (clips.size() == 8 && reference > 0, "the engine's own recording still works with the capture running alongside ("
               + juce::String (clips.size()) + " takes).");
    }
    const double lag = reference > 0 ? reference - 6.0 : 0.0;   // whatever the engine's own path adds

    // 1. Song playing from the start. Click 6 seconds in, keep the last 10.
    {
        say ("");
        say ("Test 1: 8 inputs, song playing from the start, click 6 seconds in.");
        Rig rig (engine, 8, false);
        auto& tc = rig.edit->getTransport();
        tc.play (false);
        rig.edit->dispatchPendingUpdatesSynchronously();
        rig.feed (10.0, 6.0);
        auto k = rig.capture->keepLast (10, rig.takes());
        tc.stop (false, true);
        checkPlacement (engine, k, 8, 6.0 + lag, "Test 1", true);
        if (std::getenv ("EARFIRST_DEBUG"))
            for (auto& an : rig.capture->getTimelineMap().getAnchors())
                say ("  anchor: capture " + juce::String (an.captureSample) + ", timeline " + juce::String (an.editSample)
                     + (an.playing ? ", playing" : ", stopped"));
    }

    // 2. Song playing from 30 seconds in.
    {
        say ("");
        say ("Test 2: song playing from 30 seconds in, click 6 seconds later, so it belongs at 36.");
        Rig rig (engine, 8, false);
        auto& tc = rig.edit->getTransport();
        tc.setPosition (TimePosition::fromSeconds (30.0));
        tc.play (false);
        rig.edit->dispatchPendingUpdatesSynchronously();
        rig.feed (10.0, 6.0);
        auto k = rig.capture->keepLast (10, rig.takes());
        tc.stop (false, true);
        checkPlacement (engine, k, 8, 36.0 + lag, "Test 2", true);
    }

    // 3. Playing before the song starts: stopped 4 seconds, then play from 10.
    //    The click comes 2 seconds after play, so it belongs at 12, and the
    //    4 seconds before play land just ahead of it.
    {
        say ("");
        say ("Test 3: you start playing, then start the song at 10 seconds. Click 2 seconds after the song starts, so it belongs at 12.");
        Rig rig (engine, 8, false);
        auto& tc = rig.edit->getTransport();
        tc.setPosition (TimePosition::fromSeconds (10.0));
        rig.feed (4.0);
        tc.play (false);
        rig.edit->dispatchPendingUpdatesSynchronously();
        rig.feed (6.0, 2.0);
        auto k = rig.capture->keepLast (10, rig.takes());
        tc.stop (false, true);
        checkPlacement (engine, k, 8, 12.0 + lag, "Test 3", true);
        if (std::getenv ("EARFIRST_DEBUG"))
            for (auto& an : rig.capture->getTimelineMap().getAnchors())
                say ("  anchor: capture " + juce::String (an.captureSample) + ", timeline " + juce::String (an.editSample)
                     + (an.playing ? ", playing" : ", stopped"));
        if (! k.clips.isEmpty())
            say ("Test 3: the take starts at " + juce::String (k.clips[0]->getPosition().getStart().inSeconds(), 3)
                 + " seconds, so the 4 seconds before the song started come just ahead of 10.");
    }

    // 4. Song never played. It goes at the playhead.
    {
        say ("");
        say ("Test 4: song stopped with the playhead at 20 seconds. Click 6 seconds in, keep the last 10.");
        Rig rig (engine, 4, false);
        auto& tc = rig.edit->getTransport();
        tc.setPosition (TimePosition::fromSeconds (20.0));
        rig.edit->dispatchPendingUpdatesSynchronously();
        rig.feed (10.0, 6.0);
        auto k = rig.capture->keepLast (10, rig.takes());
        checkPlacement (engine, k, 4, 26.0, "Test 4", false);
    }

    // 5. A full room: 64 inputs, song playing.
    {
        say ("");
        say ("Test 5: 64 inputs, song playing from the start, click 6 seconds in.");
        Rig rig (engine, 64, false);
        auto& tc = rig.edit->getTransport();
        tc.play (false);
        rig.edit->dispatchPendingUpdatesSynchronously();
        rig.feed (10.0, 6.0);
        auto t0 = juce::Time::getMillisecondCounterHiRes();
        auto k = rig.capture->keepLast (10, rig.takes());
        auto ms = juce::Time::getMillisecondCounterHiRes() - t0;
        tc.stop (false, true);
        checkPlacement (engine, k, 64, 6.0 + lag, "Test 5", true);
        say ("Test 5: keeping 64 inputs and putting them on 64 tracks took " + juce::String (juce::roundToInt (ms)) + " milliseconds.");
        check (rig.capture->getRecorder() != nullptr && rig.capture->getRecorder()->getDroppedSamples() == 0, "Test 5: nothing dropped.");
    }

    // 6. Looping: loop 4 to 8 seconds, play from 4, keep 10 seconds = two full
    //    passes and half of a third. Click 6.5 seconds in, in pass 2, which is
    //    timeline 6.5. Pass 2 (the latest full one) should go on the tracks.
    {
        say ("");
        say ("Test 6: 8 inputs, song looping 4 to 8 seconds. Click in the second pass, at 6.5 on the timeline.");
        Rig rig (engine, 8, false);
        auto& tc = rig.edit->getTransport();
        tc.setLoopRange ({ TimePosition::fromSeconds (4.0), TimePosition::fromSeconds (8.0) });
        tc.looping = true;
        tc.setPosition (TimePosition::fromSeconds (4.0));
        tc.play (false);
        rig.edit->dispatchPendingUpdatesSynchronously();
        rig.feed (10.0, 6.5);
        auto k = rig.capture->keepLast (10, rig.takes());
        tc.stop (false, true);
        checkPlacement (engine, k, 8, 6.5 + lag, "Test 6", true);
        check (k.passes.size() == 3, "Test 6: " + juce::String ((int) k.passes.size()) + " passes found, expected 3.");
        check (k.chosenPass == 1, "Test 6: pass " + juce::String (k.chosenPass + 1) + " is on the tracks, expected pass 2.");
        bool starts = k.passes.size() == 3;
        double worst = 0;
        for (auto& p : k.passes)
            worst = std::max (worst, std::abs (p.start.inSeconds() - 4.0) * sr);
        check (starts && worst <= block, "Test 6: every pass starts at the loop start, within " + juce::String (worst, 0)
               + " samples (the first pass skips the block right after play).");
        bool offsets = starts && std::abs (k.passes[1].offsetInFile.inSeconds() - 4.0) * sr < 0.5
                              && std::abs (k.passes[2].offsetInFile.inSeconds() - 8.0) * sr < 0.5;
        check (offsets, "Test 6: passes 2 and 3 begin exactly 4 and 8 seconds into the take.");
        if (std::getenv ("EARFIRST_DEBUG"))
        {
            for (auto& p : k.passes)
                say ("  pass: timeline " + juce::String (p.start.inSeconds() * sr, 0) + ", length " + juce::String (p.length.inSeconds() * sr, 0)
                     + ", file offset " + juce::String (p.offsetInFile.inSeconds() * sr, 0));
            for (auto& an : rig.capture->getTimelineMap().getAnchors())
                say ("  anchor: capture " + juce::String (an.captureSample) + ", timeline " + juce::String (an.editSample)
                     + (an.playing ? ", playing" : ", stopped"));
        }
    }

    // 7. MIDI: a note at 6 seconds, the pedal down at 2, a note held past the end.
    {
        say ("");
        say ("Test 7: MIDI, song playing. Pedal down at 2 seconds, a note at 6 for half a second, another held from 9.5 past the end.");
        const std::vector<std::pair<double, juce::MidiMessage>> events = {
            { 2.0, juce::MidiMessage::controllerEvent (1, 64, 127) },
            { 6.0, juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100) },
            { 6.5, juce::MidiMessage::noteOff (1, 60) },
            { 9.5, juce::MidiMessage::noteOn (1, 64, (juce::uint8) 90) },
        };

        // The yardstick: the engine's own MIDI recording of the same note.
        double reference = -1;
        {
            Rig rig (engine, 2, false);
            rig.armMidiForNormalRecording();
            auto& tc = rig.edit->getTransport();
            tc.record (false, false);
            rig.edit->dispatchPendingUpdatesSynchronously();
            rig.feedMidi (10.0, events);
            tc.stop (false, true);
            for (auto* c : rig.midiClips())
                for (auto* n : c->getSequence().getNotes())
                    if (n->getNoteNumber() == 60) reference = n->getEditStartTime (*c).inSeconds();
            say ("For comparison, the engine's own MIDI recording puts the note played at 6 seconds at " + juce::String (reference, 4)
                 + " seconds, " + juce::String ((6.0 - reference) * sr, 0) + " samples away from where its own audio recording puts a click from the same moment."
                 + " Ear First lines MIDI up with the audio instead.");
        }

        Rig rig (engine, 2, false);
        auto& tc = rig.edit->getTransport();
        tc.play (false);
        rig.edit->dispatchPendingUpdatesSynchronously();
        rig.feedMidi (10.0, events);
        auto k = rig.capture->keepLast (10, rig.takes());
        say ("Test 7 says: " + k.sentence);
        check (k.midiClips.size() == 1, "Test 7: one MIDI clip, for the one MIDI input.");
        if (! k.midiClips.isEmpty())
        {
            auto* c = k.midiClips[0];
            auto& notes = c->getSequence().getNotes();
            double first = -1, firstLen = -1, held = -1, heldEnd = -1;
            for (auto* n : notes)
            {
                if (n->getNoteNumber() == 60) { first = n->getEditStartTime (*c).inSeconds(); firstLen = n->getEditEndTime (*c).inSeconds() - first; }
                if (n->getNoteNumber() == 64) { held = n->getEditStartTime (*c).inSeconds(); heldEnd = n->getEditEndTime (*c).inSeconds(); }
            }
            // The yardstick is the audio: a click played at the same moment lands at 6.0 + lag.
            const double expected = 6.0 + lag;
            // The stand-in device delivers MIDI stamped at the start of its audio
            // block (the engine's own MIDI recording shows the same), so here
            // MIDI can only be checked to within one block. A real keyboard's
            // driver timestamps are finer; that's checked by hand.
            check (first <= expected + 1.0 / sr && (expected - first) * sr <= block, "Test 7: the note lands at " + juce::String (first, 4)
                   + " seconds, within one audio block of where audio played at the same moment lands (" + juce::String (expected, 4) + ").");
            // The stand-in device stamps MIDI per block, not per sample, so within a block here.
            check (std::abs (firstLen - 0.5) * sr <= block, "Test 7: and lasts half a second, to within one audio block ("
                   + juce::String (firstLen, 3) + ").");
            check (std::abs (held - 9.5) * sr <= block && std::abs (heldEnd - 10.0) * sr <= block,
                   "Test 7: the note still held at the end ends with the take, at " + juce::String (heldEnd, 2) + " seconds.");
            bool pedal = false;
            for (auto* e : c->getSequence().getControllerEvents())
                if (e->getType() == 64 && e->getControllerValue() > 0) pedal = true;
            check (pedal, "Test 7: the sustain pedal is in the clip.");
        }

        // Keep only the last 3 seconds: the pedal went down 5 seconds before
        // the take starts, so the clip must start with it down.
        auto k2 = rig.capture->keepLast (3, rig.takes());
        tc.stop (false, true);
        if (! k2.midiClips.isEmpty())
        {
            auto* c = k2.midiClips[0];
            double pedalAt = -1;
            for (auto* e : c->getSequence().getControllerEvents())
                if (e->getType() == 64 && e->getControllerValue() > 0) pedalAt = e->getEditTime (*c).inSeconds();
            check (std::abs (pedalAt - c->getPosition().getStart().inSeconds()) < 0.002,
                   "Test 7: a take that starts mid-pedal starts with the pedal down, at " + juce::String (pedalAt, 2) + " seconds.");
        }
        else
            check (false, "Test 7: keeping the last 3 seconds made a MIDI clip.");
    }

    say ("");
    say (failures == 0 ? "All checks passed." : juce::String (failures) + " checks failed.");
    return failures == 0 ? 0 : 1;
}
