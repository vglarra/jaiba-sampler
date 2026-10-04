// Sequencer audio tests.
//
// These render the playback pool for real and assert WHERE audio lands, in
// samples.  No audio device and no GUI: SequencerEngine is driven directly with
// a synthetic sample, exactly as the audio callback drives it.
//
// This is the harness that caught two bugs the model tests could not see:
//   * start() did not silence leftovers, so Play layered over the previous run
//   * one-shot voices deliberately ignore note-off, so allNotesOff() cannot stop
//     them and the pool has to hard-stop
//
// At 120 BPM / 960 PPQ / 48 kHz: 0.04 ticks per sample, so one beat (960 ticks)
// is 24000 samples.  Every timing assertion below derives from that.
//
// Build + run: tests/run_tests.sh
#include <juce_data_structures/juce_data_structures.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "SequencerEngine.h"

#include <cmath>
#include <cstdio>
#include <vector>

static int failures = 0;

static void check (bool ok, const char* what)
{
    std::printf ("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (! ok) ++failures;
}

int main()
{
    const double sr    = 48000.0;
    const int    block = 512;
    const int    beat  = 24000;   // samples per beat at 120 BPM / 48 kHz

    juce::AudioFormatManager fm;
    SequencerEngine seq (fm);
    seq.prepareToPlay (sr, block);

    // A 1-second constant buffer: any voice that starts is instantly visible.
    auto longBuf = std::make_shared<juce::AudioBuffer<float>> (1, (int) sr);
    for (int i = 0; i < (int) sr; ++i)
        longBuf->setSample (0, i, 1.0f);

    // A short burst, so successive hits can be told apart as separate onsets.
    auto shortBuf = std::make_shared<juce::AudioBuffer<float>> (1, 2000);
    for (int i = 0; i < 2000; ++i)
        shortBuf->setSample (0, i, 1.0f);

    auto makeSound = [] (std::shared_ptr<juce::AudioBuffer<float>> buf, double rate,
                         bool oneShot = true)
    {
        SeqSound s;
        s.hasSound                  = true;
        s.sourcePad                 = 0;
        s.sampleRate                = rate;
        s.audio                     = std::move (buf);
        s.settings.sampleFilePath   = "/tmp/seqtest-kick.wav";
        s.settings.midiNote         = 60;
        s.settings.oneShotEnabled   = oneShot;
        s.settings.volumeLevel      = 1.0f;
        s.settings.startPointSeconds = 0.0;
        s.settings.endPointSeconds   = -1.0;
        return s;
    };

    seq.setTrackSound (0, makeSound (longBuf, sr));

    auto makePattern = [] (std::initializer_list<int> ticks)
    {
        auto p = std::make_shared<SeqPattern>();
        p->snap = SeqSnap::Sixteenth;
        p->bars = 1;
        p->ppq  = 960;
        for (int t : ticks)
            p->tracks[0].hits.push_back ({ t, 1.0f });
        return p;
    };

    auto pat = makePattern ({ 960 });   // one hit, one beat in
    seq.setPattern (pat);

    // Sample index of the first audible frame within `maxBlocks` of playback.
    juce::AudioBuffer<float> mix (2, block);
    auto firstAudibleSample = [&] (int maxBlocks, double bpm = 120.0,
                                   double masterGain = 1.0) -> long long
    {
        for (int b = 0; b < maxBlocks; ++b)
        {
            mix.clear();
            seq.processBlock (block, mix, bpm, masterGain);
            for (int i = 0; i < block; ++i)
                if (std::abs (mix.getSample (0, i)) > 0.001f)
                    return (long long) b * block + i;
        }
        return -1;
    };

    //==========================================================================
    // Basic scheduling
    //==========================================================================
    check (firstAudibleSample (4) < 0, "no audio before start()");

    seq.start();
    const long long first = firstAudibleSample (60);
    check (first >= 0, "audio appears once playing");
    check (std::llabs (first - beat) <= 2,
           "a hit one beat in lands on sample 24000");

    // Tempo changes where the hit lands: double tempo, half the time.
    seq.start();
    const long long at240 = firstAudibleSample (60, 240.0);
    check (std::llabs (at240 - beat / 2) <= 2,
           "at 240 BPM the same hit lands on sample 12000");

    //==========================================================================
    // Transport state
    //==========================================================================
    seq.start();
    (void) firstAudibleSample (60);
    seq.stop();
    check (firstAudibleSample (10) < 0, "stop() silences the pool");
    check (! seq.isPlaying(), "isPlaying() is false after stop()");

    // Restarting must not layer over the previous run (regression: it used to).
    seq.start();
    (void) firstAudibleSample (60);
    seq.start();
    {
        // After a fresh start the hit is a whole beat away again, not immediate.
        const long long again = firstAudibleSample (60);
        check (std::llabs (again - beat) <= 2,
               "starting again replays from the top instead of layering");
    }

    //==========================================================================
    // Mixing
    //==========================================================================
    {
        auto muted = makePattern ({ 960 });
        muted->tracks[0].mute = true;
        seq.setPattern (muted);
        seq.start();
        check (firstAudibleSample (60) < 0, "a muted track produces no audio");
    }

    seq.setPattern (pat);
    seq.start();
    {
        juce::AudioBuffer<float> quiet (2, block);
        float peak = 0.0f;
        for (int b = 0; b < 60; ++b)
        {
            quiet.clear();
            seq.processBlock (block, quiet, 120.0, 0.5);   // app master at 50%
            peak = juce::jmax (peak, quiet.getMagnitude (0, block));
        }
        check (peak > 0.3f && peak < 0.7f, "master gain 0.5 scales the pool output");
    }

    seq.setMasterVolume (0.5f);
    seq.start();
    {
        juce::AudioBuffer<float> quiet (2, block);
        float peak = 0.0f;
        for (int b = 0; b < 60; ++b)
        {
            quiet.clear();
            seq.processBlock (block, quiet, 120.0, 1.0);   // pool's own level at 50%
            peak = juce::jmax (peak, quiet.getMagnitude (0, block));
        }
        check (peak > 0.3f && peak < 0.7f, "the pool's own master level scales it too");
    }
    seq.setMasterVolume (1.0f);

    //==========================================================================
    // Position reporting (what the playhead and the click sync rely on)
    //==========================================================================
    check (seq.patternPpq() == 960, "the pattern's PPQ is mirrored for the metronome");

    seq.start();
    {
        juce::AudioBuffer<float> m (2, block);
        for (int b = 0; b < 93; ++b)          // 93 * 512 = 47616 samples
        {
            m.clear();
            seq.processBlock (block, m, 120.0, 1.0);
        }
        const double expected = 47616.0 * 0.04;   // 1904.64 ticks
        check (std::abs (seq.playheadTicks() - expected) < 1.0,
               "the playhead advances at the pattern's tick rate");
    }

    seq.start();
    {
        juce::AudioBuffer<float> m (2, block);
        for (int b = 0; b < 200; ++b)
        {
            m.clear();
            seq.processBlock (block, m, 120.0, 1.0);
        }
        check (seq.playheadTicks() >= 0.0
               && seq.playheadTicks() < (double) pat->totalTicks(),
               "the playhead wraps and stays inside the pattern");
    }

    //==========================================================================
    // Armed start: dropping in on a click beat, mid-block
    //==========================================================================
    {
        seq.setPattern (makePattern ({ 0 }));   // needs a hit ON tick 0
        seq.stop();
        seq.beginPlaybackAt (-20.0);            // 20 ticks early = 500 samples

        juce::AudioBuffer<float> m (2, block);
        m.clear();
        seq.processBlock (block, m, 120.0, 1.0);

        int at = -1;
        for (int i = 0; i < block; ++i)
            if (std::abs (m.getSample (0, i)) > 0.001f) { at = i; break; }

        check (std::abs (at - 500) <= 2,
               "an armed start puts tick 0 on the exact sample");
        check (seq.playheadTicks() >= 0.0, "the reported playhead is never negative");
    }

    //==========================================================================
    // Every beat in a bar fires, each on its own sample
    //==========================================================================
    {
        seq.stop();
        seq.setTrackSound (0, makeSound (shortBuf, sr));   // short bursts, separable
        seq.setPattern (makePattern ({ 0, 960, 1920, 2880 }));
        seq.start();

        std::vector<long long> onsets;
        long long sample = 0;
        bool wasSilent = true;

        juce::AudioBuffer<float> m (2, block);
        for (int b = 0; b < 250 && onsets.size() < 4; ++b)
        {
            m.clear();
            seq.processBlock (block, m, 120.0, 1.0);
            for (int i = 0; i < block; ++i)
            {
                const bool audible = std::abs (m.getSample (0, i)) > 0.001f;
                if (audible && wasSilent) onsets.push_back (sample + i);
                wasSilent = ! audible;
            }
            sample += block;
        }

        check (onsets.size() == 4, "all four beats in the bar fired");

        bool onTheBeat = (onsets.size() == 4);
        for (size_t k = 0; onTheBeat && k < onsets.size(); ++k)
            onTheBeat = std::llabs (onsets[k] - (long long) k * beat) <= 2;

        check (onTheBeat, "each beat lands on its own exact sample");
    }

    //==========================================================================
    // Step gate: a step is a DURATION, not just a trigger
    //==========================================================================
    {
        // lastAudibleSample runs `blocks` blocks and reports the final audible frame.
        auto lastAudibleSample = [&] (int blocks) -> long long
        {
            long long last = -1;
            juce::AudioBuffer<float> m (2, block);

            for (int b = 0; b < blocks; ++b)
            {
                m.clear();
                seq.processBlock (block, m, 120.0, 1.0);
                for (int i = 0; i < block; ++i)
                    if (std::abs (m.getSample (0, i)) > 0.001f)
                        last = (long long) b * block + i;
            }
            return last;
        };

        // A 1/16 step at 120 BPM is 240 ticks = 6000 samples.
        // oneShot OFF: the note is released at the step's end, so a 1-second sample
        // is cut off around 6000 samples plus its release tail -- not left ringing.
        seq.stop();
        seq.setTrackSound (0, makeSound (longBuf, sr, /*oneShot=*/false));
        seq.setPattern (makePattern ({ 0 }));
        seq.start();

        const long long gatedEnd = lastAudibleSample (40);   // 20480 samples of run
        check (gatedEnd > 5000, "a gated sample is held for at least its step");
        check (gatedEnd < 15000,
               "a gated sample is released at the end of its step, not played as a one-shot");

        // oneShot ON: the same sample ignores the release and rings to its end.
        seq.stop();
        seq.setTrackSound (0, makeSound (longBuf, sr, /*oneShot=*/true));
        seq.start();

        const long long oneShotEnd = lastAudibleSample (200);   // past the 1 s sample
        check (oneShotEnd > 40000,
               "a sample configured as OneShot rings past its step, to the end");
    }

    //==========================================================================
    // Loop: a looping sample fills its step instead of playing once
    //==========================================================================
    {
        // Is there audio in an absolute sample window?  Asserting on a window rather
        // than "the last audible frame" keeps this independent of release tails.
        auto audibleBetween = [&] (int blocks, long long from, long long to) -> bool
        {
            juce::AudioBuffer<float> m (2, block);
            long long pos = 0;

            for (int b = 0; b < blocks; ++b)
            {
                m.clear();
                seq.processBlock (block, m, 120.0, 1.0);

                for (int i = 0; i < block; ++i)
                    if (pos + i >= from && pos + i < to
                        && std::abs (m.getSample (0, i)) > 0.001f)
                        return true;

                pos += block;
            }
            return false;
        };

        // shortBuf is 2000 samples (~41 ms) while a 1/16 step is 6000 samples, so
        // the window at 3000..5000 is past the sample's own length: only a loop can
        // still be sounding there.
        seq.stop();
        auto looping = makeSound (shortBuf, sr, /*oneShot=*/false);
        looping.settings.loopEnabled = true;
        seq.setTrackSound (0, looping);
        seq.setPattern (makePattern ({ 0 }));
        seq.start();
        check (audibleBetween (12, 3000, 5000),
               "a looping sample keeps sounding past the sample's own length");

        seq.stop();
        auto plain = makeSound (shortBuf, sr, /*oneShot=*/false);
        plain.settings.loopEnabled = false;
        seq.setTrackSound (0, plain);
        seq.start();
        check (! audibleBetween (12, 3000, 5000),
               "an unlooped short sample stops when its audio runs out");

        // Both are still cut at the end of the step: a step is a duration.  The
        // window starts past the sound's 100 ms release tail (~sample 10800), so
        // this is about the gate, not the fade.
        seq.stop();
        seq.setTrackSound (0, looping);
        seq.start();
        check (! audibleBetween (60, 13000, 16000),
               "the loop is released at the end of its step, not left running");
    }

    std::printf ("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED",
                 failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
