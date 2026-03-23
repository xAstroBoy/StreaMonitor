// ─────────────────────────────────────────────────────────────────
// mkv_edit.cpp — In-place Matroska / EBML editor
// ─────────────────────────────────────────────────────────────────
// All-in-one:  reads / writes raw EBML bytes.
// No libebml, no libmatroska, no mkvpropedit, no remux.
// ─────────────────────────────────────────────────────────────────

#include "utils/mkv_edit.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#define FSEEK64 _fseeki64
#define FTELL64 _ftelli64
#else
#define FSEEK64 fseeko
#define FTELL64 ftello
#endif

// ═════════════════════════════════════════════════════════════════
//  § 1.  Matroska EBML element IDs
// ═════════════════════════════════════════════════════════════════

namespace
{
    // --- Level-0 / Level-1 --------------------------------------------------
    constexpr uint32_t ID_EBML = 0x1A45DFA3;
    constexpr uint32_t ID_SEGMENT = 0x18538067;
    constexpr uint32_t ID_SEEKHEAD = 0x114D9B74;
    constexpr uint32_t ID_INFO = 0x1549A966;
    constexpr uint32_t ID_TRACKS = 0x1654AE6B;
    constexpr uint32_t ID_CLUSTER = 0x1F43B675;
    constexpr uint32_t ID_CUES = 0x1C53BB6B;
    constexpr uint32_t ID_ATTACHMENTS = 0x1941A469;
    constexpr uint32_t ID_TAGS = 0x1254C367;
    constexpr uint32_t ID_CHAPTERS = 0x1043A770;
    constexpr uint32_t ID_VOID = 0xEC;

    // --- SeekHead children ---------------------------------------------------
    constexpr uint32_t ID_SEEK = 0x4DBB;
    constexpr uint32_t ID_SEEK_ID = 0x53AB;
    constexpr uint32_t ID_SEEK_POS = 0x53AC;

    // --- Tags children -------------------------------------------------------
    constexpr uint32_t ID_TAG = 0x7373;
    constexpr uint32_t ID_TARGETS = 0x63C0;
    constexpr uint32_t ID_SIMPLE_TAG = 0x67C8;
    constexpr uint32_t ID_TAG_NAME = 0x45A3;
    constexpr uint32_t ID_TAG_STRING = 0x4487;
    constexpr uint32_t ID_TAG_LANG = 0x447A;

    // --- Attachment children -------------------------------------------------
    constexpr uint32_t ID_ATTACHED_FILE = 0x61A7;
    constexpr uint32_t ID_FILE_DESC = 0x467E;
    constexpr uint32_t ID_FILE_NAME = 0x466E;
    constexpr uint32_t ID_FILE_MIME = 0x4660;
    constexpr uint32_t ID_FILE_DATA = 0x465C;
    constexpr uint32_t ID_FILE_UID = 0x46AE;

    // --- Track children ------------------------------------------------------
    constexpr uint32_t ID_TRACK_ENTRY = 0xAE;
    constexpr uint32_t ID_TRACK_NUM = 0xD7;
    constexpr uint32_t ID_TRACK_TYPE = 0x83;
    constexpr uint32_t ID_TRACK_UID = 0x73C5;
    constexpr uint32_t ID_CODEC_ID = 0x86;
    constexpr uint32_t ID_VIDEO = 0xE0;
    constexpr uint32_t ID_AUDIO = 0xE1;
    constexpr uint32_t ID_STEREO_MODE = 0x53B8;
    constexpr uint32_t ID_PROJECTION = 0x7670;
    constexpr uint32_t ID_PROJ_TYPE = 0x7671;
    constexpr uint32_t ID_PROJ_PRIVATE = 0x7672;
    constexpr uint32_t ID_PROJ_YAW = 0x7673;
    constexpr uint32_t ID_PROJ_PITCH = 0x7674;
    constexpr uint32_t ID_PROJ_ROLL = 0x7675;

    // Which IDs are Master (container) elements?
    static const std::unordered_set<uint32_t> MASTER_IDS = {
        ID_EBML,
        ID_SEGMENT,
        ID_SEEKHEAD,
        ID_SEEK,
        ID_INFO,
        ID_TRACKS,
        ID_TRACK_ENTRY,
        ID_VIDEO,
        ID_AUDIO,
        ID_PROJECTION,
        ID_TAGS,
        ID_TAG,
        ID_TARGETS,
        ID_SIMPLE_TAG,
        ID_ATTACHMENTS,
        ID_ATTACHED_FILE,
        ID_CLUSTER,
        ID_CUES,
        ID_CHAPTERS,
        0x6D80,
        0x6240,
        0x5034,
        0x5035, // ContentEncodings chain
    };

