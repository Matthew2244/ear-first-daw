// capture-live: the disk capture recorder on a real audio interface.
//
//   capture-live                  list the input devices
//   capture-live "<device name>"  record every input of that device (part of the name is enough)
//
// Then type, and press Return:
//   keep            keep the last 30 seconds of every input
//   keep 90         keep the last 90 seconds
//   status          where things stand
//   quit            stop
//
// Output is one plain sentence per line, written for a screen reader.

#include <JuceHeader.h>
#include "capture/CaptureRecorder.h"
#include <iostream>

namespace
{
void say (const juce::String& s)    { std::cout << s << std::endl; }

struct Feeder : juce::AudioIODeviceCallback
{
    earfirst::CaptureRecorder* rec = nullptr;
    std::vector<const float*> chunk;      // sized before audio starts; no allocation in the callback
    std::atomic<int64_t> callbacks { 0 }, refused { 0 };

    void audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut,
                                           int n, const juce::AudioIODeviceCallbackContext&) override
    {
        for (int c = 0; c < numOut; ++c)
            if (out[c] != nullptr) juce::FloatVectorOperations::clear (out[c], n);
        callbacks.fetch_add (1, std::memory_order_relaxed);
        if (rec == nullptr || numIn == 0) return;

        // The recorder takes at most maxBlockSize at once; split anything bigger.
        for (int done = 0; done < n;)
        {
            const int len = std::min (n - done, 4096);
            const int nc = std::min (numIn, (int) chunk.size());
            for (int c = 0; c < nc; ++c) chunk[(size_t) c] = in[c] + done;
            if (! rec->push (chunk.data(), len)) refused.fetch_add (1, std::memory_order_relaxed);
            done += len;
        }
    }
    void audioDeviceAboutToStart (juce::AudioIODevice*) override {}
    void audioDeviceStopped() override {}
};

