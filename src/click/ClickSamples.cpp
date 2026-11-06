#include "click/ClickSamples.h"
#include "ClickSoundsData.h"

namespace earfirst
{

ClickSamples& ClickSamples::get()
{
    static ClickSamples instance;
    return instance;
}

juce::File ClickSamples::fileFor (const juce::String& name) const
{
    auto dir = getFolder();
    for (auto& f : dir.findChildFiles (juce::File::findFiles, false))
        if (f.getFileNameWithoutExtension() == name && audioExtensions().contains (f.getFileExtension().substring (1).toLowerCase()))
            return f;
    return {};
}

juce::StringArray ClickSamples::names() const
{
    juce::StringArray out;
    for (auto& f : getFolder().findChildFiles (juce::File::findFiles, false))
        if (audioExtensions().contains (f.getFileExtension().substring (1).toLowerCase()) && ! f.getFileName().startsWithChar ('.'))
            out.addIfNotAlreadyThere (f.getFileNameWithoutExtension());
    out.sortNatural();
    return out;
}

juce::StringArray ClickSamples::import (const juce::Array<juce::File>& files, juce::StringArray& problems)
{
    juce::StringArray imported;
    auto dir = getFolder();
    if (! dir.createDirectory())
    {
        problems.add ("The Click Sounds folder couldn't be made.");
        return imported;
    }
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    for (auto& f : files)
    {
        std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
        if (r == nullptr || r->lengthInSamples <= 0)
        {
            problems.add (f.getFileName() + " isn't an audio file Ear First can read.");
            continue;
        }
        // Its own name; a second sound with the same name gets a number.
        auto base = f.getFileNameWithoutExtension().trim();
        if (base.isEmpty()) base = "Sound";
        if (builtInNames().contains (base, true) || base.equalsIgnoreCase ("Beep")) base << " (yours)";
        auto name = base;
        for (int n = 2; fileFor (name).existsAsFile(); ++n) name = base + " " + juce::String (n);
        auto dest = dir.getChildFile (juce::File::createLegalFileName (name) + f.getFileExtension().toLowerCase());
        if (! f.copyFileTo (dest))
        {
            problems.add (f.getFileName() + " couldn't be copied in.");
            continue;
        }
        imported.add (dest.getFileNameWithoutExtension());
    }
    return imported;
}

bool ClickSamples::remove (const juce::String& name)
{
    auto f = fileFor (name);
    return f.existsAsFile() && f.deleteFile();
}

std::shared_ptr<const ClickSample> ClickSamples::load (const juce::String& name, double rate)
{
    if (rate <= 0) return {};
    const bool builtIn = builtInNames().contains (name);
    auto file = builtIn ? juce::File() : fileFor (name);
    if (! builtIn && ! file.existsAsFile()) return {};
    const auto key = (builtIn ? "built in|" + name : file.getFullPathName() + "|" + juce::String (file.getLastModificationTime().toMilliseconds()))
                     + "|" + juce::String (rate);
    {
        std::lock_guard<std::mutex> g (lock);
        if (auto it = cache.find (key); it != cache.end()) return it->second;
    }

    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r;
    if (builtIn)
    {
        for (int i = 0; i < ClickSoundsData::namedResourceListSize; ++i)
            if (juce::String (ClickSoundsData::originalFilenames[i]) == name + ".wav")
            {
                int size = 0;
                if (auto* data = ClickSoundsData::getNamedResource (ClickSoundsData::namedResourceList[i], size))
                    r.reset (fm.createReaderFor (std::make_unique<juce::MemoryInputStream> (data, (size_t) size, false)));
            }
    }
    else
        r.reset (fm.createReaderFor (file));
    if (r == nullptr || r->lengthInSamples <= 0 || r->sampleRate <= 0) return {};

    // Read up to two seconds plus whatever silence leads in.
    const int maxIn = (int) std::min<juce::int64> (r->lengthInSamples, (juce::int64) (r->sampleRate * 3.0));
    juce::AudioBuffer<float> in (2, maxIn);
    in.clear();
    r->read (&in, 0, maxIn, 0, true, r->numChannels > 1);
    if (r->numChannels == 1) in.copyFrom (1, 0, in, 0, 0, maxIn);

    // Trim the silence in front: from 1 ms before the first sound above -50 dB.
    int first = 0;
    while (first < maxIn && std::abs (in.getSample (0, first)) < 0.00316f && std::abs (in.getSample (1, first)) < 0.00316f) ++first;
    if (first >= maxIn) return {};   // all silence
    first = std::max (0, first - (int) (r->sampleRate * 0.00025));   // a quarter millisecond, to keep the attack's rise
    const int lengthIn = std::min (maxIn - first, (int) (r->sampleRate * 2.0));

    // To the session's rate.
    const double ratio = r->sampleRate / rate;
    const int lengthOut = std::max (1, (int) std::ceil (lengthIn / ratio));
    auto s = std::make_shared<ClickSample>();
    s->sampleRate = rate;
    s->audio.setSize (2, lengthOut);
    for (int c = 0; c < 2; ++c)
    {
        if (std::abs (ratio - 1.0) < 1.0e-9)
            s->audio.copyFrom (c, 0, in, c, first, std::min (lengthIn, lengthOut));
        else
        {
            juce::LagrangeInterpolator interp;
            juce::AudioBuffer<float> padded (1, lengthIn + 8);
            padded.clear();
            padded.copyFrom (0, 0, in, c, first, lengthIn);
            interp.process (ratio, padded.getReadPointer (0), s->audio.getWritePointer (c), lengthOut, padded.getNumSamples(), 0);
        }
    }

    // A short fade at the end, so a cut never clicks.
    const int fade = std::min (lengthOut, (int) (rate * 0.01));
    for (int c = 0; c < 2; ++c) s->audio.applyGainRamp (c, lengthOut - fade, fade, 1.0f, 0.0f);

    // The built-in sounds peak around 0.6 to 0.8.
    const float peak = s->audio.getMagnitude (0, lengthOut);
    if (peak > 0.0f) s->audio.applyGain (0.7f / peak);

    std::lock_guard<std::mutex> g (lock);
    return cache.emplace (key, s).first->second;
}

} // namespace earfirst
