#include "cIGZFrameWork.h"
#include "cRZCOMDllDirector.h"
#include "utils/Logger.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <bit>
#include <cstdint>
#include <string_view>
#include <vector>

namespace {
    constexpr uint32_t kRDTSCPatchDirectorId = 0xA17E1001;
    constexpr std::string_view kLoggerName = "SC4RDTSCPatch";

    constexpr unsigned char kRdtscOpcode0 = 0x0F;
    constexpr unsigned char kRdtscOpcode1 = 0x31;
    constexpr unsigned char kUd2Opcode0 = 0x0F;
    constexpr unsigned char kUd2Opcode1 = 0x0B;
    constexpr unsigned char kXorEaxOpcode0 = 0x33;
    constexpr unsigned char kXorEaxOpcode1 = 0xC0;
    constexpr unsigned char kJnzShortOpcode = 0x75;
    constexpr unsigned char kJmpShortOpcode = 0xEB;
    constexpr unsigned char kNearJumpOpcode = 0xE9;
    constexpr unsigned char kNopOpcode = 0x90;

    constexpr uintptr_t kExpectedModuleBase = 0x00400000;
    constexpr uintptr_t kTicksPerUnitScaleRva = 0x0070A83C;
    constexpr uintptr_t kRemainingUnitsScaleRva = 0x0070A840;
    constexpr uint64_t kDefaultSyntheticCpuHz = 2400000000ULL;
    constexpr wchar_t kCpuSpeedRegistryKey[] =
        L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0";
    constexpr wchar_t kCpuSpeedRegistryValue[] = L"~MHz";

    struct BytePatchSpec {
        uintptr_t rva;
        unsigned char expected0;
        unsigned char expected1;
        unsigned char replacement0;
        unsigned char replacement1;
        bool trapWithVeh;
    };

    struct DetourPatchSpec {
        uintptr_t rva;
        std::array<unsigned char, 6> expectedBytes;
        uint8_t expectedLength;
        uint8_t overwriteLength;
        void* target;
    };

    struct PatchedSpan {
        uintptr_t address;
        std::array<unsigned char, 6> originalBytes;
        uint8_t length;
    };

    struct VehSiteInfo {
        uintptr_t address;
        volatile LONG64 hitCount;
    };

}

class RDTSCPatchDirector final : public cRZCOMDllDirector {
public:
    [[nodiscard]] uint32_t GetDirectorID() const override {
        return kRDTSCPatchDirectorId;
    }

    bool OnStart(cIGZCOM* pCOM) override {
        cRZCOMDllDirector::OnStart(pCOM);

        Logger::Initialize(std::string(kLoggerName), "", false);
        LOG_INFO("RDTSCPatchDirector: OnStart");

        if (mpFrameWork) {
            mpFrameWork->AddHook(this);
        }

        return true;
    }

    bool PostAppInit() override {
        if (installed_) {
            return true;
        }

        LOG_INFO("RDTSCPatchDirector: PostAppInit");

        if (!InstallExceptionHandler_()) {
            return true;
        }

        if (!PatchMainModule_()) {
            UninstallExceptionHandler_();
            return true;
        }

        installed_ = true;
        LOG_INFO("RDTSCPatchDirector: installed {} patch(es), {} use VEH",
                 patchedSpans_.size(),
                 vehSites_.size());
        return true;
    }

    bool PostAppShutdown() override {
        LOG_INFO("RDTSCPatchDirector: PostAppShutdown");
        LogVehHitSummary_();

        RestorePatchedSites_();
        UninstallExceptionHandler_();

        if (mpFrameWork) {
            mpFrameWork->RemoveHook(this);
        }

        Logger::Shutdown();
        return true;
    }

private:
    inline static RDTSCPatchDirector* sInstance_ = nullptr;

