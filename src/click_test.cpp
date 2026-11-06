// click-test: the metronome's timing, alone and inside the engine.
// Output is one plain sentence per line, written for a screen reader.

#include <JuceHeader.h>
#include <tracktion_engine/testing/tracktion_EnginePlayer.h>
#include "click/ClickPlugin.h"
#include <iostream>

using namespace tracktion;
using namespace earfirst;

namespace
{
void say (const juce::String& s)    { std::cout << s << std::endl; }
int failures = 0;
void check (bool ok, const juce::String& what) { if (! ok) ++failures; say ((ok ? "Pass: " : "FAIL: ") + what); }

struct TempPropertyStorage : public PropertyStorage
{
    using PropertyStorage::PropertyStorage;
    juce::File getAppPrefsFolder() override { return juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile (getApplicationName()); }
    juce::File getAppCacheFolder() override { return getAppPrefsFolder().getChildFile ("Cache"); }
};
struct QuietEngineBehaviour : public EngineBehaviour { bool autoInitialiseDeviceManager() override { return false; } };

juce::String times (const std::vector<ClickTick>& ts)
{
    juce::StringArray a;
    for (auto& t : ts) a.add (juce::String (t.time.inSeconds(), 3));
    return a.joinIntoString (" ");
}
} // namespace

