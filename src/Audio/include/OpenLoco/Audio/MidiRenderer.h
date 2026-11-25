#pragma once

#include "AudioFormat.h"
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

// OpenGraphics fork: MIDI music support. A standard MIDI file is rendered with a SoundFont
// (TinySoundFont) into 16-bit stereo PCM, which is then played like any other music sample.
namespace OpenLoco::Audio
{
    struct RenderedAudio
    {
        AudioFormat format{};
        std::vector<uint8_t> pcm; // signed 16-bit little-endian, interleaved stereo
    };

    // True if the data starts with a standard MIDI file header ("MThd").
    bool isMidiData(std::span<const uint8_t> data);

    // Renders a standard MIDI file with the given SoundFont (.sf2) and normalises its loudness
    // to roughly match the original game's music. Safe to call from a worker thread.
    // Throws std::runtime_error if the MIDI file or the SoundFont cannot be loaded.
    RenderedAudio renderMidi(std::span<const uint8_t> midi, const std::filesystem::path& soundFont, uint32_t sampleRate = 22050);
}
