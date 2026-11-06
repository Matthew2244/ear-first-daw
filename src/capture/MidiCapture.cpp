#include "capture/MidiCapture.h"
#include <map>

namespace earfirst
{

MidiCapture::MidiCapture (juce::File f, double rate, bool del)
    : folder (std::move (f)), sampleRate (rate), deleteOnClose (del)
{
    folder.createDirectory();
    writeIndex();
    writer = std::thread ([this] { writerLoop(); });
}

MidiCapture::MidiCapture (juce::File f) : folder (std::move (f)), deleteOnClose (false) {}

MidiCapture::~MidiCapture()
{
    if (writer.joinable())
    {
        running = false;
        wake.notify_all();
        writer.join();
        flushNow();
    }
    if (deleteOnClose)
        folder.deleteRecursively();
}

juce::File MidiCapture::fileFor (const juce::String& device) const
{
    return folder.getChildFile ("device-" + juce::String (deviceNames.indexOf (device)) + ".midi");
}

void MidiCapture::writeIndex() const
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("sampleRate", sampleRate);
    juce::Array<juce::var> names;
    for (auto& n : deviceNames) names.add (n);
    o->setProperty ("devices", names);
    folder.getChildFile ("midi.json").replaceWithText (juce::JSON::toString (juce::var (o)));
}

void MidiCapture::add (const juce::String& device, int64_t sample, const juce::MidiMessage& m)
{
    if (m.isActiveSense() || m.isMidiClock()) return;   // timing chatter, not playing
    bool newDevice = false;
    {
        std::lock_guard<std::mutex> g (lock);
        int index = deviceNames.indexOf (device);
        if (index < 0) { deviceNames.add (device); index = deviceNames.size() - 1; newDevice = true; }
        pending.push_back ({ index, Event { sample, m } });
        written[index].push_back (Event { sample, m });
        if (newDevice) writeIndex();   // under the lock: two devices starting at once can't race
    }
    ++count;
}

void MidiCapture::writerLoop()
{
    while (running)
    {
        {
            std::unique_lock<std::mutex> g (wakeLock);
            wake.wait_for (g, std::chrono::milliseconds (250));
        }
        flushNow();
    }
}

void MidiCapture::flushNow()
{
    // The writer thread and a keep can both flush; two writers on one file
    // would overwrite each other's records.
    std::lock_guard<std::mutex> one (flushLock);
    std::vector<std::pair<int, Event>> batch;
    juce::StringArray names;
    {
        std::lock_guard<std::mutex> g (lock);
        batch.swap (pending);
        names = deviceNames;
    }
    if (batch.empty()) return;

    std::map<int, juce::MemoryOutputStream> perDevice;
    for (auto& [index, e] : batch)
    {
        auto& out = perDevice[index];
        out.writeInt64 (e.sample);
        out.writeShort ((short) e.message.getRawDataSize());
        out.write (e.message.getRawData(), (size_t) e.message.getRawDataSize());
    }
    for (auto& [index, data] : perDevice)
    {
        auto f = folder.getChildFile ("device-" + juce::String (index) + ".midi");
        juce::FileOutputStream os (f);
        if (os.openedOk())
        {
            os.write (data.getData(), data.getDataSize());
            os.flush();
        }
    }
}

juce::StringArray MidiCapture::devices() const
{
    std::lock_guard<std::mutex> g (lock);
    return deviceNames;
}

int64_t MidiCapture::numEvents() const   { return count.load(); }

std::vector<MidiCapture::Event> MidiCapture::readAll (const juce::String& device) const
{
    std::vector<Event> out;
    juce::FileInputStream in (fileFor (device));
    if (! in.openedOk()) return out;
    while (! in.isExhausted())
    {
        Event e;
        e.sample = in.readInt64();
        const int len = (unsigned short) in.readShort();
        if (len <= 0 || len > 65535) break;
        juce::HeapBlock<juce::uint8> bytes ((size_t) len);
        if (in.read (bytes, len) != len) break;   // a record cut off by a crash: stop there
        e.message = juce::MidiMessage (bytes, len);
        out.push_back (std::move (e));
    }
    return out;
}

std::vector<MidiCapture::Event> MidiCapture::eventsIn (const juce::String& device, int64_t start, int64_t end) const
{
    std::vector<Event> all;
    {
        std::lock_guard<std::mutex> g (lock);
        const int index = deviceNames.indexOf (device);
        if (index < 0) return {};
        if (auto it = written.find (index); it != written.end()) all = it->second;
    }
    if (all.empty()) all = readAll (device);   // recovered from disk

    std::vector<Event> out;
    for (auto& e : all)
        if (e.sample >= start && e.sample < end)
            out.push_back (e);
    std::stable_sort (out.begin(), out.end(), [] (auto& a, auto& b) { return a.sample < b.sample; });
    return out;
}

std::vector<juce::MidiMessage> MidiCapture::controllerStateAt (const juce::String& device, int64_t before) const
{
    std::map<int, juce::MidiMessage> last;   // channel * 128 + controller
    for (auto& e : eventsIn (device, std::numeric_limits<int64_t>::min(), before))
        if (e.message.isController())
            last[e.message.getChannel() * 128 + e.message.getControllerNumber()] = e.message;
    std::vector<juce::MidiMessage> out;
    for (auto& [k, m] : last)
        if (m.getControllerValue() != 0 || m.getControllerNumber() != 64)   // a pedal that's up needs nothing
            out.push_back (m);
    return out;
}

std::unique_ptr<MidiCapture> MidiCapture::openExisting (const juce::File& f)
{
    auto v = juce::JSON::parse (f.getChildFile ("midi.json"));
    if (! v.isObject()) return {};
    std::unique_ptr<MidiCapture> c (new MidiCapture (f));
    c->sampleRate = (double) v["sampleRate"];
    if (auto* arr = v["devices"].getArray())
        for (auto& n : *arr) c->deviceNames.add (n.toString());
    return c;
}

} // namespace earfirst
