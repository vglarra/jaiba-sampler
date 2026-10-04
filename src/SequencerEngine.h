#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <memory>

#include "PadAudioEngine.h"
#include "Sequencer.h"

//==============================================================================
// SequencerEngine — the resident playback pool for the Seq tab (D7).
//
// One PadAudioEngine per track, created lazily and living OUTSIDE the bank swap,
// so a pattern recorded in Bank 1 keeps sounding while another bank is active.
//
// A track's sound is installed from a SeqSound, which carries a shared_ptr to the
// pad's already-decoded buffer: detaching therefore duplicates no sample RAM and
// never touches disk.  Sequencer engines are built with the FFT worker disabled
// (see PadAudioEngine's constructor) — they never display a spectrum.
//
// Threading:
//   prepareToPlay / setPattern / setTrackSound / setTrackVolume / start / stop
//       — message thread
//   processBlock — audio thread.  It reads the published pattern snapshot once
//       per block, fires each hit at an exact sample offset, renders the pool
//       into a private buffer and adds it (master-scaled) to the mix.
//==============================================================================
class SequencerEngine
{
public:
    explicit SequencerEngine (juce::AudioFormatManager& fmt) : formatManager (fmt) {}
    ~SequencerEngine() = default;

    void prepareToPlay (double newSampleRate, int blockSize);
    void releaseResources();

    /** Publish a new pattern snapshot.  The audio thread picks it up next block. */
    void setPattern (std::shared_ptr<const SeqPattern> p);

    /** Install or refresh a track's sound.  Shares `sound.audio` — no decode. */
    void setTrackSound (int trackIdx, const SeqSound& sound);

    /** Drop a track's sound (its pad was cleared, or the user unlinked it). */
    void clearTrackSound (int trackIdx);

    /** Live per-track mixer level (0-1), layered over the captured volume. */
    void setTrackVolume (int trackIdx, float volume);

    /** Push a pad's current settings onto an already-installed track sound, live
        and without rebuilding it.  This is what keeps a track in step with the
        Control panel: Loop / OneShot, the start-end trim the loop runs between,
        pitch, envelope and the mix.  A one-shot voice ignores the note-off the step
        gate emits, so the sample rings past its step; everything else is released
        at the end of the step, and a looped sample repeats for the note's length. */
    void updateTrackFromPad (int trackIdx, const PadSettings& ps)
    {
        if (trackIdx < 0 || trackIdx >= SeqPattern::kTracks) return;

        auto& slot = trackEngines[(size_t) trackIdx];
        if (slot == nullptr) return;

        auto* snd = dynamic_cast<LoopingSamplerSound*> (
                        slot->getSynthesiser().getSound (0).get());
        if (snd == nullptr) return;

        applyShape (trackIdx, ps, *snd);
        applyMix (trackIdx, ps);
    }

    /** Sound one track's note immediately, for previewing a step from the grid.
        Works with the transport stopped -- that is when a pattern is being built --
        and is safe to call from the message thread: the request is handed to the
        audio thread through an atomic rather than touching its MIDI buffers. */
    void auditionTrack (int trackIdx, float velocity = SequencerVelocity::kDefault)
    {
        if (trackIdx < 0 || trackIdx >= SeqPattern::kTracks) return;

        auditionRequest[(size_t) trackIdx].store (
            juce::jlimit (1, 127, juce::roundToInt (velocity * 127.0f)),
            std::memory_order_relaxed);
    }

    /** Stop a held audition.  0 in the request slot means "release". */
    void endAudition (int trackIdx)
    {
        if (trackIdx < 0 || trackIdx >= SeqPattern::kTracks) return;

        auditionRequest[(size_t) trackIdx].store (0, std::memory_order_relaxed);
    }

    /** Master level for the whole pool (0-1), for balancing Seq against the pads. */
    void setMasterVolume (float volume) { masterVolume.store (juce::jlimit (0.0f, 1.0f, volume)); }
    float getMasterVolume() const { return masterVolume.load(); }

    /** PPQ of the currently published pattern (for the metronome's beat maths). */
    int patternPpq() const { return ppqAtomic.load (std::memory_order_relaxed); }

    /** Loop length of the published pattern, in ticks (for capture's wrap maths). */
    int patternTotalTicks() const { return totalTicksAtomic.load (std::memory_order_relaxed); }

