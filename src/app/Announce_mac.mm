// VoiceOver ignores announcements sent from a window that doesn't have focus
// (found 6 November 2026: JUCE sends from the main window, so nothing said
// while the Find a command dialog had focus was heard). Send from whichever
// window has focus instead. Plain AppKit, no JUCE headers (their names clash).
#import <AppKit/AppKit.h>

namespace earfirst
{
void announceFromFocusedWindowUtf8 (const char* text, bool high)
{
    // The focused element first: VoiceOver reliably speaks what's sent from
    // where its focus is.
    id source = [NSApp accessibilityFocusedUIElement];
    if (source == nil) source = [NSApp keyWindow];
    if (source == nil) source = [NSApp mainWindow];
    if (source == nil) source = NSApp;
    NSString* s = [NSString stringWithUTF8String: text];
    if (const char* log = getenv ("EARFIRST_KEYLOG"))
        if (FILE* f = fopen (log, "a")) { fprintf (f, "announce from %s: %s\n", [NSStringFromClass ([source class]) UTF8String], text); fclose (f); }
    if (s == nil) return;
    NSAccessibilityPostNotificationWithUserInfo (source, NSAccessibilityAnnouncementRequestedNotification,
        @{ NSAccessibilityAnnouncementKey: s,
           NSAccessibilityPriorityKey: @(high ? NSAccessibilityPriorityHigh : NSAccessibilityPriorityMedium) });
}
}
