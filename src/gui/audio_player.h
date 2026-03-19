#pragma once

// ─────────────────────────────────────────────────────────────────
// StreaMonitor C++ — Audio player for live stream preview
// Uses miniaudio for cross-platform audio output.
// Receives PCM samples (f32, stereo, 48kHz) from the HLS recorder
// and plays them in real-time with a small ring buffer.
// ─────────────────────────────────────────────────────────────────

#include <cstdint>
#include <atomic>
#include <mutex>
#include <vector>
#include <string>

// Forward-declare miniaudio types (avoid pulling giant header into every TU)
struct ma_device;
struct ma_device_config;

namespace sm
{

    class AudioPlayer
    {
    public:
        AudioPlayer();
        ~AudioPlayer();

        // Non-copyable
        AudioPlayer(const AudioPlayer &) = delete;
        AudioPlayer &operator=(const AudioPlayer &) = delete;

        // Initialize the audio device (call once at startup)
        bool init(int sampleRate = 48000, int channels = 2);

        // Start/stop playback
        void start();
        void stop();
        bool isPlaying() const { return playing_.load(); }

        // Push PCM float samples (interleaved stereo, [-1,1] range).
        // Called from the recorder thread — must be fast.
        void pushSamples(const float *data, size_t frameCount);

        // Set volume (0.0 = mute, 1.0 = full)
        void setVolume(float vol) { volume_.store(vol); }
        float getVolume() const { return volume_.load(); }

        // Clear the ring buffer (e.g., when switching streams)
        void flush();

    private:
        // miniaudio callback (static → instance)
        static void dataCallback(ma_device *pDevice, void *pOutput,
                                 const void *pInput, uint32_t frameCount);
        void fillBuffer(float *output, uint32_t frameCount);

        // Ring buffer for audio samples (interleaved float stereo)
        static constexpr size_t RING_CAPACITY = 48000 * 2 * 2; // ~2 seconds of stereo at 48kHz
        std::vector<float> ring_;
        size_t ringWrite_ = 0;
        size_t ringRead_ = 0;
        std::mutex ringMutex_;

        // Device state
        ma_device *device_ = nullptr;
        std::atomic<bool> playing_{false};
        std::atomic<bool> initialized_{false};
        std::atomic<float> volume_{0.8f};
        int sampleRate_ = 48000;
        int channels_ = 2;
    };

} // namespace sm
