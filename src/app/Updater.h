#pragma once
// Updater: how the app finds and installs new versions.
//
// The house pattern from Wavelength and MIDI Tempo Fix: the updater never
// pops up by itself. The app checks quietly every few hours; when a new
// version is out it plays the update sound and says one sentence, once per
// version, and only when the app is in front with no dialog open. "Check for
// Updates" is a command like any other; it opens the updater's own window,
// which reads well with a screen reader. An update is never installed while
// recording, capturing something you're about to keep, or with unsaved work.
//
// Mac: Sparkle. Windows: WinSparkle, with the app reading the feed itself
// for the quiet check, since WinSparkle has no silent check.

#include <juce_core/juce_core.h>
#include <functional>

namespace earfirst
{

class Updater
{
public:
    virtual ~Updater() = default;

    // Quiet check: reports through onUpdateFound, shows nothing.
    virtual void checkQuietly() = 0;
    // The user asked: show the updater's own window. Returns what to say first.
    virtual juce::String checkWithWindow() = 0;
    // Is updating set up at all? If not, why not, in one sentence.
    virtual juce::String whyNotAvailable() const = 0;

    std::function<void (const juce::String& version)> onUpdateFound;
    std::function<bool()> isSafeToInstall;   // false while recording or with unsaved work

    static std::unique_ptr<Updater> createForThisPlatform (const juce::String& feedUrl);
};

// Used until there's a release feed to check, and in tests.
class NoUpdater : public Updater
{
public:
    explicit NoUpdater (juce::String reason) : why (std::move (reason)) {}
    void checkQuietly() override {}
    juce::String checkWithWindow() override     { return why; }
    juce::String whyNotAvailable() const override { return why; }
private:
    juce::String why;
};

} // namespace earfirst
