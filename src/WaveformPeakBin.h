#pragma once

// ===========================================================================
// Pre-computed waveform peak data.
// One bin covers (totalSamples / kWaveformPeakBins) audio samples.
// Computed on the background loading thread so WaveformComponent::paint()
// has zero disk I/O, eliminating multi-second message-thread freezes.
//
// Defined in its own header so both SampleCard.h and PadAudioEngine.h can
// include it without introducing a circular dependency.
// ===========================================================================

struct WaveformPeakBin
{
    float minL = 0.0f, maxL = 0.0f;
    float minR = 0.0f, maxR = 0.0f;
};

static constexpr int kWaveformPeakBins = 8192;
