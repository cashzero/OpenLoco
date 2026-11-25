#include "Audio/Audio.h"
#include "Config.h"
#include "Date.h"
#include "Jukebox.h"
#include "SceneManager.h"
#include "Ui/WindowManager.h"
#include <OpenLoco/Audio/AudioEngine.h>
#include <optional>

using namespace OpenLoco::Environment;

namespace OpenLoco::Audio
{
    static AudioHandle _musicHandle = AudioHandle::null;

    // OpenGraphics fork：正在背景合成（MIDI）的音樂，合成完成後由 updatePendingMusic 開始播放
    struct PendingMusic
    {
        PathId sample;
        int32_t volume;
        bool loop;
    };
    static std::optional<PendingMusic> _pendingMusic;

    static void startMusic(BufferId buffer, int32_t volume, bool loop)
    {
        AudioAttributes attribs{};
        attribs.volume = volume;
        attribs.loop = loop;
        _musicHandle = create(buffer, ChannelId::music, attribs);
        Audio::play(_musicHandle);
    }

    void revalidateCurrentTrack()
    {
        const auto currentTrack = Jukebox::getCurrentTrack();
        if (currentTrack == Jukebox::kNoSong)
        {
            return;
        }

        using Config::MusicPlaylistType;
        const auto& cfg = Config::get();

        bool trackStillApplies = true;
        switch (cfg.audio.playlist)
        {
            case MusicPlaylistType::currentEra:
            {
                auto currentYear = getCurrentYear();
                const auto& info = Jukebox::getMusicInfo(currentTrack);
                if (currentYear < info.startYear || currentYear > info.endYear)
                {
                    trackStillApplies = false;
                }
                break;
            }

            case MusicPlaylistType::all:
                return;

            case MusicPlaylistType::custom:
                if (!cfg.audio.customJukebox[currentTrack])
                {
                    trackStillApplies = false;
                }
                break;
        }

        if (!trackStillApplies)
        {
            stopMusic();
            Jukebox::resetJukebox();
        }
    }

    void playBackgroundMusic()
    {
        auto& cfg = Config::get();
        if (cfg.audio.playJukeboxMusic == 0 || !isAudioEnabled() || SceneManager::isTitleMode() || SceneManager::isEditorMode() || SceneManager::isPaused())
        {
            return;
        }

        bool musicPlaying = _musicHandle != AudioHandle::null && isPlaying(_musicHandle);
        if (!musicPlaying && !_pendingMusic) // 合成中的曲目還沒開始播放時不換曲
        {
            const auto& mi = Jukebox::changeTrack();
            playMusic(mi.pathId, cfg.audio.mainVolume, false);
            Ui::WindowManager::invalidate(Ui::WindowType::musicJukebox);
        }
    }

    void playMusic(PathId sample, int32_t volume, bool loop)
    {
        if (!isAudioEnabled())
        {
            return;
        }

        if (_musicHandle != AudioHandle::null)
        {
            destroy(_musicHandle);
            _musicHandle = AudioHandle::null;
        }
        _pendingMusic.reset();

        auto musicSample = loadMusicSample(sample);
        if (musicSample.has_value())
        {
            startMusic(*musicSample, volume, loop);
        }
        else if (isMusicSampleLoading(sample))
        {
            _pendingMusic = PendingMusic{ sample, volume, loop };
        }
    }

    void updatePendingMusic()
    {
        if (!_pendingMusic)
        {
            return;
        }
        const auto pending = *_pendingMusic;
        auto musicSample = loadMusicSample(pending.sample);
        if (musicSample.has_value())
        {
            _pendingMusic.reset();
            startMusic(*musicSample, pending.volume, pending.loop);
            Ui::WindowManager::invalidate(Ui::WindowType::musicJukebox);
        }
        else if (!isMusicSampleLoading(pending.sample))
        {
            _pendingMusic.reset(); // 合成失敗：點唱機會在下一個 tick 換下一首
        }
    }

    void resetMusic()
    {
        stopMusic();
        Jukebox::resetJukebox();
    }

    void stopMusic()
    {
        _pendingMusic.reset();
        if (_musicHandle != AudioHandle::null)
        {
            destroy(_musicHandle);
            _musicHandle = AudioHandle::null;
        }
    }

    void pauseMusic()
    {
        if (_musicHandle != AudioHandle::null && isPlaying(_musicHandle))
        {
            pause(_musicHandle);
        }
    }

    void unpauseMusic()
    {
        if (_musicHandle != AudioHandle::null && isPaused(_musicHandle))
        {
            unpause(_musicHandle);
        }
    }

    void setBgmVolume(int32_t volume)
    {
        auto& cfg = Config::get().audio;
        if (cfg.mainVolume == volume)
        {
            return;
        }

        cfg.mainVolume = volume;
        Config::write();

        if (_musicHandle != AudioHandle::null && Jukebox::getCurrentTrack() != Jukebox::kNoSong)
        {
            setVolume(_musicHandle, volume);
        }
    }
}
