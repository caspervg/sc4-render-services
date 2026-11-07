#pragma once

#include <array>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "cRZBaseSystemService.h"
#include "public/cIGZTerrainDecalService.h"
#include "public/cIGZTerrainPaintService.h"
#include "public/TerrainDecalServiceIds.h"
#include "utils/VersionDetection.h"
#include "TerrainDecalHook.h"
#include "TerrainDecalRegistry.h"
#include "TerrainPaintCodec.h"
#include "TerrainPaintMesh.h"

class cIGZMessage2;
class cIGZMessage2Standard;
class cISC4DBSegment;
class cISC4City;
class cISTEOverlayManager;

class TerrainDecalService final : public cRZBaseSystemService,
                                  public cIGZTerrainDecalService,
                                  public cIGZTerrainPaintService,
                                  private TerrainDecal::ManagedOverlaySource {
public:
    TerrainDecalService();
    ~TerrainDecalService() = default;

    uint32_t AddRef() override;
    uint32_t Release() override;
    bool QueryInterface(uint32_t riid, void** ppvObj) override;

    // cIGZTerrainDecalService
    [[nodiscard]] uint32_t GetServiceID() const override;
    uint32_t GetStateSize() const override;
    uint32_t GetSnapshotSize() const override;
    bool CreateDecal(const TerrainDecalState* initialState, uint32_t stateSize, TerrainDecalId* outId) override;
    bool RemoveDecal(TerrainDecalId id) override;
    bool GetDecal(TerrainDecalId id, TerrainDecalSnapshot* outSnapshot, uint32_t snapshotSize) const override;
    bool ReplaceDecal(TerrainDecalId id, const TerrainDecalState* newState, uint32_t stateSize) override;
    uint32_t GetDecalCount() const override;
    uint32_t CopyDecals(TerrainDecalSnapshot* buffer, uint32_t capacity, uint32_t snapshotSize) const override;

    // cIGZTerrainPaintService
    [[nodiscard]] uint32_t GetStyleSize() const override;
    bool CreatePaint(const TerrainPaintShape& shape, const TerrainPaintStyle* style, uint32_t styleSize,
                     TerrainPaintId* outId) override;
    bool RemovePaint(TerrainPaintId id) override;
    bool SetPaintShape(TerrainPaintId id, const TerrainPaintShape& shape) override;
    bool SetPaintStyle(TerrainPaintId id, const TerrainPaintStyle* style, uint32_t styleSize) override;
    bool GetPaintStyle(TerrainPaintId id, TerrainPaintStyle* outStyle, uint32_t styleSize) const override;
    [[nodiscard]] uint32_t GetPaintCount() const override;

    bool OnTick(uint32_t unknown1) override;

    void SetEnableCustomRenderer(bool enableCustomRenderer) noexcept;
    void SetCustomDefaultDepthOffset(int customDefaultDepthOffset) noexcept;
    void SetShadowRecoveryOpacityScale(float shadowRecoveryOpacityScale) noexcept;
    void SetShadowRecoveryUseStencil(bool shadowRecoveryUseStencil) noexcept;
    bool Init() override;
    bool Shutdown();
    bool HandleMessage(cIGZMessage2* message);

private:
    struct PaintRecord {
        TerrainPaintCodec::PaintSnapshot data{};
        std::unique_ptr<TerrainPaint::Mesh> mesh{};
        std::array<float, 6> uvFromGround{};
        // One overlay per texture while the paint is in the city, and what the renderer keeps of
        // each between frames.
        std::vector<uint32_t> overlayIds{};
        std::vector<TerrainDecal::PaintDrawCache> drawCaches{};
        const void* overlayManager = nullptr;
        uint32_t sequence = 0;
    };

    struct OverlayKey {
        const void* overlayManager = nullptr;
        uint32_t overlayId = 0;

        bool operator==(const OverlayKey&) const = default;
    };

    struct OverlayKeyHash {
        size_t operator()(const OverlayKey& key) const noexcept;
    };

    struct OverlayTarget {
        TerrainDecal::ManagedOverlay::Kind kind = TerrainDecal::ManagedOverlay::Kind::Decal;
        uint32_t recordId = 0;
        uint32_t textureIndex = 0;
    };

    // ManagedOverlaySource
    bool ResolveOverlay(const void* overlayManager, uint32_t overlayId, TerrainDecal::ManagedOverlay& out) override;
    std::pair<int32_t, uint32_t> OverlayDrawOrder(const void* overlayManager, uint32_t overlayId) override;

    bool CreateRuntimeDecal_(TerrainDecalId id, const TerrainDecalState& state, uint32_t sequence);
    bool ApplyStateToRuntime_(TerrainDecalRecord& record, const TerrainDecalState& state);
    bool RemoveRuntimeDecal_(TerrainDecalId id, bool removeRuntimeOverlay);
    [[nodiscard]] uint32_t AddConfiguredOverlay_(cISTEOverlayManager* overlayManager, const TerrainDecalState& state) const;

    // Builds the mesh and stores the paint; draws it now or once the city is ready.
    bool InsertPaint_(PaintRecord&& record, bool realizeNow);
    // Puts the paint in the city. With newTextures false, the overlays it already has are moved
    // and recoloured instead of made again.
    bool RealizePaint_(PaintRecord& record, bool newTextures = true);
    void UnrealizePaint_(PaintRecord& record, bool removeOverlays);
    [[nodiscard]] bool PaintsAvailable_() const noexcept;

    void ClearRuntimeState_(bool removeRuntimeOverlays);
    void OnPostCityInit_(cIGZMessage2Standard* msg);
    void OnPreCityShutdown_(cIGZMessage2Standard* msg);
    void OnLoad_(cIGZMessage2Standard* msg);
    void OnSave_(cIGZMessage2Standard* msg);
    void SaveDecals_(cISC4DBSegment& segment) const;
    void SavePaints_(cISC4DBSegment& segment) const;
    bool TryGetCurrentCity_(cISC4City*& city) const;
    cISTEOverlayManager* ResolveOverlayManager_(cISC4City* city,
                                               cISTETerrainView::tOverlayManagerType overlayType) const;
    cISC4DBSegment* QuerySegmentFromMessage_(const cIGZMessage2Standard* msg) const;
    bool ValidateState_(TerrainDecalState& state, std::string& error) const;
    void RebindLoadedDecals_();
    void LoadPaints_(std::vector<TerrainPaintCodec::PaintSnapshot>&& paints);
    void RealizeLoadedPaints_();

private:
    uint16_t versionTag_{};
    TerrainDecalRegistry registry_{};
    std::map<uint32_t, PaintRecord> paints_{};
    uint32_t nextPaintId_ = 1;
    uint32_t nextSequence_ = 1;
    std::unordered_map<OverlayKey, OverlayTarget, OverlayKeyHash> overlayIndex_{};
    std::unique_ptr<TerrainDecal::TerrainDecalHook> renderHook_{};
    std::vector<TerrainDecalSnapshot> pendingLoadedDecals_{};
    // Loaded paints whose overlays wait for the city.
    std::vector<uint32_t> unrealizedPaints_{};
    bool enableCustomRenderer_ = true;
    int customDefaultDepthOffset_ = 2;
    float shadowRecoveryOpacityScale_ = 0.25f;
    bool shadowRecoveryUseStencil_ = true;
    bool cityLoaded_ = false;
};
