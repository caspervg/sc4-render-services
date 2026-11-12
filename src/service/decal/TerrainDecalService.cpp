#include "TerrainDecalService.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <numbers>
#include <numeric>
#include <utility>

#include "GZServPtrs.h"
#include "cIGZMessage2.h"
#include "cIGZMessage2Standard.h"
#include "cIGZPersistDBSegment.h"
#include "cISC4App.h"
#include "cISC4City.h"
#include "cISC4DBSegment.h"
#include "cISC4DBSegmentIStream.h"
#include "cISC4DBSegmentOStream.h"
#include "cISTEOverlayManager.h"
#include "cISTETerrain.h"
#include "cISTETerrainView.h"
#include "utils/Logger.h"
#include "TerrainDecalSidecarCodec.h"

namespace
{
    using TerrainDecal::ManagedOverlay;

    constexpr uint32_t kSC4MessagePostCityInit = 0x26D31EC1;
    constexpr uint32_t kSC4MessagePreCityShutdown = 0x26D31EC2;
    constexpr uint32_t kSC4MessageLoad = 0x26C63341;
    constexpr uint32_t kSC4MessageSave = 0x26C63344;
    constexpr uint32_t kInvalidOverlayId = 0xFFFFFFFFu;

    // StaticLand is drawn in the static pass, so SCD3D11's live shadows land on paint there.
    // DynamicLand draws after them. Paints under lots live on StaticLand too, but are drawn in a
    // pass of their own.
    [[nodiscard]] cISTETerrainView::tOverlayManagerType PaintOverlayManager(const TerrainPaintLayer layer) noexcept
    {
        return layer == TerrainPaintLayer::OverShadows ? cISTETerrainView::tOverlayManagerType::DynamicLand
                                                       : cISTETerrainView::tOverlayManagerType::StaticLand;
    }

    [[nodiscard]] bool DecalUnderLots(const TerrainDecalState& state) noexcept
    {
        return (state.flags & kTerrainDecalFlagUnderLots) != 0 &&
               state.overlayType == cISTETerrainView::tOverlayManagerType::StaticLand;
    }
    // A paint shape larger than this is refused.
    constexpr uint64_t kMaxPaintPoints = 1u << 20;

    // AddDecal returns 0x80000000 | an index into the decal table (vector at +0xAC, 0x30 per
    // entry); the entry's +0x2C is the overlay slot draw calls see (RemoveOverlay 0x007388C0).
    // Only equal to the index on a manager with no shadows in it.
    [[nodiscard]] uint32_t OverlaySlotKey(const void* const overlayManager, const uint32_t overlayId) noexcept
    {
        constexpr uint32_t kDecalBit = 0x80000000u;
        constexpr std::ptrdiff_t kDecalTableOffset = 0xAC;
        constexpr std::ptrdiff_t kDecalStride = 0x30;
        constexpr std::ptrdiff_t kDecalSlotOffset = 0x2C;
        if ((overlayId & kDecalBit) == 0 || !overlayManager) {
            return overlayId;
        }
        const auto* const manager = static_cast<const std::byte*>(overlayManager);
        const std::byte* const* const table = reinterpret_cast<const std::byte* const*>(manager + kDecalTableOffset);
        const std::byte* const begin = table[0];
        const std::byte* const end = table[1];
        const std::ptrdiff_t entry = static_cast<std::ptrdiff_t>(overlayId & ~kDecalBit) * kDecalStride;
        if (!begin || entry + kDecalStride > end - begin) {
            return overlayId & ~kDecalBit;
        }
        int32_t slot = 0;
        std::memcpy(&slot, begin + entry + kDecalSlotOffset, sizeof(slot));
        return slot >= 0 ? static_cast<uint32_t>(slot) : overlayId & ~kDecalBit;
    }

    [[nodiscard]] bool IsValidOverlayId(const uint32_t overlayId) noexcept
    {
        return overlayId != kInvalidOverlayId;
    }

    template <typename T>
    [[nodiscard]] T CopyFromCaller(const T* const source, const uint32_t sourceSize) noexcept
    {
        T value{};
        std::memcpy(&value, source, std::min<size_t>(sourceSize, sizeof(T)));
        return value;
    }

    template <typename T>
    void CopyToCaller(const T& value, T* const destination, const uint32_t destinationSize) noexcept
    {
        std::memcpy(destination, &value, std::min<size_t>(destinationSize, sizeof(T)));
    }

    void PopulateOverrides(TerrainDecal::DecalOverrides& overrides, const TerrainDecalState& state) noexcept
    {
        overrides.hasUvWindow = state.hasUvWindow;
        overrides.uvWindow = state.uvWindow;
        overrides.aspectMultiplier = state.decalInfo.aspectMultiplier;
        overrides.uvScaleU = state.decalInfo.uvScaleU;
        overrides.uvScaleV = state.decalInfo.uvScaleV;
        overrides.uvOffset = state.decalInfo.uvOffset;
        overrides.mirror = (state.flags & kTerrainDecalFlagMirror) != 0;
        overrides.depthOffset = state.depthOffset;
    }

    [[nodiscard]] bool IsFinite(const float value) noexcept
    {
        return std::isfinite(value);
    }

