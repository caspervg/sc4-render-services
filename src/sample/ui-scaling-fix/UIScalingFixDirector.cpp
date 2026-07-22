#include <Windows.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "GZServPtrs.h"
#include "cGZPersistResourceKey.h"
#include "cIGZBuffer.h"
#include "cIGZCOM.h"
#include "cIGZFrameWork.h"
#include "cIGZGraphicSystem.h"
#include "cIGZMessage2.h"
#include "cIGZPersistDBRecord.h"
#include "cIGZPersistResourceManager.h"
#include "cIGZWin.h"
#include "cRZAutoRefCount.h"
#include "cRZMessage2COMDirector.h"
#include "cRZRect.h"

#include "utils/Logger.h"
#include "utils/VersionDetection.h"
#include "service/decal/RelativeCallPatch.h"

namespace {
    constexpr uint32_t kDirectorID = 0x4B345546;
    constexpr uint16_t kSupportedGameVersion = 641;

    // Private DBPF record written by sc4-4k-ui-patch/sims2patcher/sc4patches.py.
    constexpr uint32_t kScaleRecordType = 0x4B345549;
    constexpr uint32_t kScaleRecordGroup = 0x96A006B0;
    constexpr uint32_t kScaleRecordInstance = 0x4B345549;
    constexpr char kScaleRecordMagic[4] = {'S', 'C', '4', 'U'};
    constexpr uint32_t kScaleRecordVersion = 1;

    // SimCity 4.exe 1.1.641 addresses. Each patch checks the original target
    // before changing the vtable, so a mismatched executable is left alone.
    constexpr uintptr_t kCatalogAddItemVTableEntry = 0x00AB6D38;
    constexpr uintptr_t kCatalogAddItem = 0x0079B7B0;
    constexpr uintptr_t kCatalogSetItemSizeVTableEntry = 0x00AB6D58;
    constexpr uintptr_t kCatalogSetItemSize = 0x0079A0E0;
    constexpr uintptr_t kCatalogFrameInitVTableEntry = 0x00AB6D14;
    constexpr uintptr_t kCatalogFrameInit = 0x0079AC60;
    constexpr uintptr_t kCatalogFramePositionVTableEntry = 0x00AB6D18;
    constexpr uintptr_t kCatalogFramePosition = 0x0079AD00;
    constexpr uintptr_t kMiniMapSetAreaVTableEntry = 0x00AB8494;
    constexpr uintptr_t kMiniMapSetArea = 0x007A8E30;
    // cISC4WinMiniMap::SetOverlayBuffer, on the interface subobject at +0xD8.
    constexpr uintptr_t kMiniMapSetOverlayVTableEntry = 0x00AB839C;
    constexpr uintptr_t kMiniMapSetOverlay = 0x007A7080;
    constexpr uint32_t kMiniMapInterfaceOffset = 0xD8;
    constexpr uintptr_t kMiniMapInitTerrainRaster = 0x007A7840;
    constexpr uintptr_t kMiniMapUpdateTerrainCallSite = 0x007A8721;
    constexpr uintptr_t kMiniMapUpdateTerrain = 0x007A7FF0;
    // cSC4WinMapView's byte-grid renderer calls a power-of-two-only rasterizer.
    // Its replication loop overruns non-power-of-two targets such as 384x384.
    constexpr uintptr_t kDataViewRasterizeCallSite = 0x007A2628;
    constexpr uintptr_t kDataViewRasterize = 0x0079ED90;
    // cHTMLDocument's constructor copies the seven legacy HTML font sizes
    // through SetFontSizeTable. News/advisor text uses hardcoded HTML SIZE=3
    // tags, so scaling Font.ini alone does not affect it.
    constexpr uintptr_t kHTMLSetFontSizeTableCallSite = 0x00905C8B;
    constexpr uintptr_t kHTMLSetFontSizeTable = 0x008FEEB8;
    constexpr int32_t kHTMLFontSizeCount = 7;
    constexpr uint32_t kMiniMapInternalSizeOffset = 0xE4;
    constexpr uint32_t kMiniMapCityWidthOffset = 0xE8;
    constexpr uint32_t kMiniMapCityHeightOffset = 0xEC;
    constexpr uint32_t kMiniMapTerrainDirtyOffset = 0xFD;
    constexpr uint32_t kMiniMapCompositeDirtyOffset = 0xFE;
    constexpr uint32_t kMiniMapTerrainExponentOffset = 0x104;
    constexpr uint32_t kMiniMapTerrainRasterOffset = 0x114;
    constexpr uint32_t kMiniMapTerrainDirtyBitsOffset = 0x120;
    constexpr size_t kMiniMapTerrainDirtyBitsSize = 0x40;
    constexpr int32_t kMiniMapMinimumNativeTerrainExponent = -2;
    constexpr int32_t kMiniMapMaximumNativeTerrainExponent = 2;

    // cSC4WinMapView::DisplayLegend rewrites the UI-script positions with
    // stock-resolution immediates every time the selected data view changes.
    constexpr uintptr_t kLegendSwatchY1 = 0x007A07D6;
    constexpr uintptr_t kLegendSwatchX1 = 0x007A07E0;
    constexpr uintptr_t kLegendSwatchX2 = 0x007A07F4;
    constexpr uintptr_t kLegendSwatchY2 = 0x007A080E;
    constexpr uintptr_t kLegendTextY1 = 0x007A08F6;
    constexpr uintptr_t kLegendTextX1 = 0x007A0901;
    constexpr uintptr_t kLegendTextX2 = 0x007A090C;
    constexpr uintptr_t kLegendTextY2 = 0x007A0925;

    constexpr uint32_t kDataViewsMiniMapID = 0x4203;
    constexpr int32_t kVanillaDataViewSize = 256;
    constexpr int32_t kVanillaCatalogIconSize = 44;
    constexpr int32_t kVanillaCatalogIconStripWidth = kVanillaCatalogIconSize * 4;
    constexpr uint32_t kVanillaCatalogGap = 5;
    constexpr uint32_t kCatalogIconBufferOffset = 0x30;

#pragma pack(push, 1)
    struct ScaleRecord {
        char magic[4];
        uint32_t version;
        float scale;
    };
#pragma pack(pop)
    static_assert(sizeof(ScaleRecord) == 12);