    // ═════════════════════════════════════════════════════════════════
    //  § 2.  VINT (Variable-length Integer) utilities
    // ═════════════════════════════════════════════════════════════════

    // Length of a VINT from its first byte.
    inline int vintLen(uint8_t b)
    {
        if (b & 0x80)
            return 1;
        if (b & 0x40)
            return 2;
        if (b & 0x20)
            return 3;
        if (b & 0x10)
            return 4;
        if (b & 0x08)
            return 5;
        if (b & 0x04)
            return 6;
        if (b & 0x02)
            return 7;
        if (b & 0x01)
            return 8;
        return 0; // invalid
    }

    // Length of an EBML ID value (already decoded).
    inline int idEncodedLen(uint32_t id)
    {
        if (id <= 0xFF)
            return 1;
        if (id <= 0xFFFF)
            return 2;
        if (id <= 0xFFFFFF)
            return 3;
        return 4;
    }

    // Encode an EBML element ID into bytes. Returns byte count.
    inline int encodeId(uint32_t id, uint8_t *out)
    {
        int len = idEncodedLen(id);
        for (int i = len - 1; i >= 0; --i)
        {
            out[i] = id & 0xFF;
            id >>= 8;
        }
        return len;
    }

    // Minimum VINT length needed to encode a size value.
    inline int vintMinLen(int64_t val)
    {
        if (val < 0x7F)
            return 1;
        if (val < 0x3FFF)
            return 2;
        if (val < 0x1FFFFF)
            return 3;
        if (val < 0x0FFFFFFF)
            return 4;
        if (val < 0x07FFFFFFFFLL)
            return 5;
        if (val < 0x03FFFFFFFFFFLL)
            return 6;
        if (val < 0x01FFFFFFFFFFFFLL)
            return 7;
        return 8;
    }

    // Encode a size value as a VINT with a given byte length.
    // Returns the byte count (== len).
    inline int encodeVintSize(int64_t val, int len, uint8_t *out)
    {
        for (int i = len - 1; i > 0; --i)
        {
            out[i] = val & 0xFF;
            val >>= 8;
        }
        out[0] = static_cast<uint8_t>((0x100 >> len) | (val & 0xFF));
        return len;
    }

    // Encode a size value with minimum bytes.
    inline int encodeVintSizeOpt(int64_t val, uint8_t *out)
    {
        return encodeVintSize(val, vintMinLen(val), out);
    }

    // Read a VINT ID from a buffer. Returns the raw ID (marker bits kept).
    inline uint32_t readVintId(const uint8_t *buf, int &len)
    {
        len = vintLen(buf[0]);
        uint32_t val = 0;
        for (int i = 0; i < len; ++i)
            val = (val << 8) | buf[i];
        return val;
    }

    // Read a VINT size from a buffer. Returns decoded value (-1 = unknown).
    inline int64_t readVintSize(const uint8_t *buf, int &len)
    {
        len = vintLen(buf[0]);
        uint8_t mask = 0xFF >> len;
        int64_t val = buf[0] & mask;
        for (int i = 1; i < len; ++i)
            val = (val << 8) | buf[i];
        // Unknown size: all data bits set
        int64_t unknown = (1LL << (7 * len)) - 1;
        if (val == unknown)
            return -1;
        return val;
    }

    // ═════════════════════════════════════════════════════════════════
    //  § 3.  EBML Node — in-memory tree
    // ═════════════════════════════════════════════════════════════════

    struct EbmlNode
    {
        uint32_t id = 0;
        bool master = false;
        std::vector<uint8_t> data;      // leaf payload
        std::vector<EbmlNode> children; // master payload

        // ── Serialize to binary ──
        std::vector<uint8_t> innerBytes() const
        {
            if (master)
            {
                std::vector<uint8_t> buf;
                for (auto &c : children)
                {
                    auto cb = c.serialize();
                    buf.insert(buf.end(), cb.begin(), cb.end());
                }
                return buf;
            }
            return data;
        }

        std::vector<uint8_t> serialize() const
        {
            auto inner = innerBytes();
            uint8_t hdr[12]; // max 4 (id) + 8 (size)
            int hLen = encodeId(id, hdr);
            hLen += encodeVintSizeOpt((int64_t)inner.size(), hdr + hLen);
            std::vector<uint8_t> out(hdr, hdr + hLen);
            out.insert(out.end(), inner.begin(), inner.end());
            return out;
        }

