#include "TerrainDecalService.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
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
    constexpr uint32_t kSC4MessagePostCityInit = 0x26D31EC1;
    constexpr uint32_t kSC4MessagePreCityShutdown = 0x26D31EC2;
    constexpr uint32_t kSC4MessageLoad = 0x26C63341;
    constexpr uint32_t kSC4MessageSave = 0x26C63344;
    constexpr uint32_t kInvalidOverlayId = 0xFFFFFFFFu;

    [[nodiscard]] uint32_t NormalizeOverlayIdKey(const uint32_t overlayId) noexcept
    {
        return overlayId & 0x7FFFFFFFu;
    }

    [[nodiscard]] bool IsValidOverlayId(const uint32_t overlayId) noexcept
    {
        return overlayId != kInvalidOverlayId;
    }

    [[nodiscard]] TerrainDecalState CopyStateFromCaller(const TerrainDecalState* const source,
                                                        const uint32_t sourceSize) noexcept
    {
        TerrainDecalState state{};
        if (source && sourceSize > 0) {
            const size_t bytesToCopy = std::min<size_t>(sourceSize, sizeof(TerrainDecalState));
            std::memcpy(&state, source, bytesToCopy);
        }
        return state;
    }

    bool CopySnapshotToCaller(const TerrainDecalSnapshot& snapshot,
                              TerrainDecalSnapshot* const destination,
                              const uint32_t destinationSize) noexcept
    {
        if (!destination || destinationSize < kTerrainDecalSnapshotSize) {
            return false;
        }

        const size_t bytesToCopy = std::min<size_t>(destinationSize, sizeof(TerrainDecalSnapshot));
        std::memcpy(destination, &snapshot, bytesToCopy);
        return true;
    }
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

    return cRZBaseSystemService::QueryInterface(riid, ppvObj);
}

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
    if (!initialState || !outId || stateSize < kTerrainDecalStateSize) {
        return false;
    }

    TerrainDecalState state = CopyStateFromCaller(initialState, stateSize);
    std::string error;
    if (!ValidateState_(state, error)) {
        LOG_WARN("TerrainDecalService: CreateDecal validation failed: {}", error);
        return false;
    }

    const TerrainDecalId id = registry_.AllocateId();
    if (!CreateRuntimeDecal_(id, state, false)) {
        return false;
    }

    *outId = id;
    return true;
}

bool TerrainDecalService::RemoveDecal(const TerrainDecalId id)
{
    if (id.value == 0) {
        return false;
    }

    return RemoveRuntimeDecal_(id, true);
}

bool TerrainDecalService::GetDecal(const TerrainDecalId id,
                                   TerrainDecalSnapshot* const outSnapshot,
                                   const uint32_t snapshotSize) const
{
    if (!outSnapshot || id.value == 0 || snapshotSize < kTerrainDecalSnapshotSize) {
        return false;
    }

    const TerrainDecalRecord* const record = registry_.Find(id);
    if (!record) {
        return false;
    }

    return CopySnapshotToCaller(TerrainDecalSnapshot{.id = record->id, .state = record->state},
                                outSnapshot,
                                snapshotSize);
}

bool TerrainDecalService::ReplaceDecal(const TerrainDecalId id,
                                       const TerrainDecalState* const newState,
                                       const uint32_t stateSize)
{
    if (!newState || id.value == 0 || stateSize < kTerrainDecalStateSize) {
        return false;
    }

    TerrainDecalRecord* const record = registry_.Find(id);
    if (!record) {
        return false;
    }

    TerrainDecalState validated = CopyStateFromCaller(newState, stateSize);
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
    if (!buffer || capacity == 0 || snapshotSize < kTerrainDecalSnapshotSize) {
        return 0;
    }

    uint32_t count = 0;
    auto* const bytes = reinterpret_cast<std::byte*>(buffer);
    for (const auto& [id, record] : registry_.Records()) {
        if (count >= capacity) {
            break;
        }

        auto* const destination = reinterpret_cast<TerrainDecalSnapshot*>(bytes + (snapshotSize * count));
        if (!CopySnapshotToCaller(TerrainDecalSnapshot{.id = TerrainDecalId{id}, .state = record.state},
                                  destination,
                                  snapshotSize)) {
            break;
        }
        ++count;
    }

    return count;
}

