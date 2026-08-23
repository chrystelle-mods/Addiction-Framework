Scriptname Addiction Hidden

; Addiction Framework — public Papyrus API (global natives). Categories are addressed by their config
; STRING (the category key, e.g. "Caffeine") — stable, ASCII, treated like a function name. No Forms and
; no master required. An unknown/typo'd name is a safe no-op (level 0 / not addicted). Player-only; no
; actor arg.
;
; Mod events (RegisterForModEvent), sent as SKSE mod-callback events. The category STRING rides in the
; string arg, so handlers stay master-free:
;   "AF_OnUse"          (string asCategory, float afGained)   ; every use, any trigger source
;   "AF_OnStageChanged" (string asCategory, float afStage)    ; new stage: 0 clean / 1 satisfied / 2 withdrawal
;   "AF_OnCured"        (string asCategory, float afZero)
;   "AF_OnAcuteStart"   (string asCategory, float afOne)      ; acute status turned ON (Drunk/High/Wired/…)
;   "AF_OnAcuteEnd"     (string asCategory, float afZero)     ; acute status turned OFF
;   "AF_OnBlackout"     (string asCategory, float afPotency)  ; the player passed out (blackout tier crossed).
;                                                             ; A general notification for any mod. AF fades to
;                                                             ; black and (separately) STARTS the chosen
;                                                             ; scenario's quest, or runs the default fade +
;                                                             ; time-skip. This is NOT the scenario trigger —
;                                                             ; a scenario is driven by its own quest being
;                                                             ; started (a start-up-stage fragment), so it
;                                                             ; needs no event and no name.

; --- query ---
float    function GetLevel(string asCategory) global native            ; 0-100, applies lazy decay
int      function GetStage(string asCategory) global native            ; 0 clean, 1 satisfied, 2 withdrawal
bool     function IsAddicted(string asCategory) global native
bool     function IsInAcuteStatus(string asCategory) global native     ; any acute status active (trailing-window potency >= threshold)
bool     function IsIntoxicated(string asCategory) global native       ; acute active AND the effect is an inebriation one (carries AF_Intoxicated)
float    function GetAcuteLevel(string asCategory) global native       ; current trailing-window potency sum
string[] function GetActiveAddictions() global native                  ; category names currently addicted

; --- mutate ---
float function NotifyUse(string asCategory, float afAmount) global native  ; relay a use (potency 0-1); returns new level
function AddLevel(string asCategory, float afAmount) global native         ; raw level nudge (no use-timestamp)
function Cure(string asCategory) global native                            ; level->0, effects off, fires AF_OnCured
function CureAll() global native                                          ; cure EVERY category + strip all AF spells/effects from the player (cleanup)
function ResetWithdrawalTimer(string asCategory) global native            ; "Cure Hangover": clear the withdrawal clock (level unchanged)

; --- ongoing rate modifiers (blunt, script-owned — think Actor.ModActorValue: you apply it, you remove it).
;     asKind = "gain" (addiction gain per use) | "decay" (level decay speed) | "withdrawal" (withdrawal effect
;     strength). afPercent is a summed percent: gain 100 = fully blocked, decay 100 = +100% decay speed,
;     withdrawal 100 = fully muted; negative = a potentiator. Stacks with any keyword modifiers on the same
;     kind. Persisted in the co-save. A bad kind or unknown category is a safe no-op. For a self-clearing,
;     time-bound modifier, author a magic effect with the matching keyword instead (that's what MGEFs are for).
function AddModifier(string asCategory, string asKind, float afPercent) global native  ; add to the running offset; pass negative to undo
function ClearModifier(string asCategory, string asKind) global native                 ; hard-reset that offset to 0
float function GetModifier(string asCategory, string asKind) global native             ; current effective aggregate (keyword + script)