juce::String dbText (float peak)
{
    if (peak <= 0.0f) return "silent";
    auto db = juce::Decibels::gainToDecibels (peak);
    return "peaks at " + juce::String (juce::roundToInt (db)) + " dB";
}
} // namespace

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI init;
    juce::AudioDeviceManager dm;
    dm.initialise (0, 0, nullptr, false);

    auto* type = dm.getCurrentDeviceTypeObject();
    if (type == nullptr) { say ("No audio system found."); return 1; }
    type->scanForDevices();
    auto names = type->getDeviceNames (true);

    if (argc < 2)
    {
        say ("Input devices:");
        for (auto& n : names) say ("  " + n);
        say ("Run again with one of those names, or part of it.");
        return 0;
    }

    juce::String want (argv[1]), chosen;
    for (auto& n : names)
        if (n.containsIgnoreCase (want)) { chosen = n; break; }
    if (chosen.isEmpty()) { say ("No input device matches " + want + "."); return 1; }

    // Open it as input only, every channel on.
    juce::AudioDeviceManager::AudioDeviceSetup setup;
    dm.getAudioDeviceSetup (setup);
    setup.inputDeviceName = chosen;
    setup.outputDeviceName = {};
    setup.useDefaultInputChannels = false;
    setup.inputChannels.setRange (0, 4096, true);   // every channel the device has
    setup.useDefaultOutputChannels = false;
    setup.outputChannels.clear();
    auto err = dm.setAudioDeviceSetup (setup, true);
    auto* dev = dm.getCurrentAudioDevice();
    if (err.isNotEmpty() || dev == nullptr)
    {
        say ("Could not open " + chosen + ": " + (err.isNotEmpty() ? err : juce::String ("no device")) + ".");
        return 1;
    }

    auto chanNames = dev->getInputChannelNames();
    auto active = dev->getActiveInputChannels();
    juce::StringArray inputNames;
    for (int i = 0; i < chanNames.size(); ++i)
        if (active[i]) inputNames.add (chanNames[i].isNotEmpty() ? chanNames[i] : "Input " + juce::String (i + 1));
    if (inputNames.isEmpty())
    {
        say ("The device opened, but no inputs came through. Microphone access may be off for this app.");
        return 1;
    }

    const double rate = dev->getCurrentSampleRate();
    auto captureFolder = juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                             .getChildFile ("Ear First Capture")
                             .getChildFile (juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S"));
    auto takesFolder = juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                           .getChildFile ("Library/Mobile Documents/com~apple~CloudDocs/Ear First DAW/Live test takes");
    takesFolder.createDirectory();

    earfirst::CaptureRecorder::Options opt;
    opt.folder = captureFolder;
    opt.sampleRate = rate;
    opt.inputNames = inputNames;
    earfirst::CaptureRecorder rec (opt);

    Feeder feeder;
    feeder.rec = &rec;
    feeder.chunk.resize ((size_t) inputNames.size());
    const int xrunsAtStart = dev->getXRunCount();
    dm.addAudioCallback (&feeder);

    say ("Capturing " + juce::String (inputNames.size()) + " inputs on " + chosen + " at "
         + juce::String (juce::roundToInt (rate)) + " hertz, buffer " + juce::String (dev->getCurrentBufferSizeSamples())
         + " samples, " + juce::String (rec.getWriterThreadCount()) + " writer threads.");
    say ("Type keep, keep and a number of seconds, status, or quit, then press Return.");

    juce::Array<int> all;
    for (int i = 0; i < inputNames.size(); ++i) all.add (i);
    int takeNumber = 0;

    for (std::string line; std::getline (std::cin, line);)
    {
        auto cmd = juce::String (line).trim().toLowerCase();
        if (cmd.isEmpty()) continue;
        if (cmd == "quit" || cmd == "q") break;

        if (cmd == "status" || cmd == "s")
        {
            say (rec.status());
            const int xr = dev->getXRunCount();
            say (feeder.callbacks.load() == 0 ? juce::String ("No audio has arrived from the device yet.")
                 : xr > xrunsAtStart ? "The device reports " + juce::String (xr - xrunsAtStart) + " dropouts of its own."
                                     : juce::String ("The device reports no dropouts."));
            continue;
        }

        if (cmd.startsWith ("keep") || cmd.startsWith ("k"))
        {
            double secs = cmd.fromFirstOccurrenceOf (" ", false, false).getDoubleValue();
            if (secs <= 0) secs = 30;
            auto name = "Take " + juce::String (++takeNumber) + " " + juce::Time::getCurrentTime().formatted ("%H-%M-%S");
            auto t0 = juce::Time::getMillisecondCounterHiRes();
            auto r = rec.keepLast (secs, all, takesFolder, name);
            auto ms = juce::Time::getMillisecondCounterHiRes() - t0;
            if (! r.ok()) { say (r.error); continue; }

            say ("Kept " + juce::String ((double) (r.end - r.start) / rate, 1) + " seconds on "
                 + juce::String (r.files.size()) + " inputs in " + juce::String (juce::roundToInt (ms)) + " milliseconds.");
            if (r.note.isNotEmpty()) say (r.note);

            // Read each file back so we know real sound landed, not silence.
            juce::AudioFormatManager fm;
            fm.registerBasicFormats();
            int silent = 0;
            juce::StringArray loud;
            for (int i = 0; i < r.files.size(); ++i)
            {
                std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (r.files[i]));
                if (rd == nullptr) { say ("Could not read back " + r.files[i].getFileName() + "."); continue; }
                juce::Range<float> range[1];
                rd->readMaxLevels (0, rd->lengthInSamples, range, 1);
                float peak = std::max (std::abs (range[0].getStart()), std::abs (range[0].getEnd()));
                if (peak <= 0.0f) ++silent;
                else loud.add (inputNames[i] + " " + dbText (peak));
            }
            if (loud.isEmpty()) say ("Every input in that take is silent.");
            else
            {
                for (auto& l : loud) say (l + ".");
                if (silent > 0) say (juce::String (silent) + (silent == 1 ? " other input is" : " other inputs are") + " silent.");
            }
            say ("Saved in Ear First DAW, Live test takes.");
            continue;
        }
        say ("Commands are keep, status and quit.");
    }

    dm.removeAudioCallback (&feeder);
    rec.stop();
    say (rec.status());
    say ("Stopped. The raw capture is deleted; kept takes stay in Live test takes.");
    return 0;
}
