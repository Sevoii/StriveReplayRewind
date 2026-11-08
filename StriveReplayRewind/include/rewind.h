#pragma once

#include "arcsys.h"

/*
  Replay rewind, proof of concept: jump back in a replay, paused or playing, up to 5 seconds.

  The mod owns BATTLE_CRollbackManager instances (asw_rollback::create_owned) and snapshots the battle into them,
  together with the replay key-stream read heads. A snapshot is ~2.7 MB, so there are two tiers:
    dense:  every frame for the last DENSE_FRAMES (single-frame steps)
    sparse: one keyframe every SPARSE_INTERVAL frames, covering 5 s (longer jumps land on a keyframe)

  Going back restores a snapshot from before the target and lets one frame simulate, so the engine draws that frame
  with live visuals (and re-reads its replay inputs from the restored read heads). While playing, the same tick
  simulates that frame anyway; while paused the caller lets one frame through.

  Engine side effects: Rollback() runs the object manager's PreRollBack/PostRollBack, which use the GAME's rollback
  manager (red::btl::GetRollbackManager()) for particle/pawn/audio cleanup. For the duration of the restore its
  RollbackFrame is set to the real distance, then its particle/pawn caches are cleared, as netplay does after a
  resimulation. Its snapshot slots and the replay key buffer are never written.
*/
class ReplayRewind {
public:
  static constexpr int DENSE_FRAMES = 30;                 // 0.5 s of every frame
  static constexpr int SPARSE_INTERVAL = 30;              // keyframe every 0.5 s
  static constexpr int SPARSE_SLOTS = 5 * 60 / SPARSE_INTERVAL + 1; // 5 s of keyframes
  static constexpr int FORWARD_CHUNK = 30;                // frames simulated per tick while jumping forward

  // drop the snapshot buffers (frees memory)
  void reset();
  // snapshots are only valid within one round
  void invalidate();
  // after each UpdateBattleSub in replay mode; saves when GameFrame advanced
  void afterUpdate(class AREDGameState_Battle *GameState);
  // go back at least `frames` frames (or as far as the buffers allow). Returns true on success; while paused the
  // caller must let one frame simulate to draw the target.
  bool stepBack(int frames);

  // Jump forward by fast-simulating `frames` frames over the next few ticks, FORWARD_CHUNK per tick, through
  // UpdateBattle's own multi-frame loop (the one replay fast-forward uses; only the last frame is drawn).
  void requestForward(int frames);
  bool forwardPending() const { return forward_remaining > 0; }
  // around orig_UpdateBattle on a tick that is allowed to simulate: arms the extra frames, then puts the game's
  // rollback manager fields back
  void beginForwardTick(AREDGameState_Battle *GameState);
  void endForwardTick(AREDGameState_Battle *GameState);

private:
  struct FrameMeta {
    bool valid = false;
    int game_frame = 0;
    replay_head heads[replay_peer::COUNT] = {};
  };

  struct Ring {
    asw_rollback *instance = nullptr;
    FrameMeta *meta = nullptr;
    int size = 0;

    bool create(int slots);
    void destroy();
    void invalidate();
    int current() const { return instance->CurrentBackupDataIndex; }
    int indexBack(int k) const { return (current() + size - k) % size; }
    bool save(const FrameMeta &m);
    // restore the slot k entries back; `frames_back` is the real frame distance (for the engine's cleanup)
    bool restore(int k, int frames_back);
  };

  bool ensureInstances(AREDGameState_Battle *GameState);
  void disable(const wchar_t *reason);
  void applyRestored(const FrameMeta &m);

  Ring dense, sparse;
  AREDGameState_Battle *owner = nullptr; // buffers are sized for this battle; rebuilt when it changes
  int last_frame = -1;
  bool disabled = false;
  int mismatch_logs = 0;

  int forward_remaining = 0;
  int forward_target = -1; // GameFrame the jump ends on; dense saves are skipped until the last DENSE_FRAMES
  struct {
    bool armed = false;
    int rollback_frame = 0;
    bool fast_update_finish = false;
    int remaining_fast_forward = 0;
  } forward_saved;
};
