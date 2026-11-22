// Checks for the terrain paint record: run TerrainPaintCodecTests.exe, it prints "ok" or aborts.
// Cities saved by older versions must still load, and older versions must still load the paints
// this version saves.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

#include "cIGZIStream.h"
#include "cIGZOStream.h"
#include "service/decal/TerrainPaintCodec.h"

using TerrainPaintCodec::PaintSnapshot;

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

    // Little endian, like the game's streams on x86.
    class MemoryOStream final : public cIGZOStream
    {
    public:
        bool QueryInterface(uint32_t, void**) override { return false; }
        uint32_t AddRef() override { return 1; }
        uint32_t Release() override { return 1; }

        void Flush() override {}
        bool SetSint8(const int8_t value) override { return Put_(&value, 1); }
        bool SetUint8(const uint8_t value) override { return Put_(&value, 1); }
        bool SetSint16(const int16_t value) override { return Put_(&value, 2); }
        bool SetUint16(const uint16_t value) override { return Put_(&value, 2); }
        bool SetSint32(const int32_t value) override { return Put_(&value, 4); }
        bool SetUint32(const uint32_t value) override { return Put_(&value, 4); }
        bool SetSint64(const int64_t value) override { return Put_(&value, 8); }
        bool SetUint64(const uint64_t value) override { return Put_(&value, 8); }
        bool SetFloat32(const float value) override { return Put_(&value, 4); }
        bool SetFloat64(const double value) override { return Put_(&value, 8); }
        bool SetRZCharStr(const char*) override { return false; }
        bool SetGZStr(const cIGZString&) override { return false; }
        bool SetGZSerializable(const cIGZSerializable&) override { return false; }
        bool SetVoid(const void* const data, const uint32_t size) override { return Put_(data, size); }
        int32_t GetError() override { return 0; }
        int32_t SetUserData(cIGZVariant*) override { return 0; }
        int32_t GetUserData() override { return 0; }

        std::vector<uint8_t> bytes;

    private:
        bool Put_(const void* const data, const size_t size)
        {
            const auto* const begin = static_cast<const uint8_t*>(data);
            bytes.insert(bytes.end(), begin, begin + size);
            return true;
        }
    };

    class MemoryIStream final : public cIGZIStream
    {
    public:
        explicit MemoryIStream(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)) {}

        bool QueryInterface(uint32_t, void**) override { return false; }
        uint32_t AddRef() override { return 1; }
        uint32_t Release() override { return 1; }

        bool Skip(const uint32_t size) override { return Get_(nullptr, size); }
        bool GetSint8(int8_t& value) override { return Get_(&value, 1); }
        bool GetUint8(uint8_t& value) override { return Get_(&value, 1); }
        bool GetSint16(int16_t& value) override { return Get_(&value, 2); }
        bool GetUint16(uint16_t& value) override { return Get_(&value, 2); }
        bool GetSint32(int32_t& value) override { return Get_(&value, 4); }
        bool GetUint32(uint32_t& value) override { return Get_(&value, 4); }
        bool GetSint64(int64_t& value) override { return Get_(&value, 8); }
        bool GetUint64(uint64_t& value) override { return Get_(&value, 8); }
        bool GetFloat32(float& value) override { return Get_(&value, 4); }
        bool GetFloat64(double& value) override { return Get_(&value, 8); }
        bool GetRZCharStr(char*, uint32_t) override { return false; }
        bool GetGZStr(cIGZString&) override { return false; }
        bool GetGZSerializable(cIGZSerializable&) override { return false; }
        bool GetVoid(void* const data, const uint32_t size) override { return Get_(data, size); }
        int32_t GetError() override { return error_; }
        int32_t SetUserData(cIGZVariant*) override { return 0; }
        int32_t GetUserData() override { return 0; }

        [[nodiscard]] size_t Remaining() const { return bytes_.size() - position_; }

    private:
        bool Get_(void* const data, const size_t size)
        {
            if (size > bytes_.size() - position_) {
                error_ = 1;
                return false;
            }
            if (data) {
                std::memcpy(data, bytes_.data() + position_, size);
            }
            position_ += size;
            return true;
        }

        std::vector<uint8_t> bytes_;
        size_t position_ = 0;
        int32_t error_ = 0;
    };

    PaintSnapshot MakePaint(const uint32_t id, const TerrainPaintLayer layer)
    {
        PaintSnapshot paint{};
        paint.id = TerrainPaintId{id};
        TerrainPaintStyle& style = paint.style;
        style.textures[0].key = cGZPersistResourceKey(0x7AB50E44, 0x0986135E, 0x00001000 + id);
        style.textures[0].weight = 2.0f;
        style.textures[1].key = cGZPersistResourceKey(0x7AB50E44, 0x0986135E, 0x00002000 + id);
        style.textureCount = 2;
        style.variationSeed = 17;
        style.textureSizeMeters = 8.0f;
        style.textureRotationTurns = 0.25f;
        style.textureOffsetX = 1.5f;
        style.textureOffsetZ = -2.5f;
        style.color = cS3DVector3(0.5f, 0.75f, 1.0f);
        style.opacity = 0.8f;
        style.featherMeters = 2.0f;
        style.drawOrder = -3;
        style.depthOffset = 1;
        style.enabled = true;
        style.wear = 0.3f;
        style.wearPatchMeters = 0.6f;
        style.wearSeed = 99;
        style.layer = layer;
        paint.ringSizes = {4};
        paint.points = {{100, 100}, {180, 100}, {180, 150}, {100, 150}};
        paint.uvMap = {{100, 100, 0, 0}, {180, 100, 5, 0}, {180, 150, 5, 1}};
        return paint;
    }

    // The record as versions before the layer wrote it: minor version 2, ending with wear.
    std::vector<uint8_t> WriteMinor2(const std::vector<PaintSnapshot>& paints)
    {
        MemoryOStream out;
        out.SetUint32(TerrainPaintCodec::kMagic);
        out.SetUint16(1);
        out.SetUint16(2);
        out.SetUint32(static_cast<uint32_t>(paints.size()));
        for (const PaintSnapshot& paint : paints) {
            const TerrainPaintStyle& style = paint.style;
            const uint32_t byteCount = 4 + 4 + 4 + 16 + 20 + 8 + 4 + 4 + 4 + 12 + style.textureCount * 16u +
                                       static_cast<uint32_t>(paint.ringSizes.size()) * 4u +
                                       static_cast<uint32_t>(paint.points.size()) * 8u +
                                       static_cast<uint32_t>(paint.uvMap.size()) * 16u;
            out.SetUint32(byteCount);
            out.SetUint32(paint.id.value);
            out.SetUint32(style.textureCount);
            for (uint32_t i = 0; i < style.textureCount; ++i) {
                out.SetUint32(style.textures[i].key.type);
                out.SetUint32(style.textures[i].key.group);
                out.SetUint32(style.textures[i].key.instance);
                out.SetFloat32(style.textures[i].weight);
            }
            out.SetUint32(style.variationSeed);
            out.SetFloat32(style.textureSizeMeters);
            out.SetFloat32(style.textureRotationTurns);
            out.SetFloat32(style.textureOffsetX);
            out.SetFloat32(style.textureOffsetZ);
            out.SetFloat32(style.color.fX);
            out.SetFloat32(style.color.fY);
            out.SetFloat32(style.color.fZ);
            out.SetFloat32(style.opacity);
            out.SetFloat32(style.featherMeters);
            out.SetSint32(style.drawOrder);
            out.SetSint32(style.depthOffset);
            out.SetUint32(style.enabled ? 1u : 0u);
            out.SetUint32(static_cast<uint32_t>(paint.ringSizes.size()));
            for (const uint32_t size : paint.ringSizes) {
                out.SetUint32(size);
            }
            for (const TerrainPaintPoint& point : paint.points) {
                out.SetFloat32(point.x);
                out.SetFloat32(point.z);
            }
            out.SetUint32(static_cast<uint32_t>(paint.uvMap.size()));
            for (const TerrainPaintUvVertex& vertex : paint.uvMap) {
                out.SetFloat32(vertex.x);
                out.SetFloat32(vertex.z);
                out.SetFloat32(vertex.u);
                out.SetFloat32(vertex.v);
            }
            out.SetFloat32(style.wear);
            out.SetFloat32(style.wearPatchMeters);
            out.SetUint32(style.wearSeed);
        }
        return out.bytes;
    }

    std::vector<uint8_t> WriteCurrent(const std::vector<PaintSnapshot>& paints)
    {
        std::vector<const PaintSnapshot*> pointers;
        for (const PaintSnapshot& paint : paints) {
            pointers.push_back(&paint);
        }
        MemoryOStream out;
        CHECK(TerrainPaintCodec::Write(out, pointers));
        return out.bytes;
    }

    bool SameStyle(const TerrainPaintStyle& a, const TerrainPaintStyle& b)
    {
        if (a.textureCount != b.textureCount) {
            return false;
        }
        for (uint32_t i = 0; i < a.textureCount; ++i) {
            if (a.textures[i].key.instance != b.textures[i].key.instance ||
                a.textures[i].weight != b.textures[i].weight) {
                return false;
            }
        }
        return a.variationSeed == b.variationSeed && a.textureSizeMeters == b.textureSizeMeters &&
               a.textureRotationTurns == b.textureRotationTurns && a.textureOffsetX == b.textureOffsetX &&
               a.textureOffsetZ == b.textureOffsetZ && a.color.fX == b.color.fX && a.color.fY == b.color.fY &&
               a.color.fZ == b.color.fZ && a.opacity == b.opacity && a.featherMeters == b.featherMeters &&
               a.drawOrder == b.drawOrder && a.depthOffset == b.depthOffset && a.enabled == b.enabled &&
               a.wear == b.wear && a.wearPatchMeters == b.wearPatchMeters && a.wearSeed == b.wearSeed;
    }

    bool SameShape(const PaintSnapshot& a, const PaintSnapshot& b)
    {
        if (a.ringSizes != b.ringSizes || a.points.size() != b.points.size() || a.uvMap.size() != b.uvMap.size()) {
            return false;
        }
        for (size_t i = 0; i < a.points.size(); ++i) {
            if (a.points[i].x != b.points[i].x || a.points[i].z != b.points[i].z) {
                return false;
            }
        }
        for (size_t i = 0; i < a.uvMap.size(); ++i) {
            if (a.uvMap[i].u != b.uvMap[i].u || a.uvMap[i].v != b.uvMap[i].v) {
                return false;
            }
        }
        return true;
    }

    // A city saved before the layer: every paint loads as it was, over lots.
    void OldRecordLoads()
    {
        const std::vector<PaintSnapshot> paints = {MakePaint(1, TerrainPaintLayer::OverLots),
                                                   MakePaint(2, TerrainPaintLayer::OverLots)};
        MemoryIStream in(WriteMinor2(paints));
        const auto result = TerrainPaintCodec::Read(in);
        CHECK(result.ok);
        CHECK(result.paints.size() == 2);
        for (size_t i = 0; i < 2; ++i) {
            CHECK(result.paints[i].id.value == paints[i].id.value);
            CHECK(SameStyle(result.paints[i].style, paints[i].style));
            CHECK(SameShape(result.paints[i], paints[i]));
            CHECK(result.paints[i].style.layer == TerrainPaintLayer::OverLots);
        }
        CHECK(in.Remaining() == 0);
    }

    // What this version saves loads back the same, layer included.
    void RoundTrip()
    {
        const std::vector<PaintSnapshot> paints = {MakePaint(1, TerrainPaintLayer::UnderLots),
                                                   MakePaint(2, TerrainPaintLayer::OverLots),
                                                   MakePaint(3, TerrainPaintLayer::OverShadows)};
        MemoryIStream in(WriteCurrent(paints));
        const auto result = TerrainPaintCodec::Read(in);
        CHECK(result.ok);
        CHECK(result.paints.size() == 3);
        for (size_t i = 0; i < 3; ++i) {
            CHECK(SameStyle(result.paints[i].style, paints[i].style));
            CHECK(SameShape(result.paints[i], paints[i]));
            CHECK(result.paints[i].style.layer == paints[i].style.layer);
        }
        CHECK(in.Remaining() == 0);
    }

    // An older version reads a paint's fields up to wear and skips the rest by its byte count. The
    // new record is the old one with the layer added at the end of each paint, so that works.
    void NewRecordExtendsOldOne()
    {
        const std::vector<PaintSnapshot> paints = {MakePaint(1, TerrainPaintLayer::UnderLots),
                                                   MakePaint(2, TerrainPaintLayer::OverShadows)};
        const std::vector<uint8_t> oldBytes = WriteMinor2(paints);
        const std::vector<uint8_t> newBytes = WriteCurrent(paints);
        CHECK(newBytes.size() == oldBytes.size() + 2 * 4);

        // Header: only the minor version differs.
        CHECK(std::memcmp(oldBytes.data(), newBytes.data(), 6) == 0);
        uint16_t minor = 0;
        std::memcpy(&minor, newBytes.data() + 6, 2);
        CHECK(minor == 3);
        CHECK(std::memcmp(oldBytes.data() + 8, newBytes.data() + 8, 4) == 0);

        size_t oldAt = 12;
        size_t newAt = 12;
        for (const PaintSnapshot& paint : paints) {
            uint32_t oldCount = 0;
            uint32_t newCount = 0;
            std::memcpy(&oldCount, oldBytes.data() + oldAt, 4);
            std::memcpy(&newCount, newBytes.data() + newAt, 4);
            CHECK(newCount == oldCount + 4);
            // The old fields, unchanged, then the layer.
            CHECK(std::memcmp(oldBytes.data() + oldAt + 4, newBytes.data() + newAt + 4, oldCount) == 0);
            uint32_t layer = 0;
            std::memcpy(&layer, newBytes.data() + newAt + 4 + oldCount, 4);
            CHECK(layer == static_cast<uint32_t>(paint.style.layer));
            oldAt += 4 + oldCount;
            newAt += 4 + newCount;
        }
        CHECK(oldAt == oldBytes.size());
        CHECK(newAt == newBytes.size());
    }

    // A later version may add layers and fields: the paint still loads, over lots.
    void NewerRecordLoads()
    {
        std::vector<uint8_t> bytes = WriteCurrent({MakePaint(1, TerrainPaintLayer::UnderLots)});
        const uint16_t minor = 9;
        std::memcpy(bytes.data() + 6, &minor, 2);
        uint32_t byteCount = 0;
        std::memcpy(&byteCount, bytes.data() + 12, 4);
        const uint32_t unknownLayer = 7;
        std::memcpy(bytes.data() + 12 + byteCount, &unknownLayer, 4);
        byteCount += 8;
        std::memcpy(bytes.data() + 12, &byteCount, 4);
        bytes.insert(bytes.end(), {0xAA, 0xBB, 0xCC, 0xDD, 0x01, 0x02, 0x03, 0x04});

        MemoryIStream in(bytes);
        const auto result = TerrainPaintCodec::Read(in);
        CHECK(result.ok);
        CHECK(result.paints.size() == 1);
        CHECK(result.paints[0].style.layer == TerrainPaintLayer::OverLots);
        CHECK(result.paints[0].style.wearSeed == 99);
        CHECK(in.Remaining() == 0);
    }
}

int main()
{
    OldRecordLoads();
    RoundTrip();
    NewRecordExtendsOldOne();
    NewerRecordLoads();
    std::printf("ok\n");
    return 0;
}
