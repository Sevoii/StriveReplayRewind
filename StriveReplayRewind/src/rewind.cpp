#include "rewind.h"

#include <DynamicOutput/Output.hpp>
#include <UnrealDef.hpp>

#include <algorithm>

using RC::Output::send;
namespace LogLevel = RC::LogLevel;

/* Ring */

bool ReplayRewind::Ring::create(int slots) {
  instance = asw_rollback::create_owned(slots);
  if (!instance) return false;
  size = slots;
  meta = new FrameMeta[slots];
  return true;
}

void ReplayRewind::Ring::destroy() {
  asw_rollback::destroy_owned(instance);
  instance = nullptr;
  delete[] meta;
  meta = nullptr;
  size = 0;
}

void ReplayRewind::Ring::invalidate() {
  for (int i = 0; i < size; ++i) meta[i].valid = false;
}

bool ReplayRewind::Ring::save(const FrameMeta &m) {
  // SaveBackup has a per-tick save cap that the engine only resets on the network path; keep it from latching
  instance->InputSameFrames = 0;
  instance->HalfFrames = 0;
  instance->SavedFrameCountThisFrame = 0;
  instance->bSaveLimitReached = false;
  instance->save_backup();

  const int index = current();
  if (!instance->slot_set(index)) return false;
  meta[index] = m;
  meta[index].valid = true;
  return true;
}

bool ReplayRewind::Ring::restore(int k, int frames_back) {
  const int target = indexBack(k);
  int newer[64];
  const int newer_count = (std::min)(k, 64);
  for (int j = 0; j < newer_count; ++j) newer[j] = indexBack(j);

  // PreRollBack/PostRollBack read the GAME's RollbackFrame to age particles/audio and to decide which effect pawns
  // were spawned inside the rolled-back window. Offline it is 0, which leaves effects from after the target attached
  // or orphaned and re-links pawns to the wrong objects. Give it the real distance for the duration of the restore.
  // This runs inside UpdateBattle, after UpdateOnPreTick has already checked the field this tick.
  auto *game = asw_rollback::get();
  int game_rollback_frame = game ? game->RollbackFrame : 0; // non-const: FIELD setters take T&
  if (game) game->RollbackFrame = frames_back;

  instance->RollbackFrame = k; // Rollback() restores slot Current - RollbackFrame
  instance->IsRollBackFinish = false;
  const bool ok = instance->rollback();
  instance->RollbackFrame = 0;
  instance->RemainingFastForwardFrames = 0;
  instance->IsRollBackFinish = false;

  if (game) {
    game->RollbackFrame = game_rollback_frame;
    // Netplay clears these after resimulating to drop what the resimulation did not reclaim. We skip the
    // intermediate frames, so nothing will reclaim them: clear now.
    game->clear_psc_cache();
    game->clear_pawn_cache();
  }

  if (!ok || current() != target) return false;
  for (int j = 0; j < newer_count; ++j) meta[newer[j]].valid = false; // Rollback() unset those slots
  return true;
}

/* ReplayRewind */

void ReplayRewind::reset() {
  dense.destroy();
  sparse.destroy();
  owner = nullptr;
  last_frame = -1;
}

void ReplayRewind::invalidate() {
  if (dense.instance) dense.invalidate();
  if (sparse.instance) sparse.invalidate();
  last_frame = -1;
  forward_remaining = 0;
  forward_target = -1;
}

void ReplayRewind::disable(const wchar_t *reason) {
  send<LogLevel::Warning>(STR("[rewind] disabled: {}\n"), reason);
  disabled = true;
  reset();
}

bool ReplayRewind::ensureInstances(AREDGameState_Battle *GameState) {
  // buffer sizes come from this battle's object manager; a new battle means new buffers
  if (owner && owner != GameState) reset();
  if (owner) return true;
  if (disabled || !asw_rollback::owned_available() || !replay_peer::get(0)) return false;

  if (!dense.create(DENSE_FRAMES) || !sparse.create(SPARSE_SLOTS)) {
    disable(STR("could not create rollback managers"));
    return false;
  }
  for (auto *ring : {&dense, &sparse}) {
    auto *inst = ring->instance;
    // sanity check the constructor did what the source says before trusting the layout
    if (inst->MaxBackupFrame != ring->size || inst->ObjVectorBufferSize == 0 || inst->CharVectorBufferSize == 0) {
      send<LogLevel::Warning>(STR("[rewind] unexpected manager layout: max {} obj {} char {}\n"), inst->MaxBackupFrame,
                              inst->ObjVectorBufferSize, inst->CharVectorBufferSize);
      disable(STR("rollback manager layout mismatch"));
      return false;
    }
  }
  owner = GameState;
  last_frame = -1;

  const auto per_slot = asw_rollback::SLOT_SIZE + 0x1848 + dense.instance->ObjVectorBufferSize + dense.instance->CharVectorBufferSize;
  send<LogLevel::Verbose>(STR("[rewind] managers created: {} dense + {} sparse slots, ~{} KB per slot, ~{} MB total\n"),
                          DENSE_FRAMES, SPARSE_SLOTS, per_slot / 1024, per_slot * (DENSE_FRAMES + SPARSE_SLOTS) / (1024 * 1024));
  return true;
}

