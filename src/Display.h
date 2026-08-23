#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace AddictionFramework
{
    // One actor-value reduction within a consequence effect. A container spell's blank pool is stamped
    // from a list of these at load (Mechanism B): each used blank gets one SubEffect's actor value, and
    // its magnitude scales to `max` at level 100.
    struct SubEffect
    {
        RE::ActorValue av   = RE::ActorValue::kNone;
        float          max  = 0.0f;  // magnitude at level 100 for this AV
        std::string    name;         // this effect's display name (config or derived) → the MGEF's fullName
    };

    // Applies each addiction/withdrawal consequence as a CONTAINER spell whose blank Peak-Value-Modifier
    // effects AF retargets to the configured actor values at load (DESIGN §5.1 + Mechanism B, proven by
    // Spike C: editing `EffectSetting::data.primaryAV` before AddSpell retargets which AV the effect hits).
    // Used blanks are visible + AF-named; unused blanks are hidden (HideInUI). Each blank's magnitude IS
    // the real reduction and scales with level; the spell is re-applied only on a meaningful drift.
    //
    // Keyed by container-spell FormID, so any number of categories are handled independently.
    class EffectManager
    {
    public:
        static EffectManager* GetSingleton();

        // Configure a container spell's blank pool from a sub-effect list (ONCE, at load): retarget each
        // used blank's actor value, set its Detrimental sign, stamp AF's display name, and HIDE the unused
        // blanks. Stores the sub-effects for later scaling. `a_spellID` = the container spell.
        void ConfigureEffect(RE::FormID a_spellID, const std::vector<SubEffect>& a_effects, bool a_detrimental,
                             const std::string& a_name);

        // Ensure the ability is present iff `a_active`; when active, scale each configured blank's magnitude
        // to max*(level/100). Re-applies (remove + re-add) only on a meaningful level drift. Requires a
        // prior ConfigureEffect for this spell.
        void ApplyEffect(RE::FormID a_spellID, bool a_active, float a_level);

        // Strip every applied ability off the player and reset bookkeeping (keeps configuration).
        void ClearAll();

    private:
        struct Applied
        {
            RE::SpellItem*         spell        = nullptr;
            std::vector<SubEffect> effects;      // configured sub-effects (<= pool size)
            bool                   detrimental  = true;
            bool                   configured   = false;
            bool                   applied      = false;
            float                  appliedLevel = -1.0f;
        };

        RE::SpellItem* Resolve(RE::FormID a_spellID, Applied& a_entry);

        std::unordered_map<RE::FormID, Applied> _effects;
    };
}
