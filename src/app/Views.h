#pragma once
// What the window shows for people reading the screen: the colours, the
// transport bar, the timeline, the level meters and the message history.
//
// Rules for everything in here:
//  - Every button runs a command, so it does exactly what the key and the menu do.
//  - Every control has a title, a description and its key, for screen readers
//    and for the help tag a mouse user sees.
//  - Pictures (the timeline, the meters, the beat light) aren't in the Tab
//    order; what they show is always a key away as a spoken report.
//  - Colours come from one theme: light, dark, high contrast, or the system's.
// Included into Main.cpp.

#include <JuceHeader.h>
#include "app/AppCore.h"
#include "session/Tracks.h"

#if JUCE_MAC
 #include <objc/message.h>
 #include <objc/runtime.h>
#elif JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif

namespace earfirst
{

//==============================================================================
// Colours
inline bool systemWantsHighContrast()
{
   #if JUCE_MAC
    auto cls = (id) objc_getClass ("NSWorkspace");
    if (cls == nullptr) return false;
    auto ws = ((id (*) (id, SEL)) objc_msgSend) (cls, sel_registerName ("sharedWorkspace"));
    if (ws == nullptr) return false;
    return ((BOOL (*) (id, SEL)) objc_msgSend) (ws, sel_registerName ("accessibilityDisplayShouldIncreaseContrast"));
   #elif JUCE_WINDOWS
    HIGHCONTRASTW hc {};
    hc.cbSize = sizeof (hc);
    return SystemParametersInfoW (SPI_GETHIGHCONTRAST, sizeof (hc), &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
   #else
    return false;
   #endif
}

struct Theme
{
    juce::String name;
    juce::Colour background, panel, text, dim, accent, accentText, selection, selectionText,
                 clip, clipText, playhead, marker, loop, meterLow, meterMid, meterHigh, focus, outline;

    static juce::StringArray choices()   { return { "Follow the system", "Light", "Dark", "High contrast" }; }

    // What "Follow the system" means right now.
    static juce::String resolve (const juce::String& chosen)
    {
        if (chosen != "Follow the system" && choices().contains (chosen)) return chosen;
        if (systemWantsHighContrast()) return "High contrast";
        return juce::Desktop::getInstance().isDarkModeActive() ? "Dark" : "Light";
    }

    static Theme make (const juce::String& chosen)
    {
        const auto n = resolve (chosen);
        Theme t; t.name = n;
        if (n == "High contrast")
        {
            // Black, white and yellow: the most legible pairing, and no meaning
            // carried by colour alone.
            t.background = juce::Colours::black;          t.panel = juce::Colours::black;
            t.text = juce::Colours::white;                t.dim = juce::Colours::white;
            t.accent = juce::Colour (0xffffeb3b);         t.accentText = juce::Colours::black;
            t.selection = juce::Colour (0xffffeb3b);      t.selectionText = juce::Colours::black;
            t.clip = juce::Colours::black;                t.clipText = juce::Colours::white;
            t.playhead = juce::Colour (0xffffeb3b);       t.marker = juce::Colour (0xff00e5ff);
            t.loop = juce::Colour (0xff00e5ff);
            t.meterLow = juce::Colours::white;            t.meterMid = juce::Colour (0xffffeb3b);   t.meterHigh = juce::Colour (0xffff5252);
            t.focus = juce::Colour (0xffffeb3b);          t.outline = juce::Colours::white;
        }
        else if (n == "Dark")
        {
            t.background = juce::Colour (0xff1c1d21);     t.panel = juce::Colour (0xff26282e);
            t.text = juce::Colour (0xfff2f2f2);           t.dim = juce::Colour (0xffb8bcc4);
            t.accent = juce::Colour (0xff5aa9ff);         t.accentText = juce::Colour (0xff0b0c0e);
            t.selection = juce::Colour (0xff3d6fb0);      t.selectionText = juce::Colours::white;
            t.clip = juce::Colour (0xff2f5d8a);           t.clipText = juce::Colours::white;
            t.playhead = juce::Colour (0xffffd54f);       t.marker = juce::Colour (0xffff8a65);
            t.loop = juce::Colour (0xff81c784);
            t.meterLow = juce::Colour (0xff66bb6a);       t.meterMid = juce::Colour (0xffffca28);   t.meterHigh = juce::Colour (0xffef5350);
            t.focus = juce::Colour (0xffffd54f);          t.outline = juce::Colour (0xff6b7079);
        }
        else
        {
            t.background = juce::Colour (0xfffafafa);     t.panel = juce::Colour (0xffeef0f3);
            t.text = juce::Colour (0xff111318);           t.dim = juce::Colour (0xff4a4f58);
            t.accent = juce::Colour (0xff0b5cad);         t.accentText = juce::Colours::white;
            t.selection = juce::Colour (0xff0b5cad);      t.selectionText = juce::Colours::white;
            t.clip = juce::Colour (0xffcfe2f7);           t.clipText = juce::Colour (0xff0b2540);
            t.playhead = juce::Colour (0xffc62828);       t.marker = juce::Colour (0xffb34700);
            t.loop = juce::Colour (0xff2e7d32);
            t.meterLow = juce::Colour (0xff2e7d32);       t.meterMid = juce::Colour (0xffb58900);   t.meterHigh = juce::Colour (0xffc62828);
            t.focus = juce::Colour (0xff0b5cad);          t.outline = juce::Colour (0xff8a909a);
        }
        return t;
    }
};

// The look every part of the window uses, dialogs and menus included, with a
// thick focus ring so anyone using the keyboard can see where they are.
class EarFirstLook : public juce::LookAndFeel_V4
{
public:
    Theme theme = Theme::make ("Light");

    void apply (const Theme& t)
    {
        theme = t;
        setColourScheme ({ t.background, t.panel, t.panel, t.outline, t.text, t.panel, t.text, t.selection, t.text });
        using C = juce::Colour;
        auto set = [this] (int id, C c) { setColour (id, c); };
        set (juce::ResizableWindow::backgroundColourId, t.background);
        set (juce::DocumentWindow::textColourId, t.text);
        set (juce::Label::textColourId, t.text);
        set (juce::ListBox::backgroundColourId, t.panel);
        set (juce::ListBox::textColourId, t.text);
        set (juce::ListBox::outlineColourId, t.outline);
        set (juce::TextButton::buttonColourId, t.panel);
        set (juce::TextButton::buttonOnColourId, t.accent);
        set (juce::TextButton::textColourOffId, t.text);
        set (juce::TextButton::textColourOnId, t.accentText);
        set (juce::TextEditor::backgroundColourId, t.background);
        set (juce::TextEditor::textColourId, t.text);
        set (juce::TextEditor::outlineColourId, t.outline);
        set (juce::TextEditor::focusedOutlineColourId, t.focus);
        set (juce::TextEditor::highlightColourId, t.selection);
        set (juce::TextEditor::highlightedTextColourId, t.selectionText);
        set (juce::CaretComponent::caretColourId, t.text);
        set (juce::ComboBox::backgroundColourId, t.panel);
        set (juce::ComboBox::textColourId, t.text);
        set (juce::ComboBox::outlineColourId, t.outline);
        set (juce::ComboBox::focusedOutlineColourId, t.focus);
        set (juce::ComboBox::arrowColourId, t.text);
        set (juce::PopupMenu::backgroundColourId, t.panel);
        set (juce::PopupMenu::textColourId, t.text);
        set (juce::PopupMenu::highlightedBackgroundColourId, t.selection);
        set (juce::PopupMenu::highlightedTextColourId, t.selectionText);
        set (juce::ToggleButton::textColourId, t.text);
        set (juce::ToggleButton::tickColourId, t.text);
        set (juce::ToggleButton::tickDisabledColourId, t.dim);
        set (juce::TooltipWindow::backgroundColourId, t.panel);
        set (juce::TooltipWindow::textColourId, t.text);
        set (juce::TooltipWindow::outlineColourId, t.outline);
        set (juce::ScrollBar::thumbColourId, t.dim);
    }

    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour& bg, bool over, bool down) override
    {
        auto r = b.getLocalBounds().toFloat().reduced (1.5f);
        auto fill = b.getToggleState() ? theme.accent : bg;
        if (down) fill = fill.contrasting (0.15f);
        else if (over) fill = fill.contrasting (0.07f);
        g.setColour (fill);
        g.fillRoundedRectangle (r, 5.0f);
        g.setColour (theme.outline);
        g.drawRoundedRectangle (r, 5.0f, 1.0f);
        if (b.hasKeyboardFocus (false))
        {
            g.setColour (theme.focus);
            g.drawRoundedRectangle (r.reduced (1.0f), 5.0f, 3.0f);
        }
    }

    void drawLabel (juce::Graphics& g, juce::Label& l) override
    {
        LookAndFeel_V4::drawLabel (g, l);
        if (l.hasKeyboardFocus (true)) { g.setColour (theme.focus); g.drawRect (l.getLocalBounds(), 3); }
    }
};

//==============================================================================
// A button that runs a command, says the command's name and key, and shows a
// toggle's state in its words as well as its colour.
class CommandButton : public juce::TextButton
{
public:
    CommandButton (AppCore& c, juce::String commandId, juce::String label)
        : core (c), id (std::move (commandId)), plainLabel (std::move (label))
    {
        setButtonText (plainLabel);
        onClick = [this] { core.runFromUser (id, CommandList::Source::menu); };
        refresh();
    }