void ReplayRewind::afterUpdate(AREDGameState_Battle *GameState) {
  auto *engine = asw_engine::get();
  if (!engine || !ensureInstances(GameState)) return;

  const int frame = engine->GameFrame;
  if (frame == last_frame) return;      // paused / nothing simulated
  if (frame < last_frame) invalidate(); // went backwards without us (round reset); old snapshots are stale

  FrameMeta next;
  next.game_frame = frame;
  int reading = 0;
  const auto &prev = dense.meta[dense.current()];
  for (int i = 0; i < replay_peer::COUNT; ++i) {
    const auto *peer = replay_peer::get(i);
    next.heads[i] = peer->ReadHead;
    if (!peer->is_reading()) continue;
    ++reading;
    // unused team slots sit in read mode without advancing; only check streams that move
    if (prev.valid && mismatch_logs < 20) {
      const int moved = static_cast<int>(next.heads[i].solid_position - prev.heads[i].solid_position);
      if (moved != 0 && moved != frame - prev.game_frame) {
        ++mismatch_logs;
        send<LogLevel::Warning>(STR("[rewind] peer {} read head moved {} over {} frames (frame {})\n"), i, moved,
                                frame - prev.game_frame, frame);
      }
    }
  }
  if (reading == 0) {
    disable(STR("no replay key stream is being read"));
    return;
  }

  // while jumping forward, only the last DENSE_FRAMES frames can end up in the dense ring anyway
  const bool skip_dense = forward_target >= 0 && frame < forward_target - DENSE_FRAMES;
  if (forward_target >= 0 && frame >= forward_target) forward_target = -1;
  if (!skip_dense && !dense.save(next)) {
    disable(STR("SaveBackup did not fill its slot"));
    return;
  }
  const auto &key = sparse.meta[sparse.current()];
  if (!key.valid || frame - key.game_frame >= SPARSE_INTERVAL) {
    if (!sparse.save(next)) {
      disable(STR("SaveBackup did not fill its slot"));
      return;
    }
  }
  last_frame = frame;
}

void ReplayRewind::applyRestored(const FrameMeta &m) {
  // put the replay readers back where they were at that frame
  for (int i = 0; i < replay_peer::COUNT; ++i) {
    replay_head head = m.heads[i]; // copy: FIELD setters take T&
    replay_peer::get(i)->ReadHead = head;
  }

  // Anything newer than the restored frame is in the future now. For each ring, drop those entries and move its
  // write position back to its newest remaining entry, so new saves continue from there (SaveBackup only does
  // Current = (Current + 1) % Max; the stale slots get overwritten).
  for (auto *ring : {&dense, &sparse}) {
    int newest = -1;
    for (int i = 0; i < ring->size; ++i) {
      auto &e = ring->meta[i];
      if (!e.valid) continue;
      if (e.game_frame > m.game_frame) e.valid = false;
      else if (newest < 0 || e.game_frame > ring->meta[newest].game_frame) newest = i;
    }
    if (newest >= 0) ring->instance->CurrentBackupDataIndex = newest;
  }

  auto *engine = asw_engine::get();
  if (engine && engine->GameFrame != m.game_frame) {
    send<LogLevel::Warning>(STR("[rewind] GameFrame after restore is {}, expected {}\n"), engine->GameFrame, m.game_frame);
  }
  last_frame = m.game_frame;
  // a rewind cancels a forward jump in progress
  forward_remaining = 0;
  forward_target = -1;
}

