#include "cIGZCOM.h"
#include "cIGZFrameWork.h"
#include "cIGZMessage2.h"
#include "cRZMessage2COMDirector.h"

#include "mini/ini.h"

#include "service/decal/RelativeCallPatch.h"
#include "utils/Logger.h"
#include "utils/VersionDetection.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>

namespace {
    constexpr uint32_t kDirectorID = 0xD1A90A1F;
    constexpr uint16_t kSupportedGameVersion = 641;

    // SimCity 4.exe 1.1.641 absolute addresses.
    constexpr uintptr_t kCalculateNormalsCallSite = 0x007498CD;
    constexpr uintptr_t kOriginalCalculateNormalsAndAssignCliffTextures = 0x00743B60;
    constexpr uintptr_t kFlipInternalTriangulation = 0x00741180;

    constexpr uint32_t kTerrainCellCountXOffset = 0x28;
    constexpr uint32_t kTerrainCellCountZOffset = 0x2C;
    constexpr uint32_t kTerrainMaxCellXOffset = 0x30;
    constexpr uint32_t kTerrainMaxCellZOffset = 0x34;
    constexpr uint32_t kTerrainRowStrideOffset = 0x38;
    constexpr uint32_t kTerrainVertexDataOffset = 0x6C;
    constexpr uint32_t kTerrainCellRecordSize = 0x24;
    constexpr uint32_t kTerrainCellFlagsOffset = 0x22;
    constexpr uint16_t kCliffFlipMarkerFlag = 0x0800;
    constexpr uint16_t kInternalFlipFlag = 0x1000;

    struct SC4RectLong {
        int32_t left;
        int32_t top;
        int32_t right;
        int32_t bottom;
    };

    struct TerrainDiagonalFixSettings {
        bool enabled = true;
        bool clearNonCandidates = true;
        float minHeightDelta = 12.0f;
        float diagonalHysteresis = 0.05f;
        uint32_t logEveryNChanges = 2048;
    };

    TerrainDiagonalFixSettings gSettings;
    uint64_t gCellsExamined = 0;
    uint64_t gCandidateCells = 0;
    uint64_t gChangedCells = 0;

    using CalculateNormalsFn = void(__thiscall*)(void* terrain, SC4RectLong* rect);
    using FlipInternalTriangulationFn = void(__thiscall*)(void* terrain, uint32_t x, uint32_t z, bool flip);

