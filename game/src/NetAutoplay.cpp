#include "NetAutoplay.hpp"

#include <glm/common.hpp>
#include <glm/gtx/norm.hpp>

#include "Net.hpp"
#include "Utils/Log.hpp"

namespace oxcity {
// on foot movement towards a world point, in the screen space GameInput expects (y up = world -z)
static auto walk_towards(glm::vec2 from, glm::vec2 to) -> glm::vec2 {
  const auto d = to - from;
  if (glm::length2(d) < 0.0001f) {
    return {};
  }
  const auto n = glm::normalize(d);
  return {n.x, -n.y};
}

auto NetAutoplay::screenshot(this NetAutoplay& self, std::string_view name) -> void {
  self.shots_taken++;
  self.pending_screenshot = fmt::format("{}_{:02}_{}", self.role, self.shots_taken, name);
}

auto NetAutoplay::tap(this NetAutoplay& self) -> bool {
  if (self.time - self.last_tap < 1.0f) {
    return false;
  }
  self.last_tap = self.time;
  return true;
}

auto NetAutoplay::next_phase(this NetAutoplay& self, std::string_view what) -> void {
  OX_LOG_INFO("OxCity net autoplay ({}): [{:6.1f}s] phase {} done: {}", self.role, self.time, self.phase, what);
  self.phase++;
  self.phase_time = 0.0f;
}

// the closest other player that's in the game, Invalid if there's nobody
static auto other_player(const World& world) -> PlayerID {
  auto best = PlayerID::Invalid;
  auto best_d = std::numeric_limits<f32>::max();
  const auto me = world.player_position(world.local);
  for (auto id : ALL_PLAYERS) {
    if (id == world.local || !world.pl(id).active) {
      continue;
    }
    const auto d = glm::distance(world.player_position(id), me);
    if (d < best_d) {
      best_d = d;
      best = id;
    }
  }
  return best;
}

auto NetAutoplay::update(this NetAutoplay& self, World& world, const NetSession& net, f32 dt) -> GameInput {
  auto input = GameInput{};
  const auto before = self.phase_time;
  self.time += dt;
  self.phase_time += dt;
  auto at = [&](f32 t) { return before < t && self.phase_time >= t; };

  auto* me = world.local_player();
  if (!me || !me->active) {
    return input;
  }
  const auto pos = world.player_position(world.local);
  if (glm::floor(self.time / 5.0f) != glm::floor((self.time - dt) / 5.0f)) {
    OX_LOG_INFO(
      "OxCity net autoplay ({}): t={:.0f}s at ({:.1f}, {:.1f}) life {} car {} camera ({:.1f}, {:.1f})",
      self.role,
      self.time,
      pos.x,
      pos.y,
      static_cast<i32>(me->life),
      static_cast<i32>(me->car),
      world.camera_position.x,
      world.camera_position.z
    );
  }
  if (me->life != Life::Alive && !self.got_wasted && self.joined) {
    self.got_wasted = true;
    self.screenshot("wasted");
    OX_LOG_INFO("OxCity net autoplay ({}): got wasted ({})", self.role, me->life_note);
  }
  if (self.got_wasted && me->life == Life::Alive && !self.respawned) {
    self.respawned = true;
    self.respawn_shot_at = self.time + 1.5f; // once the camera has caught up
    OX_LOG_INFO("OxCity net autoplay ({}): back on the street", self.role);
  }
  if (self.respawn_shot_at > 0.0f && self.time >= self.respawn_shot_at) {
    self.respawn_shot_at = 0.0f;
    self.screenshot("back_on_street");
  }
  const auto other = other_player(world);
  self.saw_other_player = self.saw_other_player || other != PlayerID::Invalid;

  // --- host: stand still, watch the others ---
  if (self.role == "host") {
    self.joined = net.status == NetSession::Status::Hosting;
    if (other != PlayerID::Invalid) {
      const auto& o = world.pl(other);
      if (!self.remote_seen) {
        self.remote_seen = true;
        self.remote_start = world.player_position(other);
      }
      self.remote_moved = glm::max(self.remote_moved, glm::distance(world.player_position(other), self.remote_start));
      self.remote_ped_kills = glm::max(self.remote_ped_kills, o.stats.peds_killed);
      self.remote_player_kills = glm::max(self.remote_player_kills, o.kills);
      // face whoever came to visit, so the screenshots show them
      input.has_aim = true;
      input.aim = world.player_position(other);
      if (self.phase == 0 && self.phase_time > 4.0f) {
        self.screenshot("visitor");
        self.next_phase("a remote player is in the city");
      }
    }
    if (self.phase == 1 && self.phase_time > 12.0f) {
      self.screenshot("later");
      self.next_phase("still watching");
    }
    return input;
  }

  // --- clients: wait for the welcome ---
  if (world.role != NetRole::Client) {
    return input;
  }
  if (!self.joined) {
    self.joined = true;
    self.phase_time = 0.0f;
    OX_LOG_INFO("OxCity net autoplay ({}): joined as player {}", self.role, static_cast<i32>(world.local));
  }

  if (self.role == "target") {
    // a few steps so the host sees us move, then stand around and wait for what's coming
    if (self.phase == 0) {
      input.move = self.phase_time < 2.5f ? glm::vec2(1.0f, 0.0f) : glm::vec2(0.0f);
      if (self.phase_time > 3.0f) {
        self.screenshot("arrived");
        self.next_phase("walked a few steps");
      }
    }
    if (self.phase == 1 && self.respawned && self.phase_time > 1.0f) {
      self.screenshot("respawned");
      self.next_phase("wasted and respawned");
    }
    return input;
  }

  // --- shooter ---
  if (me->life != Life::Alive) {
    return input;
  }
  switch (self.phase) {
    case 0: { // pistol out, shoot the nearest pedestrian
      if (me->weapon != Weapon::Pistol) {
        input.switch_weapon = self.tap();
      }
      auto target = pos + glm::vec2(5.0f, 0.0f);
      auto best = std::numeric_limits<f32>::max();
      for (const auto& p : world.peds) {
        if (p.alive && p.kind == PedKind::Civilian && p.state != PedState::Driving && p.entity) {
          const auto d = glm::distance(p.position, pos);
          if (d < best) {
            best = d;
            target = p.position;
          }
        }
      }
      input.move = best > 10.0f ? walk_towards(pos, target) : glm::vec2(0.0f);
      input.has_aim = true;
      input.aim = target;
      input.attack = best < 14.0f && self.phase_time > 1.0f;
      if (at(2.0f)) {
        self.screenshot("shooting_ped");
      }
      if (me->stats.peds_killed > 0) {
        self.killed_ped = true;
        self.next_phase(fmt::format("the host says we killed {} pedestrian(s)", me->stats.peds_killed));
      } else if (self.phase_time > 25.0f) {
        self.next_phase("no pedestrian went down");
      }
      break;
    }
    case 1: { // hunt the other player
      if (other == PlayerID::Invalid) {
        if (self.phase_time > 20.0f) {
          self.next_phase("nobody else to shoot");
        }
        break;
      }
      const auto& o = world.pl(other);
      const auto target = world.player_position(other);
      const auto d = glm::distance(target, pos);
      input.move = d > 7.0f ? walk_towards(pos, target) : glm::vec2(0.0f);
      input.has_aim = true;
      input.aim = target;
      input.attack = d < 12.0f && o.life == Life::Alive;
      if (me->ammo == 0 && me->weapon != Weapon::Knife) {
        input.switch_weapon = self.tap();
      }
      if (at(1.5f)) {
        self.screenshot("pvp");
      }
      if (me->kills > 0) {
        self.wasted_player = true;
        self.screenshot("wasted_them");
        self.next_phase(fmt::format("wasted {}", o.name));
      } else if (self.phase_time > 40.0f) {
        self.next_phase("couldn't finish them");
      }
      break;
    }
    case 2: { // steal the nearest car and drive off
      if (me->car == CarID::Invalid) {
        auto best = std::numeric_limits<f32>::max();
        auto target = pos;
        for (usize i = 0; i < world.cars.size(); i++) {
          if (world.cars[i].alive && world.cars[i].entity && world.cars[i].player_driver == PlayerID::Invalid) {
            const auto d = glm::distance(world.car_position(static_cast<CarID>(i)), pos);
            if (d < best) {
              best = d;
              target = world.car_position(static_cast<CarID>(i));
            }
          }
        }
        input.move = walk_towards(pos, target);
        input.enter_exit = best < 3.5f && self.tap();
        if (self.phase_time > 45.0f) {
          self.next_phase("no car in reach");
        }
      } else {
        if (!self.stole_car) {
          self.stole_car = true;
          self.last_car_position = world.car_position(me->car);
          self.phase_time = 0.0f;
        }
        const auto p = world.car_position(me->car);
        self.distance_driven += glm::distance(p, self.last_car_position);
        self.last_car_position = p;
        input.throttle = 1.0f;
        input.steer = std::sin(self.phase_time * 0.7f) * 0.4f;
        if (at(3.0f)) {
          self.screenshot("driving");
        }
        if (self.phase_time > 6.0f) {
          self.next_phase(fmt::format("drove {:.0f} m in a stolen car", self.distance_driven));
        }
      }
      break;
    }
    default: break;
  }
  return input;
}

auto NetAutoplay::report(this const NetAutoplay& self, const World& world, const NetSession& net) -> bool {
  struct Check {
    std::string_view name;
    bool ok;
  };
  auto checks = std::vector<Check>{};
  if (self.role == "host") {
    checks = {
      {"hosted a game", self.joined},
      {"a remote player joined", self.remote_seen},
      {"the remote player walked more than 3 m (client side movement)", self.remote_moved > 3.0f},
      {"the remote player killed someone (host side hit detection)", self.remote_ped_kills > 0 || self.remote_player_kills > 0},
    };
  } else if (self.role == "target") {
    checks = {
      {"joined a game", self.joined},
      {"saw another player", self.saw_other_player},
      {"got wasted by another player", self.got_wasted},
      {"respawned afterwards", self.respawned},
    };
  } else {
    checks = {
      {"joined a game", self.joined},
      {"saw another player", self.saw_other_player},
      {"killed a pedestrian", self.killed_ped},
      {"wasted another player", self.wasted_player},
      {"stole a car and drove it more than 5 m", self.stole_car && self.distance_driven > 5.0f},
    };
  }
  OX_LOG_INFO(
    "OxCity net autoplay ({}) report: {:.1f}s, {} snapshots received, {} sent, largest {} B",
    self.role,
    self.time,
    net.snapshots_received,
    net.snapshots_sent,
    net.largest_snapshot
  );
  auto all = true;
  for (const auto& c : checks) {
    OX_LOG_INFO("  [{}] {}", c.ok ? "PASS" : "FAIL", c.name);
    all = all && c.ok;
  }
  for (auto id : ALL_PLAYERS) {
    const auto& p = world.pl(id);
    if (p.active) {
      OX_LOG_INFO("  player {} '{}': {} pts, {} kills, {} peds, ${}", static_cast<i32>(id), p.name, p.score, p.kills, p.stats.peds_killed, p.cash);
    }
  }
  return all;
}
} // namespace oxcity