    void start();
    void stop();

    /** Begin playing with the tick playhead pre-offset: a negative value means the
        pattern's tick 0 is that many ticks away, so the run starts mid-block on an
        exact sample.  Audio thread only (used to drop in on a click beat). */
    void beginPlaybackAt (double ticks)
    {
        playheadPos = ticks;
        playheadUI.store (juce::jmax (0.0, ticks), std::memory_order_relaxed);

        for (auto& m : trackMidi) m.clear();
        for (auto& a : activeNote) a = -1;
        for (auto& g : gateCountdown) g = -1;

        playing.store (true, std::memory_order_relaxed);
    }
    bool isPlaying() const { return playing.load (std::memory_order_relaxed); }
    double playheadTicks() const { return playheadUI.load (std::memory_order_relaxed); }

    /** Render the pool into `mix` (adds), scaled by `masterGain`. */
    void processBlock (int numSamples, juce::AudioBuffer<float>& mix,
                       double bpm, double masterGain);

private:
    void applyShape (int trackIdx, const PadSettings& ps, LoopingSamplerSound& snd);
    void applyMix   (int trackIdx, const PadSettings& ps);
    PadAudioEngine& getOrCreateEngine (int trackIdx);
    void triggerTrack (int trackIdx, int offset, float velocity,
                       int gateSamples, int numSamples);
    void updateTrackGain (int trackIdx);
    void silenceAllVoices();
    void releaseTrack (int trackIdx, int offset);

    juce::AudioFormatManager& formatManager;

    std::array<std::unique_ptr<PadAudioEngine>, SeqPattern::kTracks> trackEngines;
    std::array<juce::MidiBuffer, SeqPattern::kTracks> trackMidi;
    std::array<int,   SeqPattern::kTracks> trackNote  {};   // trigger note per track
    std::array<int,   SeqPattern::kTracks> activeNote {};   // -1 = nothing sounding

    // Step gate: a hit is released at the end of its step.  Countdown is measured
    // from the end of the current block so it can be carried across blocks;
    // -1 means no note is waiting to be released.
    std::array<std::atomic<int>, SeqPattern::kTracks> auditionRequest {};   // -1 = none
    static constexpr int kIdleTailBlocks = 64;   // ~0.7 s of tail after the last sound
    int idleRenderBlocks = 0;                    // blocks left to render while stopped
    std::array<int,   SeqPattern::kTracks> gateCountdown {};
    std::array<int,   SeqPattern::kTracks> gateNote      {};
    std::array<float, SeqPattern::kTracks> trackLevel {};   // mixer fader
    std::array<float, SeqPattern::kTracks> soundLevel {};   // captured settings' volume
    std::array<double, SeqPattern::kTracks> trackRate {};   // sample rate of the installed sample

    // Pattern snapshot.  The audio thread loads it once per block; the message
    // thread publishes a fresh immutable copy on every edit.
    std::shared_ptr<const SeqPattern> pattern;

    std::atomic<bool>   playing    { false };
    std::atomic<double> playheadUI { 0.0 };   // ticks, for the playhead column
    std::atomic<int>    ppqAtomic  { 960 };   // mirrors the published pattern's PPQ
    std::atomic<int>    totalTicksAtomic { 3840 };
    std::atomic<float>  masterVolume { 1.0f };

    double sampleRate    = 44100.0;
    double playheadPos = 0.0;

    juce::AudioBuffer<float> poolBuffer;   // pre-sized; the pool renders here first

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SequencerEngine)
};

//==============================================================================
inline void SequencerEngine::prepareToPlay (double newSampleRate, int blockSize)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;

    activeNote.fill (-1);
    idleRenderBlocks = 0;
    for (auto& a : auditionRequest) a.store (-1, std::memory_order_relaxed);
    gateCountdown.fill (-1);
    gateNote  .fill (60);
    trackNote .fill (60);
    trackLevel.fill (1.0f);
    soundLevel.fill (1.0f);
    trackRate .fill (44100.0);

    poolBuffer.setSize (2, juce::jmax (64, blockSize), false, true, true);

    for (auto& m : trackMidi)
        m.ensureSize (1024);

    for (auto& e : trackEngines)
        if (e != nullptr)
            e->prepareToPlay (blockSize, sampleRate);
}