    // Checks and tidies a style from a caller or a save.
    [[nodiscard]] bool ValidateStyle(TerrainPaintStyle& style, std::string& error)
    {
        if (style.textureCount < 1 || style.textureCount > kTerrainPaintMaxTextures) {
            error = "textureCount must be 1 to 8";
            return false;
        }
        for (uint32_t i = 0; i < style.textureCount; ++i) {
            if (!IsFinite(style.textures[i].weight)) {
                error = "texture weights must be finite";
                return false;
            }
        }
        if (!IsFinite(style.textureSizeMeters) || !(style.textureSizeMeters > 0.0f) ||
            !IsFinite(style.textureRotationTurns) || !IsFinite(style.textureOffsetX) || !IsFinite(style.textureOffsetZ)) {
            error = "texture size must be positive, rotation and offset finite";
            return false;
        }
        if (!IsFinite(style.featherMeters) || !IsFinite(style.opacity) ||
            !IsFinite(style.color.fX) || !IsFinite(style.color.fY) || !IsFinite(style.color.fZ)) {
            error = "colour, opacity and feather must be finite";
            return false;
        }
        if (!IsFinite(style.wear) || !IsFinite(style.wearPatchMeters)) {
            error = "wear must be finite";
            return false;
        }
        if (style.layer != TerrainPaintLayer::OverLots && style.layer != TerrainPaintLayer::UnderLots &&
            style.layer != TerrainPaintLayer::OverShadows) {
            error = "layer is invalid";
            return false;
        }
        style.opacity = std::clamp(style.opacity, 0.0f, 1.0f);
        style.featherMeters = std::max(0.0f, style.featherMeters);
        style.wear = std::clamp(style.wear, 0.0f, 0.95f);
        style.wearPatchMeters = std::clamp(style.wearPatchMeters, 0.02f, 16.0f);
        return true;
    }

    // Texture coordinates from ground coordinates for a style: turned, shifted and scaled.
    [[nodiscard]] std::array<float, 6> UvFromGround(const TerrainPaintStyle& style) noexcept
    {
        const double angle = 2.0 * std::numbers::pi * style.textureRotationTurns;
        const double c = std::cos(angle) / style.textureSizeMeters;
        const double s = std::sin(angle) / style.textureSizeMeters;
        const double ox = style.textureOffsetX;
        const double oz = style.textureOffsetZ;
        return {
            static_cast<float>(c), static_cast<float>(s), static_cast<float>(-(c * ox + s * oz)),
            static_cast<float>(-s), static_cast<float>(c), static_cast<float>(s * ox - c * oz),
        };
    }

    [[nodiscard]] bool CopyShape(const TerrainPaintShape& shape, TerrainPaintCodec::PaintSnapshot& data)
    {
        if (shape.ringCount == 0 || !shape.ringSizes || !shape.points) {
            return false;
        }
        uint64_t total = 0;
        for (uint32_t i = 0; i < shape.ringCount; ++i) {
            total += shape.ringSizes[i];
        }
        if (total == 0 || total > kMaxPaintPoints) {
            return false;
        }
        if (shape.uvVertexCount % 3u != 0 || shape.uvVertexCount > kMaxPaintPoints ||
            (shape.uvVertexCount > 0 && !shape.uvMap)) {
            return false;
        }
        data.ringSizes.assign(shape.ringSizes, shape.ringSizes + shape.ringCount);
        data.points.assign(shape.points, shape.points + total);
        data.uvMap.assign(shape.uvMap, shape.uvMap + shape.uvVertexCount);
        return true;
    }

    [[nodiscard]] std::unique_ptr<TerrainPaint::Mesh> BuildMesh(const TerrainPaintCodec::PaintSnapshot& data)
    {
        std::vector<TerrainPaint::Ring> rings;
        rings.reserve(data.ringSizes.size());
        size_t next = 0;
        for (const uint32_t size : data.ringSizes) {
            TerrainPaint::Ring ring;
            ring.points.reserve(size);
            for (uint32_t i = 0; i < size && next < data.points.size(); ++i, ++next) {
                ring.points.emplace_back(data.points[next].x, data.points[next].z);
            }
            rings.push_back(std::move(ring));
        }
        std::vector<TerrainPaint::UvVertex> uvMap;
        uvMap.reserve(data.uvMap.size());
        for (const TerrainPaintUvVertex& vertex : data.uvMap) {
            uvMap.push_back({vertex.x, vertex.z, vertex.u, vertex.v});
        }
        return std::make_unique<TerrainPaint::Mesh>(
            rings, data.style.featherMeters, uvMap,
            TerrainPaint::Wear{data.style.wear, data.style.wearPatchMeters, data.style.wearSeed});
    }
}

size_t TerrainDecalService::OverlayKeyHash::operator()(const OverlayKey& key) const noexcept
{
    return std::hash<const void*>{}(key.overlayManager) ^ (static_cast<size_t>(key.overlayId) * 0x9E3779B1u);
}

TerrainDecalService::TerrainDecalService()
    : cRZBaseSystemService(kTerrainDecalServiceID, 0)
    , versionTag_(VersionDetection::GetInstance().GetGameVersion())
{
}

uint32_t TerrainDecalService::AddRef()
{
    return cRZBaseSystemService::AddRef();
}

uint32_t TerrainDecalService::Release()
{
    return cRZBaseSystemService::Release();
}

bool TerrainDecalService::QueryInterface(const uint32_t riid, void** ppvObj)
{
    if (!ppvObj) {
        return false;
    }

    if (riid == GZIID_cIGZTerrainDecalService) {
        *ppvObj = static_cast<cIGZTerrainDecalService*>(this);
        AddRef();
        return true;
    }
    if (riid == GZIID_cIGZTerrainPaintService) {
        *ppvObj = static_cast<cIGZTerrainPaintService*>(this);
        AddRef();
        return true;
    }

    return cRZBaseSystemService::QueryInterface(riid, ppvObj);
}

// --- Decals -------------------------------------------------------------------------------------

uint32_t TerrainDecalService::GetServiceID() const
{
    return kTerrainDecalServiceID;
}

uint32_t TerrainDecalService::GetStateSize() const
{
    return static_cast<uint32_t>(sizeof(TerrainDecalState));
}

uint32_t TerrainDecalService::GetSnapshotSize() const
{
    return static_cast<uint32_t>(sizeof(TerrainDecalSnapshot));
}