    void refresh()
    {
        auto* cmd = core.commands().find (id);
        if (cmd == nullptr) return;
        auto keys = core.keymap().keysFor (id);
        const auto keyText = keys.isEmpty() ? juce::String() : keysForPeople (parseKeys (keys[0]), currentPlatform());
        setTitle (plainLabel);
        setDescription (cmd->description);
        setHelpText (keyText.isEmpty() ? cmd->description : cmd->description + " Key: " + keyText + ".");
        setTooltip (plainLabel + (keyText.isEmpty() ? juce::String() : " (" + keyText + ")"));
        if (cmd->isToggle())
        {
            auto st = cmd->toggleState();
            setToggleState (st.value_or (false), juce::dontSendNotification);
            const auto text = plainLabel + (st ? (*st ? " on" : " off") : juce::String());
            if (getButtonText() != text) setButtonText (text);
        }
        if (labelFor) { auto t = labelFor(); if (t != getButtonText()) { setButtonText (t); setTitle (t); } }
        // An unavailable button stays reachable and says why when pressed; it
        // shows in a dimmer colour, still readable.
        const bool can = cmd->available == nullptr || cmd->available().enabled;
        if (auto* look = dynamic_cast<EarFirstLook*> (&getLookAndFeel()))
            setColour (juce::TextButton::textColourOffId, can ? look->theme.text : look->theme.dim);
        setDescription (can ? cmd->description : cmd->description + " Not available now: " + cmd->available().reason);
    }