    float gScale = 1.0F;
    uint32_t gCatalogItemsSeen = 0;
    uint32_t gCatalogIconsScaled = 0;
    uint32_t gDataViewsAreaCalls = 0;
    uint32_t gHTMLDocumentsScaled = 0;
    int32_t gLastLoggedExtendedTerrainExponent = INT32_MAX;
    bool gLoggedArbitraryDataViewRaster = false;

    class VTableEntryPatch final {
    public:
        void Configure(const char* name, const uintptr_t entryAddress,
                       const uintptr_t expectedTarget, void* hook) {
            name_ = name;
            entryAddress_ = entryAddress;
            expectedTarget_ = expectedTarget;
            hook_ = reinterpret_cast<uintptr_t>(hook);
        }

        bool Install() {
            if (installed_ || entryAddress_ == 0 || hook_ == 0) {
                return false;
            }

            auto* const entry = reinterpret_cast<uintptr_t*>(entryAddress_);
            originalTarget_ = *entry;
            if (originalTarget_ != expectedTarget_) {
                LOG_ERROR("UIScalingFix: {} entry 0x{:08X} targets 0x{:08X}; expected 0x{:08X}.",
                          name_, static_cast<uint32_t>(entryAddress_),
                          static_cast<uint32_t>(originalTarget_),
                          static_cast<uint32_t>(expectedTarget_));
                return false;
            }

            DWORD oldProtection = 0;
            if (!VirtualProtect(entry, sizeof(uintptr_t), PAGE_EXECUTE_READWRITE, &oldProtection)) {
                LOG_ERROR("UIScalingFix: VirtualProtect failed for {} (error {}).", name_, GetLastError());
                return false;
            }
            *entry = hook_;
            DWORD ignored = 0;
            VirtualProtect(entry, sizeof(uintptr_t), oldProtection, &ignored);
            FlushInstructionCache(GetCurrentProcess(), entry, sizeof(uintptr_t));
            installed_ = true;
            return true;
        }

        void Uninstall() {
            if (!installed_) {
                return;
            }
            auto* const entry = reinterpret_cast<uintptr_t*>(entryAddress_);
            DWORD oldProtection = 0;
            if (VirtualProtect(entry, sizeof(uintptr_t), PAGE_EXECUTE_READWRITE, &oldProtection)) {
                *entry = originalTarget_;
                DWORD ignored = 0;
                VirtualProtect(entry, sizeof(uintptr_t), oldProtection, &ignored);
                FlushInstructionCache(GetCurrentProcess(), entry, sizeof(uintptr_t));
            }
            installed_ = false;
        }

    private:
        const char* name_ = "";
        uintptr_t entryAddress_ = 0;
        uintptr_t expectedTarget_ = 0;
        uintptr_t originalTarget_ = 0;
        uintptr_t hook_ = 0;
        bool installed_ = false;
    };

    template <typename T>
    class ImmediateValuePatch final {
    public:
        void Configure(const char* name, const uintptr_t address,
                       const T expectedValue, const T replacementValue) {
            name_ = name;
            address_ = address;
            expectedValue_ = expectedValue;
            replacementValue_ = replacementValue;
        }

        bool Install() {
            if (installed_ || address_ == 0) {
                return false;
            }
            auto* const value = reinterpret_cast<T*>(address_);
            if (*value != expectedValue_) {
                LOG_ERROR("UIScalingFix: {} at 0x{:08X} is 0x{:X}; expected 0x{:X}.",
                          name_, static_cast<uint32_t>(address_),
                          static_cast<uint32_t>(*value), static_cast<uint32_t>(expectedValue_));
                return false;
            }
            DWORD oldProtection = 0;
            if (!VirtualProtect(value, sizeof(T), PAGE_EXECUTE_READWRITE, &oldProtection)) {
                LOG_ERROR("UIScalingFix: VirtualProtect failed for {} (error {}).", name_, GetLastError());
                return false;
            }
            *value = replacementValue_;
            DWORD ignored = 0;
            VirtualProtect(value, sizeof(T), oldProtection, &ignored);
            FlushInstructionCache(GetCurrentProcess(), value, sizeof(T));
            installed_ = true;
            return true;
        }

        void Uninstall() {
            if (!installed_) {
                return;
            }
            auto* const value = reinterpret_cast<T*>(address_);
            DWORD oldProtection = 0;
            if (VirtualProtect(value, sizeof(T), PAGE_EXECUTE_READWRITE, &oldProtection)) {
                *value = expectedValue_;
                DWORD ignored = 0;
                VirtualProtect(value, sizeof(T), oldProtection, &ignored);
                FlushInstructionCache(GetCurrentProcess(), value, sizeof(T));
            }
            installed_ = false;
        }

    private:
        const char* name_ = "";
        uintptr_t address_ = 0;
        T expectedValue_{};
        T replacementValue_{};
        bool installed_ = false;
    };

    int32_t ScalePixels(const int32_t value) {
        const long scaled = std::lround(static_cast<double>(value) * gScale);
        return static_cast<int32_t>(scaled > 1 ? scaled : 1);
    }

    using HTMLSetFontSizeTableFn = void(__thiscall*)(void*, const uint32_t*, int32_t);
    void __fastcall HTMLSetFontSizeTableHook(
        void* self, void*, const uint32_t* fontSizes, const int32_t count) {
        if (fontSizes == nullptr || count != kHTMLFontSizeCount) {
            reinterpret_cast<HTMLSetFontSizeTableFn>(kHTMLSetFontSizeTable)(
                self, fontSizes, count);
            return;
        }

        uint32_t scaledFontSizes[kHTMLFontSizeCount]{};
        for (int32_t i = 0; i < kHTMLFontSizeCount; ++i) {
            scaledFontSizes[i] = static_cast<uint32_t>(ScalePixels(
                static_cast<int32_t>(fontSizes[i])));
        }

        // The original function also caps count at seven and copies the table
        // immediately, so this stack buffer does not escape the call.
        reinterpret_cast<HTMLSetFontSizeTableFn>(kHTMLSetFontSizeTable)(
            self, scaledFontSizes, kHTMLFontSizeCount);

        ++gHTMLDocumentsScaled;
        if (gHTMLDocumentsScaled == 1) {
            LOG_INFO("UIScalingFix: scaled HTML font table to [{}, {}, {}, {}, {}, {}, {}].",
                     scaledFontSizes[0], scaledFontSizes[1], scaledFontSizes[2],
                     scaledFontSizes[3], scaledFontSizes[4], scaledFontSizes[5],
                     scaledFontSizes[6]);
        }
    }

