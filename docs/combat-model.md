# What actually governs Gothic 3 combat

Extracted from the game binaries, the Community Patch manual, and the Jackydima
SDK fork. This is the map we measure and tune against.

## What to sample (per frame, from our DLL)

`gCNPC_PS::GetCombatState()` is only a 0/1 "in combat" flag. The real state
machine is on the routine property set (`eEPropertySetType_ScriptRoutine`):

| quantity | accessor | meaning |
| --- | --- | --- |
| action | `PSRoutine::Action` (`gEAction`) | 1–15 attacks, 16–24 parry/parry-stumble, 25–28 stumble, 29/32/33 knockdown, 35+ death |
| posture | `PSRoutine::AniState` (`gEAniState`) | Parade 4, SitKnockDown 15, LieKnockDown 16, LieKnockOut 17 |
| state clock | `PSRoutine::GetStateTime()` | seconds spent in the current state |
| swing phase | `Entity::GetCurrentAniPhase()` (`gEPhase`) | Raise 0, **Hit 1 = active frames**, Recover 3 |
| damage event | `gCDamageReceiver_PS::GetDamageAmount()/GetDamageType()/GetLastInflictor()` | the most recent hit |

Actions 20–34 are exactly "the player is not in control", which is the
measurement that turns "combat feels bad" into a number.

## What can be changed

| lever | where | note |
| --- | --- | --- |
| attack/animation speed | `Script_Game.dll` RVA 0x4D5B (`GetAnimationSpeedModifier`) | already wired live as `attack_speed` |
| minimum stagger duration | `ge3.INI` `[Game] MinHitDuration=6` (0–15), also live at engine setup +0x1EC | how long a hit takes control away |
| Alternative AI / Balancing | `ge3.INI` `AIMode`, `ExtendedContent`, live bools at +0x1EB | the CP's own combat rework |
| monster damage | `Script_Game.dll` RVA 0x2C06D (`CalculateMonsterDamage`) | value in ESI |
| **the whole damage + stagger decision** | script function `AssessHit`, hookable **by name** via `GetScriptAdminExt().GetScript("AssessHit")` | the Jackydima fork replaces it wholesale |
| stagger currency | inside `AssessHit`: `HitForce = ActionWeaponLevel(attacker) - ShieldLevelBonus(victim) - hyperArmor(victim)` | hyperarmor only applies during the victim's own heavy attack |
| parry rules | script functions `CanParade`, `CanParadeMoveOf`, `CanParadeMagic`, `CanParadeMissile` | also native at RVA 0xd480 |
| active hit window | AI callbacks flip the weapon's collision group to `Item_Attack` | not `.xmot` frame effects, as we assumed |

## What is wrong, per the people who studied it

- **Spam wins.** The CP team's own summary of the top complaint: the game can be
  won by monotonously clicking. QuickAttack has weapon level 0 and opens its
  active window in about 0.1 s, so spamming it out-staggers everything.
- **Machine-gun enemies.** The CP changelog explicitly reduced attack frequency
  for boars, wolves, ogres and temple guards, and removed "machine gun" attacks.
- **Stunlock with no counterplay.** Vanilla has a branch that stops NPCs from
  attacking a knocked-down hero; the fork deletes it deliberately, which tells
  you how the vanilla fight is shaped around being floored.
- **Gang-ups.** Unlimited simultaneous melee attackers in vanilla; the CP caps
  it at one to three depending on difficulty.
- **Parries leak by design.** Stab and cleave attacks pass through a parry
  unless a shield is raised.
- **Blocking is time-boxed and asymmetric.** The hero can block for at most
  2.5 s; NPCs can block longer. In vanilla a block that outlasts stamina bleeds
  the overflow into health.