inline void SequencerEngine::releaseResources()
{
    for (auto& e : trackEngines)
        if (e != nullptr)
            e->releaseResources();
}

//==============================================================================
inline PadAudioEngine& SequencerEngine::getOrCreateEngine (int trackIdx)
{
    auto& slot = trackEngines[(size_t) trackIdx];
    if (slot == nullptr)
    {
        // FFT off: a pool engine never shows a spectrum, and one worker thread per
        // track would be pure overhead.
        slot = std::make_unique<PadAudioEngine> (formatManager, /*enableFft=*/false);
        slot->prepareToPlay (juce::jmax (64, poolBuffer.getNumSamples()), sampleRate);
    }
    return *slot;
}

inline void SequencerEngine::updateTrackGain (int trackIdx)
{
    auto& slot = trackEngines[(size_t) trackIdx];
    if (slot == nullptr) return;

    const float g = juce::jlimit (0.0f, 1.0f, soundLevel[(size_t) trackIdx])
                  * juce::jlimit (0.0f, 1.0f, trackLevel[(size_t) trackIdx]);
    slot->volumeGain.store (g);
}

//==============================================================================
inline void SequencerEngine::setTrackSound (int trackIdx, const SeqSound& sound)
{
    if (trackIdx < 0 || trackIdx >= SeqPattern::kTracks) return;
    if (! sound.hasSound || sound.audio == nullptr) return;

    const auto& ps = sound.settings;
    const int   note = juce::jlimit (0, 127, ps.midiNote >= 0 ? ps.midiNote : 60);
    const double sr  = sound.sampleRate > 0.0 ? sound.sampleRate : 44100.0;

    trackNote[(size_t) trackIdx]  = note;
    activeNote[(size_t) trackIdx] = -1;
    trackRate[(size_t) trackIdx]  = sr;
    soundLevel[(size_t) trackIdx] = juce::jlimit (0.0f, 1.0f, ps.volumeLevel);

    auto& engine = getOrCreateEngine (trackIdx);

    engine.muteOutput.store (true);
    engine.clearActiveSoundFlags();
    engine.forceStopAllVoices();
    engine.clearSoundsAndVoices();
    engine.allNotesOff (0, false);

    // ---- Install the sample, sharing the pad's decoded buffer ----
    {
        juce::ScopedLock lock (engine.sampleLock);
        engine.samples.clear();
        engine.selectedSampleIndex = 0;

        auto* s = new PadAudioEngine::MappedSample();
        s->file              = juce::File (ps.sampleFilePath);
        s->name              = s->file.getFileName();
        s->rootNote          = note;
        s->lowNote           = note;
        s->highNote          = note;
        s->sampleRate        = sr;
        s->numChannels       = sound.audio->getNumChannels();
        s->lengthInSamples   = sound.audio->getNumSamples();
        s->attack            = 0.01;
        s->release           = 0.1;
        s->startPointSeconds = ps.startPointSeconds;
        s->endPointSeconds   = ps.endPointSeconds;
        s->pitchOffset       = ps.pitchCents;
        s->audioData         = sound.audio;
        engine.samples.add (s);
    }

    // ---- Build the voice-facing sound from the same buffer ----
    {
        // Tiny silent reader — only needed to satisfy SamplerSound's constructor.
        // LoopingSamplerVoice reads from sound->fullAudioData instead.
        class DummyAudioReader : public juce::AudioFormatReader
        {
        public:
            DummyAudioReader (double readerSampleRate, unsigned int ch)
                : juce::AudioFormatReader (nullptr, "Dummy")
            {
                sampleRate            = readerSampleRate;
                numChannels           = ch;
                lengthInSamples       = 1;
                bitsPerSample         = 32;
                usesFloatingPointData = true;
            }

            bool readSamples (int* const* dest, int numDest, int startOff,
                              juce::int64, int num) override
            {
                for (int ch = 0; ch < numDest; ++ch)
                    if (dest[ch] != nullptr)
                        memset (reinterpret_cast<float*> (dest[ch]) + startOff, 0,
                                (size_t) num * sizeof (float));
                return true;
            }
        };

        DummyAudioReader dummy (sr, (unsigned int) sound.audio->getNumChannels());

        const juce::int64 bufTotal = sound.audio->getNumSamples();
        juce::int64 startSmp = 0;
        juce::int64 endSmp   = juce::jmax ((juce::int64) 1, bufTotal - 1);

        if (ps.startPointSeconds > 0.0 && bufTotal > 1)
            startSmp = juce::jlimit ((juce::int64) 0, endSmp - 1,
                                     (juce::int64) (ps.startPointSeconds * sr));
        if (ps.endPointSeconds > 0.0 && bufTotal > 1)
            endSmp = juce::jlimit (startSmp + 1, bufTotal - 1,
                                   (juce::int64) (ps.endPointSeconds * sr));

        juce::BigInteger noteRange;
        noteRange.setRange (0, 128, false);
        noteRange.setBit (note);

        auto* snd = new LoopingSamplerSound (ps.sampleFilePath.isEmpty()
                                                 ? juce::String ("seq")
                                                 : juce::File (ps.sampleFilePath).getFileName(),
                                             dummy, noteRange,
                                             note, 0.01, 0.1, 10.0);
        snd->fullAudioData = sound.audio;
        snd->baseTuningRatioAtomic.store (1.0f);

        trackRate[(size_t) trackIdx] = sr;

        // Loop / OneShot / trim / pitch / envelope all come from the pad settings,
        // in one place so a later Control panel edit can reapply exactly the same.
        applyShape (trackIdx, ps, *snd);

        engine.getSynthesiser().addSound (snd);
    }

    applyMix (trackIdx, ps);

    engine.muteOutput.store (false);
}

