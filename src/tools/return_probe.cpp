// return-probe: plays Ear First's soft sweep through an interface and says
// which of its inputs carry the computer's own sound.
//
//   return-probe "<interface name>"

#include <juce_audio_devices/juce_audio_devices.h>
#include "capture/ReturnFinder.h"
#include <iostream>

using namespace earfirst;

struct Probe : juce::AudioIODeviceCallback
{
    std::vector<float> sweep, outLog;
    std::vector<std::vector<float>> inLog;
    std::atomic<int64_t> pos { 0 };
    int64_t total = 0, sweepAt = 0;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override {}
    void audioDeviceStopped() override {}
    void audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut, int n,
                                           const juce::AudioIODeviceCallbackContext&) override
    {
        const auto p = pos.load();
        for (int i = 0; i < n; ++i)
        {
            const auto t = p + i;
            const float s = (t >= sweepAt && t - sweepAt < (int64_t) sweep.size()) ? sweep[(size_t) (t - sweepAt)] : 0.0f;
            for (int c = 0; c < numOut; ++c) if (out[c]) out[c][i] = c < 2 ? s : 0.0f;
            if (t < total)
            {
                outLog[(size_t) t] = s;
                for (int c = 0; c < numIn && c < (int) inLog.size(); ++c) inLog[(size_t) c][(size_t) t] = in[c] ? in[c][i] : 0.0f;
            }
        }
        pos = p + n;
    }
};

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    if (argc < 2) { std::cout << "Say which interface: return-probe \"StudioLive 64S\"" << std::endl; return 2; }
    juce::AudioDeviceManager dm;
    juce::AudioDeviceManager::AudioDeviceSetup s;
    s.inputDeviceName = s.outputDeviceName = argv[1];
    s.useDefaultInputChannels = s.useDefaultOutputChannels = false;
    s.inputChannels.setRange (0, 256, true);
    s.outputChannels.setRange (0, 2, true);
    auto err = dm.initialise (256, 2, nullptr, false, {}, &s);
    if (err.isEmpty()) err = dm.setAudioDeviceSetup (s, true);
    auto* dev = dm.getCurrentAudioDevice();
    if (dev == nullptr || err.isNotEmpty()) { std::cout << "Couldn't open " << argv[1] << ": " << err << std::endl; return 1; }
    const double sr = dev->getCurrentSampleRate();
    const auto names = dev->getInputChannelNames();
    const auto active = dev->getActiveInputChannels();
    Probe p;
    p.sweep = makeSweep (sr);
    p.sweepAt = (int64_t) (sr * 0.5);
    p.total = (int64_t) (sr * 2.0);
    p.outLog.assign ((size_t) p.total, 0.0f);
    p.inLog.assign ((size_t) active.countNumberOfSetBits(), std::vector<float> ((size_t) p.total, 0.0f));
    dm.addAudioCallback (&p);
    while (p.pos.load() < p.total) juce::Thread::sleep (50);
    dm.removeAudioCallback (&p);

    const int len = 32768, maxLag = 8192;
    const int start = (int) p.sweepAt - 2000;   // the window: the sweep and a little either side
    std::cout << "Played a sweep on outputs 1 and 2 of " << dev->getName() << " at " << sr << " Hz." << std::endl;
    int c = 0, found = 0;
    for (int i = 0; i < names.size(); ++i)
    {
        if (! active[i]) continue;
        auto r = checkReturn (p.outLog.data() + start - maxLag, p.inLog[(size_t) c].data() + start, len, maxLag, sr);
        if (r.enoughSignal)
            std::cout << names[i] << ": " << (r.isReturn ? "COMPUTER'S OWN SOUND" : "not a return")
                      << " (correlation " << juce::String (r.correlation, 3) << ", spread " << juce::String (r.spreadDb, 1)
                      << " dB over " << r.bands << " bands, " << juce::String (r.lag * 1000.0 / sr, 2) << " ms late)" << std::endl;
        found += r.isReturn ? 1 : 0;
        ++c;
    }
    std::cout << found << " of " << c << " inputs carry the computer's own sound. Inputs with nothing to judge aren't listed." << std::endl;
    return 0;
}