        // Serialize but ensure total size == targetSize (pad size VINT).
        // Returns empty vector if impossible.
        std::vector<uint8_t> serializePadded(int64_t targetSize) const
        {
            auto inner = innerBytes();
            int idLen = idEncodedLen(id);
            int64_t neededDataLen = (int64_t)inner.size();
            int64_t maxHeader = idLen + 8; // max possible header
            if (targetSize < idLen + 1 + neededDataLen)
                return {}; // can't fit
            if (targetSize > maxHeader + neededDataLen)
                return {}; // too much gap

            int sizeVintLen = (int)(targetSize - idLen - neededDataLen);
            if (sizeVintLen < 1 || sizeVintLen > 8)
                return {};

            uint8_t hdr[12];
            int hLen = encodeId(id, hdr);
            hLen += encodeVintSize(neededDataLen, sizeVintLen, hdr + hLen);
            std::vector<uint8_t> out(hdr, hdr + hLen);
            out.insert(out.end(), inner.begin(), inner.end());
            return out;
        }

        // ── Lookup ──
        EbmlNode *find(uint32_t cid)
        {
            for (auto &c : children)
                if (c.id == cid)
                    return &c;
            return nullptr;
        }
        const EbmlNode *find(uint32_t cid) const
        {
            for (auto &c : children)
                if (c.id == cid)
                    return &c;
            return nullptr;
        }
        std::vector<EbmlNode *> findAll(uint32_t cid)
        {
            std::vector<EbmlNode *> r;
            for (auto &c : children)
                if (c.id == cid)
                    r.push_back(&c);
            return r;
        }
        void removeAll(uint32_t cid)
        {
            children.erase(
                std::remove_if(children.begin(), children.end(),
                               [cid](const EbmlNode &n)
                               { return n.id == cid; }),
                children.end());
        }

        // ── Builders ──
        static EbmlNode Master(uint32_t id, std::vector<EbmlNode> ch = {})
        {
            EbmlNode n;
            n.id = id;
            n.master = true;
            n.children = std::move(ch);
            return n;
        }
        static EbmlNode Uint(uint32_t id, uint64_t val)
        {
            EbmlNode n;
            n.id = id;
            // Encode in minimal bytes (big-endian)
            int len = 1;
            if (val > 0x00FFFFFFFFFFFFFFULL)
                len = 8;
            else if (val > 0x0000FFFFFFFFFFFFULL)
                len = 7;
            else if (val > 0x000000FFFFFFFFFFULL)
                len = 6;
            else if (val > 0x00000000FFFFFFFFULL)
                len = 5;
            else if (val > 0x0000000000FFFFFFULL)
                len = 4;
            else if (val > 0x000000000000FFFFULL)
                len = 3;
            else if (val > 0x00000000000000FFULL)
                len = 2;
            n.data.resize(len);
            for (int i = len - 1; i >= 0; --i)
            {
                n.data[i] = val & 0xFF;
                val >>= 8;
            }
            return n;
        }
        static EbmlNode Float64(uint32_t id, double val)
        {
            EbmlNode n;
            n.id = id;
            n.data.resize(8);
            uint64_t bits;
            memcpy(&bits, &val, 8);
            for (int i = 0; i < 8; ++i)
                n.data[i] = (bits >> (56 - 8 * i)) & 0xFF;
            return n;
        }
        static EbmlNode Str(uint32_t id, const std::string &s)
        {
            EbmlNode n;
            n.id = id;
            n.data.assign(s.begin(), s.end());
            return n;
        }
        static EbmlNode Bin(uint32_t id, const uint8_t *d, size_t sz)
        {
            EbmlNode n;
            n.id = id;
            n.data.assign(d, d + sz);
            return n;
        }
        static EbmlNode Bin(uint32_t id, const std::vector<uint8_t> &d)
        {
            return Bin(id, d.data(), d.size());
        }
    };

    // Parse raw EBML bytes into a tree.
    EbmlNode parseEbml(uint32_t parentId, const uint8_t *buf, size_t len)
    {
        EbmlNode root = EbmlNode::Master(parentId);
        size_t pos = 0;
        while (pos + 2 <= len)
        {
            // Read ID
            int idLen = 0;
            uint32_t eid = readVintId(buf + pos, idLen);
            if (idLen == 0 || pos + idLen >= len)
                break;
            pos += idLen;

            // Read size
            int sLen = 0;
            int64_t esize = readVintSize(buf + pos, sLen);
            if (sLen == 0)
                break;
            pos += sLen;

            if (esize < 0)
                break; // unknown size at this level — bail
            if (pos + (size_t)esize > len)
                break; // truncated

            if (MASTER_IDS.count(eid))
            {
                auto child = parseEbml(eid, buf + pos, (size_t)esize);
                root.children.push_back(std::move(child));
            }
            else
            {
                EbmlNode leaf;
                leaf.id = eid;
                leaf.data.assign(buf + pos, buf + pos + (size_t)esize);
                root.children.push_back(std::move(leaf));
            }
            pos += (size_t)esize;
        }
        return root;
    }

