#include "app/Updater.h"

namespace earfirst
{
#if EARFIRST_HAS_SPARKLE
std::unique_ptr<Updater> makeSparkleUpdater();
#endif

std::unique_ptr<Updater> Updater::createForThisPlatform (const juce::String& feedUrl)
{
    juce::ignoreUnused (feedUrl);
   #if EARFIRST_HAS_SPARKLE
    return makeSparkleUpdater();
   #endif
    // Windows (WinSparkle) comes with the first Windows installer; the
    // self-test has no updater. Until then, say so honestly.
    return std::make_unique<NoUpdater> ("Updates aren't set up in this copy yet. This is version "
                                        + juce::String (JUCE_APPLICATION_VERSION_STRING) + ".");
}

} // namespace earfirst
