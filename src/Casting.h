#pragma once

namespace AddictionFramework::CastingSpike
{
    // Recon spike for the planned CASTING trigger (DESIGN §12): detect a PLAYER spell cast and read its
    // school, to prove the seam before wiring it to the level engine. Log-only — no engine effect yet.
    // Seam = a BSTEventSink on TESSpellCastEvent (no trampoline hook needed). Call once at kDataLoaded.
    void Install();
}
