;BEGIN FRAGMENT CODE - Do not edit anything between this and the end comment
;NEXT FRAGMENT INDEX 3
Scriptname QF_AF_WakeUpInJailQuest_01 Extends Quest Hidden

;BEGIN FRAGMENT Fragment_1
Function Fragment_1()
;BEGIN CODE
Actor player = Game.GetPlayer()

; Addiction Framework hands off at the instant of the blackout and does NOTHING visual — this scenario owns
; the whole look. Pass out with the engine's own screen fade (Game.FadeOutGame survives ENB / Community
; Shaders, unlike an image-space modifier). A scenario could stagger, play audio, etc. here first.
Game.FadeOutGame(true, true, 0.0, 3.0)  ; fade to black over 3s
Utility.Wait(3.5)                        ; let the fade finish + a beat of full black

; teleport under full black
Faction crime = GetHoldCrimeFaction(player)
If (crime)
    crime.SendPlayerToJail(true, true)  ; remove inventory to the evidence chest, a "real" jail cell
EndIf

; woozy wake-up: first person + the get-up idle, revealed by the engine fade-in with the SleepyTimeFadeIn
; wobble layered on top. (Tune to taste — the imod/engine-fade interaction is setup-dependent.)
Game.DisablePlayerControls(abLooking = true, abCamSwitch = true)
Game.ForceFirstPerson()
SleepyTimeFadeIn.Apply()                  ; woozy wobble overlay
Game.FadeOutGame(false, true, 0.0, 2.0)   ; fade the black back in over 2s
player.PlayIdle(Idle_1stPersonWoozyGetUpFromBed)
Utility.Wait(3.0)
Game.EnablePlayerControls()
Self.Stop()  ; the next blackout's Start() re-runs this start-up stage
;END CODE
EndFunction
;END FRAGMENT

;END FRAGMENT CODE - Do not edit anything between this and the begin comment

; Walk the player's current-location parent chain; the first ancestor that is one of the nine hold
; locations decides the jail. Fallback = Whiterun (covers a modded/no-location cell). Every form below is a
; base-game (Skyrim.esm) property named EXACTLY after its editorID, so the CK's Auto-Fill fills them all at
; once — NOT Game.GetFormFromFile (that would be a per-call Papyrus lookup for base-game forms).
Faction Function GetHoldCrimeFaction(Actor akPlayer)
    Location loc = akPlayer.GetCurrentLocation()
    int guard = 0
    While (loc && guard < 25)
        If (loc == WhiterunHoldLocation)
            Return CrimeFactionWhiterun
        ElseIf (loc == HaafingarHoldLocation)
            Return CrimeFactionHaafingar
        ElseIf (loc == FalkreathHoldLocation)
            Return CrimeFactionFalkreath
        ElseIf (loc == HjaalmarchHoldLocation)
            Return CrimeFactionHjaalmarch
        ElseIf (loc == PaleHoldLocation)
            Return CrimeFactionPale
        ElseIf (loc == RiftHoldLocation)
            Return CrimeFactionRift
        ElseIf (loc == WinterholdHoldLocation)
            Return CrimeFactionWinterhold
        ElseIf (loc == EastmarchHoldLocation)
            Return CrimeFactionEastmarch
        ElseIf (loc == ReachHoldLocation)
            Return CrimeFactionReach
        EndIf
        loc = loc.GetParent()
        guard += 1
    EndWhile
    Return CrimeFactionWhiterun  ; fallback: Whiterun
EndFunction

; --- properties (name == editorID; use the CK's Auto-Fill) ---
Location Property WhiterunHoldLocation   Auto
Location Property HaafingarHoldLocation  Auto
Location Property FalkreathHoldLocation  Auto
Location Property HjaalmarchHoldLocation Auto
Location Property PaleHoldLocation       Auto
Location Property RiftHoldLocation       Auto
Location Property WinterholdHoldLocation Auto
Location Property EastmarchHoldLocation  Auto
Location Property ReachHoldLocation      Auto

Faction Property CrimeFactionWhiterun    Auto
Faction Property CrimeFactionHaafingar   Auto
Faction Property CrimeFactionFalkreath   Auto
Faction Property CrimeFactionHjaalmarch  Auto
Faction Property CrimeFactionPale        Auto
Faction Property CrimeFactionRift        Auto
Faction Property CrimeFactionWinterhold  Auto
Faction Property CrimeFactionEastmarch   Auto
Faction Property CrimeFactionReach       Auto

ImageSpaceModifier Property SleepyTimeFadeIn              Auto
Idle               Property Idle_1stPersonWoozyGetUpFromBed Auto
