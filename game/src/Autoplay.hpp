#pragma once

#include <string>
#include <vector>

#include "World.hpp"

namespace oxcity {
// Scripted play-through used for headless testing. Plays the game like a (very determined) player: starts from
// the menu, steals a car, drives, mugs someone, robs the bank and runs from the cops, then prints a checklist of
// what actually happened. Screenshots are requested at the interesting moments.
class Autoplay {
public:
  auto update(this Autoplay& self, World& world, f32 dt) -> GameInput;
  auto report(this const Autoplay& self, const World& world) -> bool;

  std::string pending_screenshot = {};

private:
  i32 step = 0;
  f32 step_time = 0.0f;
  f32 total_time = 0.0f;
  i32 shots_taken = 0;
  i32 cash_at_start = 0;
  i32 cash_after_mug = 0;
  i32 max_stars = 0;
  bool drove = false;
  bool stole_car = false;
  bool mugged = false;
  bool robbed_bank = false;
  bool cops_showed_up = false;
  bool entered_game = false;
  f32 distance_driven = 0.0f;
  glm::vec2 last_car_position = {};
  PedID victim = PedID::Invalid;
  CarID target_car = CarID::Invalid;
  NodeID drive_target = NodeID::Invalid;
  std::vector<std::string> log = {};

  auto next(this Autoplay& self, std::string_view what) -> void;
  auto screenshot(this Autoplay& self, std::string_view name) -> void;
  // a single key press, at most once a second, so one "press F" doesn't become three frames of F
  auto tap(this Autoplay& self) -> bool;
  f32 last_tap = -10.0f;
};
} // namespace oxcity