// click-test listen <folder>: one short recording of each sound, for choosing by ear.
int renderListens (Engine& engine, const juce::File& folder)
{
    folder.createDirectory();
    HostedAudioDeviceInterface::Parameters p; p.sampleRate = 48000; p.blockSize = 256; p.inputChannels = 0; p.outputChannels = 2;
    auto player = std::make_unique<test_utilities::EnginePlayer> (engine, p);
    int n = 0;
    for (auto& sound : ClickSettings::sounds())
    {
        auto e = Edit::createSingleTrackEdit (engine);
        e->tempoSequence.getTempo (0)->setBpm (100.0);
        auto plugin = e->getPluginCache().createNewPlugin (ClickPlugin::xmlTypeName, {});
        getAudioTracks (*e)[0]->pluginList.insertPlugin (plugin, 0, nullptr);
        ClickSettings s; s.barSound = s.beatSound = s.subdivisionSound = sound; s.subdivision = 2;
        dynamic_cast<ClickPlugin*> (plugin.get())->setSettings (s);
        auto& tc = e->getTransport();
        tc.ensureContextAllocated();
        tc.play (false);
        e->dispatchPendingUpdatesSynchronously();
        juce::AudioBuffer<float> all (2, (int) (48000 * 4.8));   // two bars at 100
        for (int done = 0; done + 256 <= all.getNumSamples(); done += 256)
        {
            auto out = player->process (256);
            for (int c = 0; c < 2; ++c) all.copyFrom (c, done, out, c, 0, 256);
        }
        tc.stop (false, false);
        plugin = nullptr;
        e.reset();
        auto f = folder.getChildFile (juce::String (++n) + " " + sound + ".wav");
        f.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> os (new juce::FileOutputStream (f));
        auto w = wav.createWriterFor (os, juce::AudioFormatWriterOptions{}.withSampleRate (48000).withNumChannels (2).withBitsPerSample (24));
        w->writeFromAudioSampleBuffer (all, 0, all.getNumSamples());
        say ("Wrote " + f.getFileName() + ".");
    }
    player.reset();
    engine.getDeviceManager().closeDevices();
    engine.getDeviceManager().removeHostedAudioDeviceInterface();
    return 0;
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    Engine engine (std::make_unique<TempPropertyStorage> ("EarFirstClickTest"), std::make_unique<UIBehaviour>(), std::make_unique<QuietEngineBehaviour>());
    engine.getPluginManager().createBuiltInType<ClickPlugin>();
    if (argc > 2 && juce::String (argv[1]) == "listen")
        return renderListens (engine, juce::File (juce::String::fromUTF8 (argv[2])));
    say ("Click test.");

    auto edit = Edit::createSingleTrackEdit (engine);
    auto& tempo = edit->tempoSequence;
    tempo.getTempo (0)->setBpm (120.0);

    say ("");
    say ("Timing on its own");
    ClickSettings s;
    auto t = clickTicks (tempo, { TimePosition(), TimePosition::fromSeconds (2.0) }, s);
    check (t.size() == 4 && times (t) == "0.000 0.500 1.000 1.500", "120 in 4/4: four clicks in two seconds, at " + times (t) + ".");
    check (t.size() == 4 && t[0].kind == ClickTick::Kind::bar && t[1].kind == ClickTick::Kind::beat && t[0].level == 1.0f && t[1].level < 1.0f,
           "the one is the bar sound and loud, the others the beat sound.");

    s.subdivision = 2;
    t = clickTicks (tempo, { TimePosition(), TimePosition::fromSeconds (1.0) }, s);
    check (times (t) == "0.000 0.250 0.500 0.750" && t[1].kind == ClickTick::Kind::subdivision, "eighths: " + times (t) + ".");
    s.swing = 0.5f;
    t = clickTicks (tempo, { TimePosition(), TimePosition::fromSeconds (1.0) }, s);
    check (times (t) == "0.000 0.375 0.500 0.875", "eighths with 50 percent swing push the offbeats late: " + times (t) + ".");
    s.swing = 0; s.subdivision = 3;
    t = clickTicks (tempo, { TimePosition(), TimePosition::fromSeconds (0.5) }, s);
    check (times (t) == "0.000 0.167 0.333", "triplets: " + times (t) + ".");

    s.subdivision = 1; s.accents = "L 0 S N";
    t = clickTicks (tempo, { TimePosition(), TimePosition::fromSeconds (2.0) }, s);
    check (t.size() == 4 && t[1].level == 0.0f && t[2].level < t[3].level, "accents L 0 S N: beat two silent, beat three soft.");

    // A count-in before bar 1: the bar before zero clicks, starting on the one.
    s = ClickSettings();
    t = clickTicks (tempo, { TimePosition::fromSeconds (-2.0), TimePosition() }, s);
    check (times (t) == "-2.000 -1.500 -1.000 -0.500" && t[0].kind == ClickTick::Kind::bar && t[1].kind == ClickTick::Kind::beat,
           "a one-bar count-in before bar 1 clicks four times, the first on the one: " + times (t) + ".");

    // Splitting into small blocks must give exactly the same clicks.
    s = ClickSettings(); s.subdivision = 4; s.swing = 0.3f;
    auto whole = clickTicks (tempo, { TimePosition(), TimePosition::fromSeconds (8.0) }, s);
    std::vector<ClickTick> pieces;
    for (double a = 0; a < 8.0; a += 256.0 / 48000.0)
        for (auto& x : clickTicks (tempo, { TimePosition::fromSeconds (a), TimePosition::fromSeconds (std::min (8.0, a + 256.0 / 48000.0)) }, s))
            pieces.push_back (x);
    bool same = whole.size() == pieces.size();
    for (size_t i = 0; same && i < whole.size(); ++i) same = std::abs (whole[i].time.inSeconds() - pieces[i].time.inSeconds()) < 1e-9;
    check (same, "split into audio blocks, nothing doubled or dropped (" + juce::String ((int) whole.size()) + " clicks either way).");

    // Tempo change: 120 for a bar, then 60.
    tempo.insertTempo (BeatPosition::fromBeats (4.0), 60.0, 1.0f);
    s = ClickSettings();
    t = clickTicks (tempo, { TimePosition(), TimePosition::fromSeconds (5.0) }, s);
    check (times (t) == "0.000 0.500 1.000 1.500 2.000 3.000 4.000", "a tempo change from 120 to 60 at bar 2: " + times (t) + ".");
    check (t.size() > 4 && t[4].kind == ClickTick::Kind::bar, "and bar 2 starts with the bar sound.");
    tempo.getTempo (1)->setBpm (120.0);

    // 7/8 with a 2+2+3 feel.
    tempo.getTimeSig (0)->setStringTimeSig ("7/8");
    s.accents = "L N L N L N N";
    t = clickTicks (tempo, tempo.toTime ({ BeatPosition(), BeatPosition::fromBeats (14) }), s);
    int bars = 0; for (auto& x : t) bars += x.kind == ClickTick::Kind::bar;
    check (t.size() == 14 && bars == 2, "7/8: fourteen beats make two bars, each starting on the bar sound (" + juce::String (bars) + ").");
    check (t.size() == 14 && t[2].level == 1.0f && t[4].level == 1.0f && t[5].level < 1.0f, "and the accents fall on 1, 3 and 5.");
    tempo.getTimeSig (0)->setStringTimeSig ("4/4");

    say ("");
    say ("In the engine");
    {
        HostedAudioDeviceInterface::Parameters p; p.sampleRate = 48000; p.blockSize = 256; p.inputChannels = 0; p.outputChannels = 2;
        auto player = std::make_unique<test_utilities::EnginePlayer> (engine, p);
        auto e = Edit::createSingleTrackEdit (engine);
        e->tempoSequence.getTempo (0)->setBpm (120.0);
        auto track = getAudioTracks (*e)[0];
        auto plugin = e->getPluginCache().createNewPlugin (ClickPlugin::xmlTypeName, {});
        track->pluginList.insertPlugin (plugin, 0, nullptr);
        auto* click = dynamic_cast<ClickPlugin*> (plugin.get());
        check (click != nullptr, "the click plugin loads on a track.");

        auto& tc = e->getTransport();
        tc.ensureContextAllocated();
        tc.play (false);
        e->dispatchPendingUpdatesSynchronously();

        juce::AudioBuffer<float> all (2, 48000 * 3);
        all.clear();
        for (int done = 0; done < all.getNumSamples(); done += 256)
        {
            auto out = player->process (256);
            for (int c = 0; c < 2; ++c) all.copyFrom (c, done, out, c, 0, 256);
        }
        tc.stop (false, false);

        // Onsets: the first loud sample after at least 50 ms of near-silence.
        std::vector<int> onsets;
        int quietFor = 100000;
        for (int i = 0; i < all.getNumSamples(); ++i)
        {
            const float v = std::abs (all.getSample (0, i));
            if (v > 0.01f && quietFor > 2400) onsets.push_back (i);
            quietFor = v < 0.01f ? quietFor + 1 : 0;   // quiet = below the trigger level, so a soft attack can't reset it
        }
        juce::StringArray o;
        for (auto x : onsets) o.add (juce::String (x / 48000.0, 4));
        say ("Heard clicks at " + o.joinIntoString (", ") + " seconds.");


        // Count-in only: clicks before the count-in ends, then silence.
        ClickSettings c; c.countInBars = 1; c.countInOnly = true;
        click->setSettings (c);
        click->countInEndsAt = 1.0;
        tc.setPosition (TimePosition());
        tc.play (false);
        e->dispatchPendingUpdatesSynchronously();
        all.clear();
        for (int done = 0; done < all.getNumSamples(); done += 256)
        {
            auto out = player->process (256);
            for (int ch = 0; ch < 2; ++ch) all.copyFrom (ch, done, out, ch, 0, 256);
        }
        tc.stop (false, false);
        float after = 0;
        for (int i = 48000 + 2400; i < all.getNumSamples(); ++i) after = std::max (after, std::abs (all.getSample (0, i)));
        check (after < 0.001f, "click only during the count-in: silent after it ends.");
        say ("The click describes itself as: " + click->getSettings().describe() + ".");

        // Settings changed straight on the song's data, the way Undo does,
        // must reach the sound: turn count-in-only off that way, then listen.
        click->state.setProperty ("countInOnly", false, nullptr);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
        tc.setPosition (TimePosition());
        tc.play (false);
        e->dispatchPendingUpdatesSynchronously();
        all.clear();
        for (int done = 0; done < all.getNumSamples(); done += 256)
        {
            auto out = player->process (256);
            for (int ch = 0; ch < 2; ++ch) all.copyFrom (ch, done, out, ch, 0, 256);
        }
        tc.stop (false, false);
        float later = 0;
        for (int i = 48000 + 2400; i < all.getNumSamples(); ++i) later = std::max (later, std::abs (all.getSample (0, i)));
        check (later > 0.05f, "a change made the way Undo makes it reaches the sound: after the count-in it clicks again.");
        click = nullptr;
        plugin = nullptr;   // let go of the plugin before its song goes
        e.reset();
        player.reset();
        engine.getDeviceManager().closeDevices();
        engine.getDeviceManager().removeHostedAudioDeviceInterface();
    }

    say ("");
    say ("Your own sounds");
    {
        auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ear-first-click-samples-test");
        tmp.deleteRecursively();
        auto src = tmp.getChildFile ("originals"); src.createDirectory();
        ClickSamples::get().setFolder (tmp.getChildFile ("Click Sounds"));

        // A stick hit at 44.1 kHz with a tenth of a second of silence in front.
        auto wavFile = src.getChildFile ("My Stick.wav");
        {
            juce::AudioBuffer<float> b (2, 44100 / 2);
            b.clear();
            for (int i = 0; i < 1323; ++i)
                for (int c = 0; c < 2; ++c)
                    b.setSample (c, 4410 + i, 0.3f * std::exp (-i / 300.0f) * (float) std::sin (2 * juce::MathConstants<double>::pi * 1000.0 * i / 44100.0 + 0.5));
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> os (new juce::FileOutputStream (wavFile));
            auto w = wav.createWriterFor (os, juce::AudioFormatWriterOptions{}.withSampleRate (44100).withNumChannels (2).withBitsPerSample (24));
            w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
        }
        auto notes = src.getChildFile ("notes.txt"); notes.replaceWithText ("not audio");

        juce::StringArray problems;
        auto names = ClickSamples::get().import ({ wavFile, notes }, problems);
        check (names == juce::StringArray { "My Stick" } && problems.size() == 1,
               "importing a WAV and a text file brings in the WAV as My Stick and says the text file isn't audio (" + problems.joinIntoString (" ") + ").");
        check (ClickSettings::sounds().contains ("My Stick"), "My Stick is offered with the built-in sounds.");
        auto again = ClickSamples::get().import ({ wavFile }, problems);
        check (again == juce::StringArray { "My Stick 2" }, "importing it again keeps both, as My Stick 2.");
        ClickSamples::get().remove ("My Stick 2");

        auto ready = ClickSamples::get().load ("My Stick", 48000);
        int first = -1;
        if (ready) for (int i = 0; i < ready->audio.getNumSamples() && first < 0; ++i) if (std::abs (ready->audio.getSample (0, i)) > 0.003f) first = i;
        check (ready != nullptr && first >= 0 && first <= 16,
               "made ready at 48 kHz, the silence in front is gone: the sound starts " + juce::String (first) + " samples in.");
        check (ready != nullptr && std::abs (ready->audio.getMagnitude (0, ready->audio.getNumSamples()) - 0.7f) < 0.02f,
               "and it's brought to the same loudest point as the built-in sounds.");

        HostedAudioDeviceInterface::Parameters p; p.sampleRate = 48000; p.blockSize = 256; p.inputChannels = 0; p.outputChannels = 2;
        auto player = std::make_unique<test_utilities::EnginePlayer> (engine, p);
        auto e = Edit::createSingleTrackEdit (engine);
        e->tempoSequence.getTempo (0)->setBpm (120.0);
        auto plugin = e->getPluginCache().createNewPlugin (ClickPlugin::xmlTypeName, {});
        getAudioTracks (*e)[0]->pluginList.insertPlugin (plugin, 0, nullptr);
        auto* click = dynamic_cast<ClickPlugin*> (plugin.get());
        ClickSettings cs; cs.barSound = cs.beatSound = cs.subdivisionSound = "My Stick";
        click->setSettings (cs);
        auto& tc = e->getTransport();
        tc.ensureContextAllocated();
        // Let the engine settle first: on a fresh machine it finds its MIDI
        // devices during this wait and stops the transport while it does.
        juce::MessageManager::getInstance()->runDispatchLoopUntil (200);
        click->refreshNow();
        tc.play (false);
        e->dispatchPendingUpdatesSynchronously();
        juce::AudioBuffer<float> all (2, 48000 * 2);
        for (int done = 0; done < all.getNumSamples(); done += 256)
        {
            auto out = player->process (256);
            for (int ch = 0; ch < 2; ++ch) all.copyFrom (ch, done, out, ch, 0, 256);
        }
        tc.stop (false, false);
        std::vector<int> onsets;
        int quietFor = 100000;
        for (int i = 0; i < all.getNumSamples(); ++i)
        {
            const float v = std::abs (all.getSample (0, i));
            if (v > 0.01f && quietFor > 2400) onsets.push_back (i);
            quietFor = v < 0.01f ? quietFor + 1 : 0;
        }
        bool onBeat = onsets.size() == 4;
        for (size_t i = 0; onBeat && i < 4; ++i) onBeat = std::abs (onsets[i] - (int) i * 24000) <= 48;
        juce::StringArray o; for (auto x : onsets) o.add (juce::String (x));
        check (onBeat, "in the engine at 120, My Stick plays on every beat, within a millisecond (onsets at samples " + o.joinIntoString (", ") + ").");

        ClickSamples::get().remove ("My Stick");
        click->refreshNow();
        check (! ClickSettings::sounds().contains ("My Stick"), "removed, it's no longer offered, and the song falls back to the woodblock.");
        click = nullptr; plugin = nullptr;
        e.reset();
        player.reset();
        engine.getDeviceManager().closeDevices();
        engine.getDeviceManager().removeHostedAudioDeviceInterface();
        tmp.deleteRecursively();
    }

    edit.reset();
    say ("");
    say (failures == 0 ? "All checks passed." : juce::String (failures) + " checks failed.");
    return failures == 0 ? 0 : 1;
}
