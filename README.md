# Ear First DAW

The ear-first DAW Matthew and Faith are building. Right now this is one test program.

## capture-test

Checks Tracktion Engine's always-on capture (retrospective record): every input is kept, playing or stopped, and one command turns the last stretch into takes.

Build:

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build --target capture-test

Run:

    build/capture-test_artefacts/Release/capture-test selftest
    build/capture-test_artefacts/Release/capture-test live

`selftest` uses no microphone. It feeds a known click into the engine through Tracktion's hosted audio device and checks every take. `live` opens the real interface (macOS will ask for microphone access the first time) and takes typed commands: keep, status, quit. Takes go to `~/Music/Ear First Capture Test`.

Tracktion Engine is a git submodule in `external/tracktion_engine`, pinned at commit a5e4499 (24 October 2026), GPLv3 or commercial licence.

## capture-recorder-test

The disk-backed recorder in `src/capture/`: every input to disk for the whole
session, crash-safe raw segments, sample-exact keeps as stamped broadcast WAVs.
Run `build/capture-recorder-test_artefacts/Release/capture-recorder-test`; it
needs no microphone and cleans up after itself.