    float ReadScaleRecord() {
        const cIGZPersistResourceManagerPtr resourceManager;
        if (!resourceManager) {
            LOG_WARN("UIScalingFix: resource manager unavailable; using scale 1.0.");
            return 1.0F;
        }

        const cGZPersistResourceKey key(kScaleRecordType, kScaleRecordGroup, kScaleRecordInstance);
        cIGZPersistDBRecord* record = nullptr;
        if (!resourceManager->OpenDBRecord(key, &record, false) || record == nullptr) {
            LOG_INFO("UIScalingFix: scale record not found; using scale 1.0.");
            return 1.0F;
        }

        ScaleRecord value{};
        const bool validSize = record->GetSize() == sizeof(value);
        record->SeekAbsolute(0);
        const bool read = validSize && record->GetFieldVoid(&value, sizeof(value));
        resourceManager->CloseDBRecord(key, &record);

        if (!read || std::memcmp(value.magic, kScaleRecordMagic, sizeof(value.magic)) != 0 ||
            value.version != kScaleRecordVersion || !std::isfinite(value.scale) ||
            value.scale <= 0.0F || value.scale > 8.0F) {
            LOG_WARN("UIScalingFix: scale record is malformed or unsupported; using scale 1.0.");
            return 1.0F;
        }
        return value.scale;
    }

    void ScaleCatalogIcon(void* item) {
        ++gCatalogItemsSeen;
        if (item == nullptr || std::abs(gScale - 1.0F) < 0.0001F) {
            return;
        }

        auto** const iconSlot = reinterpret_cast<cIGZBuffer**>(
            reinterpret_cast<uint8_t*>(item) + kCatalogIconBufferOffset);
        cIGZBuffer* const source = *iconSlot;
        if (source == nullptr || source->Width() != kVanillaCatalogIconStripWidth ||
            source->Height() != kVanillaCatalogIconSize) {
            if (gCatalogItemsSeen <= 8) {
                LOG_INFO("UIScalingFix: catalog item {} icon is {}x{}; expected 176x44, leaving it unchanged.",
                         gCatalogItemsSeen, source ? source->Width() : 0, source ? source->Height() : 0);
            }
            return;
        }

        const int32_t stateSize = ScalePixels(kVanillaCatalogIconSize);
        const cIGZGraphicSystemPtr graphics;
        if (!graphics) {
            LOG_WARN("UIScalingFix: graphics service unavailable while scaling a catalog icon.");
            return;
        }

        cRZAutoRefCount<cIGZBuffer> scaled;
        if (!graphics->CreateBuffer(scaled.AsPPObj()) || !scaled) {
            LOG_WARN("UIScalingFix: could not allocate a scaled catalog icon buffer.");
            return;
        }

        const cGZBufferColorType colorType = source->GetColorType();
        if (!scaled->Init(stateSize * 4, stateSize, colorType.bufferType, colorType.bitsPerPixel)) {
            LOG_WARN("UIScalingFix: could not initialize a {}x{} catalog icon buffer.",
                     stateSize * 4, stateSize);
            return;
        }

        uint32_t transparentColor = 0;
        const bool hasTransparency = source->GetTransparentColor(transparentColor);
        if (hasTransparency) {
            scaled->SetTransparency(transparentColor);
        }

        // cIGZBuffer::Blt does not stretch software buffers: a larger target
        // rectangle only leaves the original 44x44 pixels in a larger state.
        // Resample explicitly and keep each state isolated so hover/pressed/
        // disabled pixels cannot bleed across strip boundaries. Nearest-neighbor
        // also preserves SC4's exact transparent color key.
        for (int32_t state = 0; state < 4; ++state) {
            const int32_t sourceStateX = state * kVanillaCatalogIconSize;
            const int32_t targetStateX = state * stateSize;
            for (int32_t y = 0; y < stateSize; ++y) {
                const uint32_t sourceY = static_cast<uint32_t>(
                    (static_cast<int64_t>(y) * kVanillaCatalogIconSize) / stateSize);
                for (int32_t x = 0; x < stateSize; ++x) {
                    const uint32_t sourceX = static_cast<uint32_t>(
                        sourceStateX + (static_cast<int64_t>(x) * kVanillaCatalogIconSize) / stateSize);
                    scaled->SetPixel(static_cast<uint32_t>(targetStateX + x), static_cast<uint32_t>(y),
                                     source->GetPixel(sourceX, sourceY));
                }
            }
        }

        // cSC4CatalogItem owns this field as a cRZAutoRefCount<cIGZBuffer>.
        // Add the item's reference before releasing its old buffer; the local
        // auto-ref then drops the temporary creation reference on return.
        scaled->AddRef();
        *iconSlot = scaled;
        source->Release();
        ++gCatalogIconsScaled;
        if (gCatalogIconsScaled == 1 || (gCatalogIconsScaled % 100) == 0) {
            LOG_INFO("UIScalingFix: scaled {} catalog icon strip(s) to {}x{}.",
                     gCatalogIconsScaled, stateSize * 4, stateSize);
        }
    }

    using CatalogAddItemFn = void(__thiscall*)(void*, void*);
    void __fastcall CatalogAddItemHook(void* self, void*, void* item) {
        ScaleCatalogIcon(item);
        reinterpret_cast<CatalogAddItemFn>(kCatalogAddItem)(self, item);
    }