    std::function<juce::String()> labelFor;    // words that change: Play / Stop
    const juce::String& commandId() const      { return id; }

private:
    AppCore& core;
    juce::String id, plainLabel;
};

//==============================================================================
// The beat as a light and a number, for anyone who can't hear the click: the
// one is bright, the other beats softer. Shown while the song plays.
class BeatLight : public juce::Component
{
public:
    explicit BeatLight (AppCore& c) : core (c)
    {
        setTitle ("Beat");
        setDescription ("Lights up on every beat while the song plays; the first beat of the bar is brightest.");
        setWantsKeyboardFocus (false);
    }

    // Returns true on the moment a new beat starts.
    bool update()
    {
        auto* s = core.session();
        const bool playing = s != nullptr && s->getEdit().getTransport().isPlaying();
        if (! playing) { if (beat != -1) { beat = -1; repaint(); } return false; }
        auto& e = s->getEdit();
        const auto pos = e.getTransport().getPosition();
        const auto bb = e.tempoSequence.toBarsAndBeats (pos);
        const int whole = (int) std::floor (e.tempoSequence.toBeats (pos).inBeats() + 1.0e-6);
        if (whole != lastWhole)
        {
            lastWhole = whole;
            beat = bb.getWholeBeats();
            flashUntil = juce::Time::getMillisecondCounterHiRes() + 160.0;
            repaint();
            return true;
        }
        if (juce::Time::getMillisecondCounterHiRes() > flashUntil && lit) repaint();
        return false;
    }

