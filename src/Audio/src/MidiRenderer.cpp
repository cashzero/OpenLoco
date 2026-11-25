#include <OpenLoco/Audio/MidiRenderer.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <memory>
#include <stdexcept>

#define TSF_IMPLEMENTATION
#include <tsf.h>
#define TML_IMPLEMENTATION
#include <tml.h>

namespace OpenLoco::Audio
{
    // Loudness of the original game's music tracks (RMS of the 16-bit PCM, full scale = 1).
    static constexpr float kTargetRms = 0.16f;
    // Peaks above this level are softly compressed so that the output never clips.
    static constexpr float kLimiterThreshold = 0.8f;
    // Do not push peaks further than this into the limiter.
    static constexpr float kMaxPeakAfterGain = 1.6f;
    // Release tail rendered after the last MIDI event, at most.
    static constexpr float kMaxTailSeconds = 3.0f;
    static constexpr int kBlockFrames = 64;
    static constexpr int kDrumChannel = 9;

    bool isMidiData(std::span<const uint8_t> data)
    {
        return data.size() >= 4 && data[0] == 'M' && data[1] == 'T' && data[2] == 'h' && data[3] == 'd';
    }

    static std::vector<char> readFile(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            throw std::runtime_error("unable to open " + path.string());
        }
        return std::vector<char>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    static float softLimit(float x)
    {
        const float a = std::fabs(x);
        if (a <= kLimiterThreshold)
        {
            return x;
        }
        const float range = 1.0f - kLimiterThreshold;
        const float y = kLimiterThreshold + range * std::tanh((a - kLimiterThreshold) / range);
        return std::copysign(y, x);
    }

    RenderedAudio renderMidi(std::span<const uint8_t> midi, const std::filesystem::path& soundFont, uint32_t sampleRate)
    {
        const auto fontData = readFile(soundFont);
        std::unique_ptr<tsf, decltype(&tsf_close)> synth(tsf_load_memory(fontData.data(), static_cast<int>(fontData.size())), &tsf_close);
        if (synth == nullptr)
        {
            throw std::runtime_error("invalid SoundFont " + soundFont.string());
        }
        std::unique_ptr<tml_message, decltype(&tml_free)> messages(tml_load_memory(midi.data(), static_cast<int>(midi.size())), &tml_free);
        if (messages == nullptr)
        {
            throw std::runtime_error("invalid MIDI file");
        }

        tsf* sf = synth.get();
        tsf_set_output(sf, TSF_STEREO_INTERLEAVED, static_cast<int>(sampleRate), 0.0f);
        for (int channel = 0; channel < 16; channel++)
        {
            tsf_channel_set_presetnumber(sf, channel, 0, channel == kDrumChannel);
        }

        std::vector<float> samples;
        float block[kBlockFrames * 2];
        const double msPerBlock = kBlockFrames * 1000.0 / sampleRate;
        const int maxTailBlocks = static_cast<int>(kMaxTailSeconds * sampleRate / kBlockFrames);
        double now = 0.0;
        int tailBlocks = 0;
        const tml_message* msg = messages.get();
        while (true)
        {
            for (; msg != nullptr && now >= msg->time; msg = msg->next)
            {
                switch (msg->type)
                {
                    case TML_PROGRAM_CHANGE:
                        tsf_channel_set_presetnumber(sf, msg->channel, msg->program, msg->channel == kDrumChannel);
                        break;
                    case TML_NOTE_ON:
                        tsf_channel_note_on(sf, msg->channel, msg->key, msg->velocity / 127.0f);
                        break;
                    case TML_NOTE_OFF:
                        tsf_channel_note_off(sf, msg->channel, msg->key);
                        break;
                    case TML_PITCH_BEND:
                        tsf_channel_set_pitchwheel(sf, msg->channel, msg->pitch_bend);
                        break;
                    case TML_CONTROL_CHANGE:
                        tsf_channel_midi_control(sf, msg->channel, msg->control, msg->control_value);
                        break;
                    default:
                        break;
                }
            }
            if (msg == nullptr && (tsf_active_voice_count(sf) == 0 || ++tailBlocks > maxTailBlocks))
            {
                break;
            }
            tsf_render_float(sf, block, kBlockFrames, 0);
            samples.insert(samples.end(), std::begin(block), std::end(block));
            now += msPerBlock;
        }

        // Normalise loudness, then soft-limit the peaks.
        double sumSquares = 0.0;
        float peak = 0.0f;
        for (float s : samples)
        {
            sumSquares += static_cast<double>(s) * s;
            peak = std::max(peak, std::fabs(s));
        }
        const float rms = samples.empty() ? 0.0f : static_cast<float>(std::sqrt(sumSquares / samples.size()));
        float gain = 1.0f;
        if (rms > 0.0f)
        {
            gain = std::min(kTargetRms / rms, kMaxPeakAfterGain / peak);
        }

        RenderedAudio result;
        result.format.sampleRate = sampleRate;
        result.format.channels = 2;
        result.format.bitsPerSample = 16;
        result.pcm.resize(samples.size() * sizeof(int16_t));
        auto* out = reinterpret_cast<int16_t*>(result.pcm.data());
        for (size_t i = 0; i < samples.size(); i++)
        {
            out[i] = static_cast<int16_t>(std::lround(softLimit(samples[i] * gain) * 32767.0f));
        }
        return result;
    }
}