    using CatalogSetItemSizeFn = void(__thiscall*)(void*, uint32_t, uint32_t, uint32_t);
    void __fastcall CatalogSetItemSizeHook(
        void* self, void*, uint32_t width, uint32_t height, uint32_t gap) {
        // Only the tertiary catalog uses the stock 44x44/five-pixel setup.
        // Other catalog controls retain their own dimensions.
        if (width == kVanillaCatalogIconSize && height == kVanillaCatalogIconSize &&
            gap == kVanillaCatalogGap) {
            width = static_cast<uint32_t>(ScalePixels(static_cast<int32_t>(width)));
            height = static_cast<uint32_t>(ScalePixels(static_cast<int32_t>(height)));
            gap = static_cast<uint32_t>(ScalePixels(static_cast<int32_t>(gap)));
        }
        reinterpret_cast<CatalogSetItemSizeFn>(kCatalogSetItemSize)(self, width, height, gap);
    }

    using CatalogFrameInitFn = void(__thiscall*)(
        void*, void*, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t);
    void __fastcall CatalogFrameInitHook(
        void* self, void*, void* image, int32_t leftWidth, int32_t verticalInset,
        int32_t rightEdge, int32_t minimumHeight, int32_t rightInset,
        int32_t bottomInset, int32_t topInset) {
        // InvokeTertiaryMenu supplies these seven unscaled skin metrics even
        // after the scripted UI and the catalog cells have been enlarged.
        // Scale only that exact stock tuple so other users of this control are
        // unaffected. The increased left width moves the icon column right and
        // preserves the intended gap to the surrounding HUD chrome.
        if (leftWidth == 0x35 && verticalInset == 0x19 && rightEdge == 0x50 &&
            minimumHeight == 0x35 && rightInset == 4 && bottomInset == 0x1B &&
            topInset == 0x1D) {
            // The icon itself grows from 44 px to ScalePixels(44). Add that
            // growth to the stock inset instead of multiplying the complete
            // 53 px frame cap; multiplying it leaves an unnecessarily wide
            // text gutter at 2x.
            leftWidth += ScalePixels(kVanillaCatalogIconSize) - kVanillaCatalogIconSize;
            verticalInset = ScalePixels(verticalInset);
            rightEdge = ScalePixels(rightEdge);
            minimumHeight = ScalePixels(minimumHeight);
            rightInset = ScalePixels(rightInset);
            bottomInset = ScalePixels(bottomInset);
            topInset = ScalePixels(topInset);
            LOG_INFO("UIScalingFix: scaled tertiary catalog frame metrics; icon column inset is {} px.",
                     leftWidth);
        }
        reinterpret_cast<CatalogFrameInitFn>(kCatalogFrameInit)(
            self, image, leftWidth, verticalInset, rightEdge, minimumHeight,
            rightInset, bottomInset, topInset);
    }

    using CatalogFramePositionFn = void(__thiscall*)(
        void*, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t);
    void __fastcall CatalogFramePositionHook(
        void* self, void*, int32_t anchor, int32_t x, int32_t y, int32_t edge,
        int32_t minimum, int32_t maximum) {
        // The stock call clamps the popup to [10, screenEdge-10]. Match those
        // edge margins to the UI scale after enlarging the frame itself.
        if (minimum == 10) {
            const int32_t scaledMargin = ScalePixels(10);
            maximum -= scaledMargin - minimum;
            minimum = scaledMargin;
        }
        reinterpret_cast<CatalogFramePositionFn>(kCatalogFramePosition)(
            self, anchor, x, y, edge, minimum, maximum);
    }

    using MiniMapSetAreaFn = void(__thiscall*)(void*, int32_t, int32_t, int32_t, int32_t);
    void __fastcall MiniMapSetAreaHook(
        void* self, void*, const int32_t left, const int32_t top, int32_t right, int32_t bottom) {
        // Scripted geometry can be overwritten by cSC4WinMiniMap setup. Only
        // normalize the Data Views plot; the HUD and region minimaps are not
        // touched. The original method then rebuilds its internal plot buffer
        // from this final square.
        if (reinterpret_cast<cIGZWin*>(self)->GetID() == kDataViewsMiniMapID) {
            const int32_t desiredSize = ScalePixels(kVanillaDataViewSize);
            if (gDataViewsAreaCalls++ == 0) {
                LOG_INFO("UIScalingFix: Data Views plot SetArea received {}x{}; normalizing to {}x{}.",
                         right - left, bottom - top, desiredSize, desiredSize);
            }
            right = left + desiredSize;
            bottom = top + desiredSize;
        }
        reinterpret_cast<MiniMapSetAreaFn>(kMiniMapSetArea)(self, left, top, right, bottom);
    }

    using MiniMapSetOverlayFn = void(__thiscall*)(void*, cIGZBuffer*);
    using MiniMapInitTerrainRasterFn = void(__thiscall*)(void*);
    using MiniMapUpdateTerrainFn = void(__thiscall*)(void*);

    // MSVC's 32-bit std::vector layout. The source is a vector of byte-vector
    // rows passed by cSC4WinMapView to the stock rasterizer at 0x0079ED90.
    struct VectorLayout32 {
        uintptr_t begin;
        uintptr_t end;
        uintptr_t capacity;
    };
    static_assert(sizeof(VectorLayout32) == 12);