    bool isOne() const   { return beat == 0; }

    void paint (juce::Graphics& g) override
    {
        auto& t = static_cast<EarFirstLook&> (getLookAndFeel()).theme;
        auto r = getLocalBounds().toFloat().reduced (2.0f);
        lit = beat >= 0 && juce::Time::getMillisecondCounterHiRes() < flashUntil;
        g.setColour (lit ? (beat == 0 ? t.accent : t.accent.withAlpha (0.55f)) : t.panel);
        g.fillEllipse (r.withSizeKeepingCentre (std::min (r.getWidth(), r.getHeight()), std::min (r.getWidth(), r.getHeight())));
        g.setColour (t.outline);
        g.drawEllipse (r.withSizeKeepingCentre (std::min (r.getWidth(), r.getHeight()), std::min (r.getWidth(), r.getHeight())), 1.5f);
        if (beat >= 0)
        {
            g.setColour (lit ? t.accentText : t.text);
            g.setFont (juce::FontOptions (r.getHeight() * 0.55f, juce::Font::bold));
            g.drawText (juce::String (beat + 1), r, juce::Justification::centred);
        }
    }

private:
    AppCore& core;
    int beat = -1, lastWhole = -1;
    double flashUntil = 0;
    bool lit = false;
};

//==============================================================================
// Buttons for the things people do most, in the order they're used.
class TransportBar : public juce::Component
{
public:
    explicit TransportBar (AppCore& c) : core (c), beatLight (c)
    {
        setTitle ("Transport");
        setFocusContainerType (FocusContainerType::focusContainer);
        auto add = [this] (const char* id, const char* label)
        {
            buttons.push_back (std::make_unique<CommandButton> (core, id, label));
            addAndMakeVisible (*buttons.back());
            return buttons.back().get();
        };
        add ("transport.go-to-start", "Start");
        add ("transport.previous-bar", "Back a bar");
        play = add ("transport.play-stop", "Play");
        play->labelFor = [this] { auto* s = core.session(); return s != nullptr && s->getEdit().getTransport().isPlaying() ? juce::String ("Stop") : juce::String ("Play"); };
        add ("transport.next-bar", "Forward a bar");
        add ("transport.metronome", "Click");
        add ("transport.loop", "Loop");
        add ("capture.keep", "Keep last 30 seconds");
        add ("markers.add", "Add marker");
        add ("tracks.add", "Add track");
        add ("file.import-audio", "Import audio");
        add ("file.render", "Render");
        addAndMakeVisible (beatLight);
    }

    void refresh()   { for (auto& b : buttons) b->refresh(); }

    int heightFor (int width, float scale) const
    {
        return rowsFor (width, scale) * rowHeight (scale) + 4;
    }

    void resized() override
    {
        const float s = scale;
        const int h = rowHeight (s);
        auto r = getLocalBounds().reduced (0, 2);
        beatLight.setBounds (r.removeFromRight (h).reduced (2));
        r.removeFromRight (6);
        int x = r.getX(), y = r.getY();
        for (auto& b : buttons)
        {
            const int w = buttonWidth (*b, s);
            if (x + w > r.getRight() && x > r.getX()) { x = r.getX(); y += h; }
            b->setBounds (x, y, w, h - 4);
            x += w + 6;
        }
    }

