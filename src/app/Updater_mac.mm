// Sparkle on the Mac. The quiet check never shows anything: it reports
// through Updater::onUpdateFound, and the app plays its sound and says one
// sentence. Check for Updates opens Sparkle's own window, which reads well
// with VoiceOver. An update never relaunches the app while recording or with
// unsaved work; it waits until that's no longer true.
#import <AppKit/AppKit.h>
#import <Sparkle/Sparkle.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include "app/Updater.h"

namespace earfirst { class SparkleUpdater; }

@interface EFSparkleDelegate : NSObject <SPUUpdaterDelegate>
{
@public
    earfirst::Updater* owner;
    void (^pendingRelaunch)(void);
}
@end

@implementation EFSparkleDelegate
- (NSString*) feedURLStringForUpdater: (SPUUpdater*) updater
{
    // A local test feed, for rehearsing an update without publishing one.
    if (const char* test = getenv ("EARFIRST_TEST_FEED"))
        return [NSString stringWithUTF8String: test];
    return nil;
}

- (void) updater: (SPUUpdater*) updater didFindValidUpdate: (SUAppcastItem*) item
{
    auto version = juce::String::fromUTF8 ([[item displayVersionString] UTF8String]);
    auto* o = owner;
    juce::MessageManager::callAsync ([o, version] { if (o != nullptr && o->onUpdateFound) o->onUpdateFound (version); });
}

- (BOOL) updater: (SPUUpdater*) updater shouldPostponeRelaunchForUpdate: (SUAppcastItem*) item untilInvokingBlock: (void (^)(void)) installHandler
{
    if (owner == nullptr || owner->isSafeToInstall == nullptr || owner->isSafeToInstall())
        return NO;
    // Recording or unsaved work: wait, and look again every few seconds.
    pendingRelaunch = [installHandler copy];
    [self performSelector: @selector (retryRelaunch) withObject: nil afterDelay: 5.0];
    return YES;
}

- (void) retryRelaunch
{
    if (pendingRelaunch == nil) return;
    if (owner != nullptr && owner->isSafeToInstall != nullptr && ! owner->isSafeToInstall())
    {
        [self performSelector: @selector (retryRelaunch) withObject: nil afterDelay: 5.0];
        return;
    }
    auto block = pendingRelaunch;
    pendingRelaunch = nil;
    block();
}
@end

namespace earfirst
{
class SparkleUpdater : public Updater
{
public:
    SparkleUpdater()
    {
        delegate = [[EFSparkleDelegate alloc] init];
        delegate->owner = this;
        controller = [[SPUStandardUpdaterController alloc] initWithStartingUpdater: YES
                                                                   updaterDelegate: delegate
                                                                userDriverDelegate: nil];
        controller.updater.automaticallyChecksForUpdates = NO;
    }

    ~SparkleUpdater() override
    {
        delegate->owner = nullptr;
    }

    void checkQuietly() override
    {
        if (controller.updater.canCheckForUpdates)
            [controller.updater checkForUpdateInformation];
    }

    juce::String checkWithWindow() override
    {
        [controller checkForUpdates: nil];
        return "Checking for updates. This is version " + juce::String (JUCE_APPLICATION_VERSION_STRING) + ".";
    }

    juce::String whyNotAvailable() const override   { return {}; }

private:
    EFSparkleDelegate* delegate = nil;
    SPUStandardUpdaterController* controller = nil;
};

std::unique_ptr<Updater> makeSparkleUpdater()   { return std::make_unique<SparkleUpdater>(); }
}