    void __cdecl DataViewRasterizeHook(
        const VectorLayout32* rows, const int32_t sourceHeight, const int32_t sourceWidth,
        cIGZBuffer* target, const int32_t targetWidth, const int32_t targetHeight,
        const uint32_t* palette, const int32_t orientation) {
        if (rows == nullptr || target == nullptr || palette == nullptr ||
            sourceWidth <= 0 || sourceHeight <= 0 || targetWidth <= 0 || targetHeight <= 0 ||
            rows->end < rows->begin ||
            static_cast<size_t>(rows->end - rows->begin) /
                    sizeof(VectorLayout32) < static_cast<size_t>(sourceHeight) ||
            orientation != 0 || target->GetBitsPerPixel() != 32) {
            LOG_ERROR("UIScalingFix: rejected invalid Data Views raster {}x{} -> {}x{} "
                      "(orientation {}, {} bpp).",
                      sourceWidth, sourceHeight, targetWidth, targetHeight, orientation,
                      target != nullptr ? target->GetBitsPerPixel() : 0);
            return;
        }

        const int32_t outputWidth = (std::min)(targetWidth, target->Width());
        const int32_t outputHeight = (std::min)(targetHeight, target->Height());
        if (outputWidth <= 0 || outputHeight <= 0 || !target->Lock(0x8040)) {
            LOG_ERROR("UIScalingFix: failed to lock the Data Views {}x{} overlay buffer.",
                      targetWidth, targetHeight);
            return;
        }

        auto* const destination = reinterpret_cast<uint8_t*>(
            static_cast<uintptr_t>(target->GetColorSurfaceBits()));
        const uint32_t destinationStride = target->GetColorSurfaceStride();
        const auto* const sourceRows = reinterpret_cast<const VectorLayout32*>(rows->begin);
        if (destination == nullptr || destinationStride < static_cast<uint32_t>(outputWidth) * 4U) {
            target->Unlock(0x8040);
            LOG_ERROR("UIScalingFix: Data Views overlay has invalid bits or stride {}.",
                      destinationStride);
            return;
        }

        bool valid = true;
        for (int32_t y = 0; y < outputHeight && valid; ++y) {
            const int32_t sourceY = static_cast<int32_t>(
                static_cast<uint64_t>(y) * static_cast<uint32_t>(sourceHeight) /
                static_cast<uint32_t>(outputHeight));
            const VectorLayout32& sourceRow = sourceRows[sourceY];
            if (sourceRow.end < sourceRow.begin ||
                static_cast<size_t>(sourceRow.end - sourceRow.begin) <
                    static_cast<size_t>(sourceWidth)) {
                valid = false;
                break;
            }
            const auto* const source = reinterpret_cast<const uint8_t*>(sourceRow.begin);
            auto* const output = reinterpret_cast<uint32_t*>(
                destination + static_cast<size_t>(y) * destinationStride);
            for (int32_t x = 0; x < outputWidth; ++x) {
                const int32_t sourceX = static_cast<int32_t>(
                    static_cast<uint64_t>(x) * static_cast<uint32_t>(sourceWidth) /
                    static_cast<uint32_t>(outputWidth));
                output[x] = palette[source[sourceX]];
            }
        }
        target->Unlock(0x8040);

        if (!valid) {
            LOG_ERROR("UIScalingFix: Data Views source grid changed while rasterizing.");
        } else if (!gLoggedArbitraryDataViewRaster &&
                   (targetWidth != sourceWidth && targetWidth % sourceWidth != 0)) {
            gLoggedArbitraryDataViewRaster = true;
            LOG_INFO("UIScalingFix: rendered Data Views city overlay {}x{} -> {}x{} with "
                     "arbitrary-ratio nearest-neighbor scaling.",
                     sourceWidth, sourceHeight, outputWidth, outputHeight);
        }
    }

    [[nodiscard]] bool GetNativeTerrainRenderSize(
        const int32_t cityWidth, const int32_t cityHeight, const int32_t exponent,
        int32_t& renderWidth, int32_t& renderHeight) {
        if (cityWidth <= 0 || cityHeight <= 0 ||
            exponent < kMiniMapMinimumNativeTerrainExponent ||
            exponent > kMiniMapMaximumNativeTerrainExponent) {
            return false;
        }

        if (exponent < 0) {
            const int32_t multiplier = 1 << -exponent;
            if (cityWidth > INT32_MAX / multiplier || cityHeight > INT32_MAX / multiplier) {
                return false;
            }
            renderWidth = cityWidth * multiplier;
            renderHeight = cityHeight * multiplier;
        } else {
            const int32_t divisor = 1 << exponent;
            renderWidth = cityWidth / divisor;
            renderHeight = cityHeight / divisor;
        }
        return renderWidth > 0 && renderHeight > 0 && renderWidth == renderHeight;
    }

    void ResampleTerrainNearest(
        const uint32_t* source, const int32_t sourceWidth, const int32_t sourceHeight,
        uint32_t* destination, const int32_t destinationWidth, const int32_t destinationHeight) {
        for (int32_t y = 0; y < destinationHeight; ++y) {
            const auto sourceY = static_cast<int32_t>(
                static_cast<uint64_t>(y) * static_cast<uint32_t>(sourceHeight) /
                static_cast<uint32_t>(destinationHeight));
            const uint32_t* const sourceRow = source + sourceY * sourceWidth;
            uint32_t* const destinationRow = destination + y * destinationWidth;
            for (int32_t x = 0; x < destinationWidth; ++x) {
                const auto sourceX = static_cast<int32_t>(
                    static_cast<uint64_t>(x) * static_cast<uint32_t>(sourceWidth) /
                    static_cast<uint32_t>(destinationWidth));
                destinationRow[x] = sourceRow[sourceX];
            }
        }
    }

