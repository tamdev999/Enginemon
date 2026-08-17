# State Ownership Audit (Hardening Item C)

## Summary

**STATUS: VERIFIED CLEAN**

No divergent authoritative copies exist. The architecture correctly separates:
- **GameState** - Persistent gameplay state (saved/loaded)
- **HeadlessGameLoop** - Transient simulation state (per-instance)
- **Runtime presentation state** - Visual interpolation (renderer-local)

## Field-Level Ownership Table

### PlayerState (in HeadlessGameLoop)

| Field | Type | Ownership | Notes |
|-------|------|-----------|-------|
| `x` | int32_t | Authoritative (simulation) | Updated on movement completion |
| `y` | int32_t | Authoritative (simulation) | Updated on movement completion |
| `facing` | Direction | Authoritative (simulation) | Updated immediately on input |
| `is_moving` | bool | Transient | True during 16-frame step |
| `target_x/y` | int32_t | Transient | Movement destination during step |
| `frames_remaining` | int32_t | Transient | Step progress counter |
| `surfing` | bool | Authoritative | Mode flag |

### PlayerSaveState (in GameState)

| Field | Type | Ownership | Notes |
|-------|------|-----------|-------|
| `current_map_id` | string | Authoritative (persistent) | Saved to disk |
| `x` | int32_t | Authoritative (persistent) | Snapshot of PlayerState.x |
| `y` | int32_t | Authoritative (persistent) | Snapshot of PlayerState.y |
| `facing` | Direction | Authoritative (persistent) | Snapshot |
| `surfing` | bool | Authoritative (persistent) | Snapshot |
| `on_bike` | bool | Authoritative (persistent) | Snapshot |

### NpcState (in HeadlessGameLoop)

| Field | Type | Ownership | Notes |
|-------|------|-----------|-------|
| `id` | uint16_t | Immutable | Object local ID from map |
| `x` | int32_t | Authoritative (simulation) | Current tile position |
| `y` | int32_t | Authoritative (simulation) | Current tile position |
| `facing` | Direction | Authoritative (simulation) | Current facing |
| `is_moving` | bool | Transient | True during 16-frame step |
| `idle_timer` | int32_t | Authoritative (simulation) | Frames until next movement attempt - CRITICAL for determinism |
| `target_x/y` | int32_t | Transient | Movement destination during step |
| `move_progress` | int32_t | Transient | Step progress counter |
| `frozen` | bool | Transient | Script interaction lock |
| `behavior` | enum | Immutable | Movement type from map |
| `radius_x/y` | int8_t | Immutable | Movement bounds from map |
| `init_x/y` | int32_t | Immutable | Home position from map |
| `script_id` | string | Immutable | From map |
| `visible` | bool | Authoritative | Can change via flags |

### NpcSaveState (in GameState)

| Field | Type | Ownership | Notes |
|-------|------|-----------|-------|
| `id` | uint16_t | Key | Match to NpcState |
| `x` | int32_t | Authoritative (persistent) | Snapshot of NpcState.x |
| `y` | int32_t | Authoritative (persistent) | Snapshot of NpcState.y |
| `facing` | Direction | Authoritative (persistent) | Snapshot |
| `is_moving` | bool | Authoritative (persistent) | Mid-step state |
| `idle_timer` | int32_t | Authoritative (persistent) | CRITICAL - preserves determinism |
| `target_x/y` | int32_t | Authoritative (persistent) | Movement destination |
| `move_progress` | int32_t | Authoritative (persistent) | Step progress |
| `frozen` | bool | Authoritative (persistent) | Script lock state |
| `visible` | bool | Authoritative (persistent) | Visibility state |

### GameState Fields

| Field | Type | Ownership | Notes |
|-------|------|-----------|-------|
| `player` | PlayerSaveState | Authoritative (persistent) | Saveable player state |
| `warp_memory` | WarpMemory | Authoritative (persistent) | LAST_MAP/LAST_WARP |
| `flags` | unordered_set | Authoritative (persistent) | Event flags |
| `variables` | unordered_map | Authoritative (persistent) | Script variables |
| `rng` | RngState | Authoritative (persistent) | Determinism source |
| `npc_states` | map<map_id, vec> | Authoritative (persistent) | Per-map NPC snapshots |
| `playtime_frames` | uint64_t | Authoritative (persistent) | Play time counter |

### Runtime Presentation State (in main_tiles.cpp)

| Field | Type | Ownership | Notes |
|-------|------|-----------|-------|
| `visual_player_x/y` | float | Derived | Interpolated from PlayerState for smooth rendering |
| `render_facing` | Direction | Derived | Copied from PlayerState each frame |
| `animation_phase` | int | Derived | Walk animation frame |
| `WarpArrivalState` | struct | Transient | Controls warp trigger timing |

## Ownership Transfer Protocol

### Save (Snapshot)

```
HeadlessGameLoop::PlayerState → GameState::PlayerSaveState
HeadlessGameLoop::NpcState[] → GameState::npc_states[map_id]
```

Implementation: `HeadlessGameLoop::snapshot_npc_states(map_id)`

### Load (Restore)

```
GameState::PlayerSaveState → HeadlessGameLoop::PlayerState (spawn_player)
GameState::npc_states[map_id] → HeadlessGameLoop::NpcState[] (restore_npc_states)
```

Implementation: `HeadlessGameLoop::restore_npc_states(map_id)`

## Key Invariants

1. **Single Source of Truth During Simulation**
   - `HeadlessGameLoop::PlayerState` is authoritative during gameplay
   - `GameState::player` is a snapshot for persistence only

2. **Snapshot/Restore Symmetry**
   - `snapshot_npc_states()` must capture ALL gameplay-relevant NPC state
   - `restore_npc_states()` must restore ALL captured state
   - Missing fields would cause non-determinism

3. **RNG Ownership**
   - `GameState::rng` is the ONLY authoritative RNG
   - `HeadlessGameLoop::next_random()` REQUIRES GameState
   - No fallback RNG exists - this is intentional

4. **Instance Isolation**
   - Each HeadlessGameLoop owns its own transient state
   - Multiple instances (Runtime A, Runtime B) can coexist
   - No global mutable state

5. **Renderer Independence**
   - Presentation state (interpolation, animation) is derived each frame
   - Never writes back to simulation state
   - Renderer can run at different rate than simulation

## Verification Evidence

### No Divergent Copies

| Concept | HeadlessGameLoop Field | GameState Field | Conflict? |
|---------|----------------------|-----------------|-----------|
| Player position | `PlayerState.x/y` | `player.x/y` | NO - GameState is snapshot only |
| Player facing | `PlayerState.facing` | `player.facing` | NO - GameState is snapshot only |
| NPC position | `NpcState.x/y` | `npc_states[].x/y` | NO - snapshot/restore protocol |
| NPC idle timer | `NpcState.idle_timer` | `npc_states[].idle_timer` | NO - correctly snapshotted |
| RNG state | (uses GameState) | `rng.state` | NO - single source |

### Code Evidence

1. `HeadlessGameLoop::set_game_state(GameState*)` - takes pointer, doesn't own
2. `snapshot_npc_states()` explicitly copies ALL NPC fields including `idle_timer`
3. `restore_npc_states()` restores ALL captured fields
4. `next_random()` throws if GameState is null - no fallback

## Conclusion

**VERIFIED CLEAN** - No refactoring needed.

The state ownership model is correct:
- Clear separation between simulation (HeadlessGameLoop) and persistence (GameState)
- Explicit snapshot/restore protocol for NPC state
- No hidden state duplication
- RNG properly owned by GameState with no fallback