//==============================================================================
// The shape of the sound: what plays, and between which samples it loops.
inline void SequencerEngine::applyShape (int trackIdx, const PadSettings& ps,
                                         LoopingSamplerSound& snd)
{
    snd.loopEnabled.store (ps.loopEnabled);
    snd.oneShotEnabled.store (ps.oneShotEnabled);
    snd.pitchOffsetAtomic.store (ps.pitchCents);
    snd.customAdsrEnabled.store (ps.adsrEnabled);
    snd.customAdsrAttackMs.store (ps.adsrAttackMs);
    snd.customAdsrDecayMs.store (ps.adsrDecayMs);
    snd.customAdsrSustain.store (ps.adsrSustain);
    snd.customAdsrReleaseMs.store (ps.adsrReleaseMs);
    snd.reverseEnabled.store (ps.reverseEnabled);
    snd.bounceEnabled.store (ps.bounceEnabled);

    // The trim region is what a loop repeats between, so it has to be recomputed
    // whenever the Control panel's start/end points move -- otherwise a short
    // region set after the track was captured would simply be ignored.
    if (snd.fullAudioData != nullptr)
    {
        const juce::int64 bufTotal = snd.fullAudioData->getNumSamples();
        const double sr = trackRate[(size_t) trackIdx] > 0.0
                              ? trackRate[(size_t) trackIdx] : 44100.0;

        juce::int64 startSmp = 0;
        juce::int64 endSmp   = juce::jmax ((juce::int64) 1, bufTotal - 1);

        if (ps.startPointSeconds > 0.0 && bufTotal > 1)
            startSmp = juce::jlimit ((juce::int64) 0, endSmp - 1,
                                     (juce::int64) (ps.startPointSeconds * sr));
        if (ps.endPointSeconds > 0.0 && bufTotal > 1)
            endSmp = juce::jlimit (startSmp + 1, bufTotal - 1,
                                   (juce::int64) (ps.endPointSeconds * sr));

        snd.startSampleAtomic.store (startSmp);
        snd.endSampleAtomic.store (endSmp);
    }

    if (auto& slot = trackEngines[(size_t) trackIdx])
        slot->loopEnabled.store (ps.loopEnabled);
}