    float scale = 1.0f;
    BeatLight beatLight;
    std::vector<std::unique_ptr<CommandButton>> buttons;

private:
    static int rowHeight (float s)   { return juce::roundToInt (40 * s); }
    int buttonWidth (CommandButton& b, float s) const
    {
        juce::GlyphArrangement ga;
        ga.addLineOfText (juce::FontOptions (15.0f * s), b.getButtonText() + " off", 0, 0);   // room for the longest state
        return juce::roundToInt (ga.getBoundingBox (0, -1, true).getWidth()) + juce::roundToInt (24 * s);
    }
    int rowsFor (int width, float s) const
    {
        int rows = 1, x = 0;
        const int avail = width - rowHeight (s) - 6;
        for (auto& b : buttons)
        {
            const int w = buttonWidth (*b, s);
            if (x + w > avail && x > 0) { ++rows; x = 0; }
            x += w + 6;
        }
        return rows;
    }

    AppCore& core;
    CommandButton* play = nullptr;
};

//==============================================================================
// The song as a picture: bars along the top, markers, one lane per track
// lined up with the track list, every clip with its name, the loop and the
// playhead. Clicking moves the playhead and picks the track.
class Timeline : public juce::Component
{
public:
    explicit Timeline (AppCore& c) : core (c)
    {
        setTitle ("Timeline");
        setDescription ("A picture of the song. Everything it shows can be heard with the reports: Where am I, List markers, About this clip.");
        setWantsKeyboardFocus (false);
    }

    std::function<int()> laneTop;        // where the first lane starts, in this component, after scrolling
    std::function<int()> laneHeight;
    std::function<void (int)> onTrackClicked;
    int rulerHeight = 34;
    float scale = 1.0f;

    double visibleSeconds() const
    {
        auto* s = core.session();
        if (s == nullptr) return 30.0;
        auto& e = s->getEdit();
        return std::max ({ 30.0, e.getLength().inSeconds() * 1.08 + 4.0, e.getTransport().getPosition().inSeconds() + 8.0 });
    }
    float xFor (double seconds) const     { return (float) (seconds / visibleSeconds() * (getWidth() - 2)) + 1.0f; }
    double secondsAt (float x) const      { return juce::jmax (0.0, (double) (x - 1.0f) / (getWidth() - 2) * visibleSeconds()); }

