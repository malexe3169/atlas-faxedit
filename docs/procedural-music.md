# Procedural music kits

FaxEdit compiles an authored, sixteen-family Faxanadu world score from ordinary
MML plus `; @pmusic-*` metadata. The metadata remains a comment to existing MML
tools, so the same source can be auditioned with `m2m` and `m2l`.

```sh
eoe-cli pmusic-compile worldscore.mml worldscore.json
# short form
eoe-cli pmc worldscore.mml worldscore.json
```

Output is replaced only after the complete source validates. Diagnostics
include source line and column. Compilation is deterministic and ROM-free.

## Score model

The family order is the runtime table ABI:

```text
intro dartmoor trunk branches mist towers eolis mantra
towns boss hourglass outro king guru shop zenis
```

Each family owns `establish`, `drive`, and `crisis` arrangements. They intensify
that family's identity; they are not global genres. Only Outro is terminal.
Mantra retains a structural crisis pool, although live danger is clamped to
`drive` there.

The score is authored-only. Every state needs at least two loops so the runtime
can avoid immediate repetition. Stock-song nodes and the former stock-playback
policy are rejected.

## Declarations

```text
; @pmusic-kit version=2 id=<id> title="<title>"
; @pmusic-style id=<id> sha256="<64-lowercase-hex>"
; @pmusic-policy default_family=<family> default_state=<state> repeat=compact-no-repeat boundary=phrase clock=ntsc quantum=<1..255>
; @pmusic-family id=<family> stock_song=<1..16> default_state=<state> terminal=<true|false>
; @pmusic-state family=<family> id=<state> section=<id> intensity=<0..255>
; @pmusic-route family=<family> from_join=<id|*> to_state=<state|*> kind=<direct|bridge|cut>
; @pmusic-event-route event=<id> timing=<immediate|boundary> source_family=<family|any> from_join=<id|*> kind=<cut|bridge|stinger> landing=<fixed|requested|return> [landing_family=<family> landing_state=<state>]
```

`stock_song` identifies which game request selects a family; it does not make
vanilla audio selectable. Families and states appear in ABI order, state
intensities strictly increase, and the policy default matches its family. The
style ID and SHA-256 bind the MML to its creative brief.

```text
; loop
; @pmusic-phrase id=<id> role=loop family=<family> states="<comma-list>" weight=<1..255> entry_join=<id> exit_join=<id> tempo=<N/D> meter=<N/D>

; family-local transition
; @pmusic-phrase id=<id> role=bridge family=<family> from_join=<id> to_state=<state> weight=<1..255> entry_join=<id> exit_join=<id> tempo=<N/D> meter=<N/D>

; sparse game event
; @pmusic-phrase id=<id> role=<bridge|stinger> family=<family> event=<id> weight=<1..255> entry_join=<id|load> exit_join=<id> tempo=<N/D> meter=<N/D>
```

Tracker-generated phrases may add `timing=tracker`. This explicitly permits
raw tick lengths so a FamiTracker importer can bake speed, tempo, delay, cut,
macro, and effect timing onto Faxanadu's music clock without rounding it onto a
musical note grid. Raw lengths remain invalid when this marker is absent.

Songs are consecutive `#song 1..N`. Core loops and local bridges precede event
phrases. Every song has one song-level `t` before its channels and an explicit
`#time`; both agree with the phrase header. Channel-local tempo and raw tick
durations are rejected because they bypass the shared musical clock. All four
channels finish together and fit the duration and byte budgets.

The complete mechanical example is
`tests/fixtures/procedural-music/worldscore-minimal.mml`. Its one-note phrases
are test vectors, not a composition.

## Game intent publisher

`AtlasDevMusicIntent` publishes a family-local state from game facts:

| game fact | state |
| --- | --- |
| indoor and no live enemy | `establish` |
| outdoor and no live enemy | `drive` |
| any active entity has HP | `crisis` |

It applies hysteresis, respects conductor transactions, and commits only at a
safe phrase boundary:

```sh
eoe-cli install-music-intent conductor.nes complete.nes --region us \
  --ram-base 0x04ef --hysteresis 30 --json music-intent-install.json
```

The installer validates region, scheduler signature, RAM ownership, hooks, and
allocation before writing. It verifies that no byte outside reported ranges
changed and writes atomically. Its JSON report contains hashes, ABI ownership,
configuration, and an emitted-machine-code timing certificate.

The shared record is `$04EF-$04F7`. Positive music-load tokens, lifecycle
markers, an in-flight transaction, or an outstanding landing obligation block
publication.

## Determinism

Compilation is byte-for-byte deterministic. Runtime choice is deterministic
for the same kit, initial nonzero seed, and game-request sequence. A different
seed or play history intentionally produces a different arrangement path.