bool TerrainDecalService::CreateDecal(const TerrainDecalState* const initialState,
                                      const uint32_t stateSize,
                                      TerrainDecalId* const outId)
{
    if (!initialState || !outId || stateSize < kTerrainDecalStateSizeV1) {
        return false;
    }

    TerrainDecalState state = CopyFromCaller(initialState, stateSize);
    std::string error;
    if (!ValidateState_(state, error)) {
        LOG_WARN("TerrainDecalService: CreateDecal validation failed: {}", error);
        return false;
    }

    const TerrainDecalId id = registry_.AllocateId();
    if (!CreateRuntimeDecal_(id, state, nextSequence_++)) {
        return false;
    }

    *outId = id;
    return true;
}

bool TerrainDecalService::RemoveDecal(const TerrainDecalId id)
{
    return id.value != 0 && RemoveRuntimeDecal_(id, true);
}

bool TerrainDecalService::GetDecal(const TerrainDecalId id,
                                   TerrainDecalSnapshot* const outSnapshot,
                                   const uint32_t snapshotSize) const
{
    if (!outSnapshot || id.value == 0 || snapshotSize < kTerrainDecalSnapshotSizeV1) {
        return false;
    }

    const TerrainDecalRecord* const record = registry_.Find(id);
    if (!record) {
        return false;
    }

    CopyToCaller(TerrainDecalSnapshot{.id = record->id, .state = record->state}, outSnapshot, snapshotSize);
    return true;
}

bool TerrainDecalService::ReplaceDecal(const TerrainDecalId id,
                                       const TerrainDecalState* const newState,
                                       const uint32_t stateSize)
{
    if (!newState || id.value == 0 || stateSize < kTerrainDecalStateSizeV1) {
        return false;
    }

    TerrainDecalRecord* const record = registry_.Find(id);
    if (!record) {
        return false;
    }

    TerrainDecalState validated = CopyFromCaller(newState, stateSize);
    std::string error;
    if (!ValidateState_(validated, error)) {
        LOG_WARN("TerrainDecalService: ReplaceDecal validation failed for {}: {}", id.value, error);
        return false;
    }

    return ApplyStateToRuntime_(*record, validated);
}

uint32_t TerrainDecalService::GetDecalCount() const
{
    return registry_.GetCount();
}

uint32_t TerrainDecalService::CopyDecals(TerrainDecalSnapshot* const buffer,
                                         const uint32_t capacity,
                                         const uint32_t snapshotSize) const
{
    if (!buffer || capacity == 0 || snapshotSize < kTerrainDecalSnapshotSizeV1) {
        return 0;
    }

    uint32_t count = 0;
    auto* const bytes = reinterpret_cast<std::byte*>(buffer);
    for (const auto& [id, record] : registry_.Records()) {
        if (count >= capacity) {
            break;
        }
        auto* const destination = reinterpret_cast<TerrainDecalSnapshot*>(bytes + (snapshotSize * count));
        CopyToCaller(TerrainDecalSnapshot{.id = TerrainDecalId{id}, .state = record.state}, destination, snapshotSize);
        ++count;
    }
    return count;
}

uint32_t TerrainDecalService::AddConfiguredOverlay_(cISTEOverlayManager* const overlayManager,
                                                    const TerrainDecalState& state) const
{
    const uint32_t overlayId = overlayManager->AddDecal(
        state.textureKey,
        state.decalInfo.center,
        state.decalInfo.baseSize,
        state.decalInfo.rotationTurns);
    if (!IsValidOverlayId(overlayId)) {
        LOG_WARN("TerrainDecalService: AddDecal failed for texture {:08X}:{:08X}:{:08X}",
                 state.textureKey.type,
                 state.textureKey.group,
                 state.textureKey.instance);
        return kInvalidOverlayId;
    }

    overlayManager->UpdateDecalInfo(overlayId, state.decalInfo);
    overlayManager->MoveDecal(overlayId, state.decalInfo.center);
    overlayManager->SetOverlayAlpha(overlayId, state.opacity);
    overlayManager->SetOverlayEnabled(overlayId, state.enabled);
    overlayManager->SetOverlayColor(overlayId, state.color);
    overlayManager->SetOverlayDrawMode(overlayId, state.drawMode);
    return overlayId;
}

bool TerrainDecalService::CreateRuntimeDecal_(const TerrainDecalId id, const TerrainDecalState& state, const uint32_t sequence)
{
    cISC4City* city = nullptr;
    if (!TryGetCurrentCity_(city)) {
        LOG_WARN("TerrainDecalService: no active city for decal creation");
        return false;
    }

    cISTEOverlayManager* const overlayManager = ResolveOverlayManager_(city, state.overlayType);
    if (!overlayManager) {
        LOG_WARN("TerrainDecalService: no active overlay manager for decal creation");
        return false;
    }

    const uint32_t overlayId = AddConfiguredOverlay_(overlayManager, state);
    if (!IsValidOverlayId(overlayId)) {
        return false;
    }

    TerrainDecalRecord record{};
    record.id = id;
    record.state = state;
    record.runtime.overlayId = overlayId;
    record.runtime.overlayManager = overlayManager;
    record.sequence = sequence;
    if (!registry_.Insert(std::move(record))) {
        overlayManager->RemoveOverlay(overlayId);
        return false;
    }

    IndexOverlay_(overlayManager, overlayId,
                  OverlayTarget{ManagedOverlay::Kind::Decal, id.value, 0, DecalUnderLots(state)});
    return true;
}

