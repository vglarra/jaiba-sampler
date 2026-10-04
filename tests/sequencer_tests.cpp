// Sequencer model tests — no audio device needed.
//
// Covers the parts of the Seq feature that are pure logic: metronome accent
// grouping, session tempo sources ("songs"), session transport/mix options and
// the kit pattern round-trip.
//
// Build + run: tests/run_tests.sh   (needs a prior `ninja -C build`)
#include <juce_data_structures/juce_data_structures.h>

#include "Sequencer.h"
#include "GjmManager.h"

#include <cmath>
#include <cstdio>

static int failures = 0;

static void check (bool ok, const char* what)
{
    std::printf ("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (! ok) ++failures;
}

static bool near (double a, double b, double tol = 1e-6)
{
    return std::abs (a - b) <= tol;
}

/** Accent levels for `beats` consecutive clicks, as a compact string. */
static juce::String accents (int beats, int num, int den)
{
    juce::String r;
    for (int b = 0; b < beats; ++b)
        r += juce::String (SeqClick::accentFor (b, num, den));
    return r;
}

int main()
{
    //==========================================================================
    // Metronome accent grouping (time signature)
    // 0 = bar accent, 1 = compound pulse, 2 = plain beat.
    //==========================================================================
    {
        check (accents (8, 4, 4)  == "02220222",       "4/4 accents every 4th click");
        check (accents (6, 3, 4)  == "022022",         "3/4 accents every 3rd click");
        check (accents (6, 2, 4)  == "020202",         "2/4 accents every 2nd click");
        check (accents (10, 5, 4) == "0222202222",     "5/4 accents every 5th click");
        check (accents (14, 7, 8) == "02222220222222", "7/8 accents the bar only");

        // Compound meters: bar accent plus a pulse every 3rd click.
        check (accents (12, 6, 8) == "022122022122",
               "6/8 accents the bar and every dotted-quarter pulse");
        check (accents (18, 9, 8) == "022122122022122122",
               "9/8 accents the bar and every dotted-quarter pulse");
        check (accents (4, 6, 8)  == "0221",
               "6/8 grouping wraps correctly at an arbitrary beat");

        check (SeqClick::accentFor (-1, 4, 4) == 2,
               "negative beats do not break the grouping");
        check (SeqClick::pitchFor (0) > SeqClick::pitchFor (1)
               && SeqClick::pitchFor (1) > SeqClick::pitchFor (2),
               "bar accent is the highest pitch, then the pulse, then the beat");
        check (SeqClick::pitchFor (2) < SeqClick::pitchFor (0),
               "an unaccented click sits below the bar accent");
    }

    //==========================================================================
    // Tempo field: drag to set, double-click to reset
    //==========================================================================
    {
        check (near (SequencerTempo::kResetBpm, 120.0), "double-click reset lands on 120");

        // Dragging up is faster, down is slower.
        check (near (SequencerTempo::bpmFromDrag (120.0, 10, false), 130.0),
               "dragging up 10 px adds 10 BPM");
        check (near (SequencerTempo::bpmFromDrag (120.0, -10, false), 110.0),
               "dragging down 10 px subtracts 10 BPM");
        check (near (SequencerTempo::bpmFromDrag (120.0, 0, false), 120.0),
               "no drag leaves the tempo alone");

        // Shift is fine-grained.
        check (near (SequencerTempo::bpmFromDrag (120.0, 10, true), 122.5),
               "shift drag is quarter-rate");

        // Clamped to the supported range in both directions.
        check (near (SequencerTempo::bpmFromDrag (120.0, 100000, false), 300.0),
               "drag clamps at the top of the range");
        check (near (SequencerTempo::bpmFromDrag (120.0, -100000, false), 20.0),
               "drag clamps at the bottom of the range");
    }

    //==========================================================================
    // Session tempo sources ("songs") and transport/mix options
    //==========================================================================
    {
        const auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("jaiba_seq_model_test.gjm");
        tmp.deleteFile();

        GjmManager m;
        m.reset();

        check (m.tempoSources.size() == 1, "session starts with one tempo source");
        check (m.tempoSources[0].name == "Song 1", "default source is named \"Song 1\"");
        check (near (m.tempoSources[0].bpm, 120.0), "default source is 120 BPM");

        // Three banks tied to a second song; one on the kit's own tempo.
        m.tempoSources.push_back ({ "Song 2", 95.0 });
        m.banks[0].tempoGroup = 0;
        m.banks[1].tempoGroup = 1;
        m.banks[2].tempoGroup = 1;
        m.banks[3].tempoGroup = TempoSourceDefaults::kKitTempo;
        m.banks[3].sequence.bpm = 140.0;

        check (near (m.tempoForBank (0), 120.0), "bank 0 follows the default song");
        check (near (m.tempoForBank (1), 95.0),  "bank 1 follows Song 2");
        check (near (m.tempoForBank (2), 95.0),  "bank 2 is tied to Song 2");
        check (near (m.tempoForBank (3), 140.0), "bank 3 uses its kit tempo");
        check (near (m.tempoForBank (3, 133.0), 133.0),
               "kit-tempo override wins (live pattern)");
        check (m.tempoSourceName (1) == "Song 2", "source name resolves");
        check (m.tempoSourceName (3) == "Kit",    "kit source names as Kit");

        // Session options.
        m.syncClickToSeq  = false;
        m.seqMasterVolume = 0.42f;

        check (m.saveManifest (tmp), "session manifest writes");

        GjmManager r;
        r.reset();
        check (r.loadManifest (tmp), "session manifest re-parses");
        check (r.tempoSources.size() == 2, "both sources round-trip");
        check (r.tempoSources[0].name == "Song 1" && near (r.tempoSources[0].bpm, 120.0),
               "source 0 name + bpm round-trip");
        check (r.tempoSources[1].name == "Song 2" && near (r.tempoSources[1].bpm, 95.0),
               "source 1 name + bpm round-trip");
        check (r.banks[1].tempoGroup == 1 && r.banks[2].tempoGroup == 1,
               "tied banks round-trip");
        check (r.banks[3].tempoGroup == TempoSourceDefaults::kKitTempo,
               "kit-tempo bank round-trips");
        check (! r.syncClickToSeq, "click-sync option round-trips");
        check (near ((double) r.seqMasterVolume, 0.42), "sequencer master level round-trips");

        // An out-of-range group falls back to the default source.
        r.banks[5].tempoGroup = 99;
        check (near (r.tempoForBank (5), 120.0), "out-of-range group falls back to default");


        // ---- Backward compatibility: an old manifest with no <Tempos> ----
        const auto oldFile = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                 .getChildFile ("jaiba_seq_old_session.gjm");
        {
            juce::XmlElement root ("GlobalJaibaMap");
            auto* b0 = root.createNewChildElement ("Bank");
            b0->setAttribute ("index", 0);
            b0->setAttribute ("kitPath", "kit-001.jai");
            b0->setAttribute ("name", "Legacy");
            root.writeTo (oldFile);
        }

        GjmManager old;
        check (old.loadManifest (oldFile), "legacy manifest loads");
        check (old.tempoSources.size() == 1 && old.tempoSources[0].name == "Song 1",
               "legacy session gets one default song");
        check (old.banks[0].tempoGroup == TempoSourceDefaults::kDefaultIndex,
               "legacy banks default to the default source");
        check (near (old.tempoForBank (0), 120.0),
               "legacy session behaves as one global tempo");
        check (old.syncClickToSeq && near ((double) old.seqMasterVolume, 1.0),
               "legacy session gets sensible option defaults");

        tmp.deleteFile();
        oldFile.deleteFile();
    }

    //==========================================================================
    // Clear: a default-constructed pattern IS the cleared state
    //==========================================================================
    {
        // Build a heavily-used pattern, then reset it the way Clear does.
        SeqPattern used;
        used.bpm     = 137.0;
        used.bars    = 4;
        used.sigNum  = 7;
        used.sigDen  = 8;
        used.snap    = SeqSnap::ThirtySecond;
        used.mode    = SeqMode::Record;
        used.locked  = true;
        used.tracks[2].volume = 0.3f;
        used.tracks[2].mute   = true;
        used.tracks[2].hits.push_back ({ 240, 0.4f });
        used.tracks[5].sound.hasSound = true;

        used = SeqPattern{};

        check (used.bpm == 120.0, "clear returns the tempo to 120");
        check (used.bars == 1, "clear returns Bars to 1");
        check (used.sigNum == 4 && used.sigDen == 4, "clear returns the signature to 4/4");
        check (used.snap == SeqSnap::Sixteenth, "clear returns Snap to 1/16");
        check (! used.hasAnyHits(), "clear removes every step");
        check (used.tracks[2].hits.empty(), "and every track's hits");
        check (used.tracks[2].volume == 1.0f && ! used.tracks[2].mute,
               "clear returns track levels and mutes to default");
        check (! used.tracks[5].sound.hasSound, "clear unlinks captured sounds");
        check (! used.locked && used.mode == SeqMode::Arrange,
               "clear unlocks and returns to an editable mode");
        check (used.stepsPerBar() == 16 && used.totalTicks() == 3840,
               "the cleared grid is one 4/4 bar of 16 steps");
    }

    //==========================================================================
    // A track's captured sound keeps the pad's Loop / OneShot modes
    //==========================================================================
    {
        PadSettings ps;
        ps.sampleFilePath = "/tmp/loop.wav";
        ps.loopEnabled    = true;
        ps.oneShotEnabled = false;
        ps.volumeLevel    = 0.7f;

        SeqSound snd;
        snd.hasSound  = true;
        snd.sourcePad = 3;
        snd.settings  = ps;

        check (snd.settings.loopEnabled && ! snd.settings.oneShotEnabled,
               "a captured sound carries the pad's Loop and OneShot modes");

        SeqPattern p;
        p.tracks[3].sound = snd;
        p.tracks[3].hits.push_back ({ 0, 0.8f });

        juce::XmlElement x ("Sequencer");
        p.saveToXml (x);

        SeqPattern q;
        q.loadFromXml (x);
        check (q.tracks[3].sound.settings.loopEnabled,
               "Loop mode survives the kit file with the track");
        check (! q.tracks[3].sound.settings.oneShotEnabled,
               "OneShot mode survives the kit file with the track");
        check (near ((double) q.tracks[3].sound.settings.volumeLevel, 0.7, 1e-3),
               "and so does the rest of the captured sound");

        // OneShot on is the opposite case, kept explicit.
        SeqPattern os;
        os.tracks[0].sound.hasSound = true;
        os.tracks[0].sound.settings.oneShotEnabled = true;
        juce::XmlElement ox ("Sequencer");
        os.saveToXml (ox);
        SeqPattern osBack;
        osBack.loadFromXml (ox);
        check (osBack.tracks[0].sound.settings.oneShotEnabled,
               "OneShot on survives too");
    }

    //==========================================================================
    // Link state: detached tracks own their sound
    //==========================================================================
    {
        SeqSound fresh;
        check (fresh.linked, "a captured track starts linked to its pad");

        SeqPattern p;
        p.tracks[2].sound.hasSound = true;
        p.tracks[2].sound.linked   = false;
        p.tracks[2].sound.settings.sampleFilePath = "/tmp/track-only.wav";

        juce::XmlElement x ("Sequencer");
        p.saveToXml (x);

        SeqPattern q;
        q.loadFromXml (x);
        check (! q.tracks[2].sound.linked, "a detached track stays detached in the kit");
        check (q.tracks[2].sound.settings.sampleFilePath == "/tmp/track-only.wav",
               "and keeps its own sample reference");

        // A kit written before detaching existed has no `linked` attribute, and
        // those tracks behaved as linked -- so they must still come back linked.
        juce::XmlElement legacy ("Sequencer");
        auto* te = legacy.createNewChildElement ("Track");
        te->setAttribute ("index", 0);
        auto* se = te->createNewChildElement ("Sound");
        se->setAttribute ("pad", 0);
        se->setAttribute ("samplePath", "/tmp/old.wav");

        SeqPattern old;
        old.loadFromXml (legacy);
        check (old.tracks[0].sound.hasSound && old.tracks[0].sound.linked,
               "a track from an older kit loads linked, as it behaved before");
    }

    //==========================================================================
    // Erase: steps go, the musical setup stays
    //==========================================================================
    {
        SeqPattern p;
        p.bpm = 137.0;
        p.bars = 3;
        p.sigNum = 7;
        p.sigDen = 8;
        p.snap = SeqSnap::ThirtySecond;
        p.tracks[0].volume = 0.4f;
        p.tracks[0].mute = true;
        p.tracks[0].hits.push_back ({ 0, 0.9f });
        p.tracks[1].hits.push_back ({ 240, 0.5f });
        p.tracks[2].sound.hasSound = true;
        p.tracks[2].sound.settings.sampleFilePath = "/tmp/kick.wav";
        p.tracks[2].hits.push_back ({ 480, 0.7f });

        p.clearHits();

        check (! p.hasAnyHits(), "erase removes every step");
        check (p.bpm == 137.0, "erase keeps the tempo");
        check (p.bars == 3, "erase keeps the bar count");
        check (p.sigNum == 7 && p.sigDen == 8, "erase keeps the time signature");
        check (p.snap == SeqSnap::ThirtySecond, "erase keeps the snap");
        check (p.tracks[0].volume == 0.4f && p.tracks[0].mute,
               "erase keeps track levels and mutes");
        check (p.tracks[2].sound.hasSound
               && p.tracks[2].sound.settings.sampleFilePath == "/tmp/kick.wav",
               "erase keeps the captured sounds");
        check (p.tracks[2].hits.empty(), "erase clears hits on tracks that had sounds");
    }

    //==========================================================================
    // Live capture (Record mode): note-on -> tick -> step
    //==========================================================================
    {
        // At 120 BPM / 960 PPQ / 48 kHz a sample is 0.04 ticks, and the pool has
        // advanced its playhead to the END of the block when capture runs.
        const double tps   = 0.04;
        const int    total = 3840;   // one 4/4 bar

        check (SequencerCapture::tickFor (960.0, 0, tps, total) == 960,
               "an event on the last sample of the block is the block's end tick");
        check (SequencerCapture::tickFor (960.0, 500, tps, total) == 940,
               "an event 500 samples earlier is 20 ticks earlier");
        check (SequencerCapture::tickFor (960.0, 250, tps, total) == 950,
               "an event mid-block lands proportionally earlier");

        // Captured across the loop point: wraps instead of going negative.
        check (SequencerCapture::tickFor (10.0, 500, tps, total) == total - 10,
               "a hit captured across the loop point wraps to the end of the bar");

        // Degenerate inputs must not loop forever or divide by zero.
        check (SequencerCapture::tickFor (100.0, 10, 0.0, total) == 0,
               "a zero tick rate yields tick 0 instead of spinning");
        check (SequencerCapture::tickFor (100.0, 10, tps, 0) == 0,
               "a zero-length pattern yields tick 0 instead of spinning");
    }

    {
        // Overdubbing a step replaces its velocity rather than stacking hits.
        std::vector<SeqHit> hits;

        check (SequencerCapture::addOrUpdateHit (hits, 480, 0.9f), "a new hit is recorded");
        check (hits.size() == 1 && hits[0].tick == 480, "the hit is stored");

        check (SequencerCapture::addOrUpdateHit (hits, 0, 0.5f), "an earlier hit is recorded");
        check (hits.size() == 2, "both hits are kept");
        check (hits[0].tick == 0 && hits[1].tick == 480, "hits stay sorted by tick");

        check (SequencerCapture::addOrUpdateHit (hits, 480, 0.25f),
               "re-recording a step updates its velocity");
        check (hits.size() == 2, "re-recording a step does not duplicate it");
        check (near ((double) hits[1].velocity, 0.25, 1e-4), "the new velocity is stored");

        check (! SequencerCapture::addOrUpdateHit (hits, 480, 0.25f),
               "recording an identical value reports no change");
    }

    {
        // Snap decides where a captured tick lands; Free records it as played.
        SeqPattern p;
        p.ppq = 960;

        p.snap = SeqSnap::Sixteenth;   // 240 ticks per step
        check (p.snapTick (250) == 240, "a 1/16 grid quantises a captured tick down");
        check (p.snapTick (130) == 240, "and rounds to the nearest step, not the floor");

        p.snap = SeqSnap::Free;
        check (p.snapTick (250) == 250, "Free mode records the tick as played");

        // Recording is a live activity, never a restored state.
        SeqPattern rec;
        rec.mode = SeqMode::Record;
        check (rec.safeRestoreMode() == SeqMode::Arrange,
               "a kit saved while armed comes back in Arrange, not recording");
    }

    //==========================================================================
    // Grid modes (Live / Record / Arrange) + Lock
    //==========================================================================
    {
        // A fresh pattern must be editable, or the grid would look broken.
        SeqPattern fresh;
        check (fresh.mode == SeqMode::Arrange, "a new pattern starts in Arrange");
        check (fresh.isEditable(), "a new pattern is editable out of the box");

        SeqPattern live;
        live.mode = SeqMode::Live;
        check (! live.isEditable(), "Live mode is read-only");

        SeqPattern rec;
        rec.mode = SeqMode::Record;
        check (! rec.isEditable(), "Record mode is not mouse-editable");

        SeqPattern arr;
        arr.mode = SeqMode::Arrange;
        check (arr.isEditable(), "Arrange mode is editable");

        arr.locked = true;
        check (! arr.isEditable(), "Lock forces read-only even in Arrange");
        arr.locked = false;

        // All three modes survive a save/load.
        for (auto m : { SeqMode::Live, SeqMode::Record, SeqMode::Arrange })
        {
            SeqPattern a;
            a.mode = m;
            juce::XmlElement x ("Sequencer");
            a.saveToXml (x);

            SeqPattern b;
            b.loadFromXml (x);
            check (b.mode == m, "each mode round-trips through the kit file");
            check (b.isEditable() == (m == SeqMode::Arrange),
                   "editability follows the restored mode");
        }

        // A pre-Phase-3 pattern (only the old `live` flag) must stay editable.
        {
            juce::XmlElement legacy ("Sequencer");
            legacy.setAttribute ("snap", 3);
            legacy.setAttribute ("bars", 1);
            legacy.setAttribute ("live", 1);     // the old "Live" default

            SeqPattern p;
            p.loadFromXml (legacy);
            check (p.mode == SeqMode::Arrange,
                   "an old pattern saved as Live loads as editable Arrange");

            juce::XmlElement legacy2 ("Sequencer");
            legacy2.setAttribute ("live", 0);
            SeqPattern p2;
            p2.loadFromXml (legacy2);
            check (p2.mode == SeqMode::Arrange, "and so does an old Arrange pattern");
        }

        check (juce::String (seqModeName (SeqMode::Live)) == "Live"
               && juce::String (seqModeName (SeqMode::Record)) == "Record"
               && juce::String (seqModeName (SeqMode::Arrange)) == "Arrange",
               "mode names are as shown on the button");
    }

    //==========================================================================
    // Velocity drag editing
    //==========================================================================
    {
        check (near ((double) SequencerVelocity::kDefault, 0.8),
               "a new hit and a velocity reset both land on 0.8");

        // 150 px of travel spans the full range.
        check (near ((double) SequencerVelocity::fromDrag (0.5f, 0, false), 0.5, 1e-4),
               "no drag leaves the velocity alone");
        check (near ((double) SequencerVelocity::fromDrag (0.5f, 15, false), 0.6, 1e-4),
               "dragging up raises the velocity");
        check (near ((double) SequencerVelocity::fromDrag (0.5f, -15, false), 0.4, 1e-4),
               "dragging down lowers the velocity");

        check (near ((double) SequencerVelocity::fromDrag (0.5f, 15, true), 0.525, 1e-4),
               "shift drag is quarter-rate");

        // Clamped at both ends, never silent and never above full scale.
        check (near ((double) SequencerVelocity::fromDrag (0.5f, 100000, false), 1.0, 1e-4),
               "velocity clamps at full scale");
        check (near ((double) SequencerVelocity::fromDrag (0.5f, -100000, false),
                     (double) SequencerVelocity::kMin, 1e-4),
               "velocity clamps above silence");
        check (SequencerVelocity::fromDrag (0.0f, -100000, false) > 0.0f,
               "a hit can never be dragged to zero");
    }

    //==========================================================================
    // Grid layout: 16 tracks fill the available height
    //==========================================================================
    {
        const int tracks = 16;
        const int minRow = 12;

        // The default card leaves ~217 px under the toolbars and ruler.
        check (SequencerLayout::rowHeightFor (217, tracks, minRow) == 13,
               "the default window fits 16 tracks at 13 px");

        // A maximised window must actually grow the rows, not leave a gap.
        check (SequencerLayout::rowHeightFor (848, tracks, minRow) == 53,
               "a tall window gives proportionally taller rows");
        check (SequencerLayout::rowHeightFor (400, tracks, minRow) == 25,
               "rows scale with the window");

        // Short window: floor applies, and the grid scrolls rather than squashing.
        check (SequencerLayout::rowHeightFor (100, tracks, minRow) == minRow,
               "a short window floors the row height instead of squashing");
        check (SequencerLayout::rowHeightFor (0, tracks, minRow) == minRow,
               "a zero-height window still yields usable rows");
        // The guard is against division by zero, not a special value: it must
        // return something usable rather than trapping.
        check (SequencerLayout::rowHeightFor (400, 0, minRow) >= minRow,
               "a zero track count does not divide by zero");

        // The property that matters: the rows never overflow the space.
        for (int h : { 200, 217, 400, 848, 1200 })
            check (tracks * SequencerLayout::rowHeightFor (h, tracks, minRow) <= h,
                   "16 rows always fit inside the available height");

        // Horizontal scrollbar is only charged for when the content is too wide.
        check (SequencerLayout::scrollbarReserve (800, 700, 8) == 8,
               "a wide pattern reserves the scrollbar's height");
        check (SequencerLayout::scrollbarReserve (700, 700, 8) == 0,
               "a pattern that fits reserves nothing");
    }

    //==========================================================================
    // Time signature decides the grid's bar length
    //==========================================================================
    {
        auto stepsInBar = [] (int num, int den, SeqSnap snap)
        {
            SeqPattern p;
            p.sigNum = num;
            p.sigDen = den;
            p.snap   = snap;
            return p.stepsPerBar();
        };

        const auto sx = SeqSnap::Sixteenth;

        check (stepsInBar (4, 4, sx) == 16, "4/4 at 1/16 is 16 steps per bar");
        check (stepsInBar (3, 4, sx) == 12, "3/4 at 1/16 is 12 steps per bar");
        check (stepsInBar (2, 4, sx) == 8,  "2/4 at 1/16 is 8 steps per bar");
        check (stepsInBar (5, 4, sx) == 20, "5/4 at 1/16 is 20 steps per bar");
        check (stepsInBar (6, 8, sx) == 12, "6/8 at 1/16 is 12 steps per bar");
        check (stepsInBar (7, 8, sx) == 14, "7/8 at 1/16 is 14 steps per bar");
        check (stepsInBar (12, 8, sx) == 24, "12/8 at 1/16 is 24 steps per bar");
        check (stepsInBar (4, 4, SeqSnap::Eighth) == 8,
               "4/4 at 1/8 is 8 steps per bar");
        check (stepsInBar (3, 4, SeqSnap::Quarter) == 3,
               "3/4 at 1/4 is 3 steps per bar");

        // Bar length in ticks must stay consistent with the tick model.
        SeqPattern p3;
        p3.sigNum = 3;
        p3.sigDen = 4;
        check (p3.stepsPerBar() * p3.ticksPerStep() == 3 * p3.ppq,
               "3/4 bar is exactly three quarter notes of ticks");

        // Degenerate input must not divide by zero or return an empty bar.
        SeqPattern bad;
        bad.sigNum = 0;
        bad.sigDen = 0;
        check (bad.stepsPerBar() >= 1, "a degenerate signature still yields a bar");

        // --- Overhang: notes kept past the loop end -------------------------
        SeqPattern ov;
        ov.sigNum = 4;
        ov.sigDen = 4;                       // 16 steps / 3840 ticks
        ov.tracks[0].hits.push_back ({ 240, 1.0f });    // inside
        check (ov.displaySteps() == 16 && ! ov.hasHitsPastLoop(),
               "an in-loop pattern needs no overhang");

        ov.tracks[0].hits.push_back ({ 3600, 1.0f });   // inside 4/4, near the end
        check (ov.displaySteps() == 16 && ov.hitsPastLoop() == 0,
               "a hit inside the loop adds no overhang");

        ov.sigNum = 3;                       // loop is now 2880 ticks
        check (ov.hitsPastLoop() == 1, "shrinking the bar exposes a stored note");
        check (ov.displaySteps() == 16,
               "the exposed note extends the drawing area to its column");
        check (ov.tracks[0].hits.size() == 2, "exposing a note does not delete it");

        ov.sigNum = 4;
        check (ov.hitsPastLoop() == 0 && ov.displaySteps() == 16,
               "restoring the signature clears the overhang");

        // The boundary itself counts as outside: the engine stops at totalTicks.
        SeqPattern edge;
        edge.tracks[0].hits.push_back ({ edge.totalTicks(), 1.0f });
        check (edge.hitsPastLoop() == 1,
               "a hit exactly on the loop end counts as outside");

        SeqPattern neg;
        neg.tracks[0].hits.push_back ({ -5, 1.0f });
        check (neg.displaySteps() == neg.totalSteps(),
               "negative ticks never widen the drawing area");
    }

    //==========================================================================
    // Kit pattern + captured track sound round-trip (the .jai side)
    //==========================================================================
    {
        SeqPattern p;
        p.snap = SeqSnap::Sixteenth;
        p.bars = 2;
        p.ppq  = 960;
        p.bpm  = 137.0;
        p.clickVolume = 0.33f;
        p.mode = SeqMode::Arrange;

        p.tracks[0].volume = 0.75f;
        p.tracks[0].mute   = true;
        p.sigNum = 7;
        p.sigDen = 8;

        p.tracks[0].hits.push_back ({ 0,   0.9f });
        p.tracks[0].hits.push_back ({ 240, 0.4f });

        p.tracks[3].sound.hasSound  = true;
        p.tracks[3].sound.sourcePad = 3;
        p.tracks[3].sound.settings.sampleFilePath = "/tmp/kick.wav";
        p.tracks[3].sound.settings.volumeLevel    = 0.8f;
        p.tracks[3].sound.settings.eqEnabled      = true;
        p.tracks[3].sound.settings.eq1Freq        = 120.0f;
        p.tracks[3].hits.push_back ({ 480, 1.0f });

        // This pattern is 7/8, so 2 bars at 1/16 is 2 * 14 = 28 steps.
        check (p.totalSteps() == 28, "2 bars of 7/8 at 1/16 is 28 steps");
        check (p.totalTicks() == 28 * 240, "step count converts to ticks");

        juce::XmlElement xml ("Sequencer");
        p.saveToXml (xml);

        SeqPattern q;
        q.loadFromXml (xml);
        check (q.tracks[0].hits.size() == 2, "pattern XML loads back at all");
        check (q.bars == 2 && q.ppq == 960, "bars + ppq round-trip");
        check (q.sigNum == 7 && q.sigDen == 8, "time signature round-trips with the kit");
        check (near (q.bpm, 137.0), "kit tempo round-trips");
        check (near ((double) q.clickVolume, 0.33), "click volume round-trips");
        check (q.mode == SeqMode::Arrange, "mode round-trips");
        check (near ((double) q.tracks[0].volume, 0.75), "track volume round-trips");
        check (q.tracks[0].mute, "track mute round-trips");
        check (q.tracks[0].hits.size() == 2, "track hits round-trip");
        check (q.tracks[0].hits[1].tick == 240, "hit tick round-trips");
        check (near ((double) q.tracks[0].hits[1].velocity, 0.4, 1e-3),
               "hit velocity round-trips");
        check (q.tracks[3].sound.hasSound, "captured sound flag round-trips");
        check (q.tracks[3].sound.settings.sampleFilePath == "/tmp/kick.wav",
               "captured sample path round-trips");
        check (near ((double) q.tracks[3].sound.settings.volumeLevel, 0.8, 1e-3),
               "captured volume round-trips");
        check (q.tracks[3].sound.audio == nullptr,
               "audio buffers are NOT serialised (rebuilt on load)");

        // The non-destructive guarantee: a hit past the end of a shorter loop is
        // still written, so switching signature back and forth loses nothing.
        SeqPattern wide;
        wide.sigNum = 4;
        wide.sigDen = 4;
        wide.tracks[0].hits.push_back ({ 3600, 1.0f });   // past a 3/4 bar (2880)
        check (wide.totalTicks() == 3840, "a 4/4 bar is 3840 ticks at 960 PPQ");

        wide.sigNum = 3;
        check (wide.totalTicks() == 2880, "the same pattern in 3/4 loops at 2880");
        check (wide.tracks[0].hits.size() == 1,
               "changing signature does not drop hits outside the loop");

        juce::XmlElement wideXml ("Sequencer");
        wide.saveToXml (wideXml);
        SeqPattern wideBack;
        wideBack.loadFromXml (wideXml);
        check (wideBack.tracks[0].hits.size() == 1
               && wideBack.tracks[0].hits[0].tick == 3600,
               "a hit outside the loop survives save + load");

        wide.sigNum = 4;
        check (wide.totalTicks() == 3840 && wide.tracks[0].hits[0].tick == 3600,
               "returning to 4/4 restores the original loop and its hits");
    }

    std::printf ("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED",
                 failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
