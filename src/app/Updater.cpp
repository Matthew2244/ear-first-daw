#include "app/Updater.h"

namespace earfirst
{

std::unique_ptr<Updater> Updater::createForThisPlatform (const juce::String& feedUrl)
{
    // Sparkle and WinSparkle plug in here once there's a published release
    // feed to check. Until then, say so honestly.
    juce::ignoreUnused (feedUrl);
    return std::make_unique<NoUpdater> ("Updates aren't set up yet, because there's no published release to update from. This is version "
                                        + juce::String (JUCE_APPLICATION_VERSION_STRING) + ".");
}

} // namespace earfirst
