# Ear First: handoff

Read this first in a new session. It describes where the project stands now. The README says what the app does and how to build it.

Ear First is an ear-first, accessibility-first DAW on JUCE and Tracktion Engine, for Mac and Windows. The repo is public under AGPL-3.0, so nothing personal goes in it: no addresses, credentials or people's private details.

## Working rules

- Every action is a command in one list (`src/app/AppCore.cpp`, `registerCommands`). Menus, keys, MIDI controls, Find a command and `--run` all go through it. A new feature is a new command.
- Everything spoken goes through `say` or `CommandResult`, with a kind, so people can choose how much they hear. A short version is given where it helps.
- Nothing may block, lock or allocate on the audio thread.
- Never capped: tracks, inputs, takes, capture length. Only the machine limits, and it warns by ear first.
- Works by ear and by eye, for everyone. Nothing personal is hardcoded: not everyone has the same interface.
- Sounds are chosen by Matthew's ear. When a measurement disagrees with his ear, the measurement is wrong.
- Say what a sound is before playing it.
- Tests must never touch real settings. `Options.saveSettings=false` and `TempPropertyStorage` exist for this.
- After every push, watch CI until it's green on Mac and Windows. A newer push cancels the run before it.

## What's built

- **Capture** (`src/capture`). `CaptureRecorder` writes every input to disk all the time. `MidiCapture` keeps all MIDI on the same clock. `EngineCapture` wraps the engine's audio callback, maps capture time to the timeline (`TimelineMap`), and places a Keep on tracks. Long keeps export on a background thread. Crash leftovers are found at launch.
- **Quit.** Unkept capture stays on disk, and the next launch offers it ("From last time"), but only if a note was played or an input went from quiet to playing. The oldest leftover goes first when the disk runs low.
- **The computer's own sound** (`ReturnFinder`). Finds inputs that carry the computer's playback. A return has correlation above 0.9 and level within 3 dB across third-octave bands. The first time an interface is used, it announces and plays a soft sweep. It also notices while broadband sound plays, and can be run by hand. Those inputs never count as playing. An input you unmark is never marked again by itself. Saved per interface in App.json.
- **Commands, keys, speech** (`src/commands`, `src/speech`). Detail per kind: Full, Short, Sound, Off. Earcons go to the outputs only. MIDI controls run any command. Custom keys are stored apart from defaults.
- **Songs** (`src/session`). A song is a folder, with safe saves, autosave and recovery. A hidden idle Edit keeps MIDI alive with no song open.
- **Click** (`src/click`). On its own track, locked to tempo and meter; accents, subdivisions, swing, count-in. Sounds are the four recorded takes Matthew picked (`src/click/sounds`, embedded as binary data), plus the Beep, which is made in code. `ClickSamples` imports any audio file: trims the lead-in, resamples, matches level, and caches for the whole run so the audio thread never frees one. Defaults: Cowbell 1 on the one, Clave on beats, Hi-hat on subdivisions.
- **Routing.** Any input to any output (`src/capture/RouteMatrix.h`), mixed at the audio callback, with or without a song. Inputs are copied before the engine runs and mixed in last, after the "what went out" note, so a routed input is never taken for a computer return. An input marked as the computer's own sound is refused and never goes out, since that's a feedback loop. Commands: Send input to output (one channel or a neighbouring pair, with a level), Remove a route, Remove every route, What's routed. Saved per interface by channel name in App.json. Play track through picks a track's outputs through the engine, and is saved in the song.
  Checked on a 64-input interface with VoiceOver: all 64 outputs open, Play track through reads its choices and result, the forms speak each field and the result, the feedback refusal is spoken, and a routed input came back through the interface's computer return at a steady round trip, stronger with level and gone once removed. Not yet heard by a person, and Play track through hasn't been used with VoiceOver.
- **Command line.** `--run <command-id>` runs a command once the app is up. It's the way to do live checks without driving the window.
- **Mac app.** Signed with `EARFIRST_SIGN_IDENTITY` through `cmake/sign_mac.sh`, Sparkle embedded.

## Next

1. **Routing, the rest.** Keys for the routing commands, a spoken or on-screen matrix, sends, and a cue mix per player.
2. **Click sounds still open.** Matthew hasn't yet judged round two: five ticks and two woodblocks in iCloud, `Ear First DAW/Click candidates 2`. There's no good rim: 0 clean of 14 from ElevenLabs. A real recording or an import is the way.
3. From the README: grooves as the click, a spoken count, editing takes, mixing, plugins. The living list of ideas is iCloud `Ear First DAW/Feature list.md`.

## Not verified

- Import click sounds and Find the computer's own sound have not been used by a person with VoiceOver. The file chooser has not been exercised at all.
- `--run` handed to a copy that's already running: the second copy exits cleanly, but nothing has confirmed the running copy ran the command. Run the app's binary directly; `open -a` won't pass arguments to a running copy.
- Detecting a return by itself has only run in the engine test, not live with a song playing.
- Windows builds and passes CI, but nobody has tried it with NVDA or JAWS.

## Known weak spots

- Tracktion frees `EditPlaybackContext` on the message thread without the audio lock, so `noteTimeline` has a very small window where it could touch a freed context. It can't be fully closed from outside the engine.
- `MidiCapture::written` holds every MIDI event in memory for the run.
- A mic loud enough to hear the click or song through the speakers counts as playing, so quit keeps that session.
- `capture-engine-test 4096`: Test 6e (the sweep finds the return) fails at that block size, and failed the same way before routing. Every other size passes.
- Launch runs the audio interface checks and opens the device on the message thread before the event loop starts. On a 64-channel interface that's a couple of seconds with no window, long enough for VoiceOver to say the app isn't responding. Most of it is the saved-interface check and creating the device types; opening every output is the smaller part.
- Nobody has heard a route or Play track through by ear yet.
- The click is too narrow to judge a return by, so that check depends on the song or the sweep.

## Traps already paid for

- Settings load after capture starts, so anything read from App.json must be applied after `loadAppSettings`.
- Tracktion rescans MIDI every 4 seconds. A test keyboard must wait at least 8 seconds before playing.
- On fresh settings, the engine finding its MIDI devices stops the transport. Tests start playing after the engine settles (this failed on CI and passed locally).
- Primary+Alt+key is AltGr on Windows; the selftest catches key clashes.
- A StudioLive 64S can wedge coreaudiod. Startup probes the interface on a thread with a timeout, then falls back.
- ElevenLabs takes: ask for 0.5 seconds, because longer requests come back with hummy floors. What matched his ear: rings about 130 ms or less, floor at -73 dB or lower.

## Tests

`commands-test`, `speech-test`, `session-test`, `click-test`, `return-finder-test`, `capture-recorder-test`, `capture-engine-test`, `ear-first-selftest`, all in `build/<name>_artefacts/Release/`. CI runs them all. `return-probe "<interface>"` plays the sweep through a real interface and says which inputs are returns. `test-keyboard` is a pretend MIDI keyboard. `click-test listen <folder>` renders each click sound.