//==============================================================================
// The mix: EQ, pad gain, normalize, then the per-track fader on top.
inline void SequencerEngine::applyMix (int trackIdx, const PadSettings& ps)
{
    auto& engine = getOrCreateEngine (trackIdx);

    engine.eqActive.store (ps.eqEnabled);
    engine.eqFilterModes[0] = ps.eq1Mode;
    engine.eqFilterModes[1] = ps.eq2Mode;
    engine.eqFilterModes[2] = ps.eq3Mode;
    engine.padGain.store (juce::jlimit (0.0f, 2.0f, ps.padGain));

    {
        const double eqSr = engine.getSampleRate() > 0.0 ? engine.getSampleRate()
                                                         : juce::jmax (1.0, trackRate[(size_t) trackIdx]);
        PadAudioEngine::EqCoeffDoubleBuffer::Coeffs c[3];
        c[0] = engine.computeEqCoeffs (ps.eq1Freq, ps.eq1Gain, ps.eq1Q, ps.eq1Mode, eqSr);
        c[1] = engine.computeEqCoeffs (ps.eq2Freq, ps.eq2Gain, ps.eq2Q, ps.eq2Mode, eqSr);
        c[2] = engine.computeEqCoeffs (ps.eq3Freq, ps.eq3Gain, ps.eq3Q, ps.eq3Mode, eqSr);
        engine.eqCoeffDB.writeFromUI (c);
    }

    engine.normGain.store (ps.normEnabled ? engine.computeNormGainFromAudio (ps.normTargetDb)
                                          : 1.0f);

    soundLevel[(size_t) trackIdx] = juce::jlimit (0.0f, 1.0f, ps.volumeLevel);
    updateTrackGain (trackIdx);
}

inline void SequencerEngine::clearTrackSound (int trackIdx)
{
    if (trackIdx < 0 || trackIdx >= SeqPattern::kTracks) return;

    activeNote[(size_t) trackIdx] = -1;
    soundLevel[(size_t) trackIdx] = 1.0f;

    auto& slot = trackEngines[(size_t) trackIdx];
    if (slot == nullptr) return;

    slot->muteOutput.store (true);
    slot->clearActiveSoundFlags();
    slot->forceStopAllVoices();
    slot->clearSoundsAndVoices();
    slot->allNotesOff (0, false);
    {
        juce::ScopedLock lock (slot->sampleLock);
        slot->samples.clear();
        slot->selectedSampleIndex = 0;
    }
    slot->clearPeakCache();
    slot->muteOutput.store (false);
}

inline void SequencerEngine::setTrackVolume (int trackIdx, float volume)
{
    if (trackIdx < 0 || trackIdx >= SeqPattern::kTracks) return;
    trackLevel[(size_t) trackIdx] = juce::jlimit (0.0f, 1.0f, volume);
    updateTrackGain (trackIdx);
}

//==============================================================================
inline void SequencerEngine::setPattern (std::shared_ptr<const SeqPattern> p)
{
    // Mirror the PPQ so the metronome can convert the playhead to beats without
    // touching the message-thread pattern.
    ppqAtomic.store (p != nullptr && p->ppq > 0 ? p->ppq : 960, std::memory_order_relaxed);
    totalTicksAtomic.store (p != nullptr ? juce::jmax (1, p->totalTicks()) : 3840,
                            std::memory_order_relaxed);
    std::atomic_store (&pattern, std::move (p));
}

// Halt anything still sounding.  Used by stop() and by start(), so pressing Play
// always restarts from a clean slate instead of layering over the last run.
//
// forceStopAllVoices(), not allNotesOff(): one-shot voices deliberately ignore
// note-off (that is the point of OneShot), so a hard stop is the only way to
// silence them.
inline void SequencerEngine::silenceAllVoices()
{
    idleRenderBlocks = 0;

    for (int t = 0; t < SeqPattern::kTracks; ++t)
    {
        activeNote[(size_t) t]   = -1;
        gateCountdown[(size_t) t] = -1;
        if (auto& e = trackEngines[(size_t) t])
            e->forceStopAllVoices();   // one-shots ignore note-off, so hard-stop
    }
}

inline void SequencerEngine::start()
{
    silenceAllVoices();

    playheadPos = 0.0;
    playheadUI.store (0.0, std::memory_order_relaxed);

    for (int t = 0; t < SeqPattern::kTracks; ++t)
    {
        trackMidi[(size_t) t].clear();
        gateCountdown[(size_t) t] = -1;
    }

    playing.store (true, std::memory_order_relaxed);
}

inline void SequencerEngine::stop()
{
    playing.store (false, std::memory_order_relaxed);
    playheadPos = 0.0;
    playheadUI.store (0.0, std::memory_order_relaxed);

    // Same pattern the app uses elsewhere for stopping pad voices from the
    // message thread.
    silenceAllVoices();
}