    void paint (juce::Graphics& g) override
    {
        auto& t = static_cast<EarFirstLook&> (getLookAndFeel()).theme;
        g.fillAll (t.panel);
        auto* s = core.session();
        if (s == nullptr)
        {
            g.setColour (t.text);
            g.setFont (juce::FontOptions (20.0f * scale));
            g.drawFittedText ("No song open.\nCommand-N makes a new one; Command-O opens one.\nEverything you play is already being captured.",
                              getLocalBounds().reduced (20), juce::Justification::centred, 4);
            return;
        }
        auto& e = s->getEdit();
        auto& tempo = e.tempoSequence;
        const auto ruler = getLocalBounds().removeFromTop (rulerHeight);

        // Bars: a line each, numbered as often as there's room.
        g.setFont (juce::FontOptions (13.0f * scale));
        const double secs = visibleSeconds();
        const int lastBar = tempo.toBarsAndBeats (tracktion::TimePosition::fromSeconds (secs)).bars + 1;
        const float barWidth = xFor (tempo.toTime (tracktion::tempo::BarsAndBeats { 1, {} }).inSeconds()) - xFor (0.0);
        const int every = std::max (1, (int) std::ceil (36.0f * scale / std::max (1.0f, barWidth)));
        for (int bar = 0; bar <= lastBar; ++bar)
        {
            const float x = xFor (tempo.toTime (tracktion::tempo::BarsAndBeats { bar, {} }).inSeconds());
            g.setColour (t.outline.withAlpha (bar % every == 0 ? 0.6f : 0.25f));
            g.drawVerticalLine (juce::roundToInt (x), (float) ruler.getBottom() - 8.0f, (float) getHeight());
            if (bar % every == 0)
            {
                g.setColour (t.dim);
                g.drawText (juce::String (bar + 1), juce::Rectangle<float> (x + 3, (float) ruler.getBottom() - 18 * scale, 60 * scale, 16 * scale),
                            juce::Justification::centredLeft, false);
            }
        }
        // Loop range.
        if (e.getTransport().looping.get())
        {
            auto lr = e.getTransport().getLoopRange();
            g.setColour (t.loop.withAlpha (0.18f));
            g.fillRect (juce::Rectangle<float>::leftTopRightBottom (xFor (lr.getStart().inSeconds()), 0, xFor (lr.getEnd().inSeconds()), (float) getHeight()));
            g.setColour (t.loop);
            g.fillRect (juce::Rectangle<float>::leftTopRightBottom (xFor (lr.getStart().inSeconds()), 0, xFor (lr.getEnd().inSeconds()), 4.0f));
        }
        // Lanes and clips.
        const int top = laneTop ? laneTop() : rulerHeight, h = laneHeight ? laneHeight() : 30;
        auto tracks = contentTracks (e);
        g.saveState();
        g.reduceClipRegion (getLocalBounds().withTrimmedTop (rulerHeight));
        for (int i = 0; i < tracks.size(); ++i)
        {
            const int y = top + i * h;
            if (y > getHeight() || y + h < rulerHeight) continue;
            if (i == core.selectedTrack) { g.setColour (t.selection.withAlpha (0.16f)); g.fillRect (0, y, getWidth(), h); }
            g.setColour (t.outline.withAlpha (0.35f));
            g.drawHorizontalLine (y + h - 1, 0.0f, (float) getWidth());
            const bool muted = tracks[i]->isMuted (false);
            for (auto* clip : tracks[i]->getClips())
            {
                auto pos = clip->getPosition();
                auto r = juce::Rectangle<float>::leftTopRightBottom (xFor (pos.getStart().inSeconds()), (float) y + 3, xFor (pos.getEnd().inSeconds()), (float) (y + h - 4));
                g.setColour (muted ? t.clip.withAlpha (0.45f) : t.clip);
                g.fillRoundedRectangle (r, 3.0f);
                g.setColour (t.outline);
                g.drawRoundedRectangle (r, 3.0f, 1.0f);
                g.setColour (t.clipText);
                g.setFont (juce::FontOptions (13.0f * scale));
                g.drawText (clip->getName(), r.reduced (5, 0), juce::Justification::centredLeft, true);
            }
        }
        g.restoreState();
        // Markers: a line and the name, along the top.
        g.setFont (juce::FontOptions (13.0f * scale, juce::Font::bold));
        for (auto* m : e.getMarkerManager().getMarkers())
        {
            const float x = xFor (m->getPosition().getStart().inSeconds());
            g.setColour (t.marker);
            g.drawLine (x, 0.0f, x, (float) getHeight(), 2.0f);
            g.drawText (m->getName(), juce::Rectangle<float> (x + 4, 2, 160 * scale, 16 * scale), juce::Justification::centredLeft, true);
        }
        // The playhead, with a triangle on the ruler so it's not a hairline alone.
        const float px = xFor (e.getTransport().getPosition().inSeconds());
        g.setColour (t.playhead);
        g.drawLine (px, 0.0f, px, (float) getHeight(), 2.5f);
        juce::Path tri;
        tri.addTriangle (px - 7, (float) ruler.getBottom() - 12, px + 7, (float) ruler.getBottom() - 12, px, (float) ruler.getBottom());
        g.fillPath (tri);
        g.setColour (t.outline);
        g.drawHorizontalLine (ruler.getBottom(), 0.0f, (float) getWidth());
        g.drawRect (getLocalBounds());
    }

    void mouseDown (const juce::MouseEvent& m) override
    {
        auto* s = core.session();
        if (s == nullptr) return;
        s->getEdit().getTransport().setPosition (tracktion::TimePosition::fromSeconds (secondsAt ((float) m.x)));
        if (m.y > rulerHeight && laneTop && laneHeight && onTrackClicked)
        {
            const int row = (m.y - laneTop()) / std::max (1, laneHeight());
            if (row >= 0 && row < contentTracks (s->getEdit()).size()) onTrackClicked (row);
        }
        repaint();
    }

private:
    AppCore& core;
};

//==============================================================================
// Level meters for every input and output, with a clip light that stays on
// until it's clicked. Green, then yellow from -12 dB, red from -3 dB, plus a
// line for the recent peak.
class MeterStrip : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit MeterStrip (AppCore& c) : core (c)
    {
        setTitle ("Levels");
        setDescription ("Input and output meters. What's the level says the same thing out loud.");
        setWantsKeyboardFocus (false);
        setTooltip ("Levels. Click to clear the clip lights.");
    }