    static LONG CALLBACK VectoredHandler_(PEXCEPTION_POINTERS exceptionInfo) {
        if (!sInstance_) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        if (!exceptionInfo || !exceptionInfo->ExceptionRecord || !exceptionInfo->ContextRecord) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        if (exceptionInfo->ExceptionRecord->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        const uintptr_t instructionPointer = static_cast<uintptr_t>(exceptionInfo->ContextRecord->Eip);
        VehSiteInfo* const vehSite = sInstance_->FindVehSite_(instructionPointer);
        if (!vehSite) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        InterlockedIncrement64(&vehSite->hitCount);

        const uint64_t fakeTsc = sInstance_->ComputeSyntheticTsc_();
        exceptionInfo->ContextRecord->Eax = static_cast<DWORD>(fakeTsc);
        exceptionInfo->ContextRecord->Edx = static_cast<DWORD>(fakeTsc >> 32);
        exceptionInfo->ContextRecord->Eip += 2;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    static double ReadProcessFloat_(const uintptr_t address) {
        return static_cast<double>(*reinterpret_cast<const float*>(address));
    }

    static uint64_t MulDivU64_(const uint64_t value, const uint64_t multiplier, const uint64_t divisor) {
        if (divisor == 0) {
            return 0;
        }

        const uint64_t quotient = value / divisor;
        const uint64_t remainder = value % divisor;
        return (quotient * multiplier) + ((remainder * multiplier) / divisor);
    }

    static uint64_t ReadNominalCpuHz_() {
        HKEY keyHandle = nullptr;
        DWORD mhz = 0;
        DWORD dataSize = sizeof(mhz);
        DWORD valueType = 0;

        const LSTATUS openStatus = RegOpenKeyExW(
            HKEY_LOCAL_MACHINE,
            kCpuSpeedRegistryKey,
            0,
            KEY_QUERY_VALUE,
            &keyHandle);
        if (openStatus != ERROR_SUCCESS) {
            return kDefaultSyntheticCpuHz;
        }

        const LSTATUS queryStatus = RegQueryValueExW(
            keyHandle,
            kCpuSpeedRegistryValue,
            nullptr,
            &valueType,
            reinterpret_cast<LPBYTE>(&mhz),
            &dataSize);
        RegCloseKey(keyHandle);

        if (queryStatus != ERROR_SUCCESS || valueType != REG_DWORD || mhz == 0) {
            return kDefaultSyntheticCpuHz;
        }

        return static_cast<uint64_t>(mhz) * 1000000ULL;
    }

    static uint64_t ReadEpochMilliseconds_() {
        FILETIME systemTime{};
        GetSystemTimeAsFileTime(&systemTime);

        ULARGE_INTEGER fileTimeValue{};
        fileTimeValue.LowPart = systemTime.dwLowDateTime;
        fileTimeValue.HighPart = systemTime.dwHighDateTime;

        constexpr uint64_t kWindowsToUnixEpoch100ns = 116444736000000000ULL;
        if (fileTimeValue.QuadPart <= kWindowsToUnixEpoch100ns) {
            return 0;
        }

        return (fileTimeValue.QuadPart - kWindowsToUnixEpoch100ns) / 10000ULL;
    }

    static uint64_t ComputeSyntheticTscStatic_() {
        return sInstance_ ? sInstance_->ComputeSyntheticTsc_() : 0;
    }

    static void __fastcall Detour_0068BF50(uint64_t* outValue, uint32_t, int units) {
        const uint64_t now = ComputeSyntheticTscStatic_();
        const int64_t offset = static_cast<int64_t>(
            static_cast<double>(units) * ReadProcessFloat_(sInstance_->ticksPerUnitScaleAddress_));
        *outValue = now + static_cast<uint64_t>(offset);
    }

    static int __fastcall Detour_0068BF80(const uint64_t* deadline) {
        const uint64_t now = ComputeSyntheticTscStatic_();
        return *deadline < now ? 1 : 0;
    }

    static int __fastcall Detour_0070E2B0(const uint64_t* deadline) {
        const uint64_t now = ComputeSyntheticTscStatic_();
        const int64_t remainingTicks = static_cast<int64_t>(*deadline - now);
        const int value = static_cast<int>(
            static_cast<double>(remainingTicks) * ReadProcessFloat_(sInstance_->remainingUnitsScaleAddress_));
        return value > 1 ? value : 1;
    }

    static uint32_t ReadU32Slot_(const float* base, const size_t index) {
        return std::bit_cast<uint32_t>(base[index]);
    }

    static void WriteU32Slot_(float* base, const size_t index, const uint32_t value) {
        base[index] = std::bit_cast<float>(value);
    }

    static uint64_t ReadU64Slots_(const float* base, const size_t lowIndex, const size_t highIndex) {
        return static_cast<uint64_t>(ReadU32Slot_(base, lowIndex)) |
               (static_cast<uint64_t>(ReadU32Slot_(base, highIndex)) << 32);
    }

    static void WriteU64Slots_(float* base, const size_t lowIndex, const size_t highIndex, const uint64_t value) {
        WriteU32Slot_(base, lowIndex, static_cast<uint32_t>(value));
        WriteU32Slot_(base, highIndex, static_cast<uint32_t>(value >> 32));
    }

    static void __fastcall Detour_005D9370(float* state) {
        LARGE_INTEGER qpcNow{};
        if (!QueryPerformanceCounter(&qpcNow)) {
            return;
        }

        const uint64_t nowQpc = static_cast<uint64_t>(qpcNow.QuadPart);
        const uint64_t previousQpc = ReadU64Slots_(state, 4, 5);
        const uint64_t minimumQpcDelta = ReadU64Slots_(state, 6, 7);

        const uint64_t qpcDelta = nowQpc - previousQpc;
        if (qpcDelta <= minimumQpcDelta) {
            return;
        }

        const uint64_t nowTsc = ComputeSyntheticTscStatic_();
        const uint64_t previousTsc = ReadU64Slots_(state, 2, 3);
        const int64_t tscDelta = static_cast<int64_t>(nowTsc - previousTsc);

        if (tscDelta > 0) {
            const double smoothed = static_cast<double>(state[0]) * 0.9;
            const double measured =
                static_cast<double>(qpcDelta) * (static_cast<double>(state[8]) / static_cast<double>(tscDelta)) * 0.1;
            state[0] = static_cast<float>(smoothed + measured);
        }

        WriteU64Slots_(state, 4, 5, nowQpc);
        WriteU64Slots_(state, 2, 3, nowTsc);
    }

    bool InstallExceptionHandler_() {
        if (vehHandle_) {
            return true;
        }

        sInstance_ = this;
        vehHandle_ = AddVectoredExceptionHandler(1, &RDTSCPatchDirector::VectoredHandler_);
        if (!vehHandle_) {
            LOG_ERROR("RDTSCPatchDirector: AddVectoredExceptionHandler failed ({})", GetLastError());
            sInstance_ = nullptr;
            return false;
        }

        LARGE_INTEGER qpcFrequency{};
        LARGE_INTEGER qpcStart{};
        if (!QueryPerformanceFrequency(&qpcFrequency) || !QueryPerformanceCounter(&qpcStart) ||
            qpcFrequency.QuadPart <= 0) {
            LOG_ERROR("RDTSCPatchDirector: QueryPerformanceCounter initialization failed");
            RemoveVectoredExceptionHandler(vehHandle_);
            vehHandle_ = nullptr;
            sInstance_ = nullptr;
            return false;
        }

        qpcFrequency_ = static_cast<uint64_t>(qpcFrequency.QuadPart);
        qpcStart_ = static_cast<uint64_t>(qpcStart.QuadPart);
        syntheticCpuHz_ = ReadNominalCpuHz_();
        const uint64_t epochMilliseconds = ReadEpochMilliseconds_();
        syntheticTscBase_ = MulDivU64_(epochMilliseconds, syntheticCpuHz_, 1000ULL);
        fakeTsc_.store(syntheticTscBase_, std::memory_order_relaxed);

        LOG_INFO("RDTSCPatchDirector: synthetic clock seeded at {} Hz from {} ms since epoch",
                 syntheticCpuHz_,
                 epochMilliseconds);

        return true;
    }

    void UninstallExceptionHandler_() {
        if (vehHandle_) {
            RemoveVectoredExceptionHandler(vehHandle_);
            vehHandle_ = nullptr;
        }
        sInstance_ = nullptr;
    }

    bool PatchMainModule_() {
        HMODULE mainModule = GetModuleHandleW(nullptr);
        if (!mainModule) {
            LOG_ERROR("RDTSCPatchDirector: GetModuleHandleW(nullptr) failed ({})", GetLastError());
            return false;
        }

        const uintptr_t moduleBase = reinterpret_cast<uintptr_t>(mainModule);
        if (moduleBase != kExpectedModuleBase) {
            LOG_WARN("RDTSCPatchDirector: unexpected module base {:08X}", moduleBase);
        }

        ticksPerUnitScaleAddress_ = moduleBase + kTicksPerUnitScaleRva;
        remainingUnitsScaleAddress_ = moduleBase + kRemainingUnitsScaleRva;

        for (const BytePatchSpec& spec : GetBytePatchSpecs_()) {
            if (!ApplyBytePatch_(moduleBase + spec.rva, spec)) {
                RestorePatchedSites_();
                return false;
            }
        }

        for (const DetourPatchSpec& spec : GetDetourPatchSpecs_()) {
            if (!ApplyDetourPatch_(moduleBase + spec.rva, spec)) {
                RestorePatchedSites_();
                return false;
            }
        }

        std::sort(patchedSpans_.begin(), patchedSpans_.end(), [](const PatchedSpan& a, const PatchedSpan& b) {
            return a.address < b.address;
        });
        std::sort(vehSites_.begin(), vehSites_.end(), [](const VehSiteInfo& a, const VehSiteInfo& b) {
            return a.address < b.address;
        });

        if (patchedSpans_.empty()) {
            LOG_WARN("RDTSCPatchDirector: no patch sites were applied");
        }

        return !patchedSpans_.empty();
    }

    static const std::vector<BytePatchSpec>& GetBytePatchSpecs_() {
        static const std::vector<BytePatchSpec> specs = {
            {0x006584D6, kRdtscOpcode0, kRdtscOpcode1, kXorEaxOpcode0, kXorEaxOpcode1, false},
            {0x006584DD, kRdtscOpcode0, kRdtscOpcode1, kXorEaxOpcode0, kXorEaxOpcode1, false},
            {0x006584E3, kRdtscOpcode0, kRdtscOpcode1, kXorEaxOpcode0, kXorEaxOpcode1, false},
            {0x006585C1, kRdtscOpcode0, kRdtscOpcode1, kXorEaxOpcode0, kXorEaxOpcode1, false},
            {0x006585C8, kRdtscOpcode0, kRdtscOpcode1, kXorEaxOpcode0, kXorEaxOpcode1, false},
            {0x006585CE, kRdtscOpcode0, kRdtscOpcode1, kXorEaxOpcode0, kXorEaxOpcode1, false},
            {0x006587A6, kRdtscOpcode0, kRdtscOpcode1, kXorEaxOpcode0, kXorEaxOpcode1, false},
            {0x006587AD, kRdtscOpcode0, kRdtscOpcode1, kXorEaxOpcode0, kXorEaxOpcode1, false},
            {0x006587B3, kRdtscOpcode0, kRdtscOpcode1, kXorEaxOpcode0, kXorEaxOpcode1, false},
            {0x0065886E, kRdtscOpcode0, kRdtscOpcode1, kXorEaxOpcode0, kXorEaxOpcode1, false},
            {0x00658875, kRdtscOpcode0, kRdtscOpcode1, kXorEaxOpcode0, kXorEaxOpcode1, false},
            {0x0065887B, kRdtscOpcode0, kRdtscOpcode1, kXorEaxOpcode0, kXorEaxOpcode1, false},

            {0x00490567, kJnzShortOpcode, 0x16, kJmpShortOpcode, 0x16, false},
            {0x00490599, kJnzShortOpcode, 0x17, kJmpShortOpcode, 0x17, false},
            {0x004905C8, kJnzShortOpcode, 0x1A, kJmpShortOpcode, 0x1A, false},

            {0x00493153, kRdtscOpcode0, kRdtscOpcode1, kUd2Opcode0, kUd2Opcode1, true},
            {0x0049310F, kRdtscOpcode0, kRdtscOpcode1, kUd2Opcode0, kUd2Opcode1, true},
            {0x004904E7, kRdtscOpcode0, kRdtscOpcode1, kUd2Opcode0, kUd2Opcode1, true},
            {0x004903A6, kRdtscOpcode0, kRdtscOpcode1, kUd2Opcode0, kUd2Opcode1, true},
            {0x004902A9, kRdtscOpcode0, kRdtscOpcode1, kUd2Opcode0, kUd2Opcode1, true},
            {0x004901C7, kRdtscOpcode0, kRdtscOpcode1, kUd2Opcode0, kUd2Opcode1, true},
            {0x00490143, kRdtscOpcode0, kRdtscOpcode1, kUd2Opcode0, kUd2Opcode1, true},
            {0x00490137, kRdtscOpcode0, kRdtscOpcode1, kUd2Opcode0, kUd2Opcode1, true},
            {0x0048FE80, kRdtscOpcode0, kRdtscOpcode1, kUd2Opcode0, kUd2Opcode1, true},
            {0x0048FE45, kRdtscOpcode0, kRdtscOpcode1, kUd2Opcode0, kUd2Opcode1, true},
            {0x0048FDEB, kRdtscOpcode0, kRdtscOpcode1, kUd2Opcode0, kUd2Opcode1, true},
            {0x00320ADB, kRdtscOpcode0, kRdtscOpcode1, kUd2Opcode0, kUd2Opcode1, true},
            {0x00320A76, kRdtscOpcode0, kRdtscOpcode1, kUd2Opcode0, kUd2Opcode1, true},
            {0x0032081B, kRdtscOpcode0, kRdtscOpcode1, kUd2Opcode0, kUd2Opcode1, true},
        };
        return specs;
    }

    static const std::vector<DetourPatchSpec>& GetDetourPatchSpecs_() {
        static const std::vector<DetourPatchSpec> specs = {
            {0x0028BF50, {0x53, 0x56, 0x57, 0x8B, 0xF1, 0x00}, 5, 5,
             reinterpret_cast<void*>(&RDTSCPatchDirector::Detour_0068BF50)},
            {0x0028BF80, {0x56, 0x0F, 0x31, 0x8B, 0x31, 0x00}, 5, 5,
             reinterpret_cast<void*>(&RDTSCPatchDirector::Detour_0068BF80)},
            {0x0030E2B0, {0x83, 0xEC, 0x08, 0x56, 0x0F, 0x31}, 6, 6,
             reinterpret_cast<void*>(&RDTSCPatchDirector::Detour_0070E2B0)},
            {0x001D9370, {0x83, 0xEC, 0x18, 0x53, 0x55, 0x00}, 5, 5,
             reinterpret_cast<void*>(&RDTSCPatchDirector::Detour_005D9370)},
        };
        return specs;
    }

    bool ApplyBytePatch_(const uintptr_t address, const BytePatchSpec& spec) {
        auto* bytes = reinterpret_cast<unsigned char*>(address);
        if (bytes[0] != spec.expected0 || bytes[1] != spec.expected1) {
            LOG_WARN("RDTSCPatchDirector: expected bytes {:02X} {:02X} missing at {:08X}",
                     spec.expected0,
                     spec.expected1,
                     address);
            return false;
        }

        DWORD oldProtect = 0;
        if (!VirtualProtect(bytes, 2, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            LOG_WARN("RDTSCPatchDirector: VirtualProtect failed at {:08X} ({})", address, GetLastError());
            return false;
        }

        PatchedSpan span{};
        span.address = address;
        span.length = 2;
        span.originalBytes[0] = bytes[0];
        span.originalBytes[1] = bytes[1];
        patchedSpans_.push_back(span);

        if (spec.trapWithVeh) {
            vehSites_.push_back(VehSiteInfo{address, 0});
        }

        bytes[0] = spec.replacement0;
        bytes[1] = spec.replacement1;

        DWORD restoredProtect = 0;
        VirtualProtect(bytes, 2, oldProtect, &restoredProtect);
        FlushInstructionCache(GetCurrentProcess(), bytes, 2);
        return true;
    }

    bool ApplyDetourPatch_(const uintptr_t address, const DetourPatchSpec& spec) {
        auto* bytes = reinterpret_cast<unsigned char*>(address);
        for (uint8_t i = 0; i < spec.expectedLength; ++i) {
            if (bytes[i] != spec.expectedBytes[i]) {
                LOG_WARN("RDTSCPatchDirector: detour precondition failed at {:08X}", address);
                return false;
            }
        }

        DWORD oldProtect = 0;
        if (!VirtualProtect(bytes, spec.overwriteLength, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            LOG_WARN("RDTSCPatchDirector: VirtualProtect failed at {:08X} ({})", address, GetLastError());
            return false;
        }

        PatchedSpan span{};
        span.address = address;
        span.length = spec.overwriteLength;
        for (uint8_t i = 0; i < spec.overwriteLength; ++i) {
            span.originalBytes[i] = bytes[i];
        }
        patchedSpans_.push_back(span);

        const intptr_t displacement =
            reinterpret_cast<intptr_t>(spec.target) - static_cast<intptr_t>(address + 5);

        bytes[0] = kNearJumpOpcode;
        *reinterpret_cast<int32_t*>(bytes + 1) = static_cast<int32_t>(displacement);
        for (uint8_t i = 5; i < spec.overwriteLength; ++i) {
            bytes[i] = kNopOpcode;
        }

        DWORD restoredProtect = 0;
        VirtualProtect(bytes, spec.overwriteLength, oldProtect, &restoredProtect);
        FlushInstructionCache(GetCurrentProcess(), bytes, spec.overwriteLength);
        return true;
    }

    void RestorePatchedSites_() {
        if (patchedSpans_.empty()) {
            installed_ = false;
            return;
        }

        for (const PatchedSpan& span : patchedSpans_) {
            auto* address = reinterpret_cast<unsigned char*>(span.address);

            DWORD oldProtect = 0;
            if (!VirtualProtect(address, span.length, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                continue;
            }

            for (uint8_t i = 0; i < span.length; ++i) {
                address[i] = span.originalBytes[i];
            }

            DWORD restoredProtect = 0;
            VirtualProtect(address, span.length, oldProtect, &restoredProtect);
            FlushInstructionCache(GetCurrentProcess(), address, span.length);
        }

        patchedSpans_.clear();
        vehSites_.clear();
        installed_ = false;
    }

    VehSiteInfo* FindVehSite_(const uintptr_t address) {
        const auto it = std::lower_bound(
            vehSites_.begin(),
            vehSites_.end(),
            address,
            [](const VehSiteInfo& left, const uintptr_t right) {
                return left.address < right;
            });

        if (it != vehSites_.end() && it->address == address) {
            return &(*it);
        }

        return nullptr;
    }

    void LogVehHitSummary_() {
        if (vehSites_.empty()) {
            return;
        }

        std::vector<VehSiteInfo> hitSites;
        hitSites.reserve(vehSites_.size());
        for (VehSiteInfo& site : vehSites_) {
            const LONG64 hitCount = InterlockedCompareExchange64(&site.hitCount, 0, 0);
            if (hitCount != 0) {
                hitSites.push_back(VehSiteInfo{site.address, hitCount});
            }
        }

        if (hitSites.empty()) {
            LOG_INFO("RDTSCPatchDirector: no VEH traps were hit");
            return;
        }

        std::sort(hitSites.begin(), hitSites.end(), [](const VehSiteInfo& a, const VehSiteInfo& b) {
            if (a.hitCount != b.hitCount) {
                return a.hitCount > b.hitCount;
            }
            return a.address < b.address;
        });

        LOG_INFO("RDTSCPatchDirector: VEH hit summary ({} active site(s))", hitSites.size());
        for (const VehSiteInfo& site : hitSites) {
            LOG_INFO("RDTSCPatchDirector: VEH {:08X} hit {} time(s)", site.address, site.hitCount);
        }
    }

    [[nodiscard]] uint64_t ComputeSyntheticTsc_() {
        if (qpcFrequency_ == 0) {
            return fakeTsc_.fetch_add(1, std::memory_order_relaxed);
        }

        LARGE_INTEGER qpcNow{};
        if (!QueryPerformanceCounter(&qpcNow)) {
            return fakeTsc_.fetch_add(1, std::memory_order_relaxed);
        }

        const uint64_t currentQpc = static_cast<uint64_t>(qpcNow.QuadPart);
        const uint64_t elapsedQpc = currentQpc - qpcStart_;

        const uint64_t syntheticTsc = syntheticTscBase_ + MulDivU64_(elapsedQpc, syntheticCpuHz_, qpcFrequency_);

        uint64_t observed = fakeTsc_.load(std::memory_order_relaxed);
        while (observed < syntheticTsc &&
               !fakeTsc_.compare_exchange_weak(observed, syntheticTsc, std::memory_order_relaxed)) {
        }

        if (observed >= syntheticTsc) {
            return observed;
        }

        return syntheticTsc;
    }

    void* vehHandle_ = nullptr;
    std::vector<PatchedSpan> patchedSpans_;
    std::vector<VehSiteInfo> vehSites_;
    std::atomic<uint64_t> fakeTsc_{0};
    uint64_t qpcFrequency_ = 0;
    uint64_t qpcStart_ = 0;
    uint64_t syntheticCpuHz_ = kDefaultSyntheticCpuHz;
    uint64_t syntheticTscBase_ = 0;
    uintptr_t ticksPerUnitScaleAddress_ = 0;
    uintptr_t remainingUnitsScaleAddress_ = 0;
    bool installed_ = false;
};

static RDTSCPatchDirector sDirector;

cRZCOMDllDirector* RZGetCOMDllDirector() {
    static auto sAddedRef = false;
    if (!sAddedRef) {
        sDirector.AddRef();
        sAddedRef = true;
    }
    return &sDirector;
}
