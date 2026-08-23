#include "Hooks.h"

#include "Addiction.h"

namespace
{
    // Per-frame update call-site (mirrors OAR's main-update nullsub). Drives the addiction tick on the
    // main thread each frame; the manager throttles internally. Chains the original (safe alongside OAR).
    // (The consumption trigger is NOT here — it moved to the TESSpellCastEvent sink in Casting.cpp, since
    // the Actor::DrinkPotion 0x10F vtable hook misses real inventory-menu drinks. Proven in-game 2026-08-15.)
    struct FrameUpdateHook
    {
        static void thunk()
        {
            static REL::Relocation<std::uintptr_t> deltaTimeAddr{ REL::VariantID(523660, 410199, 0x30C3A08) };
            const float delta = *reinterpret_cast<float*>(deltaTimeAddr.address());
            AddictionFramework::AddictionManager::GetSingleton()->Tick(delta);
            func();
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };
}

void AddictionFramework::Hooks::Install()
{
    SKSE::AllocTrampoline(64);  // FrameUpdateHook's write_call<5>
    auto& trampoline = SKSE::GetTrampoline();

    REL::Relocation<std::uintptr_t> mainUpdate{ REL::VariantID(35565, 36564, 0x5BAB10) };
    FrameUpdateHook::func = trampoline.write_call<5>(
        mainUpdate.address() + REL::VariantOffset(0x748, 0xC26, 0x7EE).offset(), FrameUpdateHook::thunk);

    logger::info("AddictionFramework: hooks installed (per-frame tick).");
}