bool TerrainDecalService::ApplyStateToRuntime_(TerrainDecalRecord& record, const TerrainDecalState& state)
{
    if (!record.runtime.overlayId.has_value()) {
        return false;
    }

    cISC4City* city = nullptr;
    if (!TryGetCurrentCity_(city)) {
        return false;
    }

    auto* const oldOverlayManager = static_cast<cISTEOverlayManager*>(const_cast<void*>(record.runtime.overlayManager));
    cISTEOverlayManager* const newOverlayManager = ResolveOverlayManager_(city, state.overlayType);
    if (!oldOverlayManager || !newOverlayManager) {
        return false;
    }

    // The game cannot change a decal's texture in place, so the overlay is made again.
    const uint32_t replacementOverlayId = AddConfiguredOverlay_(newOverlayManager, state);
    if (!IsValidOverlayId(replacementOverlayId)) {
        return false;
    }

    const uint32_t oldOverlayId = *record.runtime.overlayId;
    UnindexOverlay_(oldOverlayManager, oldOverlayId);
    oldOverlayManager->RemoveOverlay(oldOverlayId);

    record.state = state;
    record.runtime.overlayId = replacementOverlayId;
    record.runtime.overlayManager = newOverlayManager;
    IndexOverlay_(newOverlayManager, replacementOverlayId,
                  OverlayTarget{ManagedOverlay::Kind::Decal, record.id.value, 0, DecalUnderLots(state)});
    return true;
}

bool TerrainDecalService::RemoveRuntimeDecal_(const TerrainDecalId id, const bool removeRuntimeOverlay)
{
    TerrainDecalRecord* const record = registry_.Find(id);
    if (!record) {
        return false;
    }

    if (record->runtime.overlayId.has_value()) {
        auto* const overlayManager = static_cast<cISTEOverlayManager*>(const_cast<void*>(record->runtime.overlayManager));
        // Otherwise the city is closing: the index is cleared and the manager may be gone.
        if (removeRuntimeOverlay && overlayManager) {
            UnindexOverlay_(overlayManager, *record->runtime.overlayId);
            overlayManager->RemoveOverlay(*record->runtime.overlayId);
        }
    }

    return registry_.Remove(id);
}

// --- Paints -------------------------------------------------------------------------------------

uint32_t TerrainDecalService::GetStyleSize() const
{
    return static_cast<uint32_t>(sizeof(TerrainPaintStyle));
}

bool TerrainDecalService::PaintsAvailable_() const noexcept
{
    return enableCustomRenderer_ && renderHook_ && renderHook_->IsInstalled();
}

bool TerrainDecalService::CreatePaint(const TerrainPaintShape& shape,
                                      const TerrainPaintStyle* const style,
                                      const uint32_t styleSize,
                                      TerrainPaintId* const outId)
{
    if (!style || !outId || styleSize < kTerrainPaintStyleSizeV1) {
        return false;
    }
    if (!PaintsAvailable_()) {
        LOG_WARN("TerrainDecalService: terrain paint needs EnableCustomTerrainDecalRenderer=true and the render hook");
        return false;
    }
    cISC4City* city = nullptr;
    if (!TryGetCurrentCity_(city)) {
        return false;
    }

    PaintRecord record{};
    record.data.style = CopyFromCaller(style, styleSize);
    std::string error;
    if (!ValidateStyle(record.data.style, error)) {
        LOG_WARN("TerrainDecalService: CreatePaint: {}", error);
        return false;
    }
    if (!CopyShape(shape, record.data)) {
        LOG_WARN("TerrainDecalService: CreatePaint: the shape has no points or too many");
        return false;
    }

    record.data.id = TerrainPaintId{nextPaintId_++};
    record.sequence = nextSequence_++;
    const TerrainPaintId id = record.data.id;
    if (!InsertPaint_(std::move(record), true)) {
        return false;
    }

    *outId = id;
    return true;
}

bool TerrainDecalService::InsertPaint_(PaintRecord&& record, const bool realizeNow)
{
    record.mesh = BuildMesh(record.data);
    if (!record.mesh->HasArea()) {
        LOG_WARN("TerrainDecalService: paint {} has no area", record.data.id.value);
        return false;
    }
    record.uvFromGround = UvFromGround(record.data.style);

    auto [it, inserted] = paints_.emplace(record.data.id.value, std::move(record));
    if (!inserted) {
        LOG_WARN("TerrainDecalService: duplicate paint id {}", it->first);
        return false;
    }
    if (!realizeNow) {
        unrealizedPaints_.push_back(it->first);
        return true;
    }
    if (!RealizePaint_(it->second)) {
        paints_.erase(it);
        return false;
    }
    return true;
}

bool TerrainDecalService::RemovePaint(const TerrainPaintId id)
{
    const auto it = paints_.find(id.value);
    if (it == paints_.end()) {
        return false;
    }
    UnrealizePaint_(it->second, true);
    paints_.erase(it);
    return true;
}

bool TerrainDecalService::SetPaintShape(const TerrainPaintId id, const TerrainPaintShape& shape)
{
    const auto it = paints_.find(id.value);
    if (it == paints_.end()) {
        return false;
    }

    TerrainPaintCodec::PaintSnapshot data = it->second.data;
    if (!CopyShape(shape, data)) {
        return false;
    }
    auto mesh = BuildMesh(data);
    if (!mesh->HasArea()) {
        return false;
    }

    PaintRecord& record = it->second;
    record.data = std::move(data);
    mesh->ReuseCellsFrom(std::move(record.mesh));
    record.mesh = std::move(mesh);
    return RealizePaint_(record, false);
}

bool TerrainDecalService::SetPaintStyle(const TerrainPaintId id, const TerrainPaintStyle* const style, const uint32_t styleSize)
{
    const auto it = paints_.find(id.value);
    if (it == paints_.end() || !style || styleSize < kTerrainPaintStyleSizeV1) {
        return false;
    }

    TerrainPaintStyle validated = CopyFromCaller(style, styleSize);
    std::string error;
    if (!ValidateStyle(validated, error)) {
        LOG_WARN("TerrainDecalService: SetPaintStyle for {}: {}", id.value, error);
        return false;
    }

    PaintRecord& record = it->second;
    const TerrainPaintStyle& current = record.data.style;
    // These are in the mesh: it is made again when one of them changes.
    const bool featherChanged = validated.featherMeters != current.featherMeters || validated.wear != current.wear ||
                                validated.wearPatchMeters != current.wearPatchMeters ||
                                validated.wearSeed != current.wearSeed;
    bool newTextures = validated.textureCount != current.textureCount;
    for (uint32_t i = 0; !newTextures && i < validated.textureCount; ++i) {
        const cGZPersistResourceKey& a = validated.textures[i].key;
        const cGZPersistResourceKey& b = current.textures[i].key;
        newTextures = a.type != b.type || a.group != b.group || a.instance != b.instance;
    }
    // Overlays made again in another layer also make the game draw the static view again
    // where they lie.
    newTextures = newTextures || validated.layer != current.layer;
    record.data.style = validated;
    record.uvFromGround = UvFromGround(validated);
    if (featherChanged) {
        // Only the cells of a paint whose settings did not change can be kept; ReuseCellsFrom checks.
        auto mesh = BuildMesh(record.data);
        mesh->ReuseCellsFrom(std::move(record.mesh));
        record.mesh = std::move(mesh);
    }
    // A texture is an overlay's: new textures need new overlays.
    return RealizePaint_(record, newTextures);
}

