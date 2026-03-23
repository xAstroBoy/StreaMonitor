#pragma once
// ─────────────────────────────────────────────────────────────────
// mkv_edit — In-place Matroska/EBML editor
// Zero external dependencies.  Pure binary I/O on the MKV file.
// Edits metadata, attachments, and track headers WITHOUT re-reading
// or re-writing the video/audio data (instant, even on 100 GB files).
//
// Approach (same as mkvpropedit):
//   1. Parse the EBML structure to build a position index.
//   2. Read the target L1 element (Tags/Tracks/Attachments) into RAM.
//   3. Parse its EBML sub-tree, modify in memory, serialize back.
//   4. If new bytes fit in old space → overwrite in-place + Void pad.
//      If not → Void old position, append at file end.
//   5. Update Segment size if it was finite and we grew the file.
// ─────────────────────────────────────────────────────────────────

#include <string>
#include <functional>

namespace mkv
{
    using LogCb = std::function<void(const std::string &)>;

    // Write a global MKV tag (key=value) in-place.
    // Preserves all existing tags, adds/updates the specified one.
    bool writeTag(const std::string &mkvPath,
                  const std::string &key,
                  const std::string &value,
                  LogCb log = nullptr);

    // Add or replace cover art (JPEG attachment named "cover.jpg") in-place.
    bool setCoverArt(const std::string &mkvPath,
                     const std::string &jpegPath,
                     LogCb log = nullptr);

    // Fix existing cover attachment metadata (name→"cover.jpg", desc→"cover").
    bool fixCoverMeta(const std::string &mkvPath,
                      LogCb log = nullptr);

    // Set VR 180° SBS spatial metadata on first video track:
    //   StereoMode=1  (side-by-side, left eye first)
    //   Projection { Type=1 equirectangular, Yaw/Pitch/Roll=0 }
    bool setVR180SBS(const std::string &mkvPath,
                     LogCb log = nullptr);

} // namespace mkv