// One hit = one note held for the length of its step, then released.  That is what
// makes a step a *duration*: a gated sample is cut at the step boundary, while a
// sample configured as OneShot ignores the release and rings on to its end -- the
// Control panel's setting, honoured here rather than assumed.
inline void SequencerEngine::triggerTrack (int trackIdx, int offset, float velocity,
                                           int gateSamples, int numSamples)
{
    auto& buf  = trackMidi[(size_t) trackIdx];
    const int note = trackNote[(size_t) trackIdx];
    const auto& idx = (size_t) trackIdx;

    // Retrigger: anything still held (or still waiting to be released) is let go
    // first, so notes cannot stack up or be cut by a stale gate.
    if (activeNote[idx] >= 0 || gateCountdown[idx] >= 0)
        buf.addEvent (juce::MidiMessage::noteOff (1, note), offset);

    buf.addEvent (juce::MidiMessage::noteOn (1, note,
                                             juce::jlimit (0.01f, 1.0f, velocity)),
                  offset);
    activeNote[idx] = note;

    if (gateSamples <= 0)
    {
        // Held until released -- what an audition does, so it lasts exactly as long
        // as the mouse is down.  A one-shot voice ignores the release and rings on.
        gateCountdown[idx] = -1;
        return;
    }

    const int offAt = offset + gateSamples;

    if (offAt < numSamples)
    {
        buf.addEvent (juce::MidiMessage::noteOff (1, note), offAt);
        activeNote[idx]    = -1;
        gateCountdown[idx] = -1;
    }
    else
    {
        gateNote[idx]      = note;
        gateCountdown[idx] = offAt - numSamples;   // carried into later blocks
    }
}

//==============================================================================
inline void SequencerEngine::releaseTrack (int trackIdx, int offset)
{
    const size_t idx = (size_t) trackIdx;

    if (activeNote[idx] >= 0 || gateCountdown[idx] >= 0)
        trackMidi[idx].addEvent (juce::MidiMessage::noteOff (1, trackNote[idx]), offset);

    activeNote[idx]    = -1;
    gateCountdown[idx] = -1;
}

