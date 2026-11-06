// test-keyboard: a pretend MIDI keyboard for testing without one. It appears
// as a MIDI device called "Ear First Test Keyboard" and plays an exact
// pattern through the system's MIDI driver, so the receiving app sees real
// driver timestamps.
//
//   test-keyboard [seconds to wait first]
//
// The pattern, from the moment it starts: sustain pedal down at 0; middle C
// at 1.0 for exactly 0.5 s; E at 2.0 for exactly 1.0 s; pedal up at 3.5;
// G at 4.0 for exactly 0.25 s. Each event is sent with a busy wait, so it
// leaves within a fraction of a millisecond of its time.

#include <juce_audio_devices/juce_audio_devices.h>
#include <iostream>

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    const double wait = argc > 1 ? juce::String (argv[1]).getDoubleValue() : 6.0;

    auto out = juce::MidiOutput::createNewDevice ("Ear First Test Keyboard");
    if (out == nullptr) { std::cout << "Couldn't make the test keyboard. Virtual MIDI devices need macOS or Linux." << std::endl; return 1; }
    std::cout << "Ear First Test Keyboard is ready. Playing in " << wait << " seconds." << std::endl;

    const double startMs = juce::Time::getMillisecondCounterHiRes() + wait * 1000.0;
    struct Ev { double at; juce::MidiMessage m; const char* what; };
    const Ev pattern[] = {
        { 0.0,  juce::MidiMessage::controllerEvent (1, 64, 127), "pedal down" },
        { 1.0,  juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), "middle C on" },
        { 1.5,  juce::MidiMessage::noteOff (1, 60), "middle C off" },
        { 2.0,  juce::MidiMessage::noteOn (1, 64, (juce::uint8) 90), "E on" },
        { 3.0,  juce::MidiMessage::noteOff (1, 64), "E off" },
        { 3.5,  juce::MidiMessage::controllerEvent (1, 64, 0), "pedal up" },
        { 4.0,  juce::MidiMessage::noteOn (1, 67, (juce::uint8) 80), "G on" },
        { 4.25, juce::MidiMessage::noteOff (1, 67), "G off" },
    };
    for (auto& e : pattern)
    {
        const double due = startMs + e.at * 1000.0;
        while (juce::Time::getMillisecondCounterHiRes() < due - 2.0) juce::Thread::sleep (1);
        while (juce::Time::getMillisecondCounterHiRes() < due) {}
        out->sendMessageNow (e.m);
        std::cout << "Sent " << e.what << ", " << juce::String (juce::Time::getMillisecondCounterHiRes() - due, 3) << " ms late." << std::endl;
    }
    juce::Thread::sleep (1500);
    std::cout << "Done." << std::endl;
    return 0;
}