bool TerrainDecalService::GetPaintStyle(const TerrainPaintId id, TerrainPaintStyle* const outStyle, const uint32_t styleSize) const
{
    const auto it = paints_.find(id.value);
    if (it == paints_.end() || !outStyle || styleSize < kTerrainPaintStyleSizeV1) {
        return false;
    }
    CopyToCaller(it->second.data.style, outStyle, styleSize);
    return true;
}

uint32_t TerrainDecalService::GetPaintCount() const
{
    return static_cast<uint32_t>(paints_.size());
}

bool TerrainDecalService::RealizePaint_(PaintRecord& record, const bool newTextures)
{
    const bool keepOverlays = !newTextures && record.overlayManager &&
                              record.overlayIds.size() == record.data.style.textureCount;
    if (!keepOverlays) {
        UnrealizePaint_(record, true);
    }
    // The paint changed: what the renderer kept of it is out of date.
    record.drawCaches.assign(record.data.style.textureCount, {});
    if (!PaintsAvailable_()) {
        // Kept, so it is saved again, but not drawn.
        return true;
    }

    cISC4City* city = nullptr;
    if (!TryGetCurrentCity_(city)) {
        return false;
    }
    const auto overlayType = PaintOverlayManager(record.data.style.layer);
    cISTEOverlayManager* const overlayManager = ResolveOverlayManager_(city, overlayType);
    if (!overlayManager) {
        return false;
    }

    // One overlay per texture over the paint's cells; the renderer draws the paint in place of
    // the overlay's square.
    const TerrainPaint::CellRange& cells = record.mesh->Cells();
    const float x0 = static_cast<float>(cells.minX) * TerrainPaint::kCellSize;
    const float z0 = static_cast<float>(cells.minZ) * TerrainPaint::kCellSize;
    const float x1 = static_cast<float>(cells.maxX + 1) * TerrainPaint::kCellSize;
    const float z1 = static_cast<float>(cells.maxZ + 1) * TerrainPaint::kCellSize;

    TerrainDecalState overlayState{};
    overlayState.overlayType = overlayType;
    overlayState.decalInfo.center = cS3DVector2((x0 + x1) * 0.5f, (z0 + z1) * 0.5f);
    overlayState.decalInfo.baseSize = std::max(x1 - x0, z1 - z0);
    overlayState.decalInfo.rotationTurns = 0.0f;
    overlayState.decalInfo.aspectMultiplier = 1.0f;
    overlayState.decalInfo.uvScaleU = 1.0f;
    overlayState.decalInfo.uvScaleV = 1.0f;
    overlayState.opacity = record.data.style.opacity;
    overlayState.enabled = record.data.style.enabled;
    overlayState.color = record.data.style.color;

    if (keepOverlays && overlayManager == record.overlayManager) {
        for (const uint32_t overlayId : record.overlayIds) {
            overlayManager->UpdateDecalInfo(overlayId, overlayState.decalInfo);
            overlayManager->MoveDecal(overlayId, overlayState.decalInfo.center);
            overlayManager->SetOverlayAlpha(overlayId, overlayState.opacity);
            overlayManager->SetOverlayEnabled(overlayId, overlayState.enabled);
            overlayManager->SetOverlayColor(overlayId, overlayState.color);
        }
        return true;
    }
    UnrealizePaint_(record, true);

    record.overlayManager = overlayManager;
    for (uint32_t i = 0; i < record.data.style.textureCount; ++i) {
        overlayState.textureKey = record.data.style.textures[i].key;
        const uint32_t overlayId = AddConfiguredOverlay_(overlayManager, overlayState);
        if (!IsValidOverlayId(overlayId)) {
            UnrealizePaint_(record, true);
            return false;
        }
        record.overlayIds.push_back(overlayId);
        IndexOverlay_(overlayManager, overlayId,
                      OverlayTarget{ManagedOverlay::Kind::Paint, record.data.id.value, i,
                                    record.data.style.layer == TerrainPaintLayer::UnderLots});
    }
    return true;
}

void TerrainDecalService::UnrealizePaint_(PaintRecord& record, const bool removeOverlays)
{
    auto* const overlayManager = static_cast<cISTEOverlayManager*>(const_cast<void*>(record.overlayManager));
    for (const uint32_t overlayId : record.overlayIds) {
        if (removeOverlays && overlayManager) {
            UnindexOverlay_(overlayManager, overlayId);
            overlayManager->RemoveOverlay(overlayId);
        }
    }
    record.overlayIds.clear();
    record.overlayManager = nullptr;
}

// --- Renderer callbacks -------------------------------------------------------------------------

