#!/bin/sh
# Embeds Sparkle into the app and signs everything, inside out. Never --deep:
# it breaks notarization. Usage: sign_mac.sh <identity> <app> <entitlements> <sparkle dir>
set -e
ID="$1"; APP="$2"; ENT="$3"; SPARKLE="$4"
FW="$APP/Contents/Frameworks"
mkdir -p "$FW"
rm -rf "$FW/Sparkle.framework"
ditto "$SPARKLE/Sparkle.framework" "$FW/Sparkle.framework"
# Not sandboxed, so the XPC services aren't needed (Sparkle's sandboxing guide).
rm -rf "$FW/Sparkle.framework/Versions/B/XPCServices" "$FW/Sparkle.framework/XPCServices"
TS="--timestamp=none"
codesign --force $TS --options runtime --sign "$ID" "$FW/Sparkle.framework/Versions/B/Autoupdate"
codesign --force $TS --options runtime --sign "$ID" "$FW/Sparkle.framework/Versions/B/Updater.app"
codesign --force $TS --options runtime --sign "$ID" "$FW/Sparkle.framework"
codesign --force $TS --options runtime --entitlements "$ENT" --sign "$ID" "$APP"