bool TerrainDecalService::OnTick(const uint32_t unknown1)
{
    (void)unknown1;

    if (cityLoaded_ && !pendingLoadedDecals_.empty()) {
        RebindLoadedDecals_();
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
    });

    if (!renderHook_->Install()) {
        LOG_WARN("TerrainDecalService: render hook install failed: {}", renderHook_->GetLastError());
        renderHook_.reset();
    } else {
        renderHook_->SetOverlayOverridesResolver(&TerrainDecalService::ResolveOverlayOverrides_, this);
    }

    return true;
}

bool TerrainDecalService::Shutdown()
{
    ClearRuntimeState_(false);
    pendingLoadedDecals_.clear();

    if (renderHook_) {
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

bool TerrainDecalService::CreateRuntimeDecal_(const TerrainDecalId id, const TerrainDecalState& state, const bool updateNextId)
{
    cISC4City* city = nullptr;
    if (!TryGetCurrentCity_(city) || !city) {
        LOG_WARN("TerrainDecalService: no active city for decal creation");
        return false;
    }

    cISTEOverlayManager* const overlayManager = ResolveOverlayManager_(city, state.overlayType);
    if (!overlayManager) {
        LOG_WARN("TerrainDecalService: no active overlay manager for decal creation");
        return false;
    }

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
        return false;
    }

    overlayManager->UpdateDecalInfo(overlayId, state.decalInfo);
    overlayManager->MoveDecal(overlayId, state.decalInfo.center);
    overlayManager->SetOverlayAlpha(overlayId, state.opacity);
    overlayManager->SetOverlayEnabled(overlayId, state.enabled);
    overlayManager->SetOverlayColor(overlayId, state.color);
    overlayManager->SetOverlayDrawMode(overlayId, state.drawMode);
    // Temporary test: do not force overlay flags from TerrainDecal state.
    // The old Plop and Paint overlay sample never wrote flags on create/update.
    // overlayManager->SetOverlayFlags(overlayId, state.flags);

    TerrainDecalRecord record{};
    record.id = id;
    record.state = state;
    record.runtime.overlayId = overlayId;

    if (updateNextId) {
        registry_.UpdateNextIdFromLoaded(id);
    }

    if (!registry_.Insert(std::move(record))) {
        overlayManager->RemoveOverlay(overlayId);
        return false;
    }

    if (renderHook_ && state.hasUvWindow) {
        renderHook_->SetOverlayUvWindow(overlayId, state.uvWindow);
    }

    return true;
}

bool TerrainDecalService::ApplyStateToRuntime_(TerrainDecalRecord& record, const TerrainDecalState& state)
{
    if (!record.runtime.overlayId.has_value()) {
        return false;
    }

    cISC4City* city = nullptr;
    if (!TryGetCurrentCity_(city) || !city) {
        return false;
    }

    cISTEOverlayManager* const oldOverlayManager = ResolveOverlayManager_(city, record.state.overlayType);
    cISTEOverlayManager* const newOverlayManager = ResolveOverlayManager_(city, state.overlayType);
    if (!oldOverlayManager || !newOverlayManager) {
        return false;
    }

    const uint32_t replacementOverlayId = newOverlayManager->AddDecal(
        state.textureKey,
        state.decalInfo.center,
        state.decalInfo.baseSize,
        state.decalInfo.rotationTurns);

    if (!IsValidOverlayId(replacementOverlayId)) {
        LOG_WARN("TerrainDecalService: replacement AddDecal failed for {} with texture {:08X}:{:08X}:{:08X}",
                 record.id.value,
                 state.textureKey.type,
                 state.textureKey.group,
                 state.textureKey.instance);
        return false;
    }

    const uint32_t overlayId = *record.runtime.overlayId;
    newOverlayManager->UpdateDecalInfo(replacementOverlayId, state.decalInfo);
    newOverlayManager->MoveDecal(replacementOverlayId, state.decalInfo.center);
    newOverlayManager->SetOverlayAlpha(replacementOverlayId, state.opacity);
    newOverlayManager->SetOverlayEnabled(replacementOverlayId, state.enabled);
    newOverlayManager->SetOverlayColor(replacementOverlayId, state.color);
    newOverlayManager->SetOverlayDrawMode(replacementOverlayId, state.drawMode);
    // Temporary test: do not force overlay flags from TerrainDecal state.
    // The old Plop and Paint overlay sample never wrote flags on create/update.
    // newOverlayManager->SetOverlayFlags(replacementOverlayId, state.flags);

    oldOverlayManager->RemoveOverlay(overlayId);

    record.state = state;
    record.runtime.overlayId = replacementOverlayId;

    if (renderHook_) {
        (void)renderHook_->RemoveOverlayUvWindow(overlayId);
        if (state.hasUvWindow) {
            renderHook_->SetOverlayUvWindow(replacementOverlayId, state.uvWindow);
        }
        else {
            (void)renderHook_->RemoveOverlayUvWindow(replacementOverlayId);
        }
    }

    return true;
}

bool TerrainDecalService::RemoveRuntimeDecal_(const TerrainDecalId id, const bool removeRuntimeObject)
{
    TerrainDecalRecord* const record = registry_.Find(id);
    if (!record) {
        return false;
    }

    if (renderHook_ && record->runtime.overlayId.has_value()) {
        (void)renderHook_->RemoveOverlayUvWindow(*record->runtime.overlayId);
    }

    if (removeRuntimeObject && record->runtime.overlayId.has_value()) {
        cISC4City* city = nullptr;
        if (TryGetCurrentCity_(city) && city) {
            cISTEOverlayManager* const overlayManager = ResolveOverlayManager_(city, record->state.overlayType);
            if (overlayManager) {
                overlayManager->RemoveOverlay(*record->runtime.overlayId);
            }
        }
    }

    return registry_.Remove(id);
}

void TerrainDecalService::ClearRuntimeState_(const bool removeRuntimeObjects)
{
    std::vector<TerrainDecalId> ids;
    ids.reserve(registry_.GetCount());
    for (const auto& [idValue, record] : registry_.Records()) {
        (void)record;
        ids.push_back(TerrainDecalId{idValue});
    }

    for (const TerrainDecalId id : ids) {
        RemoveRuntimeDecal_(id, removeRuntimeObjects);
    }

    registry_.Clear();
    if (renderHook_) {
        renderHook_->ClearOverlayUvWindows();
    }
}

void TerrainDecalService::OnPostCityInit_(cIGZMessage2Standard* msg)
{
    (void)msg;
    cityLoaded_ = true;
    RebindLoadedDecals_();
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
    if (!segment->OpenIStream(TerrainDecalSidecar::kSidecarKey, &stream) || !stream) {
        segment->Release();
        return;
    }

    const auto result = TerrainDecalSidecar::Read(*stream);
    segment->CloseIStream(stream);
    segment->Release();

    if (!result.ok) {
        LOG_WARN("TerrainDecalService: failed to load sidecar: {}", result.error);
        return;
    }

    pendingLoadedDecals_ = result.decals;
}

void TerrainDecalService::OnSave_(cIGZMessage2Standard* const msg)
{
    cISC4DBSegment* const segment = QuerySegmentFromMessage_(msg);
    if (!segment) {
        return;
    }

    cIGZPersistDBSegment* const dbSegment = segment->AsIGZPersistDBSegment();

    std::vector<TerrainDecalSnapshot> snapshots;
    snapshots.reserve(registry_.GetCount());
    for (const auto& [id, record] : registry_.Records()) {
        snapshots.push_back(TerrainDecalSnapshot{.id = TerrainDecalId{id}, .state = record.state});
    }

    if (snapshots.empty()) {
        TerrainDecalSidecar::DeleteRecord(dbSegment);
        segment->Release();
        return;
    }

    cISC4DBSegmentOStream* stream = nullptr;
    if (!segment->OpenOStream(TerrainDecalSidecar::kSidecarKey, &stream, true) || !stream) {
        segment->Release();
        return;
    }

    const bool ok = TerrainDecalSidecar::Write(*stream, snapshots);
    segment->CloseOStream(stream);
    segment->Release();

    if (!ok) {
        LOG_WARN("TerrainDecalService: failed to write terrain decal sidecar");
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

bool TerrainDecalService::CaptureLiveState_(const TerrainDecalRecord& record, TerrainDecalState& state) const
{
    if (!record.runtime.overlayId.has_value()) {
        return false;
    }

    cISC4City* city = nullptr;
    if (!TryGetCurrentCity_(city) || !city) {
        return false;
    }

    cISTEOverlayManager* const overlayManager = ResolveOverlayManager_(city, record.state.overlayType);
    if (!overlayManager) {
        return false;
    }

    const uint32_t overlayId = *record.runtime.overlayId;
    if (!IsValidOverlayId(overlayId)) {
        return false;
    }
    overlayManager->DecalInfo(overlayId, &state.decalInfo);
    state.opacity = overlayManager->OverlayAlpha(overlayId);
    state.enabled = overlayManager->OverlayIsEnabled(overlayId);
    state.drawMode = overlayManager->OverlayDrawMode(overlayId);
    state.flags = overlayManager->OverlayFlags(overlayId);
    (void)overlayManager->OverlayColor(overlayId, state.color);
    return true;
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
    if (!TryGetCurrentCity_(city) || !city) {
        return;
    }

    std::vector<TerrainDecalSnapshot> pending = std::move(pendingLoadedDecals_);
    std::vector<TerrainDecalSnapshot> remaining;
    remaining.reserve(pending.size());

    for (const TerrainDecalSnapshot& snapshot : pending) {
        if (snapshot.id.value == 0) {
            continue;
        }

        if (registry_.Find(snapshot.id)) {
            LOG_WARN("TerrainDecalService: duplicate loaded decal id {}", snapshot.id.value);
            continue;
        }

        if (!ResolveOverlayManager_(city, snapshot.state.overlayType)) {
            remaining.push_back(snapshot);
            continue;
        }

        if (!CreateRuntimeDecal_(snapshot.id, snapshot.state, true)) {
            LOG_WARN("TerrainDecalService: failed to recreate decal {}", snapshot.id.value);
            remaining.push_back(snapshot);
        }
    }

    pendingLoadedDecals_ = std::move(remaining);
}

bool TerrainDecalService::ResolveOverlayOverrides_(void* const overlayManager,
                                                   const uint32_t overlayId,
                                                   TerrainDecal::TerrainDecalOverlayOverrides& overrides,
                                                   void* const userData)
{
    auto* const service = static_cast<TerrainDecalService*>(userData);
    auto* const typedOverlayManager = static_cast<cISTEOverlayManager*>(overlayManager);
    return service && typedOverlayManager &&
           service->ResolveOverlayOverridesImpl_(typedOverlayManager, overlayId, overrides);
}

namespace
{
    void PopulateOverrides(TerrainDecal::TerrainDecalOverlayOverrides& overrides,
                           const TerrainDecalState& state) noexcept
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
}

bool TerrainDecalService::ResolveOverlayOverridesImpl_(cISTEOverlayManager* const overlayManager,
                                                       const uint32_t overlayId,
                                                       TerrainDecal::TerrainDecalOverlayOverrides& overrides)
{
    if (!overlayManager) {
        return false;
    }

    cISC4City* city = nullptr;
    if (!TryGetCurrentCity_(city) || !city) {
        return false;
    }

    TerrainDecalRecord* const record = FindRecordByOverlayId_(overlayManager, overlayId);
    if (!record) {
        return false;
    }

    PopulateOverrides(overrides, record->state);
    return true;
}

TerrainDecalRecord* TerrainDecalService::FindRecordByOverlayId_(cISTEOverlayManager* const overlayManager,
                                                                const uint32_t overlayId) noexcept
{
    const uint32_t normalizedOverlayId = NormalizeOverlayIdKey(overlayId);
    for (const auto& [idValue, recordConst] : registry_.Records()) {
        auto* const record = registry_.Find(TerrainDecalId{idValue});
        if (record && record->runtime.overlayId.has_value() &&
            NormalizeOverlayIdKey(*record->runtime.overlayId) == normalizedOverlayId) {
            cISC4City* city = nullptr;
            if (!TryGetCurrentCity_(city) || !city) {
                continue;
            }
            if (overlayManager != ResolveOverlayManager_(city, record->state.overlayType)) {
                continue;
            }
            return record;
        }
    }
    return nullptr;
}