    void __fastcall MiniMapUpdateTerrainHook(void* self, void*) {
        auto* const fullObject = static_cast<uint8_t*>(self);
        if (reinterpret_cast<cIGZWin*>(fullObject)->GetID() != kDataViewsMiniMapID) {
            reinterpret_cast<MiniMapUpdateTerrainFn>(kMiniMapUpdateTerrain)(self);
            return;
        }

        const int32_t exponent = *reinterpret_cast<int32_t*>(
            fullObject + kMiniMapTerrainExponentOffset);
        if (exponent >= kMiniMapMinimumNativeTerrainExponent &&
            exponent <= kMiniMapMaximumNativeTerrainExponent) {
            reinterpret_cast<MiniMapUpdateTerrainFn>(kMiniMapUpdateTerrain)(self);
            return;
        }

        const int32_t targetSize = *reinterpret_cast<int32_t*>(
            fullObject + kMiniMapInternalSizeOffset);
        const int32_t cityWidth = *reinterpret_cast<int32_t*>(
            fullObject + kMiniMapCityWidthOffset);
        const int32_t cityHeight = *reinterpret_cast<int32_t*>(
            fullObject + kMiniMapCityHeightOffset);
        auto* const raster = fullObject + kMiniMapTerrainRasterOffset;
        auto* const targetPixels = *reinterpret_cast<uint32_t**>(raster);
        const int32_t rasterWidth = *reinterpret_cast<int32_t*>(raster + 4);
        const int32_t rasterHeight = *reinterpret_cast<int32_t*>(raster + 8);
        if (targetSize <= 0 || targetPixels == nullptr || rasterWidth != targetSize ||
            rasterHeight != targetSize) {
            LOG_ERROR("UIScalingFix: cannot extend Data Views terrain exponent {} with "
                      "invalid target raster {}x{} (internal size {}).",
                      exponent, rasterWidth, rasterHeight, targetSize);
            return;
        }

        const int32_t nativeExponent = std::clamp(
            exponent, kMiniMapMinimumNativeTerrainExponent,
            kMiniMapMaximumNativeTerrainExponent);
        int32_t renderWidth = 0;
        int32_t renderHeight = 0;
        if (!GetNativeTerrainRenderSize(
                cityWidth, cityHeight, nativeExponent, renderWidth, renderHeight)) {
            LOG_ERROR("UIScalingFix: cannot derive an intermediate Data Views terrain size for "
                      "city {}x{} at exponent {}.",
                      cityWidth, cityHeight, exponent);
            return;
        }

        const size_t renderWidthSize = static_cast<size_t>(renderWidth);
        const size_t renderHeightSize = static_cast<size_t>(renderHeight);
        if (renderWidthSize > (std::numeric_limits<size_t>::max)() / renderHeightSize ||
            renderWidthSize * renderHeightSize >
                (std::numeric_limits<size_t>::max)() / sizeof(uint32_t)) {
            LOG_ERROR("UIScalingFix: intermediate Data Views terrain {}x{} is too large.",
                      renderWidth, renderHeight);
            return;
        }
        const size_t renderPixelCount = renderWidthSize * renderHeightSize;

        try {
            std::vector<uint32_t> intermediate(renderPixelCount, 0);

            *reinterpret_cast<int32_t*>(fullObject + kMiniMapInternalSizeOffset) = renderWidth;
            *reinterpret_cast<int32_t*>(fullObject + kMiniMapTerrainExponentOffset) = nativeExponent;
            *reinterpret_cast<uint32_t**>(raster) = intermediate.data();
            *reinterpret_cast<int32_t*>(raster + 4) = renderWidth;
            *reinterpret_cast<int32_t*>(raster + 8) = renderHeight;
            std::memset(fullObject + kMiniMapTerrainDirtyBitsOffset, 0xFF,
                        kMiniMapTerrainDirtyBitsSize);
            *(fullObject + kMiniMapTerrainDirtyOffset) = 1;

            reinterpret_cast<MiniMapUpdateTerrainFn>(kMiniMapUpdateTerrain)(self);

            *reinterpret_cast<int32_t*>(fullObject + kMiniMapInternalSizeOffset) = targetSize;
            *reinterpret_cast<int32_t*>(fullObject + kMiniMapTerrainExponentOffset) = exponent;
            *reinterpret_cast<uint32_t**>(raster) = targetPixels;
            *reinterpret_cast<int32_t*>(raster + 4) = rasterWidth;
            *reinterpret_cast<int32_t*>(raster + 8) = rasterHeight;

            ResampleTerrainNearest(intermediate.data(), renderWidth, renderHeight,
                                   targetPixels, rasterWidth, rasterHeight);
            *(fullObject + kMiniMapCompositeDirtyOffset) = 1;

            if (gLastLoggedExtendedTerrainExponent != exponent) {
                gLastLoggedExtendedTerrainExponent = exponent;
                LOG_INFO("UIScalingFix: extended Data Views terrain exponent {} via {}x{} -> "
                         "{}x{} nearest-neighbor resampling.",
                         exponent, renderWidth, renderHeight, rasterWidth, rasterHeight);
            }
        } catch (const std::bad_alloc&) {
            *reinterpret_cast<int32_t*>(fullObject + kMiniMapInternalSizeOffset) = targetSize;
            *reinterpret_cast<int32_t*>(fullObject + kMiniMapTerrainExponentOffset) = exponent;
            *reinterpret_cast<uint32_t**>(raster) = targetPixels;
            *reinterpret_cast<int32_t*>(raster + 4) = rasterWidth;
            *reinterpret_cast<int32_t*>(raster + 8) = rasterHeight;
            LOG_ERROR("UIScalingFix: failed to allocate the {}x{} intermediate Data Views "
                      "terrain raster.", renderWidth, renderHeight);
        }
    }

    void __fastcall MiniMapSetOverlayHook(void* interfaceSelf, void*, cIGZBuffer* overlay) {
        auto* const fullObject = reinterpret_cast<uint8_t*>(interfaceSelf) - kMiniMapInterfaceOffset;
        auto* const window = reinterpret_cast<cIGZWin*>(fullObject);
        if (window->GetID() == kDataViewsMiniMapID && overlay != nullptr) {
            // SetArea updates +E4, but if the minimap has already initialized,
            // rebuild the terrain raster and exponent together. Calling SC4's
            // InitTZMap equivalent is important: resizing +114 alone leaves
            // the converter exponent at +104 stale.
            const int32_t internalSize = *reinterpret_cast<int32_t*>(
                fullObject + kMiniMapInternalSizeOffset);
            auto* const terrainRaster = fullObject + kMiniMapTerrainRasterOffset;
            const int32_t rasterWidth = *reinterpret_cast<int32_t*>(terrainRaster + 4);
            const int32_t rasterHeight = *reinterpret_cast<int32_t*>(terrainRaster + 8);
            if (internalSize > 0 &&
                (rasterWidth != internalSize || rasterHeight != internalSize)) {
                reinterpret_cast<MiniMapInitTerrainRasterFn>(
                    kMiniMapInitTerrainRaster)(fullObject);
                const int32_t exponent = *reinterpret_cast<int32_t*>(
                    fullObject + kMiniMapTerrainExponentOffset);
                LOG_INFO("UIScalingFix: rebuilt Data Views terrain raster {}x{} -> {}x{} "
                         "with exponent {}.",
                         rasterWidth, rasterHeight, internalSize, internalSize, exponent);
            }
        }
        reinterpret_cast<MiniMapSetOverlayFn>(kMiniMapSetOverlay)(interfaceSelf, overlay);
    }
}

