#include <polyhook2/Detour/x64Detour.hpp>

#include "arcsys.h"
#include "bind_watcher.h"
#include "rewind.h"
#include "sigscan.h"

#include <DynamicOutput/DynamicOutput.hpp>
#include <Mod/CppUserModBase.hpp>
#include <UnrealDef.hpp>

#define WIN32_LEAN_AND_MEAN
#include "Windows.h"

#define STRIVEREPLAYREWIND_API __declspec(dllexport)

/* Definitions */

using funcMatchStart_t = void (*)(AREDGameState_Battle *);
using funcUpdateBattle_t = void (*)(AREDGameState_Battle *, float);
using funcUpdateBattleSub_t = void (*)(AREDGameState_Battle *, float, bool);

void hook_MatchStart(AREDGameState_Battle *);
void hook_UpdateBattle(AREDGameState_Battle *, float);
void hook_UpdateBattleSub(AREDGameState_Battle *, float, bool);

funcMatchStart_t orig_MatchStart;
funcUpdateBattle_t orig_UpdateBattle;
funcUpdateBattleSub_t orig_UpdateBattleSub;

/* Helpers */

// rewind only ever runs on offline replay playback
bool isReplayPlayback(const AREDGameState_Battle *GameState) {
  return getGameMode() == GAME_MODE_REPLAY && !GameState->bNetworkBattle;
}

// true on the tick a round resets or starts; snapshots from before it are stale
bool roundResetting() {
  auto *events = asw_events::get();
  auto count = events->event_count;
  if (count > 10) count = 10;
  for (unsigned int idx = 0; idx < count; ++idx) {
    auto e_type = events->events[idx].type;
    if (e_type == BOM_EVENT_RESET || e_type == BOM_EVENT_DECISION || e_type == BOM_EVENT_BATTLE_START) return true;
  }
  return false;
}

class Keybinds {
public:
  static const int BUTTON_COUNT = 3;

  int REWIND_FRAME_BUTTON = VK_F6; // back one frame
  int REWIND_SEEK_BUTTON = VK_F7;  // back REWIND_SEEK_FRAMES
  int FORWARD_SEEK_BUTTON = VK_F8; // forward REWIND_SEEK_FRAMES

  int buttons[BUTTON_COUNT] = {REWIND_FRAME_BUTTON, REWIND_SEEK_BUTTON, FORWARD_SEEK_BUTTON};

  int getButtonIndex(int input) {
    for (int i = 0; i < BUTTON_COUNT; i++) {
      if (buttons[i] == input) return i;
    }
    return -1;
  }

  void setButtonState(int input, bool val) {
    int index = getButtonIndex(input);
    if (index != -1) buttonStates[index] = val;
  }

  bool getButtonState(int input) {
    int index = getButtonIndex(input);
    return index != -1 && buttonStates[index];
  }

  void resetButtons() {
    for (int i = 0; i < BUTTON_COUNT; i++) buttonStates[i] = false;
  }

  void loadButtons() {
    for (int i = 0; i < BUTTON_COUNT; i++) BindWatcherI::addToFilter(buttons[i]);
  }

  void checkBinds() {
    for (const auto &input : BindWatcherI::getInputs(false)) setButtonState(input, true);
  }

private:
  bool buttonStates[BUTTON_COUNT] = {};
} keybindings;

ReplayRewind replay_rewind;
constexpr int REWIND_SEEK_FRAMES = 3 * 60; // fixed 3 s

/* Hooks */

void hook_MatchStart(AREDGameState_Battle *GameState) {
  // MatchStart fires several times while a replay loads; buffers are kept per battle (rebuilt when the GameState
  // changes), only the snapshots are dropped here
  replay_rewind.invalidate();

  orig_MatchStart(GameState);
}