    // ═════════════════════════════════════════════════════════════════
    //  § 4.  MKV file-level operations
    // ═════════════════════════════════════════════════════════════════

    struct L1Element
    {
        uint32_t id = 0;
        int64_t pos = 0; // file position of element start (ID byte)
        int idLen = 0;
        int sizeLen = 0;
        int64_t dataSize = 0; // -1 = unknown
        int64_t dataPos() const { return pos + idLen + sizeLen; }
        int64_t totalSize() const
        {
            return (dataSize < 0) ? -1 : (int64_t)(idLen + sizeLen + dataSize);
        }
    };

    struct MkvCtx
    {
        FILE *f = nullptr;
        int64_t fileSize = 0;
        // Segment element info
        int64_t segPos = 0; // position of Segment ID
        int segIdLen = 0;
        int segSizeLen = 0;
        int64_t segDataSize = -1; // -1 = unknown
        int64_t segDataStart = 0; // absolute file position of segment data

        std::vector<L1Element> elts;

        ~MkvCtx() { close(); }

        void close()
        {
            if (f)
                fclose(f);
            f = nullptr;
        }

        bool open(const std::string &path)
        {
#ifdef _WIN32
            f = _wfopen(std::filesystem::path(path).wstring().c_str(), L"r+b");
#else
            f = fopen(path.c_str(), "r+b");
#endif
            if (!f)
                return false;
            FSEEK64(f, 0, SEEK_END);
            fileSize = FTELL64(f);
            FSEEK64(f, 0, SEEK_SET);
            return true;
        }

        // Read N bytes from file at position.
        std::vector<uint8_t> readBytes(int64_t pos, size_t count)
        {
            std::vector<uint8_t> buf(count);
            FSEEK64(f, pos, SEEK_SET);
            size_t got = fread(buf.data(), 1, count, f);
            buf.resize(got);
            return buf;
        }

        // Read element header (ID + size) at current file position.
        bool readElementHeader(L1Element &elt)
        {
            elt.pos = FTELL64(f);
            uint8_t buf[12];
            size_t got = fread(buf, 1, 12, f);
            if (got < 2)
                return false;

            elt.id = readVintId(buf, elt.idLen);
            if (elt.idLen == 0 || elt.idLen > (int)got)
                return false;

            elt.dataSize = readVintSize(buf + elt.idLen, elt.sizeLen);
            if (elt.sizeLen == 0)
                return false;

            // Seek to right after the header
            FSEEK64(f, elt.pos + elt.idLen + elt.sizeLen, SEEK_SET);
            return true;
        }

        // Parse the file: find Segment, scan its L1 children.
        bool parse()
        {
            FSEEK64(f, 0, SEEK_SET);
            elts.clear();

            // Skip EBML header
            L1Element ebmlHdr;
            if (!readElementHeader(ebmlHdr))
                return false;
            if (ebmlHdr.id != ID_EBML)
                return false;
            if (ebmlHdr.dataSize < 0)
                return false;
            FSEEK64(f, ebmlHdr.dataPos() + ebmlHdr.dataSize, SEEK_SET);

            // Read Segment header
            L1Element seg;
            if (!readElementHeader(seg))
                return false;
            if (seg.id != ID_SEGMENT)
                return false;
            segPos = seg.pos;
            segIdLen = seg.idLen;
            segSizeLen = seg.sizeLen;
            segDataSize = seg.dataSize;
            segDataStart = seg.dataPos();

            // Determine scan limit
            int64_t scanEnd = fileSize;
            if (segDataSize >= 0)
                scanEnd = std::min(fileSize, segDataStart + segDataSize);

            // Scan L1 children (just headers — don't read data)
            FSEEK64(f, segDataStart, SEEK_SET);
            while (FTELL64(f) < scanEnd)
            {
                L1Element child;
                int64_t before = FTELL64(f);
                if (!readElementHeader(child))
                    break;
                if (child.idLen == 0)
                    break;

                // Don't store Cluster elements (huge, useless for metadata editing)
                if (child.id != ID_CLUSTER)
                    elts.push_back(child);

                // Skip past this element's data
                if (child.dataSize < 0)
                    break; // unknown-size element = rest of file
                int64_t nextPos = child.dataPos() + child.dataSize;
                if (nextPos > scanEnd)
                    break;
                FSEEK64(f, nextPos, SEEK_SET);
            }

            return true;
        }

