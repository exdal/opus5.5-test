#pragma once

#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <tuple>

#include "Autoplay.hpp"
#include "World.hpp"

namespace ox {
class AssetManager;
class AudioEngine;
class Physics;
class Input;
class Renderer;
class RmlUI;
class Timestep;
} // namespace ox

namespace oxcity {
struct GameOptions {
  bool autoplay = false;
  i32 frame_limit = 0; // quit after this many frames, 0 runs forever
  std::filesystem::path screenshot_dir = {};
  u32 seed = 1999;
  f32 fixed_dt = 0.0f; // non zero steps the game with a fixed delta, for deterministic headless runs
  i32 autoplay_from = 0; // skip the autoplay ahead to this step once the game has started
};

// The whole game as one engine module: `App` calls `update` once per frame, which runs the simulation, the
// scene and the frame submission. Registered after the default modules in Main.cpp.
class Game {
public:
  constexpr static auto MODULE_NAME = "OxCity";
  using module_dependencies = std::tuple<ox::AssetManager, ox::AudioEngine, ox::Physics, ox::Input, ox::Renderer, ox::RmlUI>;

  explicit Game(GameOptions options_);

  auto init(this Game& self) -> std::expected<void, std::string>;
  auto deinit(this Game& self) -> std::expected<void, std::string>;
  auto update(this Game& self, const ox::Timestep& timestep) -> void;

private:
  GameOptions options = {};
  std::unique_ptr<World> world = nullptr;
  Autoplay autoplay = {};
  u64 frame = 0;

  auto read_input(this Game& self) -> GameInput;
  auto render_frame(this Game& self) -> void;
  auto capture_screenshot(this Game& self, const std::filesystem::path& path) -> bool;
};
} // namespace oxcity