bool ReplayRewind::stepBack(int frames) {
  if (!owner || frames <= 0 || last_frame < 0) return false;

  // Restoring snapshot S and simulating one frame shows S + 1, so the ideal snapshot is current - frames - 1.
  const int now = last_frame;
  const int ideal = now - frames - 1;

  // dense: exact frame, if it is still within the last DENSE_FRAMES
  int dense_k = 0;
  const auto &dense_cur = dense.meta[dense.current()];
  if (dense_cur.valid && dense_cur.game_frame == now) {
    for (int k = (std::min)(frames + 1, dense.size - 1); k >= 2; --k) {
      const auto &m = dense.meta[dense.indexBack(k)];
      if (m.valid && m.game_frame == now - k) {
        dense_k = k;
        break;
      }
    }
  }
  if (dense_k && now - dense_k == ideal) {
    const auto m = dense.meta[dense.indexBack(dense_k)];
    if (!dense.restore(dense_k, now - m.game_frame)) {
      disable(STR("Rollback did not restore the expected slot"));
      return false;
    }
    applyRestored(m);
    return true;
  }

  // sparse: newest keyframe at or before the ideal frame; otherwise the oldest one (as far back as possible).
  // k = 0 (the ring's current slot) can't be restored through Rollback(), skip it.
  int sparse_k = 0;
  int best = -1, oldest = -1;
  for (int k = 1; k < sparse.size; ++k) {
    const auto &m = sparse.meta[sparse.indexBack(k)];
    if (!m.valid || m.game_frame > now - 2) continue;
    if (m.game_frame <= ideal && (best < 0 || m.game_frame > sparse.meta[sparse.indexBack(best)].game_frame)) best = k;
    if (oldest < 0 || m.game_frame < sparse.meta[sparse.indexBack(oldest)].game_frame) oldest = k;
  }
  sparse_k = best > 0 ? best : oldest;

  // use the keyframe if it reaches the ideal frame, or reaches further back than the dense ring can
  const bool use_sparse =
      sparse_k > 0 && (best > 0 || dense_k == 0 || sparse.meta[sparse.indexBack(sparse_k)].game_frame < now - dense_k);
  if (use_sparse) {
    const auto m = sparse.meta[sparse.indexBack(sparse_k)];
    if (!sparse.restore(sparse_k, now - m.game_frame)) {
      disable(STR("Rollback did not restore the expected slot"));
      return false;
    }
    applyRestored(m);
    return true;
  }
  if (dense_k) {
    const auto m = dense.meta[dense.indexBack(dense_k)];
    if (!dense.restore(dense_k, now - m.game_frame)) {
      disable(STR("Rollback did not restore the expected slot"));
      return false;
    }
    applyRestored(m);
    return true;
  }

  send<LogLevel::Verbose>(STR("[rewind] nothing to step back to\n"));
  return false;
}

void ReplayRewind::requestForward(int frames) {
  if (!owner || frames <= 0 || last_frame < 0) return;
  forward_remaining = frames;
  forward_target = last_frame + frames;
}

void ReplayRewind::beginForwardTick(AREDGameState_Battle *GameState) {
  forward_saved.armed = false;
  auto *game = GameState->Rollback;
  if (forward_remaining <= 0 || !game) return;

  const int chunk = (std::min)(forward_remaining, FORWARD_CHUNK);
  forward_remaining -= chunk;
  if (chunk <= 1) return; // the tick's own frame covers it

  // UpdateBattle adds RollBackFrame to this tick's frame count when RollBackFrame > 0 && !IsFastUpdateFinish, then
  // sets IsFastUpdateFinish. Offline nothing else in the loop reads RollBackFrame, and UpdateOnPreTick (the only
  // place that would start a rollback from it) already ran this tick.
  forward_saved.armed = true;
  forward_saved.rollback_frame = game->RollbackFrame;
  forward_saved.fast_update_finish = game->IsFastUpdateFinish;
  forward_saved.remaining_fast_forward = game->RemainingFastForwardFrames;
  game->RollbackFrame = chunk - 1;
  game->IsFastUpdateFinish = false;
}

void ReplayRewind::endForwardTick(AREDGameState_Battle *GameState) {
  if (!forward_saved.armed) return;
  forward_saved.armed = false;
  auto *game = GameState->Rollback;
  if (!game) return;
  game->RollbackFrame = forward_saved.rollback_frame;
  game->IsFastUpdateFinish = forward_saved.fast_update_finish;
  game->RemainingFastForwardFrames = forward_saved.remaining_fast_forward;
  game->IsFastUpdateDoing = false;
}