//==============================================================================
inline void SequencerEngine::processBlock (int numSamples, juce::AudioBuffer<float>& mix,
                                           double bpm, double masterGain)
{
    auto snap = std::atomic_load (&pattern);
    const bool isPlaying = playing.load (std::memory_order_relaxed);

    // Collect audition requests first.  They have to sound with the transport
    // stopped, which is exactly when a pattern is being built, so they cannot sit
    // behind the "is it playing" guard.
    std::array<int, SeqPattern::kTracks> auditionVel {};
    bool anyAudition = false;

    for (int t = 0; t < SeqPattern::kTracks; ++t)
    {
        auditionVel[(size_t) t] = auditionRequest[(size_t) t].exchange (-1, std::memory_order_relaxed);
        if (auditionVel[(size_t) t] >= 0) anyAudition = true;
    }

    // While stopped the pool normally renders nothing, but an audition has to keep
    // sounding for as long as it is held -- and its release tail has to finish, or
    // it would be cut off mid-fade.  idleRenderBlocks keeps the engines running for
    // a moment after the last sound.
    bool heldNote = false;
    for (const auto& a : activeNote)
        if (a >= 0) { heldNote = true; break; }

    const bool mustRender = isPlaying || anyAudition || heldNote || idleRenderBlocks > 0;

    if ((snap == nullptr && ! anyAudition && ! heldNote && idleRenderBlocks <= 0) || ! mustRender)
        return;

    if (! isPlaying)
        idleRenderBlocks = (anyAudition || heldNote) ? kIdleTailBlocks : idleRenderBlocks - 1;

    const int nCh = mix.getNumChannels();
    if (nCh <= 0 || numSamples <= 0) return;

    if (poolBuffer.getNumChannels() < nCh || poolBuffer.getNumSamples() < numSamples)
        poolBuffer.setSize (nCh, numSamples, false, true, true);   // only on block-size growth

    poolBuffer.clear (0, numSamples);

    const double ppq      = (snap != nullptr && snap->ppq > 0) ? (double) snap->ppq : 960.0;
    const double totalTks = snap != nullptr ? (double) snap->totalTicks() : 3840.0;
    const double safeBpm  = juce::jlimit (20.0, 300.0, bpm);

    if (totalTks <= 0.0)
        return;

    const double ticksPerSample = (safeBpm / 60.0) * ppq / sampleRate;

    for (auto& m : trackMidi)
        m.clear();

    // A step is a duration, and an audition gets one step's worth of gate too.
    const int gateSamples = juce::jmax (1, (int) std::llround (
                                (double) juce::jmax (1, snap != nullptr ? snap->ticksPerStep() : 240)
                                / juce::jmax (1.0e-9, ticksPerSample)));

    const double blockStart = playheadPos;
    const double blockEnd   = playheadPos + ticksPerSample * (double) numSamples;

    // ---- Pattern playback (only while the transport runs) ----
    if (isPlaying && snap != nullptr)
    {
    for (int t = 0; t < SeqPattern::kTracks; ++t)
    {
        auto& left = gateCountdown[(size_t) t];
        if (left < 0) continue;

        if (left < numSamples)
        {
            trackMidi[(size_t) t].addEvent (juce::MidiMessage::noteOff (1, gateNote[(size_t) t]),
                                            juce::jlimit (0, numSamples - 1, left));
            activeNote[(size_t) t] = -1;
            left = -1;
        }
        else
        {
            left -= numSamples;
        }
    }

    // Collect this block's hits, wrapping at the pattern end if it crosses.
    auto emitRange = [&] (double from, double to, double virtualBase)
    {
        for (int t = 0; t < SeqPattern::kTracks; ++t)
        {
            const auto& tr = snap->tracks[(size_t) t];
            if (tr.mute || tr.hits.empty()) continue;

            for (const auto& h : tr.hits)
            {
                const double tick = (double) h.tick;
                if (tick < from || tick >= to) continue;

                const double vTick  = tick + virtualBase;
                int offset = (int) std::floor ((vTick - blockStart) / ticksPerSample);
                offset = juce::jlimit (0, juce::jmax (0, numSamples - 1), offset);

                triggerTrack (t, offset, h.velocity, gateSamples, numSamples);
            }
        }
    };

    if (blockEnd <= totalTks)
    {
        emitRange (blockStart, blockEnd, 0.0);
    }
    else
    {
        emitRange (blockStart, totalTks, 0.0);
        if (blockEnd - totalTks > 0.0)
            emitRange (0.0, blockEnd - totalTks, totalTks);
    }

    // Advance and wrap.  Only while playing: an audition must not move the
    // playhead, or previewing a step would shift the pattern underneath you.
    playheadPos = blockEnd;
    while (playheadPos >= totalTks)
        playheadPos -= totalTks;

    playheadUI.store (juce::jmax (0.0, playheadPos), std::memory_order_relaxed);
    }   // end of pattern playback

    // ---- Auditions: a previewed step sounds whether or not the transport runs ----
    for (int t = 0; t < SeqPattern::kTracks; ++t)
    {
        const int vel = auditionVel[(size_t) t];
        if (vel < 0 || trackEngines[(size_t) t] == nullptr) continue;

        if (vel == 0)
            releaseTrack (t, 0);                       // mouse up
        else
            triggerTrack (t, 0, (float) vel / 127.0f,  // mouse down: hold it
                          /*gateSamples=*/0, numSamples);
    }

    // Render every track engine that exists into the private pool buffer.
    for (int t = 0; t < SeqPattern::kTracks; ++t)
    {
        auto& e = trackEngines[(size_t) t];
        if (e == nullptr) continue;
        e->renderNextBlock (poolBuffer, trackMidi[(size_t) t], 0, numSamples);
    }

    // Master volume applies here, exactly as the metronome beep applies it: the
    // pad mix already carries it, so scaling the shared buffer would double it.
    // The pool's own master sits on top, to balance Seq against the pads.
    const float poolGain = (float) masterGain * masterVolume.load (std::memory_order_relaxed);
    if (poolGain != 1.0f)
        poolBuffer.applyGain (0, numSamples, poolGain);

    for (int ch = 0; ch < nCh; ++ch)
        mix.addFrom (ch, 0, poolBuffer, ch, 0, numSamples);
}
