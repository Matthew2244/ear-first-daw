# Ear First

A DAW built to be used by ear first, for blind and sighted musicians alike. It's early: the foundation is in place and being tested, and it isn't ready for real sessions yet.

The idea: every input you have is recorded all the time, whether the song is playing or not. When you play something you like, press K and it becomes a take, lined up on the timeline where you played it. Everything the app does is a named command you can reach from a key, a menu, a search, or a control surface, and the app tells you what happened in a short sentence through your screen reader.

## What works now

- **Always-on capture.** Every input goes to disk for the whole session, with no time limit but the disk. Tested at real speed from 1 to 1,024 inputs with nothing dropped, and for an hour on a 64-input interface. Takes come back sample-exact, as 24-bit broadcast WAVs stamped with their position. It survives a crash: audio captured but never kept is found on the next launch. Quitting does the same when you'd played something: a note, or an input going from quiet to sound. Inputs that carry the computer's own sound never count as playing. Ear First finds them itself, on any interface: the first time it opens one it plays a short, soft sweep (after saying so) and marks the inputs that carry it straight back, and it keeps noticing while the song plays. A mic hearing the speakers isn't mistaken for one, because the room colors it. Find the computer's own sound and Computer's own sound, in the Audio menu, do it by hand. When the disk runs low the oldest leftover goes first.
- **Takes on the timeline.** A kept take lands on one track per input, where you played it, matched to the sample against the engine's own recording. That holds when you start playing before the song does and when the song loops: every pass is kept and the latest full one goes on the tracks.
- **One list of commands.** Each action has a stable name, takes settings (nudge by how much), and speaks its result. Toggles say their new state. A command that can't run says why. Macros speak once and undo as one step.
- **Keys.** One keymap for Mac and Windows, written once (Primary is Command on the Mac and Control on Windows). Keys can work in one place, such as the track list, and fall through to the main window. Your own keys are stored apart from the defaults, so an update never moves them, and a moved default is announced once. Keys VoiceOver, NVDA, JAWS or the system already use are flagged.
- **Speech.** Urgent messages cut in. The rest wait for a gap in the music, and never longer than a few seconds. Repeated updates on the same thing replace each other. You can step back through everything that's been said.
- **Songs.** A song is a folder. Saving can't leave a half-written file, songs autosave, and after a crash the next open says what it found and recovered.
- **The app.** A window whose menus come from the command list, key help (Command-slash: press a key to hear what it does), Find a command (Command-Shift-P), and announcements through VoiceOver on the Mac. Choose the interface you record from and play through, with the sample rate and buffer. Any command with settings gets a form. The window shows the song, the transport and capture at a glance, with bigger and smaller text.
- **What's said, your way.** Every message has a kind (Transport, Capture, Click, File, Errors, Answers and more). For each, choose Full, Short or Off, and whether places are said as bars and beats, time, or both. Don't say that kind turns off whatever was just said. Errors and answers you ask for never go fully silent, and the speech history keeps every message in full.
- **MIDI controls.** Any pedal, footswitch, pad, key or button on a MIDI device can run any command: choose the command, press the control.
- **The click.** Its own track, never in a mix, locked to the song's tempo and meter to the sample: different sounds for the one, the beats and subdivisions; any beat loud, normal, soft or silent; eighths, triplets and sixteenths with swing; odd meters; a count-in of one or two bars, and click only during the count-in if you want. One key says how it's set. Tempo and time signature are commands like everything else.
- **MIDI too.** Every MIDI input is kept all the time on the same clock as the audio, so a keep gets both, lined up, with the sustain pedal where it was. MIDI is kept even with no audio inputs open.
- **Updates.** On the Mac, Sparkle looks quietly every few hours and, when a new version is out, the app says so once, in one sentence. Check for Updates opens Sparkle's own window. An update never restarts the app while you're recording or have unsaved work.

## Not done yet

Windows builds in CI but hasn't been tried with NVDA or JAWS, and Windows updates (WinSparkle) come with the first Windows installer. Grooves as the click, a spoken count, editing takes, mixing and plugins are still to come.

## Building

You need CMake, Ninja and a C++20 compiler (Xcode on the Mac, Visual Studio on Windows).

    git clone --recursive https://github.com/Matthew2244/ear-first-daw.git
    cd ear-first-daw
    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build

On the Mac, sign the app with your own Developer ID so macOS remembers the microphone permission between builds:

    cmake -S . -B build -DEARFIRST_SIGN_IDENTITY="Developer ID Application: Your Name (TEAMID)"

## Tests

Every part has a test program that prints one plain sentence per line, so it reads well with a screen reader. None of them uses a microphone.

- `ear-first-selftest`: the app's own window code, menus, keys, key help and Find a command, driven without showing anything.
- `capture-recorder-test`: the disk recorder. `capture-recorder-test scale` runs 128 to 1,024 inputs at real speed.
- `capture-engine-test [block size]`: takes landing on the timeline, including loops, checked against the engine's own recording.
- `click-test`: the click's timing on its own and in the engine. `click-test listen <folder>` records each click sound for choosing by ear.
- `test-keyboard`: a pretend MIDI keyboard that plays an exact pattern through the system's MIDI driver.
- `commands-test`, `speech-test`, `session-test`: the command list and keymap, the speaking layer, and saving and crash recovery.
- `capture-live "<device name>"`: the recorder on a real interface. Type keep, status or quit.

## Built with

[Tracktion Engine](https://github.com/Tracktion/tracktion_engine) and [JUCE](https://juce.com).

## Licence

Ear First is free software under the GNU Affero General Public License, version 3. See [LICENSE](LICENSE). Contributions are welcome; see [CONTRIBUTING.md](CONTRIBUTING.md).