bool TerrainDecalService::ResolveOverlay(const void* const overlayManager,
                                         const uint32_t overlayId,
                                         ManagedOverlay& out)
{
    const auto it = overlayIndex_.find(OverlayKey{overlayManager, overlayId});
    if (it == overlayIndex_.end()) {
        return false;
    }

    const OverlayTarget& target = it->second;
    out.kind = target.kind;
    if (target.kind == ManagedOverlay::Kind::Decal) {
        const TerrainDecalRecord* const record = registry_.Find(TerrainDecalId{target.recordId});
        if (!record) {
            return false;
        }
        PopulateOverrides(out.decal, record->state);
        return true;
    }

    const auto paint = paints_.find(target.recordId);
    if (paint == paints_.end()) {
        return false;
    }
    out.paint = TerrainDecal::PaintOverlay{
        .mesh = paint->second.mesh.get(),
        .style = &paint->second.data.style,
        .textureIndex = target.textureIndex,
        .uvFromGround = paint->second.uvFromGround,
        .cache = target.textureIndex < paint->second.drawCaches.size() ? &paint->second.drawCaches[target.textureIndex]
                                                                       : nullptr,
    };
    return true;
}

std::pair<int32_t, uint32_t> TerrainDecalService::OverlayDrawOrder(const void* const overlayManager, const uint32_t overlayId)
{
    const auto it = overlayIndex_.find(OverlayKey{overlayManager, overlayId});
    if (it == overlayIndex_.end()) {
        return {0, 0};
    }

    if (it->second.kind == ManagedOverlay::Kind::Decal) {
        const TerrainDecalRecord* const record = registry_.Find(TerrainDecalId{it->second.recordId});
        return record ? std::pair{record->state.drawOrder, record->sequence} : std::pair{0, 0u};
    }
    const auto paint = paints_.find(it->second.recordId);
    return paint != paints_.end() ? std::pair{paint->second.data.style.drawOrder, paint->second.sequence}
                                  : std::pair{0, 0u};
}

bool TerrainDecalService::OverlayUnderLots(const void* const overlayManager, const uint32_t overlayId)
{
    const auto it = overlayIndex_.find(OverlayKey{overlayManager, overlayId});
    return it != overlayIndex_.end() && it->second.underLots;
}

bool TerrainDecalService::HasOverlaysUnderLots(const void* const overlayManager)
{
    const auto it = underLotsCounts_.find(overlayManager);
    return it != underLotsCounts_.end() && it->second > 0;
}

void TerrainDecalService::IndexOverlay_(const void* const overlayManager, const uint32_t overlayId,
                                        const OverlayTarget& target)
{
    UnindexOverlay_(overlayManager, overlayId);
    overlayIndex_[OverlayKey{overlayManager, OverlaySlotKey(overlayManager, overlayId)}] = target;
    if (target.underLots) {
        ++underLotsCounts_[overlayManager];
    }
}

void TerrainDecalService::UnindexOverlay_(const void* const overlayManager, const uint32_t overlayId)
{
    const auto it = overlayIndex_.find(OverlayKey{overlayManager, OverlaySlotKey(overlayManager, overlayId)});
    if (it == overlayIndex_.end()) {
        return;
    }
    if (it->second.underLots) {
        const auto count = underLotsCounts_.find(overlayManager);
        if (count != underLotsCounts_.end() && --count->second == 0) {
            underLotsCounts_.erase(count);
        }
    }
    overlayIndex_.erase(it);
}

// --- Drawing under lots -------------------------------------------------------------------------

void TerrainDecalService::OnPreStaticPass_(const DrawServicePass pass, const bool begin, void* const userData)
{
    // At the end of the pass the terrain is drawn and the lots and networks are not yet.
    if (pass == DrawServicePass::PreStatic && !begin && userData) {
        static_cast<TerrainDecalService*>(userData)->DrawUnderLots_();
    }
}

void TerrainDecalService::DrawUnderLots_()
{
    if (!cityLoaded_ || !renderHook_ || !drawService_ || underLotsCounts_.empty()) {
        return;
    }
    cISC4City* city = nullptr;
    if (!TryGetCurrentCity_(city)) {
        return;
    }
    cISTEOverlayManager* const overlayManager =
        ResolveOverlayManager_(city, cISTETerrainView::tOverlayManagerType::StaticLand);
    const SC4DrawContextHandle drawContext = drawService_->WrapActiveRendererDrawContext();
    if (!overlayManager || !drawContext.ptr) {
        return;
    }
    renderHook_->DrawUnderLots(overlayManager, static_cast<SC4DrawContext*>(drawContext.ptr));
}

// --- Lifetime, save and load --------------------------------------------------------------------

bool TerrainDecalService::OnTick(const uint32_t unknown1)
{
    (void)unknown1;

    if (cityLoaded_) {
        if (!pendingLoadedDecals_.empty()) {
            RebindLoadedDecals_();
        }
        if (!unrealizedPaints_.empty()) {
            RealizeLoadedPaints_();
        }
    }
    return true;
}

void TerrainDecalService::SetEnableCustomRenderer(const bool enableCustomRenderer) noexcept
{
    enableCustomRenderer_ = enableCustomRenderer;
}

void TerrainDecalService::SetCustomDefaultDepthOffset(const int customDefaultDepthOffset) noexcept
{
    customDefaultDepthOffset_ = customDefaultDepthOffset;
}

void TerrainDecalService::SetShadowRecoveryOpacityScale(const float shadowRecoveryOpacityScale) noexcept
{
    shadowRecoveryOpacityScale_ = shadowRecoveryOpacityScale;
}

void TerrainDecalService::SetShadowRecoveryUseStencil(const bool shadowRecoveryUseStencil) noexcept
{
    shadowRecoveryUseStencil_ = shadowRecoveryUseStencil;
}

void TerrainDecalService::SetDrawService(cIGZDrawService* const drawService) noexcept
{
    drawService_ = drawService;
}