class UIScalingFixDirector final : public cRZMessage2COMDirector {
public:
    [[nodiscard]] uint32_t GetDirectorID() const override {
        return kDirectorID;
    }

    bool OnStart(cIGZCOM* pCOM) override {
        cRZMessage2COMDirector::OnStart(pCOM);
        Logger::Initialize("SC4UIScalingFix", "");
        mpFrameWork->AddHook(this);
        return true;
    }

    bool PostAppInit() override {
        const uint16_t version = VersionDetection::GetInstance().GetGameVersion();
        if (version != kSupportedGameVersion) {
            LOG_WARN("UIScalingFix: game version {} unsupported; addresses target {}.",
                     version, kSupportedGameVersion);
            return true;
        }

        gScale = ReadScaleRecord();
        if (std::abs(gScale - 1.0F) < 0.0001F) {
            LOG_INFO("UIScalingFix: effective scale is 1.0; no hooks installed.");
            return true;
        }

        catalogAddItemPatch_.Configure("catalog AddItem", kCatalogAddItemVTableEntry,
                                       kCatalogAddItem, reinterpret_cast<void*>(&CatalogAddItemHook));
        catalogSetItemSizePatch_.Configure(
            "catalog SetItemSize", kCatalogSetItemSizeVTableEntry, kCatalogSetItemSize,
            reinterpret_cast<void*>(&CatalogSetItemSizeHook));
        catalogFrameInitPatch_.Configure(
            "catalog frame Init", kCatalogFrameInitVTableEntry, kCatalogFrameInit,
            reinterpret_cast<void*>(&CatalogFrameInitHook));
        catalogFramePositionPatch_.Configure(
            "catalog frame Position", kCatalogFramePositionVTableEntry, kCatalogFramePosition,
            reinterpret_cast<void*>(&CatalogFramePositionHook));
        miniMapSetAreaPatch_.Configure("Data Views SetArea", kMiniMapSetAreaVTableEntry,
                                       kMiniMapSetArea, reinterpret_cast<void*>(&MiniMapSetAreaHook));
        miniMapSetOverlayPatch_.Configure(
            "Data Views SetOverlayBuffer", kMiniMapSetOverlayVTableEntry, kMiniMapSetOverlay,
            reinterpret_cast<void*>(&MiniMapSetOverlayHook));
        miniMapUpdateTerrainPatch_.Configure(
            "cSC4WinMiniMap::Update terrain call", kMiniMapUpdateTerrainCallSite,
            reinterpret_cast<void*>(&MiniMapUpdateTerrainHook));
        dataViewRasterizePatch_.Configure(
            "Data Views arbitrary-ratio rasterizer", kDataViewRasterizeCallSite,
            reinterpret_cast<void*>(&DataViewRasterizeHook));
        htmlSetFontSizeTablePatch_.Configure(
            "cHTMLDocument::SetFontSizeTable constructor call",
            kHTMLSetFontSizeTableCallSite,
            reinterpret_cast<void*>(&HTMLSetFontSizeTableHook));

        // The Y operands are signed disp8 values, so this exact patch supports
        // scales through 2x. Larger factors need a small code cave rather than
        // changing the instruction length in place.
        const int32_t legendSwatchY = ScalePixels(0x3D);
        const int32_t legendTextY = ScalePixels(0x18);
        if (legendSwatchY > INT8_MAX || legendTextY > INT8_MAX) {
            LOG_ERROR("UIScalingFix: scale {:.3f} exceeds the safe Data Views legend operand range.",
                      gScale);
            return true;
        }
        legendSwatchY1Patch_.Configure("legend swatch top", kLegendSwatchY1,
                                       uint8_t{0x3D}, static_cast<uint8_t>(legendSwatchY));
        legendSwatchX1Patch_.Configure("legend swatch left", kLegendSwatchX1,
                                       uint32_t{0x173}, static_cast<uint32_t>(ScalePixels(0x173)));
        legendSwatchX2Patch_.Configure("legend swatch right", kLegendSwatchX2,
                                       uint32_t{0x173}, static_cast<uint32_t>(ScalePixels(0x173)));
        legendSwatchY2Patch_.Configure("legend swatch bottom", kLegendSwatchY2,
                                       uint8_t{0x3D}, static_cast<uint8_t>(legendSwatchY));
        legendTextY1Patch_.Configure("legend text top", kLegendTextY1,
                                     uint8_t{0x18}, static_cast<uint8_t>(legendTextY));
        legendTextX1Patch_.Configure("legend text left", kLegendTextX1,
                                     uint32_t{0x116}, static_cast<uint32_t>(ScalePixels(0x116)));
        legendTextX2Patch_.Configure("legend text right", kLegendTextX2,
                                     uint32_t{0x116}, static_cast<uint32_t>(ScalePixels(0x116)));
        legendTextY2Patch_.Configure("legend text bottom", kLegendTextY2,
                                     uint8_t{0x18}, static_cast<uint8_t>(legendTextY));

        const bool terrainPatchInstalled = miniMapUpdateTerrainPatch_.Install();
        const bool terrainPatchTargetMatches = terrainPatchInstalled &&
            miniMapUpdateTerrainPatch_.GetOriginalTarget() == kMiniMapUpdateTerrain;
        if (terrainPatchInstalled && !terrainPatchTargetMatches) {
            LOG_ERROR("UIScalingFix: terrain call at 0x{:08X} targets 0x{:08X}; expected "
                      "0x{:08X}.",
                      static_cast<uint32_t>(kMiniMapUpdateTerrainCallSite),
                      static_cast<uint32_t>(miniMapUpdateTerrainPatch_.GetOriginalTarget()),
                      static_cast<uint32_t>(kMiniMapUpdateTerrain));
        }

        const bool htmlFontPatchInstalled = htmlSetFontSizeTablePatch_.Install();
        const bool htmlFontPatchTargetMatches = htmlFontPatchInstalled &&
            htmlSetFontSizeTablePatch_.GetOriginalTarget() == kHTMLSetFontSizeTable;
        if (htmlFontPatchInstalled && !htmlFontPatchTargetMatches) {
            LOG_ERROR("UIScalingFix: HTML font-table call at 0x{:08X} targets 0x{:08X}; "
                      "expected 0x{:08X}.",
                      static_cast<uint32_t>(kHTMLSetFontSizeTableCallSite),
                      static_cast<uint32_t>(htmlSetFontSizeTablePatch_.GetOriginalTarget()),
                      static_cast<uint32_t>(kHTMLSetFontSizeTable));
        }

        const bool dataViewRasterPatchInstalled = dataViewRasterizePatch_.Install();
        const bool dataViewRasterPatchTargetMatches = dataViewRasterPatchInstalled &&
            dataViewRasterizePatch_.GetOriginalTarget() == kDataViewRasterize;
        if (dataViewRasterPatchInstalled && !dataViewRasterPatchTargetMatches) {
            LOG_ERROR("UIScalingFix: Data Views rasterizer call at 0x{:08X} targets 0x{:08X}; "
                      "expected 0x{:08X}.",
                      static_cast<uint32_t>(kDataViewRasterizeCallSite),
                      static_cast<uint32_t>(dataViewRasterizePatch_.GetOriginalTarget()),
                      static_cast<uint32_t>(kDataViewRasterize));
        }

        if (!terrainPatchTargetMatches || !htmlFontPatchTargetMatches ||
            !dataViewRasterPatchTargetMatches ||
            !catalogAddItemPatch_.Install() ||
            !catalogSetItemSizePatch_.Install() ||
            !catalogFrameInitPatch_.Install() || !catalogFramePositionPatch_.Install() ||
            !miniMapSetAreaPatch_.Install() || !miniMapSetOverlayPatch_.Install() ||
            !legendSwatchY1Patch_.Install() ||
            !legendSwatchX1Patch_.Install() || !legendSwatchX2Patch_.Install() ||
            !legendSwatchY2Patch_.Install() || !legendTextY1Patch_.Install() ||
            !legendTextX1Patch_.Install() || !legendTextX2Patch_.Install() ||
            !legendTextY2Patch_.Install()) {
            legendTextY2Patch_.Uninstall();
            legendTextX2Patch_.Uninstall();
            legendTextX1Patch_.Uninstall();
            legendTextY1Patch_.Uninstall();
            legendSwatchY2Patch_.Uninstall();
            legendSwatchX2Patch_.Uninstall();
            legendSwatchX1Patch_.Uninstall();
            legendSwatchY1Patch_.Uninstall();
            dataViewRasterizePatch_.Uninstall();
            htmlSetFontSizeTablePatch_.Uninstall();
            miniMapUpdateTerrainPatch_.Uninstall();
            miniMapSetOverlayPatch_.Uninstall();
            miniMapSetAreaPatch_.Uninstall();
            catalogFramePositionPatch_.Uninstall();
            catalogFrameInitPatch_.Uninstall();
            catalogSetItemSizePatch_.Uninstall();
            catalogAddItemPatch_.Uninstall();
            LOG_ERROR("UIScalingFix: hook installation was incomplete; all hooks were reverted.");
            return true;
        }

        LOG_INFO("UIScalingFix: scale {:.3f} active; HTML text, tertiary catalog, Data Views "
                 "geometry, and extended terrain scaling installed.",
                 gScale);
        return true;
    }