        // Find first L1 element with given ID.
        L1Element *findL1(uint32_t id)
        {
            for (auto &e : elts)
                if (e.id == id)
                    return &e;
            return nullptr;
        }

        // Read the data payload of an L1 element into memory.
        std::vector<uint8_t> readL1Data(const L1Element &elt)
        {
            if (elt.dataSize <= 0)
                return {};
            return readBytes(elt.dataPos(), (size_t)elt.dataSize);
        }

        // Write an EbmlVoid element at [pos, pos+size).
        void writeVoid(int64_t pos, int64_t size)
        {
            if (size < 2)
            {
                // Can't fit a Void element in 1 byte.
                // Write a 0x00 byte (will be ignored by parsers as invalid leading byte).
                if (size == 1)
                {
                    FSEEK64(f, pos, SEEK_SET);
                    uint8_t zero = 0;
                    fwrite(&zero, 1, 1, f);
                }
                return;
            }
            FSEEK64(f, pos, SEEK_SET);
            // Void ID = 0xEC (1 byte). Size VINT for (size - 1 - vintLen(dataSize)).
            int64_t dataLen = size - 1; // subtract ID byte
            // Find a VINT encoding length for dataLen that makes total == size.
            // total = 1 (ID) + sizeVintLen + actualDataLen
            // We need: sizeVintLen + actualDataLen = size - 1
            // And sizeVintLen encodes actualDataLen.
            // actualDataLen = size - 1 - sizeVintLen
            for (int svl = 1; svl <= 8; ++svl)
            {
                int64_t actualData = size - 1 - svl;
                if (actualData < 0)
                    continue;
                if (vintMinLen(actualData) <= svl)
                {
                    uint8_t hdr[9];
                    hdr[0] = 0xEC; // Void ID
                    encodeVintSize(actualData, svl, hdr + 1);
                    fwrite(hdr, 1, 1 + svl, f);
                    // Write zero-fill for actualData bytes
                    std::vector<uint8_t> zeros(actualData, 0);
                    if (!zeros.empty())
                        fwrite(zeros.data(), 1, zeros.size(), f);
                    return;
                }
            }
        }

        // Replace an existing L1 element with new serialized bytes.
        // Strategy: in-place if fits, else reuse a Void, else append at end.
        // Returns true on success.
        bool replaceL1(L1Element *old, const std::vector<uint8_t> &newBytes)
        {
            int64_t oldTotal = old->totalSize();
            int64_t newTotal = (int64_t)newBytes.size();

            if (oldTotal >= newTotal)
            {
                // Fits in-place
                FSEEK64(f, old->pos, SEEK_SET);
                fwrite(newBytes.data(), 1, newBytes.size(), f);
                int64_t remaining = oldTotal - newTotal;
                if (remaining >= 2)
                    writeVoid(old->pos + newTotal, remaining);
                else if (remaining == 1)
                {
                    FSEEK64(f, old->pos + newTotal, SEEK_SET);
                    uint8_t zero = 0;
                    fwrite(&zero, 1, 1, f);
                }
                fflush(f);
                return true;
            }

            // Doesn't fit in-place.
            // First, void the old element.
            writeVoid(old->pos, oldTotal);

            // Try to find an existing Void element large enough to reuse.
            for (auto &e : elts)
            {
                if (e.id != ID_VOID) continue;
                int64_t voidTotal = e.totalSize();
                if (voidTotal < newTotal) continue;
                // Need at least 2 extra bytes for leftover Void, or exact fit
                if (voidTotal != newTotal && voidTotal < newTotal + 2) continue;

                // Found a suitable Void — overwrite it
                FSEEK64(f, e.pos, SEEK_SET);
                fwrite(newBytes.data(), 1, newBytes.size(), f);
                int64_t remaining = voidTotal - newTotal;
                if (remaining >= 2)
                    writeVoid(e.pos + newTotal, remaining);
                else if (remaining == 1)
                {
                    uint8_t zero = 0;
                    fwrite(&zero, 1, 1, f);
                }
                // Mark this void as consumed (set ID to 0 so it won't be reused)
                e.id = 0;
                fflush(f);
                return true;
            }

            // No suitable Void found — append at end of file
            return appendAtEnd(newBytes);
        }

