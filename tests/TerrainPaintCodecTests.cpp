// Checks for the terrain paint and decal save records: run TerrainPaintCodecTests.exe, it prints "ok" or aborts.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

#include "cIGZIStream.h"
#include "cIGZOStream.h"
#include "service/decal/TerrainPaintCodec.h"
#include "service/decal/TerrainDecalSidecarCodec.h"

namespace
{
    void Check(const bool condition, const char* what, const int line)
    {
        if (!condition) {
            std::fprintf(stderr, "FAILED line %d: %s\n", line, what);
            std::abort();
        }
    }
#define CHECK(x) Check((x), #x, __LINE__)

    class MemoryOut final : public cIGZOStream
    {
    public:
        std::vector<uint8_t> bytes;

        bool QueryInterface(uint32_t, void**) override { return false; }
        uint32_t AddRef() override { return 1; }
        uint32_t Release() override { return 1; }
        void Flush() override {}
        bool SetSint8(const int8_t v) override { return Put(v); }
        bool SetUint8(const uint8_t v) override { return Put(v); }
        bool SetSint16(const int16_t v) override { return Put(v); }
        bool SetUint16(const uint16_t v) override { return Put(v); }
        bool SetSint32(const int32_t v) override { return Put(v); }
        bool SetUint32(const uint32_t v) override { return Put(v); }
        bool SetSint64(const int64_t v) override { return Put(v); }
        bool SetUint64(const uint64_t v) override { return Put(v); }
        bool SetFloat32(const float v) override { return Put(v); }
        bool SetFloat64(const double v) override { return Put(v); }
        bool SetRZCharStr(const char*) override { return false; }
        bool SetGZStr(const cIGZString&) override { return false; }
        bool SetGZSerializable(const cIGZSerializable&) override { return false; }
        bool SetVoid(const void* data, const uint32_t size) override
        {
            const auto* p = static_cast<const uint8_t*>(data);
            bytes.insert(bytes.end(), p, p + size);
            return true;
        }
        int32_t GetError() override { return 0; }
        int32_t SetUserData(cIGZVariant*) override { return 0; }
        int32_t GetUserData() override { return 0; }

    private:
        template <typename T>
        bool Put(const T v) { return SetVoid(&v, sizeof(v)); }
    };

    class MemoryIn final : public cIGZIStream
    {
    public:
        explicit MemoryIn(std::vector<uint8_t> data) : bytes_(std::move(data)) {}

        bool QueryInterface(uint32_t, void**) override { return false; }
        uint32_t AddRef() override { return 1; }
        uint32_t Release() override { return 1; }
        bool Skip(const uint32_t n) override { return GetVoid(nullptr, n); }
        bool GetSint8(int8_t& v) override { return Get(v); }
        bool GetUint8(uint8_t& v) override { return Get(v); }
        bool GetSint16(int16_t& v) override { return Get(v); }
        bool GetUint16(uint16_t& v) override { return Get(v); }
        bool GetSint32(int32_t& v) override { return Get(v); }
        bool GetUint32(uint32_t& v) override { return Get(v); }
        bool GetSint64(int64_t& v) override { return Get(v); }
        bool GetUint64(uint64_t& v) override { return Get(v); }
        bool GetFloat32(float& v) override { return Get(v); }
        bool GetFloat64(double& v) override { return Get(v); }
        bool GetRZCharStr(char*, uint32_t) override { return false; }
        bool GetGZStr(cIGZString&) override { return false; }
        bool GetGZSerializable(cIGZSerializable&) override { return false; }
        bool GetVoid(void* out, const uint32_t size) override
        {
            if (bytes_.size() - pos_ < size) {
                error_ = 1;
                return false;
            }
            if (out) {
                std::memcpy(out, bytes_.data() + pos_, size);
            }
            pos_ += size;
            return true;
        }
        int32_t GetError() override { return error_; }
        int32_t SetUserData(cIGZVariant*) override { return 0; }
        int32_t GetUserData() override { return 0; }

    private:
        template <typename T>
        bool Get(T& v) { return GetVoid(&v, sizeof(v)); }

        std::vector<uint8_t> bytes_;
        size_t pos_ = 0;
        int32_t error_ = 0;
    };

    // The decal struct sizes callers may pass: before drawOrder, before frame, and now.
    static_assert(kTerrainDecalStateSizeV1 == 108 && kTerrainDecalStateSizeV2 == 112 && kTerrainDecalStateSize == 116);
    static_assert(kTerrainDecalSnapshotSizeV1 == 112 && kTerrainDecalSnapshotSizeV2 == 116 &&
                  kTerrainDecalSnapshotSize == 120);

    // magic, major, minor, paint count, then the first paint's byte count.
    constexpr size_t kMinorOffset = 6;
    constexpr size_t kByteCountOffset = 12;

    void Patch32(std::vector<uint8_t>& bytes, const size_t offset, const uint32_t value)
    {
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    }

    uint32_t Read32(const std::vector<uint8_t>& bytes, const size_t offset)
    {
        uint32_t value = 0;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    }

    TerrainPaintCodec::ReadResult ReadBack(std::vector<uint8_t> bytes)
    {
        MemoryIn in(std::move(bytes));
        return TerrainPaintCodec::Read(in);
    }
}

