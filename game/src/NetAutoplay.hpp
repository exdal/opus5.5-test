#pragma once

#include <string>
#include <vector>

#include "World.hpp"

namespace oxcity {
class NetSession;

// Scripted multiplayer test (`--net-autoplay ROLE`), one role per process, run together by tools/run_net_test.sh:
//   host     a listen server that stands still and watches the remote players do things
//   shooter  a client: shoots a pedestrian, hunts down the nearest other player, then steals a car and drives
//   target   a client: walks a few steps, then waits to get wasted and checks it respawns
// Each prints a PASS/FAIL checklist when its frames run out.
class NetAutoplay {
public:
  std::string role = {};
  std::string pending_screenshot = {};

  auto update(this NetAutoplay& self, World& world, const NetSession& net, f32 dt) -> GameInput;
  auto report(this const NetAutoplay& self, const World& world, const NetSession& net) -> bool;

private:
  f32 time = 0.0f;
  f32 phase_time = 0.0f;
  i32 phase = 0;
  i32 shots_taken = 0;
  f32 last_tap = -10.0f;

  // what happened, for the report
  bool joined = false;
  bool saw_other_player = false;
  bool killed_ped = false;
  bool wasted_player = false;
  bool got_wasted = false;
  bool respawned = false;
  f32 respawn_shot_at = 0.0f;
  bool stole_car = false;
  f32 distance_driven = 0.0f;
  glm::vec2 last_car_position = {};
  // host side
  glm::vec2 remote_start = {};
  bool remote_seen = false;
  f32 remote_moved = 0.0f;
  i32 remote_ped_kills = 0;
  i32 remote_player_kills = 0;

  auto screenshot(this NetAutoplay& self, std::string_view name) -> void;
  auto tap(this NetAutoplay& self) -> bool;
  auto next_phase(this NetAutoplay& self, std::string_view what) -> void;
};
} // namespace oxcity
