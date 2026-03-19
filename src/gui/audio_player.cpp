// ─────────────────────────────────────────────────────────────────
// StreaMonitor C++ — Audio player implementation (miniaudio)
// ─────────────────────────────────────────────────────────────────

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "gui/audio_player.h"
#include <algorithm>
#include <cstring>

namespace sm
{

    AudioPlayer::AudioPlayer()
    {
        ring_.resize(RING_CAPACITY, 0.0f);
    }

    AudioPlayer::~AudioPlayer()
    {
        stop();
        if (device_)
        {
            ma_device_uninit(device_);
            delete device_;
            device_ = nullptr;
        }
    }

    bool AudioPlayer::init(int sampleRate, int channels)
    {
        if (initialized_.load())
            return true;

        sampleRate_ = sampleRate;
        channels_ = channels;

        device_ = new ma_device();

        ma_device_config config = ma_device_config_init(ma_device_type_playback);
        config.playback.format = ma_format_f32;
        config.playback.channels = static_cast<ma_uint32>(channels);
        config.sampleRate = static_cast<ma_uint32>(sampleRate);
        config.dataCallback = &AudioPlayer::dataCallback;
        config.pUserData = this;
        config.periodSizeInFrames = 1024; // Low latency (~21ms at 48kHz)

        if (ma_device_init(nullptr, &config, device_) != MA_SUCCESS)
        {
            delete device_;
            device_ = nullptr;
            return false;
        }

        initialized_.store(true);
        return true;
    }

    void AudioPlayer::start()
    {
        if (!initialized_.load() || playing_.load())
            return;
        if (ma_device_start(device_) == MA_SUCCESS)
            playing_.store(true);
    }

    void AudioPlayer::stop()
    {
        if (!initialized_.load() || !playing_.load())
            return;
        ma_device_stop(device_);
        playing_.store(false);
    }

    void AudioPlayer::pushSamples(const float *data, size_t frameCount)
    {
        if (!initialized_.load())
            return;

        size_t sampleCount = frameCount * static_cast<size_t>(channels_);

        std::lock_guard lock(ringMutex_);
        for (size_t i = 0; i < sampleCount; ++i)
        {
            ring_[ringWrite_] = data[i];
            ringWrite_ = (ringWrite_ + 1) % RING_CAPACITY;
            // If write catches up to read, advance read (drop oldest samples)
            if (ringWrite_ == ringRead_)
                ringRead_ = (ringRead_ + 1) % RING_CAPACITY;
        }
    }

    void AudioPlayer::flush()
    {
        std::lock_guard lock(ringMutex_);
        ringRead_ = ringWrite_ = 0;
        std::fill(ring_.begin(), ring_.end(), 0.0f);
    }

    void AudioPlayer::dataCallback(ma_device *pDevice, void *pOutput,
                                   const void * /*pInput*/, uint32_t frameCount)
    {
        auto *self = static_cast<AudioPlayer *>(pDevice->pUserData);
        self->fillBuffer(static_cast<float *>(pOutput), frameCount);
    }

    void AudioPlayer::fillBuffer(float *output, uint32_t frameCount)
    {
        size_t sampleCount = static_cast<size_t>(frameCount) * channels_;
        float vol = volume_.load();

        std::lock_guard lock(ringMutex_);
        for (size_t i = 0; i < sampleCount; ++i)
        {
            if (ringRead_ != ringWrite_)
            {
                output[i] = ring_[ringRead_] * vol;
                ringRead_ = (ringRead_ + 1) % RING_CAPACITY;
            }
            else
            {
                output[i] = 0.0f; // underrun — silence
            }
        }
    }

} // namespace sm