    float scale = 1.0f;

    void paint (juce::Graphics& g) override
    {
        auto& t = static_cast<EarFirstLook&> (getLookAndFeel()).theme;
        g.fillAll (t.panel);
        auto r = getLocalBounds().reduced (6, 4);
        const int labelW = juce::roundToInt (38 * scale);
        auto inArea = r.removeFromLeft (r.getWidth() * 3 / 4);
        r.removeFromLeft (10);
        drawGroup (g, t, inArea, "In", core.meters.input, core.meters.inputHeld, core.meters.inputClipped, labelW);
        drawGroup (g, t, r, "Out", core.meters.output, core.meters.outputHeld, core.meters.outputClipped, labelW);
        g.setColour (t.outline);
        g.drawRect (getLocalBounds());
    }

    void mouseDown (const juce::MouseEvent&) override
    {
        core.meters.inputClipped = core.meters.outputClipped = false;
        repaint();
    }

private:
    static float toBar (float gain)
    {
        const float db = juce::Decibels::gainToDecibels (gain, -60.0f);
        return juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
    }

    void drawGroup (juce::Graphics& g, const Theme& t, juce::Rectangle<int> area, const juce::String& label,
                    const std::vector<float>& level, const std::vector<float>& held, bool clipped, int labelW)
    {
        auto labelArea = area.removeFromLeft (labelW);
        g.setColour (t.text);
        g.setFont (juce::FontOptions (13.0f * scale, juce::Font::bold));
        g.drawText (label, labelArea.removeFromTop (labelArea.getHeight() / 2), juce::Justification::centredLeft);
        area.removeFromLeft (4);
        if (clipped)
        {
            g.setColour (t.meterHigh);
            g.fillRect (labelArea.reduced (0, 2));
            g.setColour (juce::Colours::white);
            g.setFont (juce::FontOptions (11.0f * scale, juce::Font::bold));
            g.drawText ("CLIP", labelArea, juce::Justification::centred);
        }
        const int n = (int) level.size();
        if (n == 0)
        {
            g.setColour (t.dim);
            g.setFont (juce::FontOptions (13.0f * scale));
            g.drawText ("none open", area, juce::Justification::centredLeft);
            return;
        }
        const float w = (float) area.getWidth() / (float) n;
        for (int i = 0; i < n; ++i)
        {
            auto bar = juce::Rectangle<float> (area.getX() + i * w + 0.5f, (float) area.getY(), std::max (1.0f, w - 1.5f), (float) area.getHeight());
            g.setColour (t.background);
            g.fillRect (bar);
            const float v = toBar (level[(size_t) i]);
            const float db = juce::Decibels::gainToDecibels (level[(size_t) i], -60.0f);
            g.setColour (db > -3.0f ? t.meterHigh : db > -12.0f ? t.meterMid : t.meterLow);
            g.fillRect (bar.withTop (bar.getBottom() - bar.getHeight() * v));
            if (i < (int) held.size() && held[(size_t) i] > 0.001f)
            {
                g.setColour (t.text);
                g.fillRect (bar.withHeight (2.0f).withY (bar.getBottom() - bar.getHeight() * toBar (held[(size_t) i])));
            }
        }
    }

    AppCore& core;
};

//==============================================================================
// Everything the app has said, newest at the bottom, for anyone who missed it
// or can't hear it. Tab reaches it; Up and Down read each one.
class MessageHistory : public juce::ListBox, private juce::ListBoxModel
{
public:
    explicit MessageHistory (const juce::StringArray& l) : log (l)
    {
        setModel (this);
        setTitle ("Messages");
        setDescription ("Everything Ear First has said, newest at the bottom.");
        setWantsKeyboardFocus (true);
    }

