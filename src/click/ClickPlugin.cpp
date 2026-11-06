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

std::vector<ClickTick> clickTicks (const TempoSequence& tempo, TimeRange range, const ClickSettings& s)
{
    std::vector<ClickTick> out;
    const int sub = juce::jlimit (1, 8, s.subdivision);
    auto accentFor = [&] (int beatInBar) -> char
    {
        auto a = s.accents.removeCharacters (" ,");
        if (a.isEmpty()) return beatInBar == 0 ? 'L' : 'N';
        return (char) a[beatInBar % a.length()];
    };

    const double b0 = tempo.toBeats (range.getStart()).inBeats();
    const double b1 = tempo.toBeats (range.getEnd()).inBeats();
    // Ticks are counted on the subdivided beat grid; swing delays the odd ones.
    const double swingDelay = (sub % 2 == 0) ? juce::jlimit (0.0f, 0.75f, s.swing) / sub : 0.0;
    for (auto k = (int64_t) std::floor (b0 * sub) - 1; k <= (int64_t) std::ceil (b1 * sub); ++k)
    {
        double beat = (double) k / sub;
        const bool onBeat = (k % sub) == 0;
        if (! onBeat && (k % 2 != 0) && swingDelay > 0) beat += swingDelay;
        if (beat < 0) continue;
        const auto t = tempo.toTime (BeatPosition::fromBeats (beat));
        if (t < range.getStart() || t >= range.getEnd()) continue;

        ClickTick tick { t, ClickTick::Kind::subdivision, s.subdivisionLevel };
        if (onBeat)
        {
            const auto bb = tempo.toBarsAndBeats (t);
            const int beatInBar = bb.getWholeBeats();
            const char a = accentFor (beatInBar);
            tick.kind = beatInBar == 0 ? ClickTick::Kind::bar : ClickTick::Kind::beat;
            tick.level = a == 'L' ? 1.0f : a == 'S' ? 0.4f : a == '0' ? 0.0f : 0.7f;
        }
        out.push_back (tick);
    }
    return out;
}

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
    cached = getSettings();
}

ClickPlugin::~ClickPlugin()
{
    notifyListenersOfDeletion();
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
    const juce::ScopedLock sl (settingsLock);
    cached = getSettings();
}

void ClickPlugin::initialise (const PluginInitialisationInfo&)
{
    voices.clear();
    voices.reserve (64);
    pending.reserve (64);
    const juce::ScopedLock sl (settingsLock);
    cached = getSettings();
}

void ClickPlugin::deinitialise() {}

namespace
{
// Each sound: a base frequency, a length in milliseconds, and how it's made.
struct SoundDef { double freq; double ms; int kind; };   // 0 sine pluck, 1 two-tone, 2 noise
const SoundDef& soundDef (int i)
{
    static const SoundDef defs[] = {
        { 1200.0, 30.0, 0 },   // Woodblock
        { 1000.0, 70.0, 0 },   // Beep
        { 2500.0, 40.0, 0 },   // Clave
        { 1700.0, 25.0, 2 },   // Rim
        { 540.0, 140.0, 1 },   // Cowbell
        { 8000.0, 45.0, 2 },   // Hi-hat
    };
    return defs[juce::jlimit (0, 5, i)];
}
}

void ClickPlugin::start (int sound, double pitch, float amp, int offset)
{
    Voice v;
    v.sound = sound; v.pitch = pitch; v.amp = amp;
    v.length = (int) (soundDef (sound).ms * 0.001 * sampleRate);
    if (pending.size() < pending.capacity()) pending.push_back ({ offset, v });
}

void ClickPlugin::render (float* left, float* right, int startSample, int num)
{
    for (auto& v : voices)
    {
        const auto& d = soundDef (v.sound);
        const double f = d.freq * v.pitch;
        for (int i = startSample; i < startSample + num && v.age < v.length; ++i, ++v.age)
        {
            const float env = std::exp (-5.0f * (float) v.age / (float) v.length) * (v.age < 16 ? (float) v.age / 16.0f : 1.0f);
            float x = 0;
            if (d.kind == 0)
            {
                // A pluck with a quick pitch drop: woody, not a beep.
                const double drop = 1.0 + 0.25 * std::exp (-(double) v.age / (0.004 * sampleRate));
                v.phase += juce::MathConstants<double>::twoPi * f * drop / sampleRate;
                x = (float) std::sin (v.phase);
            }
            else if (d.kind == 1)
            {
                v.phase += juce::MathConstants<double>::twoPi * f / sampleRate;
                v.phase2 += juce::MathConstants<double>::twoPi * f * 1.48 / sampleRate;
                x = 0.5f * ((std::sin (v.phase) > 0 ? 1.0f : -1.0f) + (std::sin (v.phase2) > 0 ? 1.0f : -1.0f)) * 0.6f;
            }
            else
            {
                v.noise = v.noise * 1664525u + 1013904223u;
                x = ((float) (v.noise >> 9) / 4194304.0f - 1.0f) * 0.7f;
            }
            const float out = x * env * v.amp;
            left[i] += out;
            if (right != left) right[i] += out;
        }
    }
    voices.erase (std::remove_if (voices.begin(), voices.end(), [] (auto& v) { return v.age >= v.length; }), voices.end());
}

void ClickPlugin::applyToBuffer (const PluginRenderContext& rc)
{
    if (rc.destBuffer == nullptr || rc.bufferNumSamples <= 0) return;
    rc.destBuffer->clear (rc.bufferStartSample, rc.bufferNumSamples);
    auto* left = rc.destBuffer->getWritePointer (0, rc.bufferStartSample);
    auto* right = rc.destBuffer->getNumChannels() > 1 ? rc.destBuffer->getWritePointer (1, rc.bufferStartSample) : left;

    ClickSettings s;
    {
        const juce::ScopedTryLock sl (settingsLock);
        if (sl.isLocked()) s = cached; else s = ClickSettings();
    }

    pending.clear();
    if (rc.isPlaying && ! rc.isScrubbing && ! rc.editTime.isEmpty())
    {
        const double countInEnd = countInEndsAt.load();
        const auto sounds = ClickSettings::sounds();
        for (auto& t : clickTicks (edit.tempoSequence, rc.editTime, s))
        {
            if (t.level <= 0.0f) continue;
            if (s.countInOnly && t.time.inSeconds() >= countInEnd) continue;
            const auto& name = t.kind == ClickTick::Kind::bar ? s.barSound : t.kind == ClickTick::Kind::beat ? s.beatSound : s.subdivisionSound;
            const double pitch = t.kind == ClickTick::Kind::bar ? 1.5 : t.kind == ClickTick::Kind::beat ? 1.0 : 0.8;
            const int offset = juce::jlimit (0, rc.bufferNumSamples - 1,
                                             (int) std::llround ((t.time - rc.editTime.getStart()).inSeconds() * sampleRate));
            start (std::max (0, sounds.indexOf (name)), pitch, t.level * s.level, offset);
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
