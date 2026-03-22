#pragma once

// ─────────────────────────────────────────────────────────────────
// StreaMonitor C++ — Async Image Loader + OpenGL Texture Cache
// Downloads JPEG/PNG from URL → decodes → uploads to GL texture
// Used for model preview thumbnails in the ImGui GUI
// ─────────────────────────────────────────────────────────────────

#include <string>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <atomic>
#include <vector>
#include <chrono>
#include <functional>
#include <deque>

#ifdef _WIN32
#include <Windows.h>
#endif
#include <GL/gl.h>

// GL_CLAMP_TO_EDGE is OpenGL 1.2+ — not in the Windows gl.h
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

namespace sm
{

    class ImageCache
    {
    public:
        ImageCache() = default;
        ~ImageCache();

        ImageCache(const ImageCache &) = delete;
        ImageCache &operator=(const ImageCache &) = delete;

        // Request a texture for the given URL.
        // Returns 0 if not yet loaded (will start async download).
        // Returns the GL texture ID if already cached.
        // Call once per frame from the render thread.
        GLuint getTexture(const std::string &url);

        // Must be called from the GL thread each frame to upload any
        // decoded images that arrived from background threads.
        void uploadPending();

        // Invalidate a specific URL (force re-download next request)
        void invalidate(const std::string &url);

        // Clear all cached textures
        void clear();

        // Number of textures currently cached
        size_t size() const;

    private:
        enum class State
        {
            Loading,
            Ready,
            Failed
        };

        struct Entry
        {
            State state = State::Loading;
            GLuint textureId = 0;
            int width = 0;
            int height = 0;
            std::chrono::steady_clock::time_point loadedAt;
        };

        // Pending upload from background thread
        struct PendingUpload
        {
            std::string url;
            std::vector<unsigned char> pixels; // RGBA
            int width = 0;
            int height = 0;
        };

        void downloadAndDecode(const std::string &url);

        mutable std::mutex mutex_;
        std::unordered_map<std::string, Entry> cache_;

        std::mutex uploadMutex_;
        std::deque<PendingUpload> pendingUploads_;

        // Textures queued for deletion — must be freed on GL thread
        std::mutex deleteMutex_;
        std::vector<GLuint> pendingDeletes_;

        // Concurrency limiter for download threads
        std::atomic<int> activeDownloads_{0};

        // Shutdown flag — set before destruction to prevent detached threads
        // from accessing destroyed mutexes / members after ImageCache dies.
        std::atomic<bool> shuttingDown_{false};
    };

} // namespace sm
