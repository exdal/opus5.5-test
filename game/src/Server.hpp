#pragma once

#include <expected>
#include <memory>
#include <string>
#include <tuple>

#include "Game.hpp"
#include "Net.hpp"
#include "World.hpp"

namespace ox {
class AssetManager;
class Physics;
struct NetworkManager;
class Timestep;
} // namespace ox

namespace oxcity {
// `OxCity --server`: the whole city with nobody in front of it. No window, no renderer, no audio, no RmlUi; just
// the AssetManager (for the manifest), Jolt, and the NetworkManager. Registered instead of the Game module.
class Server {
public:
  constexpr static auto MODULE_NAME = "OxCityServer";
  using module_dependencies = std::tuple<ox::AssetManager, ox::Physics, ox::NetworkManager>;

  explicit Server(GameOptions options_);

  auto init(this Server& self) -> std::expected<void, std::string>;
  auto deinit(this Server& self) -> std::expected<void, std::string>;
  auto update(this Server& self, const ox::Timestep& timestep) -> void;

private:
  GameOptions options = {};
  std::unique_ptr<World> world = nullptr;
  NetSession net = {};
  u64 frame = 0;
  f32 report_timer = 0.0f;
};
} // namespace oxcity