        // Append serialized bytes at the end of the file.
        // Updates Segment size if it was finite.
        bool appendAtEnd(const std::vector<uint8_t> &bytes)
        {
            FSEEK64(f, 0, SEEK_END);
            int64_t writePos = FTELL64(f);
            fwrite(bytes.data(), 1, bytes.size(), f);
            fflush(f);

            fileSize = writePos + (int64_t)bytes.size();

            // Update Segment size if finite
            if (segDataSize >= 0)
            {
                int64_t newSegDataSize = fileSize - segDataStart;
                // Check if the new size fits in the original VINT encoding length
                if (vintMinLen(newSegDataSize) <= segSizeLen)
                {
                    uint8_t buf[8];
                    encodeVintSize(newSegDataSize, segSizeLen, buf);
                    FSEEK64(f, segPos + segIdLen, SEEK_SET);
                    fwrite(buf, 1, segSizeLen, f);
                    fflush(f);
                    segDataSize = newSegDataSize;
                }
                else
                {
                    // Can't fit new size in original VINT length.
                    // This should be extremely rare (>128TB file with 1-byte VINT).
                    // Just leave the size stale — most players handle it.
                }
            }
            return true;
        }
    };

    // ═════════════════════════════════════════════════════════════════
    //  § 5.  Public API implementations
    // ═════════════════════════════════════════════════════════════════

    // Helper: generate a random 64-bit UID
    uint64_t randomUID()
    {
        std::random_device rd;
        std::mt19937_64 rng(rd());
        return rng();
    }

