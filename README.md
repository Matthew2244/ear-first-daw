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

## capture-engine-test

The capture recorder inside the Tracktion engine. `EngineCapture` wraps the
engine's audio callback: each block it reads every input untouched, then hands
the same buffers to the engine, so the two can never overwrite each other.
`TimelineMap` notes where the song was for each block (only storing changes),
and keep puts each input on its own track, lined up with where it was played.
The yardstick is the engine's own normal recording of the same click.

    build/capture-engine-test_artefacts/Release/capture-engine-test [block size]

Covers: song playing from the start and from 30 seconds, playing before the
song starts, song never played (goes to the playhead), 64 inputs, and
looping (every pass kept, the latest full pass on the tracks, the rest kept
for comping, wraps noted to the sample even mid-block). Passes at block sizes
64, 256, 1000, 1024 and 4096. Not yet covered: a real interface's latency.

## commands-test

The command list (`src/commands/Commands.*`) and keymap (`src/commands/Keymap.*`).
Every action is one command with a stable id (`transport.play`), typed
settings, and its own spoken result; toggles say their new state, a command
that can't run says why, macros speak once and undo as one step, and repeat
reruns the last change with its settings. Keys are written once for both
systems (`Primary+Shift+K` = Command on the Mac, Control on Windows), work in
scopes that fall through to global, can be two-key sequences, and are stored
as your changes over versioned defaults, so a new version never moves your
keys and a moved default is announced once. Keys VoiceOver, NVDA, JAWS or the
system already use are flagged. Research behind the design is in iCloud, Ear
First DAW.

    build/commands-test_artefacts/Release/commands-test