bool TerrainDecalService::Init()
{
    if (versionTag_ != 641) {
        LOG_WARN("TerrainDecalService: not registering, game version {} != 641", versionTag_);
        return false;
    }

    renderHook_ = std::make_unique<TerrainDecal::TerrainDecalHook>(TerrainDecal::TerrainDecalHook::Options{
        .installEnabled = true,
        .enableCustomRenderer = enableCustomRenderer_,
        .customDefaultDepthOffset = customDefaultDepthOffset_,
        .shadowRecoveryOpacityScale = shadowRecoveryOpacityScale_,
        .shadowRecoveryUseStencil = shadowRecoveryUseStencil_,
    });

    if (!renderHook_->Install()) {
        LOG_WARN("TerrainDecalService: render hook install failed: {}", renderHook_->GetLastError());
        renderHook_.reset();
    }
    else {
        renderHook_->SetOverlaySource(this);
        if (drawService_ &&
            drawService_->RegisterDrawPassCallback(DrawServicePass::PreStatic, &OnPreStaticPass_, this,
                                                   &preStaticCallbackToken_)) {
            renderHook_->SetUnderLotsPassAvailable(true);
        }
        else {
            preStaticCallbackToken_ = 0;
            LOG_WARN("TerrainDecalService: without the draw service, paints and decals under lots are drawn "
                     "with the others");
        }
    }

    return true;
}

bool TerrainDecalService::Shutdown()
{
    if (drawService_ && preStaticCallbackToken_ != 0) {
        drawService_->UnregisterDrawPassCallback(preStaticCallbackToken_);
    }
    preStaticCallbackToken_ = 0;

    ClearRuntimeState_(false);
    pendingLoadedDecals_.clear();

    if (renderHook_) {
        renderHook_->SetUnderLotsPassAvailable(false);
        renderHook_->SetOverlaySource(nullptr);
        renderHook_->Uninstall();
        renderHook_.reset();
    }

    cityLoaded_ = false;
    return true;
}

bool TerrainDecalService::HandleMessage(cIGZMessage2* const message)
{
    auto* standardMessage = static_cast<cIGZMessage2Standard*>(message);
    if (!standardMessage) {
        return false;
    }

    switch (standardMessage->GetType()) {
    case kSC4MessagePostCityInit:
        OnPostCityInit_(standardMessage);
        return true;
    case kSC4MessagePreCityShutdown:
        OnPreCityShutdown_(standardMessage);
        return true;
    case kSC4MessageLoad:
        OnLoad_(standardMessage);
        return true;
    case kSC4MessageSave:
        OnSave_(standardMessage);
        return true;
    default:
        return false;
    }
}

void TerrainDecalService::ClearRuntimeState_(const bool removeRuntimeObjects)
{
    std::vector<TerrainDecalId> ids;
    ids.reserve(registry_.GetCount());
    for (const auto& [idValue, record] : registry_.Records()) {
        ids.push_back(TerrainDecalId{idValue});
    }
    for (const TerrainDecalId id : ids) {
        RemoveRuntimeDecal_(id, removeRuntimeObjects);
    }
    registry_.Clear();

    for (auto& [id, paint] : paints_) {
        UnrealizePaint_(paint, removeRuntimeObjects);
    }
    paints_.clear();
    unrealizedPaints_.clear();
    nextPaintId_ = 1;
    nextSequence_ = 1;
    overlayIndex_.clear();
    underLotsCounts_.clear();
}

void TerrainDecalService::OnPostCityInit_(cIGZMessage2Standard* msg)
{
    (void)msg;
    cityLoaded_ = true;
    RebindLoadedDecals_();
    RealizeLoadedPaints_();
}

void TerrainDecalService::OnPreCityShutdown_(cIGZMessage2Standard* msg)
{
    (void)msg;
    cityLoaded_ = false;
    ClearRuntimeState_(false);
    pendingLoadedDecals_.clear();
}

void TerrainDecalService::OnLoad_(cIGZMessage2Standard* const msg)
{
    pendingLoadedDecals_.clear();
    ClearRuntimeState_(false);

    cISC4DBSegment* const segment = QuerySegmentFromMessage_(msg);
    if (!segment) {
        return;
    }

    cISC4DBSegmentIStream* stream = nullptr;
    if (segment->OpenIStream(TerrainDecalSidecar::kSidecarKey, &stream) && stream) {
        auto result = TerrainDecalSidecar::Read(*stream);
        segment->CloseIStream(stream);
        if (result.ok) {
            pendingLoadedDecals_ = std::move(result.decals);
        }
        else {
            LOG_WARN("TerrainDecalService: failed to load decals: {}", result.error);
        }
    }

    stream = nullptr;
    if (segment->OpenIStream(TerrainPaintCodec::kRecordKey, &stream) && stream) {
        auto result = TerrainPaintCodec::Read(*stream);
        segment->CloseIStream(stream);
        if (result.ok) {
            LoadPaints_(std::move(result.paints));
        }
        else {
            LOG_WARN("TerrainDecalService: failed to load paints: {}", result.error);
        }
    }

    segment->Release();
}

void TerrainDecalService::OnSave_(cIGZMessage2Standard* const msg)
{
    cISC4DBSegment* const segment = QuerySegmentFromMessage_(msg);
    if (!segment) {
        return;
    }
    SaveDecals_(*segment);
    SavePaints_(*segment);
    segment->Release();
}

void TerrainDecalService::SaveDecals_(cISC4DBSegment& segment) const
{
    std::vector<TerrainDecalSnapshot> snapshots;
    snapshots.reserve(registry_.GetCount());
    for (const auto& [id, record] : registry_.Records()) {
        snapshots.push_back(TerrainDecalSnapshot{.id = TerrainDecalId{id}, .state = record.state});
    }

    if (snapshots.empty()) {
        TerrainDecalSidecar::DeleteRecord(segment.AsIGZPersistDBSegment());
        return;
    }

    cISC4DBSegmentOStream* stream = nullptr;
    if (!segment.OpenOStream(TerrainDecalSidecar::kSidecarKey, &stream, true) || !stream) {
        LOG_WARN("TerrainDecalService: could not open the decal record for writing");
        return;
    }
    const bool ok = TerrainDecalSidecar::Write(*stream, snapshots);
    segment.CloseOStream(stream);
    if (!ok) {
        LOG_WARN("TerrainDecalService: failed to write the decal record");
    }
}