    bool PostAppShutdown() override {
        legendTextY2Patch_.Uninstall();
        legendTextX2Patch_.Uninstall();
        legendTextX1Patch_.Uninstall();
        legendTextY1Patch_.Uninstall();
        legendSwatchY2Patch_.Uninstall();
        legendSwatchX2Patch_.Uninstall();
        legendSwatchX1Patch_.Uninstall();
        legendSwatchY1Patch_.Uninstall();
        dataViewRasterizePatch_.Uninstall();
        htmlSetFontSizeTablePatch_.Uninstall();
        miniMapUpdateTerrainPatch_.Uninstall();
        miniMapSetOverlayPatch_.Uninstall();
        miniMapSetAreaPatch_.Uninstall();
        catalogFramePositionPatch_.Uninstall();
        catalogFrameInitPatch_.Uninstall();
        catalogSetItemSizePatch_.Uninstall();
        catalogAddItemPatch_.Uninstall();
        if (mpFrameWork) {
            mpFrameWork->RemoveHook(this);
        }
        return true;
    }

    bool DoMessage(cIGZMessage2*) override {
        return true;
    }

private:
    VTableEntryPatch catalogAddItemPatch_{};
    VTableEntryPatch catalogSetItemSizePatch_{};
    VTableEntryPatch catalogFrameInitPatch_{};
    VTableEntryPatch catalogFramePositionPatch_{};
    VTableEntryPatch miniMapSetAreaPatch_{};
    VTableEntryPatch miniMapSetOverlayPatch_{};
    TerrainDecal::RelativeCallPatch htmlSetFontSizeTablePatch_{};
    TerrainDecal::RelativeCallPatch miniMapUpdateTerrainPatch_{};
    TerrainDecal::RelativeCallPatch dataViewRasterizePatch_{};
    ImmediateValuePatch<uint8_t> legendSwatchY1Patch_{};
    ImmediateValuePatch<uint32_t> legendSwatchX1Patch_{};
    ImmediateValuePatch<uint32_t> legendSwatchX2Patch_{};
    ImmediateValuePatch<uint8_t> legendSwatchY2Patch_{};
    ImmediateValuePatch<uint8_t> legendTextY1Patch_{};
    ImmediateValuePatch<uint32_t> legendTextX1Patch_{};
    ImmediateValuePatch<uint32_t> legendTextX2Patch_{};
    ImmediateValuePatch<uint8_t> legendTextY2Patch_{};
};

static UIScalingFixDirector sDirector;

cRZCOMDllDirector* RZGetCOMDllDirector() {
    static bool sAddedRef = false;
    if (!sAddedRef) {
        sDirector.AddRef();
        sAddedRef = true;
    }
    return &sDirector;
}