    // Helper: read uint from big-endian EBML data
    uint64_t ebmlReadUint(const std::vector<uint8_t> &d)
    {
        uint64_t v = 0;
        for (auto b : d)
            v = (v << 8) | b;
        return v;
    }

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────
//  writeTag — add/update a global MKV tag in-place
// ─────────────────────────────────────────────────────────────────
bool mkv::writeTag(const std::string &mkvPath,
                   const std::string &key,
                   const std::string &value,
                   LogCb log)
{
    auto L = [&](const std::string &m)
    { if (log) log(m); };

    MkvCtx ctx;
    if (!ctx.open(mkvPath))
    {
        L("mkv-tag: cannot open file");
        return false;
    }
    if (!ctx.parse())
    {
        L("mkv-tag: cannot parse EBML");
        return false;
    }

    // Read existing Tags element (if any)
    EbmlNode tagsNode = EbmlNode::Master(ID_TAGS);
    auto *oldTags = ctx.findL1(ID_TAGS);
    if (oldTags && oldTags->dataSize > 0)
    {
        auto rawData = ctx.readL1Data(*oldTags);
        tagsNode = parseEbml(ID_TAGS, rawData.data(), rawData.size());
    }

    // Find or create our Tag (containing the key)
    bool found = false;
    for (auto &tag : tagsNode.children)
    {
        if (tag.id != ID_TAG)
            continue;
        for (auto &st : tag.children)
        {
            if (st.id != ID_SIMPLE_TAG)
                continue;
            auto *nameNode = st.find(ID_TAG_NAME);
            if (!nameNode)
                continue;
            std::string existingName(nameNode->data.begin(), nameNode->data.end());
            if (existingName == key)
            {
                // Update existing tag value
                auto *valNode = st.find(ID_TAG_STRING);
                if (valNode)
                {
                    valNode->data.assign(value.begin(), value.end());
                }
                else
                {
                    st.children.push_back(EbmlNode::Str(ID_TAG_STRING, value));
                }
                found = true;
                break;
            }
        }
        if (found)
            break;
    }

    if (!found)
    {
        // Add a new Tag with this key=value
        auto simpleTag = EbmlNode::Master(ID_SIMPLE_TAG, {
                                                             EbmlNode::Str(ID_TAG_NAME, key),
                                                             EbmlNode::Str(ID_TAG_STRING, value),
                                                         });
        auto tag = EbmlNode::Master(ID_TAG, {
                                                EbmlNode::Master(ID_TARGETS), // empty Targets = global scope
                                                std::move(simpleTag),
                                            });
        tagsNode.children.push_back(std::move(tag));
    }

    // Serialize new Tags
    auto newBytes = tagsNode.serialize();

    // Write back
    if (oldTags)
    {
        L("mkv-tag: updating Tags element in-place (" +
          std::to_string(oldTags->totalSize()) + " → " +
          std::to_string(newBytes.size()) + " bytes)");
        bool ok = ctx.replaceL1(oldTags, newBytes);
        if (ok)
            L("mkv-tag: wrote " + key + "=" + value);
        return ok;
    }
    else
    {
        L("mkv-tag: appending new Tags element (" +
          std::to_string(newBytes.size()) + " bytes)");
        bool ok = ctx.appendAtEnd(newBytes);
        if (ok)
            L("mkv-tag: wrote " + key + "=" + value);
        return ok;
    }
}

// ─────────────────────────────────────────────────────────────────
//  setCoverArt — add/replace cover art attachment in-place
// ─────────────────────────────────────────────────────────────────
bool mkv::setCoverArt(const std::string &mkvPath,
                      const std::string &jpegPath,
                      LogCb log)
{
    namespace fs = std::filesystem;
    auto L = [&](const std::string &m)
    { if (log) log(m); };

    // Read JPEG data
    if (!fs::exists(jpegPath))
    {
        L("mkv-cover: JPEG not found");
        return false;
    }
    std::ifstream jf(jpegPath, std::ios::binary);
    std::vector<uint8_t> jpegData(
        (std::istreambuf_iterator<char>(jf)),
        std::istreambuf_iterator<char>());
    jf.close();
    if (jpegData.empty())
    {
        L("mkv-cover: JPEG is empty");
        return false;
    }

    MkvCtx ctx;
    if (!ctx.open(mkvPath))
    {
        L("mkv-cover: cannot open file");
        return false;
    }
    if (!ctx.parse())
    {
        L("mkv-cover: cannot parse EBML");
        return false;
    }

    // Build new Attachments element
    EbmlNode attachNode = EbmlNode::Master(ID_ATTACHMENTS);
    auto *oldAttach = ctx.findL1(ID_ATTACHMENTS);

    if (oldAttach && oldAttach->dataSize > 0)
    {
        // Read existing attachments, keep non-image ones
        auto rawData = ctx.readL1Data(*oldAttach);
        auto existing = parseEbml(ID_ATTACHMENTS, rawData.data(), rawData.size());
        for (auto &af : existing.children)
        {
            if (af.id != ID_ATTACHED_FILE)
            {
                attachNode.children.push_back(af);
                continue;
            }
            // Check mime type — skip existing images
            auto *mimeNode = af.find(ID_FILE_MIME);
            if (mimeNode)
            {
                std::string mime(mimeNode->data.begin(), mimeNode->data.end());
                if (mime.find("image/") == 0)
                {
                    L("mkv-cover: removing old cover attachment");
                    continue; // skip old cover
                }
            }
            attachNode.children.push_back(af); // keep non-image attachments
        }
    }

    // Build new AttachedFile for cover
    auto newFile = EbmlNode::Master(ID_ATTACHED_FILE, {
                                                          EbmlNode::Str(ID_FILE_NAME, "cover.jpg"),
                                                          EbmlNode::Str(ID_FILE_DESC, "cover"),
                                                          EbmlNode::Str(ID_FILE_MIME, "image/jpeg"),
                                                          EbmlNode::Bin(ID_FILE_DATA, jpegData),
                                                          EbmlNode::Uint(ID_FILE_UID, randomUID()),
                                                      });
    attachNode.children.push_back(std::move(newFile));

    auto newBytes = attachNode.serialize();
    L("mkv-cover: new Attachments element: " + std::to_string(newBytes.size()) + " bytes");

    if (oldAttach)
    {
        bool ok = ctx.replaceL1(oldAttach, newBytes);
        if (ok)
            L("mkv-cover: cover art embedded");
        return ok;
    }
    else
    {
        bool ok = ctx.appendAtEnd(newBytes);
        if (ok)
            L("mkv-cover: cover art appended");
        return ok;
    }
}

// ─────────────────────────────────────────────────────────────────
//  fixCoverMeta — fix attachment name/description for DLNA compat
// ─────────────────────────────────────────────────────────────────
bool mkv::fixCoverMeta(const std::string &mkvPath, LogCb log)
{
    auto L = [&](const std::string &m)
    { if (log) log(m); };

    MkvCtx ctx;
    if (!ctx.open(mkvPath))
    {
        L("mkv-fixcover: cannot open");
        return false;
    }
    if (!ctx.parse())
    {
        L("mkv-fixcover: cannot parse");
        return false;
    }

    auto *oldAttach = ctx.findL1(ID_ATTACHMENTS);
    if (!oldAttach || oldAttach->dataSize <= 0)
    {
        L("mkv-fixcover: no attachments found");
        return false;
    }

    auto rawData = ctx.readL1Data(*oldAttach);
    auto attachNode = parseEbml(ID_ATTACHMENTS, rawData.data(), rawData.size());

    bool modified = false;
    for (auto &af : attachNode.children)
    {
        if (af.id != ID_ATTACHED_FILE)
            continue;
        // Check if this is an image attachment
        auto *mimeNode = af.find(ID_FILE_MIME);
        if (!mimeNode)
            continue;
        std::string mime(mimeNode->data.begin(), mimeNode->data.end());
        if (mime.find("image/") != 0)
            continue;

        // Fix FileName → "cover.jpg"
        auto *nameNode = af.find(ID_FILE_NAME);
        if (nameNode)
        {
            nameNode->data.assign({'c', 'o', 'v', 'e', 'r', '.', 'j', 'p', 'g'});
        }
        else
        {
            af.children.push_back(EbmlNode::Str(ID_FILE_NAME, "cover.jpg"));
        }

        // Fix FileDescription → "cover"
        auto *descNode = af.find(ID_FILE_DESC);
        if (descNode)
        {
            descNode->data.assign({'c', 'o', 'v', 'e', 'r'});
        }
        else
        {
            af.children.push_back(EbmlNode::Str(ID_FILE_DESC, "cover"));
        }

        modified = true;
        break; // only fix first image attachment
    }

    if (!modified)
    {
        L("mkv-fixcover: no image attachment found");
        return false;
    }

    auto newBytes = attachNode.serialize();
    L("mkv-fixcover: patching Attachments (" +
      std::to_string(oldAttach->totalSize()) + " → " +
      std::to_string(newBytes.size()) + " bytes)");
    return ctx.replaceL1(oldAttach, newBytes);
}

// ─────────────────────────────────────────────────────────────────
//  setVR180SBS — inject VR spatial metadata on first video track
// ─────────────────────────────────────────────────────────────────
bool mkv::setVR180SBS(const std::string &mkvPath, LogCb log)
{
    auto L = [&](const std::string &m)
    { if (log) log(m); };

    MkvCtx ctx;
    if (!ctx.open(mkvPath))
    {
        L("mkv-vr: cannot open");
        return false;
    }
    if (!ctx.parse())
    {
        L("mkv-vr: cannot parse");
        return false;
    }

    auto *oldTracks = ctx.findL1(ID_TRACKS);
    if (!oldTracks || oldTracks->dataSize <= 0)
    {
        L("mkv-vr: no Tracks element found");
        return false;
    }

    auto rawData = ctx.readL1Data(*oldTracks);
    auto tracksNode = parseEbml(ID_TRACKS, rawData.data(), rawData.size());

    bool modified = false;
    for (auto &te : tracksNode.children)
    {
        if (te.id != ID_TRACK_ENTRY)
            continue;

        // Check if this is a video track (TrackType == 1)
        auto *ttNode = te.find(ID_TRACK_TYPE);
        if (!ttNode)
            continue;
        uint64_t trackType = ebmlReadUint(ttNode->data);
        if (trackType != 1)
            continue; // not video

        // Find or create Video sub-element
        auto *videoNode = te.find(ID_VIDEO);
        if (!videoNode)
        {
            te.children.push_back(EbmlNode::Master(ID_VIDEO));
            videoNode = &te.children.back();
        }

        // Set StereoMode = 1 (side-by-side, left eye first)
        videoNode->removeAll(ID_STEREO_MODE);
        videoNode->children.push_back(EbmlNode::Uint(ID_STEREO_MODE, 1));

        // Set Projection { Type=1, Yaw=0, Pitch=0, Roll=0 }
        videoNode->removeAll(ID_PROJECTION);
        auto proj = EbmlNode::Master(ID_PROJECTION, {
                                                        EbmlNode::Uint(ID_PROJ_TYPE, 1), // equirectangular
                                                        EbmlNode::Float64(ID_PROJ_YAW, 0.0),
                                                        EbmlNode::Float64(ID_PROJ_PITCH, 0.0),
                                                        EbmlNode::Float64(ID_PROJ_ROLL, 0.0),
                                                    });
        videoNode->children.push_back(std::move(proj));

        modified = true;
        L("mkv-vr: set StereoMode=1 + Projection(equirect) on video track");
        break; // only first video track
    }

    if (!modified)
    {
        L("mkv-vr: no video track found");
        return false;
    }

    auto newBytes = tracksNode.serialize();
    L("mkv-vr: patching Tracks (" +
      std::to_string(oldTracks->totalSize()) + " → " +
      std::to_string(newBytes.size()) + " bytes)");
    return ctx.replaceL1(oldTracks, newBytes);
}