/*
  Pausing is left entirely to the game's replay controls (CReplayManager). When the replay is paused and the mod
  needs a frame simulated (to draw a rewound frame, or to run a forward-jump chunk), it sets the game's own
  StepPlay flag after UpdateBattle, so the next tick advances exactly like pressing the replay step button.
*/
void hook_UpdateBattle(AREDGameState_Battle *GameState, float DeltaTime) {
  auto *replay = GameState->ReplayManager;
  if (!replay || !isReplayPlayback(GameState)) {
    orig_UpdateBattle(GameState, DeltaTime);
    return;
  }

  // whether the engine simulates this tick; must be read before orig_UpdateBattle (ReplayManager::Update clears StepPlay)
  const bool simulating = !replay->holds_pause();

  // this still runs while the engine is paused (the battle game state ticks when paused)
  keybindings.checkBinds();
  // Runs before orig_UpdateBattle: while playing, this tick simulates the target frame anyway; while paused, the
  // target frame is drawn on the next tick through StepPlay.
  int rewind_frames = 0;
  if (keybindings.getButtonState(keybindings.REWIND_FRAME_BUTTON)) rewind_frames = 1;
  if (keybindings.getButtonState(keybindings.REWIND_SEEK_BUTTON)) rewind_frames = REWIND_SEEK_FRAMES;
  const bool rewound = rewind_frames && replay_rewind.stepBack(rewind_frames);
  if (keybindings.getButtonState(keybindings.FORWARD_SEEK_BUTTON)) replay_rewind.requestForward(REWIND_SEEK_FRAMES);
  keybindings.resetButtons();

  if (roundResetting()) replay_rewind.invalidate();

  // forward-jump chunks only run on ticks that simulate; on held ticks the extra frames would be skipped
  const bool forward_tick = simulating && replay_rewind.forwardPending();
  if (forward_tick) replay_rewind.beginForwardTick(GameState);
  orig_UpdateBattle(GameState, DeltaTime);
  if (forward_tick) replay_rewind.endForwardTick(GameState);

  // Update() has run by now, so this StepPlay survives into the next tick's UpdatePauseState
  if (replay->Pause && (rewound || replay_rewind.forwardPending())) replay->StepPlay = true;
}

void hook_UpdateBattleSub(AREDGameState_Battle *GameState, float DeltaTime, bool bSkip) {
  orig_UpdateBattleSub(GameState, DeltaTime, bSkip);

  // snapshot every simulated replay frame (no-op when GameFrame did not advance)
  if (isReplayPlayback(GameState)) replay_rewind.afterUpdate(GameState);
}

/* Mod Definition */

class StriveReplayRewind : public CppUserModBase {
public:
  PLH::x64Detour *UpdateBattle_Detour = nullptr;
  PLH::x64Detour *UpdateBattleSub_Detour = nullptr;
  PLH::x64Detour *MatchStart_Detour = nullptr;

  StriveReplayRewind()
  : CppUserModBase() {
    ModName = STR("Strive Replay Rewind");
    ModVersion = STR("0.1");
    ModDescription = STR("Rewind and seek through replays.");
    ModAuthors = STR("pbozai, Sevoi");

    Output::send<LogLevel::Verbose>(STR("Strive Replay Rewind Started\n"));
  }

  ~StriveReplayRewind() override {}

  auto on_update() -> void override {}

  auto on_unreal_init() -> void override {
    const uint64_t UpdateBattle_Addr = sigscan::get().scan("\x40\x53\x57\x41\x54\x41\x55\x48\x81\xEC\x88\x00\x00\x00", "xxxxxxxxxxxxxx");
    UpdateBattle_Detour = new PLH::x64Detour(UpdateBattle_Addr, reinterpret_cast<uint64_t>(&hook_UpdateBattle), reinterpret_cast<uint64_t *>(&orig_UpdateBattle));
    UpdateBattle_Detour->hook();

    // AREDGameState_Battle::UpdateBattleSub (RVA 0x112B6B0), called from UpdateBattle
    const uint64_t UpdateBattleSub_Addr = sigscan::get().scan(
        "\x48\x8B\xC4\x48\x89\x58\x20\x41\x56\x48\x81\xEC\xA0\x00\x00\x00\x48\x89\x68\x08\x41\x0F\xB6\xE8\x48\x89\x78\x18\x48\x8B\xF9\x0F\x29\x70\xE8\x0F\x28\xF1",
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
    UpdateBattleSub_Detour = new PLH::x64Detour(UpdateBattleSub_Addr, reinterpret_cast<uint64_t>(&hook_UpdateBattleSub), reinterpret_cast<uint64_t *>(&orig_UpdateBattleSub));
    UpdateBattleSub_Detour->hook();

    // https://github.com/TheLettuceClub/StriveSAMMI/blob/7292cc225bd4de2b389e92b227c1b2793609c4e1/GGSTSammi/src/dllmain.cpp#L435
    const uint64_t MatchStart_Addr = sigscan::get().scan("\x48\x89\x5c\x24\x10\x55\x56\x57\x41\x54\x41\x55\x41\x56\x41\x57\x48\x8d\x6c\x24\xf0\x48\x81\xec\x10\x01\x00\x00\x0f\x29\xb4\x24", "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
    MatchStart_Detour = new PLH::x64Detour(MatchStart_Addr, reinterpret_cast<uint64_t>(&hook_MatchStart), reinterpret_cast<uint64_t *>(&orig_MatchStart));
    MatchStart_Detour->hook();

    ASWInitFunctions();
    keybindings.loadButtons();
  }
};

extern "C" {
STRIVEREPLAYREWIND_API CppUserModBase *start_mod() {
  return new StriveReplayRewind();
}

STRIVEREPLAYREWIND_API void uninstall_mod(CppUserModBase *mod) {
  delete mod;
}
}
