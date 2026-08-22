#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "RelativeCallPatch.h"

#include <Windows.h>

#include <cstring>
#include <limits>

#include "utils/Logger.h"

namespace
{
    constexpr size_t kCallInstructionLength = 5;
}

namespace TerrainDecal
{
    RelativeCallPatch::RelativeCallPatch(const std::string_view name,
                                         const uintptr_t callSiteAddress,
                                         void* hookFn)
    {
        Configure(name, callSiteAddress, hookFn);
    }

    void RelativeCallPatch::Configure(const std::string_view name, const uintptr_t callSiteAddress, void* hookFn)
    {
        if (installed_) {
            configurationRejected_ = true;
            LOG_ERROR("TerrainDecalHook: refusing to reconfigure installed call patch {}", name_);
            return;
        }
        name_ = name;
        callSiteAddress_ = callSiteAddress;
        hookFn_ = hookFn;
        originalRel_ = 0;
        patchedRel_ = 0;
        originalTarget_ = 0;
        installed_ = false;
        configurationRejected_ = false;
    }

    bool RelativeCallPatch::Install()
    {
        if (installed_) {
            return true;
        }
        if (configurationRejected_) {
            LOG_ERROR("TerrainDecalHook: cannot install {} after a rejected reconfiguration", name_);
            return false;
        }

        if (callSiteAddress_ == 0 || hookFn_ == nullptr) {
            LOG_ERROR("TerrainDecalHook: incomplete call patch configuration for {}", name_);
            return false;
        }

        auto* const site = reinterpret_cast<uint8_t*>(callSiteAddress_);
        if (site[0] != 0xE8) {
            LOG_ERROR("TerrainDecalHook: expected CALL rel32 at 0x{:08X} for {}",
                      static_cast<uint32_t>(callSiteAddress_), name_);
            return false;
        }

        std::memcpy(&originalRel_, site + 1, sizeof(originalRel_));
        originalTarget_ = static_cast<uintptr_t>(
            static_cast<intptr_t>(callSiteAddress_ + kCallInstructionLength) + originalRel_);

        int32_t newRel = 0;
        if (!ComputeRelativeCallTarget(callSiteAddress_, reinterpret_cast<uintptr_t>(hookFn_), newRel)) {
            LOG_ERROR("TerrainDecalHook: rel32 range failure for {}", name_);
            return false;
        }

        DWORD oldProtect = 0;
        if (!VirtualProtect(site + 1, sizeof(newRel), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            LOG_ERROR("TerrainDecalHook: VirtualProtect failed for {}", name_);
            return false;
        }

        patchedRel_ = newRel;
        std::memcpy(site + 1, &patchedRel_, sizeof(patchedRel_));
        FlushInstructionCache(GetCurrentProcess(), site, kCallInstructionLength);
        installed_ = true;
        DWORD ignored = 0;
        if (!VirtualProtect(site + 1, sizeof(patchedRel_), oldProtect, &ignored)) {
            LOG_ERROR("TerrainDecalHook: failed to restore page protection after installing {}", name_);
            // Install(false) must never leave a live callback behind: several consumers return
            // immediately on failure. The page is still writable after the successful first
            // VirtualProtect, so restore the original displacement before reporting failure.
            std::memcpy(site + 1, &originalRel_, sizeof(originalRel_));
            FlushInstructionCache(GetCurrentProcess(), site, kCallInstructionLength);
            installed_ = false;
            DWORD rollbackIgnored = 0;
            if (!VirtualProtect(site + 1, sizeof(originalRel_), oldProtect, &rollbackIgnored)) {
                LOG_ERROR("TerrainDecalHook: failed to restore page protection after rolling back {}", name_);
            }
            return false;
        }

        return true;
    }

    void RelativeCallPatch::Uninstall()
    {
        if (!installed_) {
            return;
        }

        auto* const site = reinterpret_cast<uint8_t*>(callSiteAddress_);
        int32_t currentRel = 0;
        std::memcpy(&currentRel, site + 1, sizeof(currentRel));
        if (site[0] != 0xE8 || currentRel != patchedRel_) {
            LOG_ERROR("TerrainDecalHook: refusing to uninstall {} because its CALL site is no longer "
                      "owned by this patch", name_);
            return;
        }

        DWORD oldProtect = 0;
        if (!VirtualProtect(site + 1, sizeof(originalRel_), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            LOG_ERROR("TerrainDecalHook: VirtualProtect failed while uninstalling {}", name_);
            return;
        }

        std::memcpy(site + 1, &originalRel_, sizeof(originalRel_));
        FlushInstructionCache(GetCurrentProcess(), site, kCallInstructionLength);
        installed_ = false;
        DWORD ignored = 0;
        if (!VirtualProtect(site + 1, sizeof(originalRel_), oldProtect, &ignored)) {
            LOG_ERROR("TerrainDecalHook: failed to restore page protection after uninstalling {}", name_);
        }
    }

    bool RelativeCallPatch::IsInstalled() const noexcept
    {
        return installed_;
    }

    uintptr_t RelativeCallPatch::GetOriginalTarget() const noexcept
    {
        return originalTarget_;
    }

    bool RelativeCallPatch::ComputeRelativeCallTarget(const uintptr_t callSiteAddress,
                                                      const uintptr_t targetAddress,
                                                      int32_t& relOut)
    {
        const auto delta = static_cast<int64_t>(targetAddress) -
            static_cast<int64_t>(callSiteAddress + kCallInstructionLength);
        if (delta < std::numeric_limits<int32_t>::min() || delta > std::numeric_limits<int32_t>::max()) {
            return false;
        }

        relOut = static_cast<int32_t>(delta);
        return true;
    }
}
