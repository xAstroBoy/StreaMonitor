#pragma once

// ─────────────────────────────────────────────────────────────────
// StreaMonitor C++ — Custom spdlog sink for ImGui log panel
// Buffers all spdlog messages so the GUI can drain & display them.
// ─────────────────────────────────────────────────────────────────

#include "core/types.h"
#include <spdlog/sinks/base_sink.h>
#include <spdlog/details/log_msg.h>
#include <string>
#include <vector>
#include <deque>
#include <mutex>

namespace sm
{

    class ImGuiLogSink : public spdlog::sinks::base_sink<std::mutex>
    {
    public:
        struct Entry
        {
            std::string level;   // "info", "warn", "error", "debug", "trace", "critical"
            std::string source;  // logger name (e.g. "sm", "CB/username")
            std::string message; // the actual log text
            TimePoint time;
        };

        ImGuiLogSink() = default;

        /// Global singleton so per-plugin loggers can include this sink.
        static void setInstance(std::shared_ptr<ImGuiLogSink> sink) { instance_ = std::move(sink); }
        static std::shared_ptr<ImGuiLogSink> instance() { return instance_; }

        /// Drain all buffered entries (call from GUI thread each frame).
        /// Returns the entries accumulated since the last drain.
        std::vector<Entry> drain()
        {
            std::lock_guard<std::mutex> lock(bufMutex_);
            std::vector<Entry> result;
            result.swap(buffer_);
            return result;
        }

        /// How many entries are currently buffered (approximate).
        size_t pending() const
        {
            std::lock_guard<std::mutex> lock(bufMutex_);
            return buffer_.size();
        }

    protected:
        // Called by spdlog with the base_sink mutex held — we use our own
        // separate bufMutex_ so drain() never blocks spdlog callers.
        void sink_it_(const spdlog::details::log_msg &msg) override
        {
            Entry e;
            auto sv = spdlog::level::to_string_view(msg.level);
            e.level.assign(sv.data(), sv.size());
            e.source.assign(msg.logger_name.data(), msg.logger_name.size());

            // Strip the internal uniqueness suffix (e.g. "_1234567890")
            // that SitePlugin appends for spdlog registry uniqueness.
            // The suffix is "_" followed by a decimal pointer address.
            auto pos = e.source.rfind('_');
            if (pos != std::string::npos && pos > 0 && pos + 1 < e.source.size())
            {
                bool allDigits = true;
                for (size_t i = pos + 1; i < e.source.size(); ++i)
                {
                    if (!std::isdigit(static_cast<unsigned char>(e.source[i])))
                    {
                        allDigits = false;
                        break;
                    }
                }
                if (allDigits && (e.source.size() - pos) > 4) // at least 4 digits
                    e.source.erase(pos);
            }

            e.message.assign(msg.payload.data(), msg.payload.size());
            e.time = Clock::now();

            std::lock_guard<std::mutex> lock(bufMutex_);
            buffer_.push_back(std::move(e));
            // Cap the buffer so we don't eat RAM if the GUI isn't draining
            while (buffer_.size() > kMaxBuffer)
                buffer_.erase(buffer_.begin());
        }

        void flush_() override {}

    private:
        static constexpr size_t kMaxBuffer = 20000;
        mutable std::mutex bufMutex_;
        std::vector<Entry> buffer_;
        static inline std::shared_ptr<ImGuiLogSink> instance_;
    };

} // namespace sm