void TerrainDecalService::SavePaints_(cISC4DBSegment& segment) const
{
    std::vector<const TerrainPaintCodec::PaintSnapshot*> paints;
    paints.reserve(paints_.size());
    for (const auto& [id, paint] : paints_) {
        if ((paint.data.style.flags & kTerrainPaintFlagTransient) == 0) {
            paints.push_back(&paint.data);
        }
    }

    if (paints.empty()) {
        TerrainPaintCodec::DeleteRecord(segment.AsIGZPersistDBSegment());
        return;
    }

    cISC4DBSegmentOStream* stream = nullptr;
    if (!segment.OpenOStream(TerrainPaintCodec::kRecordKey, &stream, true) || !stream) {
        LOG_WARN("TerrainDecalService: could not open the paint record for writing");
        return;
    }
    const bool ok = TerrainPaintCodec::Write(*stream, paints);
    segment.CloseOStream(stream);
    if (!ok) {
        LOG_WARN("TerrainDecalService: failed to write the paint record");
    }
}

bool TerrainDecalService::TryGetCurrentCity_(cISC4City*& city) const
{
    const cISC4AppPtr app;
    city = app ? app->GetCity() : nullptr;
    return city != nullptr;
}

cISTEOverlayManager* TerrainDecalService::ResolveOverlayManager_(cISC4City* const city,
                                                                 const cISTETerrainView::tOverlayManagerType overlayType) const
{
    if (!city) {
        return nullptr;
    }

    cISTETerrain* const terrain = city->GetTerrain();
    cISTETerrainView* const terrainView = terrain ? terrain->GetView() : nullptr;
    return terrainView ? terrainView->GetOverlayManager(overlayType) : nullptr;
}

cISC4DBSegment* TerrainDecalService::QuerySegmentFromMessage_(const cIGZMessage2Standard* const msg) const
{
    if (!msg) {
        return nullptr;
    }

    auto* unknown = static_cast<cIGZUnknown*>(msg->GetVoid1());
    if (!unknown) {
        return nullptr;
    }

    cISC4DBSegment* segment = nullptr;
    if (!unknown->QueryInterface(GZIID_cISC4DBSegment, reinterpret_cast<void**>(&segment)) || !segment) {
        return nullptr;
    }
    return segment;
}

bool TerrainDecalService::ValidateState_(TerrainDecalState& state, std::string& error) const
{
    state.opacity = std::clamp(state.opacity, 0.0f, 1.0f);

    if (state.overlayType != cISTETerrainView::tOverlayManagerType::StaticLand &&
        state.overlayType != cISTETerrainView::tOverlayManagerType::StaticWater &&
        state.overlayType != cISTETerrainView::tOverlayManagerType::DynamicLand &&
        state.overlayType != cISTETerrainView::tOverlayManagerType::DynamicWater) {
        error = "overlayType is invalid";
        return false;
    }

    if (!(state.decalInfo.baseSize > 0.0f)) {
        error = "baseSize must be greater than zero";
        return false;
    }

    if (state.hasUvWindow &&
        (!(state.uvWindow.u1 <= state.uvWindow.u2) || !(state.uvWindow.v1 <= state.uvWindow.v2))) {
        error = "uv window bounds are invalid";
        return false;
    }

    return true;
}

void TerrainDecalService::RebindLoadedDecals_()
{
    if (pendingLoadedDecals_.empty()) {
        return;
    }

    cISC4City* city = nullptr;
    if (!TryGetCurrentCity_(city)) {
        return;
    }

    std::vector<TerrainDecalSnapshot> pending = std::move(pendingLoadedDecals_);
    std::vector<TerrainDecalSnapshot> remaining;
    for (const TerrainDecalSnapshot& snapshot : pending) {
        if (snapshot.id.value == 0) {
            continue;
        }
        if (registry_.Find(snapshot.id)) {
            LOG_WARN("TerrainDecalService: duplicate loaded decal id {}", snapshot.id.value);
            continue;
        }
        if (!ResolveOverlayManager_(city, snapshot.state.overlayType) ||
            !CreateRuntimeDecal_(snapshot.id, snapshot.state, nextSequence_)) {
            remaining.push_back(snapshot);
            continue;
        }
        ++nextSequence_;
        registry_.UpdateNextIdFromLoaded(snapshot.id);
    }
    pendingLoadedDecals_ = std::move(remaining);
}

void TerrainDecalService::LoadPaints_(std::vector<TerrainPaintCodec::PaintSnapshot>&& paints)
{
    for (TerrainPaintCodec::PaintSnapshot& snapshot : paints) {
        std::string error;
        if (snapshot.id.value == 0 || !ValidateStyle(snapshot.style, error)) {
            LOG_WARN("TerrainDecalService: dropping loaded paint {}: {}", snapshot.id.value, error);
            continue;
        }
        const uint32_t id = snapshot.id.value;
        PaintRecord record{};
        record.data = std::move(snapshot);
        record.sequence = nextSequence_++;
        if (InsertPaint_(std::move(record), false)) {
            nextPaintId_ = std::max(nextPaintId_, id + 1);
        }
    }
}

void TerrainDecalService::RealizeLoadedPaints_()
{
    cISC4City* city = nullptr;
    if (unrealizedPaints_.empty() || !TryGetCurrentCity_(city) ||
        !ResolveOverlayManager_(city, cISTETerrainView::tOverlayManagerType::StaticLand)) {
        return;
    }

    for (const uint32_t id : std::exchange(unrealizedPaints_, {})) {
        const auto it = paints_.find(id);
        if (it != paints_.end() && !RealizePaint_(it->second)) {
            // Kept, so the next save keeps it, even though it cannot be drawn.
            LOG_WARN("TerrainDecalService: loaded paint {} could not be drawn", id);
        }
    }
}
