#include "click/ClickPlugin.h"

namespace earfirst
{
using namespace tracktion;

namespace ids
{
    static const juce::Identifier subdivision ("subdivision"), swing ("swing"), accents ("accents"),
        subdivisionLevel ("subdivisionLevel"), barSound ("barSound"), beatSound ("beatSound"),
        subdivisionSound ("subdivisionSound"), level ("level"), countInBars ("countInBars"), countInOnly ("countInOnly");
}

juce::String ClickSettings::describe() const
{
    static const char* subs[] = { "", "beats", "eighths", "triplets", "sixteenths", "quintuplets", "sextuplets", "septuplets", "thirty-seconds" };
    juce::String s = juce::String (subs[juce::jlimit (1, 8, subdivision)]);
    if (swing > 0.01f && (subdivision % 2) == 0) s << " with " << juce::roundToInt (swing * 100.0f) << " percent swing";
    s << (accents.trim().isEmpty() ? juce::String (", the one accented")
                                    : ", accents " + accents.replace ("L", "loud ").replace ("N", "normal ").replace ("S", "soft ").replace ("0", "silent ").trim());
    s << ", " << (barSound == beatSound ? barSound : barSound + " on the one, " + beatSound + " on the beats");
    if (countInBars > 0) s << ", count-in " << countInBars << (countInBars == 1 ? " bar" : " bars") << (countInOnly ? " then silent" : "");
    return s;
}

template <typename TempoMap>
void clickTicks (const TempoMap& tempo, TimeRange range, const ClickSettings& s, std::vector<ClickTick>& out)
{
    out.clear();
    const int sub = juce::jlimit (1, 8, s.subdivision);
    // Accent letters read without making new strings (this runs on the audio thread).
    auto accentFor = [&] (int beatInBar) -> char
    {
        int letters = 0;
        for (auto ch : s.accents) if (ch != ' ' && ch != ',') ++letters;
        if (letters == 0) return beatInBar == 0 ? 'L' : 'N';
        int want = beatInBar % letters, n = 0;
        for (auto ch : s.accents)
            if (ch != ' ' && ch != ',') { if (n == want) return (char) ch; ++n; }
        return 'N';
    };
    const int numeratorAtZero = juce::jmax (1, tempo.toBarsAndBeats (TimePosition()).numerator);

    const double b0 = tempo.toBeats (range.getStart()).inBeats();
    const double b1 = tempo.toBeats (range.getEnd()).inBeats();
    const double swingDelay = (sub % 2 == 0) ? juce::jlimit (0.0f, 0.75f, s.swing) / sub : 0.0;
    for (auto k = (int64_t) std::floor (b0 * sub) - 1; k <= (int64_t) std::ceil (b1 * sub); ++k)
    {
        double beat = (double) k / sub;
        const bool onBeat = ((k % sub) + sub) % sub == 0;
        if (! onBeat && ((k % 2) + 2) % 2 != 0 && swingDelay > 0) beat += swingDelay;
        const auto t = tempo.toTime (BeatPosition::fromBeats (beat));
        if (t < range.getStart() || t >= range.getEnd()) continue;

        ClickTick tick { t, ClickTick::Kind::subdivision, s.subdivisionLevel };
        if (onBeat)
        {
            // Before bar 1 (a count-in), count bars back from zero in the
            // first meter; after it, ask the tempo map.
            const int wholeBeat = (int) std::llround (beat);
            const int beatInBar = beat < 0 ? ((wholeBeat % numeratorAtZero) + numeratorAtZero) % numeratorAtZero
                                           : tempo.toBarsAndBeats (t).getWholeBeats();
            const char a = accentFor (beatInBar);
            tick.kind = beatInBar == 0 ? ClickTick::Kind::bar : ClickTick::Kind::beat;
            tick.level = a == 'L' ? 1.0f : a == 'S' ? 0.4f : a == '0' ? 0.0f : 0.7f;
        }
        out.push_back (tick);
    }
}

template void clickTicks<TempoSequence> (const TempoSequence&, TimeRange, const ClickSettings&, std::vector<ClickTick>&);
template void clickTicks<tempo::Sequence> (const tempo::Sequence&, TimeRange, const ClickSettings&, std::vector<ClickTick>&);

//==============================================================================
const char* ClickPlugin::xmlTypeName = "earFirstClick";

ClickPlugin::ClickPlugin (PluginCreationInfo info) : Plugin (info)
{
    auto um = getUndoManager();
    subdivision.referTo (state, ids::subdivision, um, 1);
    swing.referTo (state, ids::swing, um, 0.0f);
    accents.referTo (state, ids::accents, um, {});
    subdivisionLevel.referTo (state, ids::subdivisionLevel, um, 0.5f);
    barSound.referTo (state, ids::barSound, um, "Woodblock");
    beatSound.referTo (state, ids::beatSound, um, "Woodblock");
    subdivisionSound.referTo (state, ids::subdivisionSound, um, "Woodblock");
    level.referTo (state, ids::level, um, 0.8f);
    countInBars.referTo (state, ids::countInBars, um, 0);
    countInOnly.referTo (state, ids::countInOnly, um, false);

    // Rebuild what the audio thread reads whenever the click's settings or the
    // song's tempo and meter change, undo included.
    state.addListener (&watcher);
    tempoState = edit.tempoSequence.getState();
    tempoState.addListener (&watcher);
    voices.reserve (64);
    pending.reserve (64);
    ticks.reserve (512);
    rebuildSnapshot();
}

ClickPlugin::~ClickPlugin()
{
    cancelPendingUpdate();
    tempoState.removeListener (&watcher);
    state.removeListener (&watcher);
    notifyListenersOfDeletion();
}

void ClickPlugin::rebuildSnapshot()
{
    auto s = std::make_shared<Snapshot> (Snapshot { edit.tempoSequence.getInternalSequence(), getSettings() });
    const auto names = ClickSettings::builtInSounds();
    // A name that isn't built in is one of yours; one that can't be found
    // (a song from another computer) falls back to the woodblock.
    auto pick = [&] (const juce::String& name, int& builtIn, std::shared_ptr<const ClickSample>& sample)
    {
        builtIn = std::max (0, names.indexOf (name));
        if (! names.contains (name)) sample = ClickSamples::get().load (name, sampleRate > 0 ? sampleRate : 48000.0);
    };
    pick (s->settings.barSound, s->barSound, s->barSample);
    pick (s->settings.beatSound, s->beatSound, s->beatSample);
    pick (s->settings.subdivisionSound, s->subdivisionSound, s->subdivisionSample);
    if (snapshot != nullptr) retired.push_back (snapshot);
    std::atomic_store (&snapshot, std::shared_ptr<const Snapshot> (std::move (s)));
    // Only the retired list still holds these: safe to free here.
    retired.erase (std::remove_if (retired.begin(), retired.end(), [] (auto& p) { return p.use_count() == 1; }), retired.end());
}

ClickSettings ClickPlugin::getSettings() const
{
    ClickSettings s;
    s.subdivision = subdivision.get(); s.swing = swing.get(); s.accents = accents.get();
    s.subdivisionLevel = subdivisionLevel.get(); s.barSound = barSound.get(); s.beatSound = beatSound.get();
    s.subdivisionSound = subdivisionSound.get(); s.level = level.get();
    s.countInBars = countInBars.get(); s.countInOnly = countInOnly.get();
    return s;
}

void ClickPlugin::setSettings (const ClickSettings& s)
{
    subdivision = juce::jlimit (1, 8, s.subdivision); swing = juce::jlimit (0.0f, 0.75f, s.swing); accents = s.accents;
    subdivisionLevel = juce::jlimit (0.0f, 1.0f, s.subdivisionLevel); barSound = s.barSound; beatSound = s.beatSound;
    subdivisionSound = s.subdivisionSound; level = juce::jlimit (0.0f, 1.0f, s.level);
    countInBars = juce::jlimit (0, 16, s.countInBars); countInOnly = s.countInOnly;
    rebuildSnapshot();
}

void ClickPlugin::initialise (const PluginInitialisationInfo&)
{
    voices.clear();
    triggerAsyncUpdate();   // your own sounds are made ready at the sample rate now in use
}

void ClickPlugin::deinitialise() {}

void ClickPlugin::start (int sound, double pitch, float amp, int offset)
{
    Voice v;
    v.start (sound, pitch, amp, sampleRate);
    if (pending.size() < pending.capacity()) pending.push_back ({ offset, v });
}

void ClickPlugin::render (float* left, float* right, int startSample, int num)
{
    for (auto& v : voices)
        for (int i = startSample; i < startSample + num && ! v.finished(); ++i)
        {
            float l, r;
            v.nextStereo (l, r);
            left[i] += l;
            if (right != left) right[i] += r;
        }
    voices.erase (std::remove_if (voices.begin(), voices.end(), [] (auto& v) { return v.finished(); }), voices.end());
}

void ClickPlugin::applyToBuffer (const PluginRenderContext& rc)
{
    if (rc.destBuffer == nullptr || rc.bufferNumSamples <= 0) return;
    rc.destBuffer->clear (rc.bufferStartSample, rc.bufferNumSamples);
    auto* left = rc.destBuffer->getWritePointer (0, rc.bufferStartSample);
    auto* right = rc.destBuffer->getNumChannels() > 1 ? rc.destBuffer->getWritePointer (1, rc.bufferStartSample) : left;

    const auto snap = std::atomic_load (&snapshot);
    pending.clear();
    if (snap != nullptr && rc.isPlaying && ! rc.isScrubbing && ! rc.editTime.isEmpty())
    {
        const auto& s = snap->settings;
        const double countInEnd = countInEndsAt.load();
        clickTicks (snap->tempo, rc.editTime, s, ticks);
        for (auto& t : ticks)
        {
            if (t.level <= 0.0f) continue;
            if (s.countInOnly && t.time.inSeconds() >= countInEnd) continue;
            const int sound = t.kind == ClickTick::Kind::bar ? snap->barSound : t.kind == ClickTick::Kind::beat ? snap->beatSound : snap->subdivisionSound;
            const auto* sample = (t.kind == ClickTick::Kind::bar ? snap->barSample : t.kind == ClickTick::Kind::beat ? snap->beatSample : snap->subdivisionSample).get();
            const double pitch = t.kind == ClickTick::Kind::bar ? 1.5 : t.kind == ClickTick::Kind::beat ? 1.0 : 0.8;
            const int offset = juce::jlimit (0, rc.bufferNumSamples - 1,
                                             (int) std::llround ((t.time - rc.editTime.getStart()).inSeconds() * sampleRate));
            if (sample != nullptr)
            {
                Voice v;
                v.startSample (sample->audio.getReadPointer (0), sample->audio.getReadPointer (1), sample->audio.getNumSamples(), t.level * s.level);
                if (pending.size() < pending.capacity()) pending.push_back ({ offset, v });
            }
            else
                start (sound, pitch, t.level * s.level, offset);
        }
    }

    // Render in pieces so each click starts on its exact sample.
    std::sort (pending.begin(), pending.end(), [] (auto& a, auto& b) { return a.first < b.first; });
    int pos = 0;
    for (auto& [offset, v] : pending)
    {
        render (left, right, pos, offset - pos);
        pos = offset;
        if (voices.size() < 64) voices.push_back (v);
    }
    render (left, right, pos, rc.bufferNumSamples - pos);
}

} // namespace earfirst
