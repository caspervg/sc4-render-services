#include "cIGZCOM.h"
#include "cIGZFrameWork.h"
#include "cIGZMessage2.h"
#include "cRZMessage2COMDirector.h"

#include "service/decal/RelativeCallPatch.h"
#include "utils/Logger.h"
#include "utils/VersionDetection.h"

namespace {
    constexpr uint32_t kDirectorID            = 0xCA5E16ED;
    constexpr uint16_t kSupportedGameVersion  = 641;

    // 1.1.641 absolute addresses
    constexpr uintptr_t kGrowCallSite = 0x005E16ED; // `call grow` inside the shared pool allocate 0x005E16E0
    constexpr uintptr_t kOrigGrow     = 0x0090CF9A; // GZ fixed-size pool block-ring grow (__thiscall)

    // GZ fixed-size pool block manager. Doubly linked ring
    struct PoolBlock {
        PoolBlock* prev;     // +0x00
        PoolBlock* next;     // +0x04
        void*      freeHead; // +0x08  nullptr => this block is full
        uint32_t   counter;  // +0x0C  grow adds 0x40000000 per rotation; low byte = node count (0x7F)
    };

    // GZ fixed-size pool object (the `this` passed to grow / allocate).
    struct Pool {
        void**     vtable;   // +0x00  vtable[2] = slab allocator (returns a fresh, formatted block in EAX)
        PoolBlock* current;  // +0x04
        // +0x08 used/secondary block list (managed entirely by the original grow)
    };

    using GrowFn = void*(__thiscall*)(Pool* pool);
    // Number of times the guard had to sweep at least one full block (i.e. a crash averted).
    unsigned long long g_guardActivations = 0;
    unsigned long long g_sweptFullBlocks = 0;

    // Wrapper installed in place of `call grow`. __fastcall captures the thiscall
    // `this` (ECX) as the first argument; EDX is unused. No stack arguments, so the
    // stack is balanced exactly like the original __thiscall callee.
    void* __fastcall GrowFixed(Pool* pool, void* /*edx*/) {
        const auto originalGrow = reinterpret_cast<GrowFn>(kOrigGrow);
        void* node = originalGrow(pool);
        if (node != nullptr) {
            return node;
        }

        unsigned sweptThisCall = 1;
        do {
            node = originalGrow(pool);
            ++sweptThisCall;
        } while (node == nullptr);

        const unsigned long long n = ++g_guardActivations;
        g_sweptFullBlocks += sweptThisCall;
        if (n == 1) {
            LOG_INFO("FixedPoolGrowthFix: grow guard active first time (pool 0x{:08X}, swept {} saturated "
                     "active-ring blocks). A saturated-ring crash was likely avoided.",
                     static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pool)),
                     sweptThisCall);
        } else if ((n & 0x1FF) == 0) { // every 512 activations
            LOG_INFO("FixedPoolGrowthFix: grow guard engaged {} times so far; swept {} full blocks.",
                     n, g_sweptFullBlocks);
        }

        return node;
    }
}

class PoolGrowthFixDirector final : public cRZMessage2COMDirector {
public:
    PoolGrowthFixDirector() = default;

    [[nodiscard]] uint32_t GetDirectorID() const override {
        return kDirectorID;
    }

    bool OnStart(cIGZCOM* pCOM) override {
        cRZMessage2COMDirector::OnStart(pCOM);
        Logger::Initialize("SC4FixedPoolGrowthFix", "");
        mpFrameWork->AddHook(this);
        return true;
    }

    bool PostAppInit() override {
        const uint16_t version = VersionDetection::GetInstance().GetGameVersion();
        if (version != kSupportedGameVersion) {
            LOG_WARN("FixedPoolGrowthFix: game version {} unsupported (addresses target {}); not patching.",
                     version, kSupportedGameVersion);
            return true;
        }

        patch_.Configure("FixedPoolGrowthFix.grow", kGrowCallSite, reinterpret_cast<void*>(&GrowFixed));
        if (!patch_.Install()) {
            LOG_ERROR("FixedPoolGrowthFix: failed to install grow guard at 0x{:08X}.",
                      static_cast<uint32_t>(kGrowCallSite));
            return true;
        }

        // Defense in depth: the call site must have pointed at the expected grow.
        // If it did not, the binary is not what we mapped these offsets against;
        // restore it rather than risk redirecting an unrelated call.
        if (patch_.GetOriginalTarget() != kOrigGrow) {
            LOG_ERROR("FixedPoolGrowthFix: call site 0x{:08X} targets 0x{:08X}, expected 0x{:08X}; reverting.",
                      static_cast<uint32_t>(kGrowCallSite),
                      static_cast<uint32_t>(patch_.GetOriginalTarget()),
                      static_cast<uint32_t>(kOrigGrow));
            patch_.Uninstall();
            return true;
        }

        LOG_INFO("FixedPoolGrowthFix: grow guard installed (call site 0x{:08X} -> GrowFixed, original grow 0x{:08X}).",
                 static_cast<uint32_t>(kGrowCallSite), static_cast<uint32_t>(kOrigGrow));
        return true;
    }

    bool PostAppShutdown() override {
        patch_.Uninstall();
        if (mpFrameWork) {
            mpFrameWork->RemoveHook(this);
        }
        return true;
    }

    // No notifications are registered; required override for the message director base.
    bool DoMessage(cIGZMessage2*) override {
        return true;
    }

private:
    TerrainDecal::RelativeCallPatch patch_{};
};

static PoolGrowthFixDirector sDirector;

cRZCOMDllDirector* RZGetCOMDllDirector() {
    static bool sAddedRef = false;
    if (!sAddedRef) {
        sDirector.AddRef();
        sAddedRef = true;
    }
    return &sDirector;
}
