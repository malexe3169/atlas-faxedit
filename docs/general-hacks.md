<hr>

# General Hacks

<hr>

[Echoes of Eolis](https://github.com/kaimitai/faxedit) ships a library of optional general hacks: self-contained gameplay and engine modifications that are enabled from the configuration and injected into the ROM at build time. Unlike the extended script opcodes, a general hack needs no script changes at all — enabling it is the whole integration.

General hacks are completely optional. A project that enables none of them produces behavior identical to the original game. Installers validate their parameters and available output capacity, and newer engine-level installers also verify their hook preimages. Some legacy installers do not yet verify every overwritten byte, so use a ROM compatible with the selected configuration region and do not assume arbitrary pre-patched ROMs can safely compose with every hack.

This document describes the hacks in the current library and their parameters. It assumes you are familiar with the configuration override system described in the [advanced modding documentation](advanced-modding.md).

> **Warning:** General hacks are not automatically removed from an already patched ROM. If you build a ROM with hacks A, B and C, then load that ROM and rebuild it with only hacks D, E and F, hooks and other patches from A, B and C may remain in the ROM.
>
> Keep a clean base ROM and treat patched ROMs as build outputs. Your project XML, config overrides, ASM/music sources and other source files should be considered the authoritative project state.
>
> Rebuilding an already patched ROM is generally safe when you continue installing the same hacks, but changing the selected set of hacks should be done from a clean base ROM.


<hr>

## Table of Contents

- [Enabling General Hacks](#enabling-general-hacks)
- [The Library](#the-library)
  - [KillSwitch](#killswitch)
  - [SameWorldTransPal2Mus](#sameworldtranspal2mus)
  - [FastStart](#faststart)
  - [QuestFlagItemDrops](#questflagitemdrops)
  - [BossLockedItems](#bosslockeditems)
  - [FlexibleItems](#flexibleitems)
  - [FogRules](#fogrules)
  - [DynamicTilesets](#dynamictilesets)
  - [AtlasDevFrameScheduler](#atlasdevframescheduler)
  - [AtlasDevDayNightCycle](#atlasdevdaynightcycle)
  - [AtlasDevInfectedTint](#atlasdevinfectedtint)
  - [AtlasDevTimeOfDay](#atlasdevtimeofday)
  - [AtlasDevMusicIntent](#atlasdevmusicintent)

<hr>

## Enabling General Hacks

General hacks are listed in a `general_hacks` string in the configuration, one hack per line, with optional `key=value` parameters after the name. The entry can be scoped to a region with the `region` attribute.

```xml
<strings>
	<string name="general_hacks" region="us">
		KillSwitch
		FastStart gold=2000 ring_of_elf=false
		FlexibleItems price=250
		FogRules rules=0:1+0:3+7
	</string>
</strings>
```

Hacks are installed in the order listed. An unknown hack name or an invalid parameter stops the build with an error. Numeric parameters accept decimal, `$` or `0x` hexadecimal, and `%` binary.

<hr>

## The Library

### KillSwitch

Pressing Select while the game is paused kills the player when the game is unpaused. This gives players a way out of softlocks without resetting the console and losing progress since the last password.

No parameters.

```text
KillSwitch
```

### SameWorldTransPal2Mus

Screen transitions inside the same world apply the palette-to-music rules that normally only run when passing through a door. A transition that changes the area palette can then also change the music, which makes large single-world maps feel like distinct areas.

No parameters.

```text
SameWorldTransPal2Mus
```

### FastStart

Starts a new game with more resources: health and mana start at 80, starting gold is configurable, and the Ring of Elf can be granted from the beginning so the Eolis gate content is open immediately.

| parameter | default | meaning |
| --- | --- | --- |
| `gold` | `1500` | starting gold |
| `ring_of_elf` | `true` | start with the Ring of Elf |

```text
FastStart gold=2000 ring_of_elf=false
```

### QuestFlagItemDrops

The wyvern's mattock and the stone dropper's wing boots normally depend on quest flags, which makes the drops unrepeatable. With this hack the drop check asks whether the player actually has the item in inventory or equipped, so a lost item can be earned again.

| parameter | default | meaning |
| --- | --- | --- |
| `type` | `both` | which drops to change: `both`, `mattock` or `wing_boots` |

```text
QuestFlagItemDrops type=mattock
```

### BossLockedItems

Boss-locked item sprites appear regardless of which boss guards the screen, so custom screens can combine any boss with any locked item. Optionally the item stays hidden until every enemy sprite on the screen has been removed.

| parameter | default | meaning |
| --- | --- | --- |
| `enemies` | `true` | keep the item hidden until all enemies are cleared |

```text
BossLockedItems enemies=false
```

### FlexibleItems

> FlexibleItems is currently unsupported for JP and JP-derived ROM regions

Loosens the vanilla item restrictions in three independent ways: items can be used inside buildings, the player-state gate on item use is removed, and shops will buy any item — items without a sell-table entry sell for a configurable price.

| parameter | default | meaning |
| --- | --- | --- |
| `buildings` | `true` | allow item use inside buildings |
| `state` | `true` | ignore the player-state gate on item use |
| `selling` | `true` | shops buy any item |
| `price` | `100` | sell price for items without a sell-table entry |

```text
FlexibleItems buildings=true state=false price=250
```

### FogRules

Enables the fog effect on arbitrary world and palette combinations while reusing the vanilla fog update routine. Rules are `world:palette` pairs separated by `+`; a bare world number enables fog on every palette in that world. At least one rule is required.

| parameter | default | meaning |
| --- | --- | --- |
| `rules` | none, required | `world:palette` pairs, `+`-separated; a bare world covers the whole world |

```text
FogRules rules=0:1+0:3+0:5+6:3+7
```

### DynamicTilesets

Allows individual screens to override their world's normal tileset. Overrides are given as `world:screen:tileset` entries separated by `+`. Screens without an entry continue to use the tileset selected by the normal game logic.

The lookup code and data are placed with the rest of the hack by default. For ROMs where fixed-bank space is limited, `bank` and `addr` can place them in another PRG bank instead.

The transition hooks are individually configurable. Hooks which are disabled are not installed and their trampolines do not consume ROM space. Entering buildings is disabled by default because buildings already have their own per-screen tileset selection mechanism via the Building Scene objects.

| parameter | default | meaning |
| --- | --- | --- |
| `data` | none, required | `world:screen:tileset` entries separated by `+`
| `bank` | 15 | PRG bank containing the lookup code and data table |
| `addr` | none | CPU address of the lookup code when `bank` is not 15 |
| `enter_building` | `false` | apply overrides when entering buildings |
| `exit_building` | `true` | apply overrides when exiting buildings |
| `sameworld` | `true` | apply overrides to same-world doors and screen transitions |
| `start_screen` | `true` | apply an override when loading the starting screen |
| `otherworld` | `true` | apply overrides to otherworld-transitions |
| `stage_doors` | `true` | apply overrides to stage-door transitions |

For normal use, no placement parameters are necessary:

```text
DynamicTilesets data=0:1:6+2:2:5+2:3:5+7:0:5
```

To keep the lookup code and data in another bank:

```text
DynamicTilesets bank=28 addr=0x8000 data=0:1:6+2:2:5+2:3:5+7:0:5
```

For expanded ROMs, it is reasonable to use `bank=28` and `addr=$8000` unless something else was deliberately put there. By default bank 29 is used for the doubled tileset collection, and bank 30 for dynamic tilemap changes.

Hooks can be disabled when a project does not need those transition types:

```text
DynamicTilesets start_screen=false enter_building=false exit_building=false data=0:1:6+2:2:5
```

DynamicTilesets changes which CHR tileset is loaded; it does not change a world's metatile definitions. Alternate tilesets should therefore use a compatible tile layout for the screens and metatiles that use them. In the vanilla game, the buildings world uses a shared set of metatile definitions, but 3 different tilesets. They partitioned 256 metatiles across the tilesets, and such an approach can be taken when using this feature.

### AtlasDevFrameScheduler

A neutral frame scheduler other hacks build on: an NMI tick with three role slots and an exclusive post-deadline lane for work that must run after the frame's last critical PPU write. PRE roles run only when both the PPU queue and nametable-strip work are idle. On its own it changes nothing visible — it exists so per-frame hacks can share one hook instead of each patching the NMI. Role hacks like AtlasDevDayNightCycle require it and refuse to build without it.

The three slots are RAM, so scripts can switch roles on and off at runtime with the AtlasDevArmRole and AtlasDevDayNight opcodes. At build time, a boot slot is unclaimed only when its arm byte is zero and its PRE vector still points to the scheduler's default stub. A role installer reuses only a compatible existing kind or claims the first unclaimed slot, refusing without modifying the ROM when none is available. The single POST lane similarly refuses a second claimant.

The current runtime opcodes do not retain persistent kind-to-slot affinity: when arming an inactive kind, they select the first zero RAM slot. Runtime composition is therefore safe only while candidate slots use stub PRE vectors. A future scheduler ABI extension is required before boot-off non-stub PRE roles can reserve a lane across runtime disarm/rearm operations.

No parameters.

```text
AtlasDevFrameScheduler
```

### AtlasDevDayNightCycle

A day and night cycle: the three background palette rows dim from the engine's own palette shadow and return on a configurable day length, with the HUD row untouched. Requires AtlasDevFrameScheduler earlier in the list and exclusive ownership of its POST lane. Scripts can stop and start the cycle with AtlasDevDayNight or AtlasDevArmRole 2; stopping always completes an eight-call full-daylight sweep before going quiet, even when stopped during dawn.

| parameter | default | meaning |
| --- | --- | --- |
| `length` | `2048` | frames per full day cycle, multiple of 8 |

```text
AtlasDevDayNightCycle length=7200
```

### AtlasDevInfectedTint

Tints sprite palette 0 with three configurable colors and a pulse, for a
poisoned or cursed look on the hero. Requires AtlasDevFrameScheduler.
Runs beside other roles on the same scheduler; scripts switch it with
AtlasDevArmRole 3, and switching off restores the palette from the
engine's shadow. When combined with AtlasDevDayNightCycle, list the
tint after it - the tint chains onto a claimed post lane, while the
day cycle demands an unclaimed one.

| parameter | default | meaning |
| --- | --- | --- |
| `colors` | `$09+$19+$29` | three palette values, plus separated |
| `pulse` | `$20` | pulse mask, a power of two; `0` for a steady tint |
| `armed` | `1` | `0` installs it dormant, for scripts to switch on |

```text
AtlasDevInfectedTint colors=$0C+$1C+$2C pulse=0
```

### AtlasDevTimeOfDay

An in-game clock with a two-digit hour readout in the HUD, running as a
role on the AtlasDevFrameScheduler. The readout uses the engine's own
digit convention, so it inherits the HUD font and palette automatically.
Requires AtlasDevFrameScheduler. Like the tint, it chains onto a post
lane another role already holds, so the day cycle, the tint and the
clock can all run in the same frame. Scripts switch it with
AtlasDevArmRole 4; switching off blanks the readout and resets the
clock, so re-arming starts the day at the start hour again.

| parameter | default | meaning |
| --- | --- | --- |
| `hourlength` | `300` | frames per in-game hour |
| `start` | `12` | the hour the clock boots at, `0` to `23` |
| `cell` | `$2038` | nametable address of the two readout cells |

```text
AtlasDevTimeOfDay hourlength=600 start=6 cell=$2038
```

### AtlasDevMusicIntent

Publishes transaction-safe family-local state requests for the sixteen-family
score. It derives fixed state codes from the game's own indoor, outdoor, and
live-enemy facts:

| state | code | arrangement |
| --- | --- | --- |
| calm | `0` | `establish` |
| explore | `1` | `drive` |
| danger | `2` | `crisis` |

The active family comes from bits 5..2 of the conductor's committed landing at
`$04F7`. Mantra/Death has no game-requested crisis state, so a live danger fact
in that family is clamped to explore. Its structural crisis fallback nodes
remain valid score data. A same-family state change is legal in every other
family, including terminal Outro.

| parameter | default | meaning |
| --- | --- | --- |
| `hysteresis_frames` | `30` | consecutive eligible samples before publishing, `0` to `65535` |

```text
AtlasDevFrameScheduler
AtlasDevMusicIntent hysteresis_frames=30
```

The shared record is `$04EF-$04F7`:

| address | ownership | meaning |
| --- | --- | --- |
| `$04EF` | conductor/event wrappers | ordinary packed family/state, or an event/lifecycle command; publisher reads markers but never writes this byte |
| `$04F0-$04F1` | conductor | PRNG state |
| `$04F2` | conductor/loader | active-node token; bit 7 alone enables the publisher |
| `$04F3` | bit-shared | bit 7 `PUBLISH_READY`, bit 6 `TXN_LOCK`, bits 5..2 zero, bits 1..0 candidate state |
| `$04F4-$04F5` | publisher | 16-bit candidate dwell |
| `$04F6` | conductor/loader | staged-node token |
| `$04F7` | conductor/event producer | landing family/state plus bit 6 staged-event snapshot and bit 7 landing obligation |

Every publisher write to `$04F3` preserves conductor bit 6 and clears reserved
bits 5..2. Once bit 7 is set, candidate and dwell are immutable until the
conductor consumes them. While `TXN_LOCK` or either high bit of `$04F7` is set,
the active publisher makes no publisher-owned write at all. This closes the
snapshot-first/event-command-last NMI interleaving and lets a ready candidate
remain latched across a landing obligation.

After hysteresis, ordinary publication is still refused unless `$04EF` has no
event/lifecycle marker, `$04F6 == $04F2`, and music owner `$FA` is either zero
or negative. Every positive load token `$01-$7F` blocks publication. When all
gates agree, the publisher sets `PUBLISH_READY` in a final `$04F3` store. It
never writes `$04EF`. The
conductor sees READY later in the same NMI, merges `$04F7` family bits with the
published low state bits, and owns the resulting EF/F7/F6 transaction. A failed
attempt leaves the saturated candidate and dwell available for a later eligible
sample. Inactive cleanup clears only publisher state and preserves `TXN_LOCK`.

Install the publisher into a conductor-patched ROM with the dedicated CLI:

```sh
eoe-cli install-music-intent conductor.nes complete.nes --region us \
  --ram-base 0x04ef --hysteresis 30 --json music-intent-install.json
```

The installer rejects any selected optional RAM record that intersects through
`$04F7`, and verifies that no output byte escapes the two scheduler hooks and
the contiguous bank-15 allocation. The JSON report records the shared bit
masks, ordering and fail-closed gates, exact publisher byte range and SHA-256,
and timing certificate version 2. The measured emitted-publisher maximum is
348 cycles/114 instructions. With the unchanged scheduler overhead the full
hook is 496 cycles/154 instructions, or 1,010 cycles with maximum OAM DMA. A
budget already containing the vanilla OAM sequence adds at most 491 cycles/152
instructions, including the conservative one-cycle DMA alignment delta.