int main()
{
    TerrainPaintCodec::PaintSnapshot paint{};
    paint.id.value = 9;
    paint.style.textureCount = 3;
    for (uint32_t i = 0; i < 3; ++i) {
        paint.style.textures[i].key = cGZPersistResourceKey(0x7AB50E44, 0x0986135E, 0x100 + i);
    }
    paint.style.textureFrameMasks[0] = 0b1011;
    paint.style.textureFrameMasks[2] = 1u << 31;
    paint.style.wear = 0.25f;
    paint.style.wearSeed = 77;
    paint.points = {{0, 0}, {16, 0}, {16, 16}};
    paint.ringSizes = {3};
    paint.uvMap = {{0, 0, 0, 0}, {16, 0, 1, 0}, {16, 16, 1, 1}};

    MemoryOut out;
    CHECK(TerrainPaintCodec::Write(out, {&paint}));
    const std::vector<uint8_t> v3 = out.bytes;
    CHECK(Read32(v3, 4) >> 16 == TerrainPaintCodec::kVersionMinor);

    {
        const auto result = ReadBack(v3);
        CHECK(result.ok && result.paints.size() == 1);
        const TerrainPaintStyle& style = result.paints[0].style;
        CHECK(style.textureFrameMasks[0] == 0b1011 && style.textureFrameMasks[1] == 0 &&
              style.textureFrameMasks[2] == (1u << 31));
        CHECK(style.wear == 0.25f && style.wearSeed == 77 && result.paints[0].uvMap.size() == 3);
    }

    {
        // Minor 2, before frame masks: the same paint without them reads with masks 0.
        std::vector<uint8_t> v2(v3.begin(), v3.end() - 12);
        v2[kMinorOffset] = 2;
        Patch32(v2, kByteCountOffset, Read32(v3, kByteCountOffset) - 12);
        const auto result = ReadBack(v2);
        CHECK(result.ok && result.paints.size() == 1);
        const TerrainPaintStyle& style = result.paints[0].style;
        CHECK(style.textureFrameMasks[0] == 0 && style.textureFrameMasks[2] == 0);
        CHECK(style.wear == 0.25f && style.wearSeed == 77 && style.textureCount == 3);
    }

    {
        // A later minor version's fields after the masks are skipped.
        std::vector<uint8_t> v4 = v3;
        v4.insert(v4.end(), 8, 0xAB);
        Patch32(v4, kByteCountOffset, Read32(v3, kByteCountOffset) + 8);
        const auto result = ReadBack(v4);
        CHECK(result.ok && result.paints.size() == 1 && result.paints[0].style.textureFrameMasks[0] == 0b1011);
    }

    {
        TerrainPaintCodec::PaintSnapshot full = paint;
        full.style.textureCount = kTerrainPaintMaxTextures;
        full.style.textureFrameMasks[kTerrainPaintMaxTextures - 1] = 6;
        MemoryOut fullOut;
        CHECK(TerrainPaintCodec::Write(fullOut, {&full}));
        const auto result = ReadBack(fullOut.bytes);
        CHECK(result.ok && result.paints[0].style.textureCount == kTerrainPaintMaxTextures &&
              result.paints[0].style.textureFrameMasks[kTerrainPaintMaxTextures - 1] == 6);
    }

    {
        // Decal sidecar: frame round trips, minor 2 records read frame 0, longer records are skipped.
        TerrainDecalSnapshot decal{};
        decal.id.value = 4;
        decal.state.textureKey = cGZPersistResourceKey(0x7AB50E44, 0x1ABE787D, 0xAA40173A);
        decal.state.drawOrder = 7;
        decal.state.frame = 5;
        MemoryOut decalOut;
        CHECK(TerrainDecalSidecar::Write(decalOut, {decal, decal}));
        const std::vector<uint8_t> sidecar = decalOut.bytes;
        constexpr size_t kPayload = 20;
        constexpr size_t kRecordSize = 24;
        constexpr size_t kRecords = 32;
        constexpr uint32_t kSize = sizeof(TerrainDecalSidecar::PersistedTerrainDecal);
        CHECK(Read32(sidecar, kRecordSize) == kSize && sidecar.size() == kRecords + 2 * kSize);

        const auto read = [](std::vector<uint8_t> bytes) {
            MemoryIn in(std::move(bytes));
            return TerrainDecalSidecar::Read(in);
        };
        const auto current = read(sidecar);
        CHECK(current.ok && current.decals.size() == 2 && current.decals[1].state.frame == 5 &&
              current.decals[1].state.drawOrder == 7);

        // Each record cut or padded to recordSize bytes.
        const auto resized = [&](const uint32_t recordSize, const uint16_t minor) {
            std::vector<uint8_t> bytes(sidecar.begin(), sidecar.begin() + kRecords);
            for (size_t i = 0; i < 2; ++i) {
                const auto record = sidecar.begin() + static_cast<std::ptrdiff_t>(kRecords + i * kSize);
                bytes.insert(bytes.end(), record, record + std::min(recordSize, kSize));
                bytes.insert(bytes.end(), recordSize > kSize ? recordSize - kSize : 0u, uint8_t{0xAB});
            }
            bytes[kMinorOffset] = static_cast<uint8_t>(minor);
            Patch32(bytes, kPayload, 2 * recordSize);
            Patch32(bytes, kRecordSize, recordSize);
            return bytes;
        };
        const auto minor2 = read(resized(kSize - 4, 2));
        CHECK(minor2.ok && minor2.decals.size() == 2 && minor2.decals[1].state.frame == 0 &&
              minor2.decals[1].state.drawOrder == 7);
        const auto longer = read(resized(kSize + 8, 4));
        CHECK(longer.ok && longer.decals.size() == 2 && longer.decals[1].state.frame == 5);
    }

    std::puts("ok");
    return 0;
}