    float scale = 1.0f;

    void update()
    {
        int n = 0;
        for (auto& m : log) if (m.isNotEmpty()) ++n;
        if (n == shown) return;
        shown = n;
        updateContent();
        if (! hasKeyboardFocus (true)) scrollToEnsureRowIsOnscreen (n - 1);
    }

    int getNumRows() override                       { return shown; }
    juce::String getNameForRow (int row) override   { return messageAt (row); }
    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
    {
        auto& t = static_cast<EarFirstLook&> (getLookAndFeel()).theme;
        if (selected) g.fillAll (t.selection);
        g.setColour (selected ? t.selectionText : (row == shown - 1 ? t.text : t.dim));
        g.setFont (juce::FontOptions (14.0f * scale));
        g.drawText (messageAt (row), 8, 0, w - 16, h, juce::Justification::centredLeft, true);
    }

private:
    juce::String messageAt (int row) const
    {
        int n = 0;
        for (auto& m : log)
            if (m.isNotEmpty() && n++ == row) return m;
        return {};
    }

    const juce::StringArray& log;
    int shown = 0;
};

//==============================================================================
// The first time Ear First opens: what it is and how to start, in its own
// window that stays until it's closed. Help, Welcome shows it again.
class WelcomeComponent : public juce::Component
{
public:
    WelcomeComponent (AppCore& c, std::function<void()> closeIt) : core (c), close (std::move (closeIt))
    {
        setTitle ("Welcome to Ear First");
        text.setMultiLine (true);
        text.setReadOnly (true);
        text.setCaretVisible (true);
        text.setScrollbarsShown (true);
        text.setTitle ("Welcome");
        text.setText (welcomeText(), false);
        text.setFont (juce::FontOptions (17.0f * core.textScale));
        addAndMakeVisible (text);
        start.setButtonText ("Start a new song");
        start.setDescription ("Makes a new song, the same as Command-N.");
        start.onClick = [this] { auto c2 = &core; auto done = close; if (done) done(); c2->runFromUser ("file.new-song", CommandList::Source::menu); };
        closeButton.setButtonText ("Close");
        closeButton.onClick = [this] { if (close) close(); };
        addAndMakeVisible (start);
        addAndMakeVisible (closeButton);
        setSize (juce::roundToInt (620 * core.textScale), juce::roundToInt (460 * core.textScale));
    }

    static juce::String welcomeText()
    {
        return juce::String ("Ear First works by ear and by eye. Every action is a command: it has a key, it's in a menu, and Command-Shift-P finds it by name.\n\n")
             + "It's already listening. Every input on your interface is captured from the moment Ear First opens, so you never have to press record. "
               "When you've played something you want, press K (or R) to keep the last 30 seconds, or Shift-K for the last two minutes.\n\n"
             + "To start: Command-N makes a new song, Command-O opens one. Space plays and stops. Page Up and Page Down move by bar, Left and Right by beat, "
               "and M drops a marker. The keys follow REAPER with OSARA wherever Ear First does the same thing.\n\n"
             + "Lost? Command-Slash turns on key help: press any key to hear what it does. Command-Shift-R says the last message again.\n\n"
             + "This window comes back from the Help menu, under Welcome.";
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (16);
        auto buttons = r.removeFromBottom (juce::roundToInt (40 * core.textScale));
        closeButton.setBounds (buttons.removeFromRight (juce::roundToInt (120 * core.textScale)));
        buttons.removeFromRight (10);
        start.setBounds (buttons.removeFromRight (juce::roundToInt (200 * core.textScale)));
        r.removeFromBottom (12);
        text.setBounds (r);
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::escapeKey) { if (close) close(); return true; }
        return false;
    }

    juce::TextEditor text;
    juce::TextButton start, closeButton;

private:
    AppCore& core;
    std::function<void()> close;
};

} // namespace earfirst
