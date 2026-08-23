#pragma once

// AddictionFramework — game-code hooks.
//
// Spike A: hook Actor::DrinkPotion to prove we (a) catch the player consuming a potion and
// (b) can read the ENIT addiction data (addictionItem / addictionChance) straight off the
// AlchemyItem struct in memory. Player-only, so we hook PlayerCharacter's vtable.

namespace AddictionFramework::Hooks
{
    // Install the consumption hook. Call once, from kPostLoad.
    void Install();
}