    template <typename T>
    T ReadField(void* base, const uint32_t offset) {
        return *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(base) + offset);
    }

    uint8_t* GetCellRecord(void* terrain, const uint32_t x, const uint32_t z) {
        const auto rowStride = ReadField<uint32_t>(terrain, kTerrainRowStrideOffset);
        auto* const vertexData = ReadField<uint8_t*>(terrain, kTerrainVertexDataOffset);
        return vertexData + (static_cast<size_t>(z) * rowStride + x) * kTerrainCellRecordSize;
    }

    float GetCellHeight(void* terrain, const uint32_t x, const uint32_t z) {
        return *reinterpret_cast<float*>(GetCellRecord(terrain, x, z));
    }

    uint16_t GetCellFlags(void* terrain, const uint32_t x, const uint32_t z) {
        return *reinterpret_cast<uint16_t*>(GetCellRecord(terrain, x, z) + kTerrainCellFlagsOffset);
    }

    bool ParseBool(const std::string& value, bool defaultValue) {
        std::string text(value);
        std::ranges::transform(text, text.begin(), [](const unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });

        if (text == "true" || text == "1" || text == "yes") {
            return true;
        }
        if (text == "false" || text == "0" || text == "no") {
            return false;
        }
        return defaultValue;
    }

    float ParseFloat(const std::string& value, const float defaultValue) {
        float result = defaultValue;
        const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), result);
        if (ec != std::errc() || ptr != value.data() + value.size() || std::isnan(result) || std::isinf(result)) {
            return defaultValue;
        }
        return result;
    }

    uint32_t ParseUInt32(const std::string& value, const uint32_t defaultValue) {
        uint32_t result = defaultValue;
        const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), result);
        if (ec != std::errc() || ptr != value.data() + value.size()) {
            return defaultValue;
        }
        return result;
    }

    std::filesystem::path GetDllDirectoryPath() {
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&GetDllDirectoryPath),
                &module)) {
            return {};
        }

        wchar_t path[MAX_PATH]{};
        if (GetModuleFileNameW(module, path, MAX_PATH) == 0) {
            return {};
        }

        return std::filesystem::path(path).parent_path();
    }

    void LoadSettings() {
        gSettings = {};

        const auto settingsPath = GetDllDirectoryPath() / "SC4TerrainDiagonalFix.ini";
        mINI::INIFile file(settingsPath.string());
        mINI::INIStructure ini;
        if (!file.read(ini) || !ini.has("SC4TerrainDiagonalFix")) {
            LOG_INFO("TerrainDiagonalFix: using default settings; no readable {}.", settingsPath.string());
            return;
        }

        const auto section = ini.get("SC4TerrainDiagonalFix");
        if (section.has("Enabled")) {
            gSettings.enabled = ParseBool(section.get("Enabled"), gSettings.enabled);
        }
        if (section.has("ClearNonCandidates")) {
            gSettings.clearNonCandidates = ParseBool(section.get("ClearNonCandidates"), gSettings.clearNonCandidates);
        }
        if (section.has("MinHeightDelta")) {
            gSettings.minHeightDelta = std::max(0.0f, ParseFloat(section.get("MinHeightDelta"), gSettings.minHeightDelta));
        }
        if (section.has("DiagonalHysteresis")) {
            gSettings.diagonalHysteresis = std::max(0.0f, ParseFloat(section.get("DiagonalHysteresis"), gSettings.diagonalHysteresis));
        }
        if (section.has("LogEveryNChanges")) {
            gSettings.logEveryNChanges = ParseUInt32(section.get("LogEveryNChanges"), gSettings.logEveryNChanges);
        }

        LOG_INFO("TerrainDiagonalFix: settings enabled={}, clearNonCandidates={}, minHeightDelta={}, "
                 "diagonalHysteresis={}, logEveryNChanges={}",
                 gSettings.enabled,
                 gSettings.clearNonCandidates,
                 gSettings.minHeightDelta,
                 gSettings.diagonalHysteresis,
                 gSettings.logEveryNChanges);
    }

    void ApplyNearCliffDiagonalFix(void* terrain, const SC4RectLong* rect) {
        if (!gSettings.enabled || terrain == nullptr || rect == nullptr) {
            return;
        }

        const auto vertexCountX = ReadField<uint32_t>(terrain, kTerrainCellCountXOffset);
        const auto vertexCountZ = ReadField<uint32_t>(terrain, kTerrainCellCountZOffset);
        if (vertexCountX == 0 || vertexCountZ == 0 || ReadField<uintptr_t>(terrain, kTerrainVertexDataOffset) == 0) {
            return;
        }

        const int32_t maxCellX = static_cast<int32_t>(ReadField<uint32_t>(terrain, kTerrainMaxCellXOffset));
        const int32_t maxCellZ = static_cast<int32_t>(ReadField<uint32_t>(terrain, kTerrainMaxCellZOffset));
        const int32_t left = std::max(0, rect->left - 1);
        const int32_t top = std::max(0, rect->top - 1);
        const int32_t right = std::min(rect->right, maxCellX);
        const int32_t bottom = std::min(rect->bottom, maxCellZ);
        if (left > right || top > bottom) {
            return;
        }

        const auto flipInternal = reinterpret_cast<FlipInternalTriangulationFn>(kFlipInternalTriangulation);
        uint32_t changedThisPass = 0;
        uint32_t candidateThisPass = 0;

        for (int32_t z = top; z <= bottom; ++z) {
            for (int32_t x = left; x <= right; ++x) {
                ++gCellsExamined;

                const uint32_t ux = static_cast<uint32_t>(x);
                const uint32_t uz = static_cast<uint32_t>(z);
                const uint16_t flags = GetCellFlags(terrain, ux, uz);

                // Do not interfere with cells the engine already classified as cliff-managed.
                if ((flags & kCliffFlipMarkerFlag) != 0) {
                    continue;
                }

                const float h00 = GetCellHeight(terrain, ux, uz);
                const float h10 = GetCellHeight(terrain, ux + 1, uz);
                const float h01 = GetCellHeight(terrain, ux, uz + 1);
                const float h11 = GetCellHeight(terrain, ux + 1, uz + 1);

                const float minHeight = std::min({h00, h10, h01, h11});
                const float maxHeight = std::max({h00, h10, h01, h11});
                if (maxHeight - minHeight < gSettings.minHeightDelta) {
                    if (gSettings.clearNonCandidates && (flags & kInternalFlipFlag) != 0) {
                        flipInternal(terrain, ux, uz, false);
                        ++changedThisPass;
                    }
                    continue;
                }

                ++candidateThisPass;
                ++gCandidateCells;

                const float normalDiagonalDelta = std::fabs(h00 - h11);
                const float flippedDiagonalDelta = std::fabs(h10 - h01);
                const bool currentlyFlipped = (flags & kInternalFlipFlag) != 0;
                bool flip = currentlyFlipped;

                if (flippedDiagonalDelta + gSettings.diagonalHysteresis < normalDiagonalDelta) {
                    flip = true;
                } else if (normalDiagonalDelta + gSettings.diagonalHysteresis < flippedDiagonalDelta) {
                    flip = false;
                }

                if (flip != currentlyFlipped) {
                    flipInternal(terrain, ux, uz, flip);
                    ++changedThisPass;
                }
            }
        }

        if (changedThisPass != 0) {
            gChangedCells += changedThisPass;
            if (gSettings.logEveryNChanges == 0 ||
                (gChangedCells % gSettings.logEveryNChanges) < changedThisPass) {
                LOG_INFO("TerrainDiagonalFix: changed {} cells this pass ({} candidates); totals changed={}, "
                         "candidates={}, examined={}.",
                         changedThisPass,
                         candidateThisPass,
                         gChangedCells,
                         gCandidateCells,
                         gCellsExamined);
            }
        }
    }

    void __fastcall CalculateNormalsAndAssignCliffTexturesFixed(void* terrain, void* /*edx*/, SC4RectLong* rect) {
        const auto original = reinterpret_cast<CalculateNormalsFn>(kOriginalCalculateNormalsAndAssignCliffTextures);
        original(terrain, rect);
        ApplyNearCliffDiagonalFix(terrain, rect);
    }
}

