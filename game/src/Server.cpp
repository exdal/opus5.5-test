#include "Server.hpp"

#include <glm/common.hpp>

#include "Core/App.hpp"
#include "Utils/Log.hpp"
#include "Utils/Timestep.hpp"

namespace oxcity {
Server::Server(GameOptions options_) : options(std::move(options_)) {}

auto Server::init(this Server& self) -> std::expected<void, std::string> {
  self.world = std::make_unique<World>(self.options.seed);
  self.world->headless = true;
  self.world->local = PlayerID::Invalid;
  if (!self.world->init()) {
    return std::unexpected("OxCity server: couldn't build the city, see the log above");
  }
  const auto port = self.options.host_port != 0 ? self.options.host_port : net::DEFAULT_PORT;
  if (!self.net.host(*self.world, port, true)) {
    return std::unexpected(fmt::format("OxCity server: {}", self.net.status_text));
  }
  self.world->set_state(GameState::Playing);
  OX_LOG_INFO("OxCity server: up, {} cars and {} peds in the city", self.world->cars.size(), self.world->peds.size());
  return {};
}

auto Server::deinit(this Server& self) -> std::expected<void, std::string> {
  self.net.leave();
  self.world.reset();
  return {};
}

auto Server::update(this Server& self, const ox::Timestep& timestep) -> void {
  ZoneScoped;

  // App::with_frame_limit paces the loop; the simulation still gets a clamped delta like the game does
  const auto dt = self.options.fixed_dt > 0.0f ? self.options.fixed_dt
                                              : glm::clamp(static_cast<f32>(timestep.get_seconds()), 0.001f, 1.0f / 15.0f);
  self.net.poll(*self.world, timestep);
  self.world->update(GameInput{}, dt);
  self.net.flush(*self.world);

  self.report_timer += dt;
  if (self.report_timer > 10.0f) {
    self.report_timer = 0.0f;
    auto players = std::string{};
    for (auto id : ALL_PLAYERS) {
      const auto& p = self.world->pl(id);
      if (p.active) {
        players += fmt::format(" [{}: {} pts, {} kills, {} stars]", p.name, p.score, p.kills, self.world->stars(id));
      }
    }
    OX_LOG_INFO(
      "OxCity server: t={:.0f}s, {} snapshots sent (largest {} B){}",
      self.world->time,
      self.net.snapshots_sent,
      self.net.largest_snapshot,
      players.empty() ? " nobody online" : players
    );
  }

  self.frame++;
  if (self.options.frame_limit > 0 && self.frame >= static_cast<u64>(self.options.frame_limit)) {
    ox::App::get()->should_stop();
  }
}
} // namespace oxcity