class TerrainDiagonalFixDirector final : public cRZMessage2COMDirector {
public:
    TerrainDiagonalFixDirector() = default;

    [[nodiscard]] uint32_t GetDirectorID() const override {
        return kDirectorID;
    }

    bool OnStart(cIGZCOM* pCOM) override {
        cRZMessage2COMDirector::OnStart(pCOM);
        Logger::Initialize("SC4TerrainDiagonalFix", "");
        LoadSettings();
        mpFrameWork->AddHook(this);
        return true;
    }

    bool PostAppInit() override {
        const uint16_t version = VersionDetection::GetInstance().GetGameVersion();
        if (version != kSupportedGameVersion) {
            LOG_WARN("TerrainDiagonalFix: game version {} unsupported (addresses target {}); not patching.",
                     version,
                     kSupportedGameVersion);
            return true;
        }

        patch_.Configure(
            "TerrainDiagonalFix.CalculateNormalsAndAssignCliffTextures",
            kCalculateNormalsCallSite,
            reinterpret_cast<void*>(&CalculateNormalsAndAssignCliffTexturesFixed));
        if (!patch_.Install()) {
            LOG_ERROR("TerrainDiagonalFix: failed to install call patch at 0x{:08X}.",
                      static_cast<uint32_t>(kCalculateNormalsCallSite));
            return true;
        }

        if (patch_.GetOriginalTarget() != kOriginalCalculateNormalsAndAssignCliffTextures) {
            LOG_ERROR("TerrainDiagonalFix: call site 0x{:08X} targets 0x{:08X}, expected 0x{:08X}; reverting.",
                      static_cast<uint32_t>(kCalculateNormalsCallSite),
                      static_cast<uint32_t>(patch_.GetOriginalTarget()),
                      static_cast<uint32_t>(kOriginalCalculateNormalsAndAssignCliffTextures));
            patch_.Uninstall();
            return true;
        }

        LOG_INFO("TerrainDiagonalFix: installed (call site 0x{:08X} -> wrapper, original 0x{:08X}).",
                 static_cast<uint32_t>(kCalculateNormalsCallSite),
                 static_cast<uint32_t>(kOriginalCalculateNormalsAndAssignCliffTextures));
        return true;
    }

    bool PostAppShutdown() override {
        patch_.Uninstall();
        if (mpFrameWork) {
            mpFrameWork->RemoveHook(this);
        }
        return true;
    }

    bool DoMessage(cIGZMessage2*) override {
        return true;
    }

private:
    TerrainDecal::RelativeCallPatch patch_{};
};

static TerrainDiagonalFixDirector sDirector;

cRZCOMDllDirector* RZGetCOMDllDirector() {
    static bool sAddedRef = false;
    if (!sAddedRef) {
        sDirector.AddRef();
        sAddedRef = true;
    }
    return &sDirector;
}
