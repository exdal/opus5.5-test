#include "Autoplay.hpp"

#include <glm/common.hpp>
#include <glm/gtx/norm.hpp>

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

auto Autoplay::next(this Autoplay& self, std::string_view what) -> void {
  self.log.emplace_back(fmt::format("[{:6.1f}s] step {} done: {}", self.total_time, self.step, what));
  OX_LOG_INFO("OxCity autoplay: {}", self.log.back());
  self.step++;
  self.step_time = 0.0f;
}

auto Autoplay::screenshot(this Autoplay& self, std::string_view name) -> void {
  self.shots_taken++;
  self.pending_screenshot = fmt::format("{:02}_{}", self.shots_taken, name);
}

auto Autoplay::tap(this Autoplay& self) -> bool {
  if (self.total_time - self.last_tap < 1.0f) {
    return false;
  }
  self.last_tap = self.total_time;
  return true;
}

auto Autoplay::update(this Autoplay& self, World& world, f32 dt) -> GameInput {
  auto input = GameInput{};
  const auto before = self.step_time;
  self.step_time += dt;
  self.total_time += dt;
  auto at = [&](f32 t) { return before < t && self.step_time >= t; };

  self.max_stars = glm::max(self.max_stars, world.stars());
  for (const auto& c : world.cars) {
    if (c.alive && c.role == CarRole::Police && c.driver != PedID::Invalid && world.stars() > 0) {
      self.cops_showed_up = true;
    }
  }

  const auto me = world.player.position;
  const auto in_car = world.player.car != CarID::Invalid;

  switch (self.step) {
    case 0: { // title screen
      if (at(2.0f)) {
        self.screenshot("title_menu");
      }
      if (self.step_time > 3.0f) {
        input.confirm = true;
      }
      if (world.state == GameState::Playing) {
        self.entered_game = true;
        self.cash_at_start = world.player.cash;
        self.next("started a new game from the menu");
        if (self.start_step > self.step) {
          OX_LOG_INFO("OxCity autoplay: skipping ahead to step {}", self.start_step);
          self.step = self.start_step;
          if (self.step >= 7 && self.step < 20) {
            world.wanted.heat = 3.5f; // the chase needs someone to be chasing
          }
        }
      }
      break;
    }
    case 1: { // walk up to the nearest empty car
      if (self.target_car == CarID::Invalid) {
        auto best = std::numeric_limits<f32>::max();
        for (usize i = 0; i < world.cars.size(); i++) {
          const auto& c = world.cars[i];
          if (c.alive && c.role == CarRole::Parked) {
            const auto d = glm::distance(world.car_position(static_cast<CarID>(i)), me);
            if (d < best) {
              best = d;
              self.target_car = static_cast<CarID>(i);
            }
          }
        }
        if (self.target_car != CarID::Invalid && best > 25.0f) {
          // no time for a hike, the test is about the car, not the walk
          const auto p = world.car_position(self.target_car);
          world.teleport_player(p - right_of(forward_of(world.car_heading(self.target_car))) * 5.0f);
        }
      }
      if (at(1.0f)) {
        self.screenshot("on_foot");
      }
      if (self.target_car != CarID::Invalid) {
        const auto target = world.car_position(self.target_car);
        input.move = walk_towards(me, target);
        if (glm::distance(me, target) < 3.6f) {
          input.enter_exit = self.tap();
        }
      }
      if (in_car) {
        self.stole_car = true;
        self.last_car_position = world.car_position(world.player.car);
        self.next(fmt::format("got into a {}", world.car(world.player.car).display_name));
      } else if (self.step_time > 25.0f) {
        self.next("gave up on the parked car");
      }
      break;
    }
    case 2: { // drive around the block following the road graph
      if (!in_car) {
        self.next("fell out of the car?");
        break;
      }
      const auto id = world.player.car;
      const auto pos = world.car_position(id);
      self.distance_driven += glm::distance(pos, self.last_car_position);
      self.last_car_position = pos;
      if (self.drive_target == NodeID::Invalid || glm::distance(world.nodes[static_cast<usize>(self.drive_target)].position, pos) < 8.0f) {
        // next intersection roughly ahead
        const auto here = world.nearest_node(pos);
        const auto fwd = forward_of(world.car_heading(id));
        auto best = -2.0f;
        for (auto n : world.nodes[static_cast<usize>(here)].neighbours) {
          const auto dir = glm::normalize(world.nodes[static_cast<usize>(n)].position - pos);
          const auto score = glm::dot(dir, fwd) + (world.random_float(0.0f, 0.4f));
          if (score > best) {
            best = score;
            self.drive_target = n;
          }
        }
      }
      const auto aim = world.nodes[static_cast<usize>(self.drive_target)].position;
      const auto angle = wrap_angle(heading_of(aim - pos) - world.car_heading(id));
      const auto speed = glm::length(world.car_velocity(id));
      input.steer = glm::clamp(-angle * 2.0f, -1.0f, 1.0f);
      input.throttle = speed < (glm::abs(angle) > 0.5f ? 6.0f : 13.0f) ? 0.9f : 0.0f;
      if (speed < 0.5f && self.step_time > 3.0f && glm::fract(self.step_time * 0.25f) < 0.3f) {
        input.throttle = -1.0f; // stuck, back up
        input.steer = -input.steer;
      }
      if (at(6.0f)) {
        self.screenshot("driving");
      }
      if (self.step_time > 14.0f) {
        self.drove = self.distance_driven > 20.0f;
        self.next(fmt::format("drove {:.0f} m", self.distance_driven));
      }
      break;
    }
    case 3: { // get out
      if (in_car) {
        input.enter_exit = self.tap();
      } else {
        self.next("got out of the car");
      }
      break;
    }
    case 4: { // mug the nearest pedestrian, then pick up the cash
      if (self.victim == PedID::Invalid || !world.ped(self.victim).alive || world.ped(self.victim).cash <= 0) {
        if (self.mugged) {
          // walk over the dropped cash
          auto nearest = std::numeric_limits<f32>::max();
          auto where = me;
          for (const auto& pk : world.pickups) {
            const auto d = glm::distance(pk.position, me);
            if (d < nearest) {
              nearest = d;
              where = pk.position;
            }
          }
          input.move = walk_towards(me, where);
          if (world.player.cash > self.cash_at_start || world.pickups.empty() || self.step_time > 30.0f) {
            self.cash_after_mug = world.player.cash;
            self.next(fmt::format("mugged someone, cash ${} -> ${}", self.cash_at_start, world.player.cash));
          }
          break;
        }
        auto best = std::numeric_limits<f32>::max();
        self.victim = PedID::Invalid;
        for (usize i = 0; i < world.peds.size(); i++) {
          const auto& p = world.peds[i];
          if (p.alive && p.kind == PedKind::Civilian && p.state != PedState::Driving && p.cash > 0) {
            const auto d = glm::distance(p.position, me);
            if (d < best) {
              best = d;
              self.victim = static_cast<PedID>(i);
            }
          }
        }
        if (self.victim != PedID::Invalid && best > 30.0f) {
          world.teleport_player(world.ped(self.victim).position + glm::vec2(2.0f, 0.0f));
        }
      }
      if (self.victim != PedID::Invalid) {
        auto& v = world.ped(self.victim);
        if (glm::distance(v.position, me) > 1.4f) {
          input.move = walk_towards(me, v.position);
        } else {
          input.interact = true;
          if (!self.mugged) {
            self.screenshot("mugging");
          }
          self.mugged = true;
        }
      }
      if (self.step_time > 40.0f) {
        self.next("couldn't find anyone to mug");
      }
      break;
    }
    case 5: { // pistol whip the neighbourhood
      if (world.player.weapon != Weapon::Pistol) {
        input.switch_weapon = self.tap();
      }
      auto best = std::numeric_limits<f32>::max();
      auto target = me + glm::vec2(0.0f, -5.0f);
      for (const auto& p : world.peds) {
        if (p.alive && p.kind == PedKind::Civilian && p.state != PedState::Driving) {
          const auto d = glm::distance(p.position, me);
          if (d < best) {
            best = d;
            target = p.position;
          }
        }
      }
      // turn towards them with a tiny step, then fire
      input.move = walk_towards(me, target) * (best > 12.0f ? 1.0f : 0.15f);
      input.attack = best < 20.0f && self.step_time > 0.5f;
      if (at(1.2f)) {
        self.screenshot("shooting");
      }
      if (self.step_time > 4.0f) {
        self.next(fmt::format("fired the pistol, {} ammo left, {} stars", world.player.ammo, world.stars()));
      }
      break;
    }
    case 6: { // rob the bank
      if (self.step_time < dt * 1.5f) {
        world.teleport_player(world.heist.position + glm::vec2(0.0f, 1.0f));
      }
      input.move = walk_towards(me, world.heist.position) * (glm::distance(me, world.heist.position) > 0.6f ? 0.5f : 0.0f);
      input.interact = true;
      if (at(4.0f)) {
        self.screenshot("bank_heist");
      }
      if (world.stats.banks_robbed > 0) {
        self.robbed_bank = true;
        self.screenshot("heist_done");
        self.next(fmt::format("robbed the bank, cash now ${}", world.player.cash));
      } else if (self.step_time > 30.0f) {
        self.next("the vault didn't open");
      }
      break;
    }
    case 7: { // grab a car and run
      if (!in_car) {
        auto best = std::numeric_limits<f32>::max();
        auto target = me;
        for (usize i = 0; i < world.cars.size(); i++) {
          if (world.cars[i].alive && world.cars[i].role != CarRole::Police) {
            const auto d = glm::distance(world.car_position(static_cast<CarID>(i)), me);
            if (d < best) {
              best = d;
              target = world.car_position(static_cast<CarID>(i));
            }
          }
        }
        if (best > 30.0f && self.step_time < dt * 1.5f) {
          world.teleport_player(target + glm::vec2(3.0f, 0.0f));
        }
        input.move = walk_towards(me, target);
        input.enter_exit = best < 3.6f && self.tap();
      } else {
        const auto id = world.player.car;
        const auto pos = world.car_position(id);
        const auto away = pos - world.heist.position;
        const auto node = world.nearest_node(pos + glm::normalize(away + glm::vec2(0.01f)) * 30.0f);
        const auto aim = world.nodes[static_cast<usize>(node)].position;
        const auto angle = wrap_angle(heading_of(aim - pos) - world.car_heading(id));
        input.steer = glm::clamp(-angle * 2.0f, -1.0f, 1.0f);
        input.throttle = 1.0f;
      }
      if (at(8.0f)) {
        self.screenshot("police_chase");
      }
      if (self.step_time > 16.0f) {
        self.next(fmt::format("ran from the cops with {} stars", world.stars()));
      }
      break;
    }
    case 8: { // hands up
      if (in_car) {
        input.enter_exit = self.tap();
      }
      if (world.state == GameState::Arrested || world.state == GameState::Dead) {
        if (world.state_timer > 1.0f) {
          self.screenshot(world.state == GameState::Arrested ? "arrested" : "flatlined");
          self.next(world.state == GameState::Arrested ? "got arrested" : "flatlined");
        }
      } else if (self.step_time > 40.0f) {
        self.next("the cops never caught up");
      }
      break;
    }
    case 9: { // back on the street
      if (world.state == GameState::Playing && self.step_time > 6.0f) {
        self.screenshot("respawned");
        self.next("respawned");
      } else if (self.step_time > 15.0f) {
        self.next("didn't respawn");
      }
      break;
    }
    case 20: { // engine check: a destroyed mesh must take its cached VSM shadow with it
      if (at(3.0f)) {
        self.screenshot("shadow_before");
      }
      if (at(3.5f)) {
        self.probe = world.spawn_model(world.assets.van, to3(me + glm::vec2(5.0f, 0.0f), 0.0f), 0.0f);
      }
      if (at(5.5f)) {
        self.screenshot("shadow_with_van");
      }
      if (at(6.0f) && self.probe) {
        self.probe.destruct();
        self.probe = {};
      }
      if (at(8.0f)) {
        self.screenshot("shadow_after");
        self.next("spawned and destroyed a van for the shadow check");
      }
      break;
    }
    case 21: { // knife, blood, combo, then the pistol's muzzle flash and sparks
      // bring a civilian to the player and slash it; the aim goes where the victim stands
      auto victim_at = [&](f32 t, glm::vec2 offset) {
        if (at(t)) {
          world.player.weapon = Weapon::Knife;
          for (auto& ped : world.peds) {
            if (ped.alive && ped.kind == PedKind::Civilian && ped.state != PedState::Driving) {
              ped.position = me + offset;
              ped.state = PedState::Idle;
              break;
            }
          }
        }
      };
      victim_at(0.5f, {1.1f, 0.0f});
      victim_at(1.2f, {0.0f, -1.1f});
      if (self.step_time > 0.6f && self.step_time < 0.7f) {
        input.has_aim = true;
        input.aim = me + glm::vec2(3.0f, 0.0f);
        input.attack = true;
      }
      if (self.step_time > 1.3f && self.step_time < 1.4f) {
        input.has_aim = true;
        input.aim = me + glm::vec2(0.0f, -3.0f);
        input.attack = true;
      }
      if (at(0.75f)) {
        self.screenshot("knife_kill");
      }
      if (at(1.5f)) {
        self.screenshot("knife_combo");
      }
      if (at(2.4f)) {
        world.player.weapon = Weapon::Pistol;
      }
      if (self.step_time > 2.5f && self.step_time < 2.56f) {
        input.has_aim = true;
        input.aim = me + glm::vec2(-8.0f, 0.0f);
        input.attack = true;
      }
      if (at(2.56f)) {
        self.screenshot("muzzle_and_sparks");
      }
      if (at(2.2f)) {
        self.screenshot("blood_spatter");
      }
      // a car parked next to us, then blown up
      if (at(3.0f)) {
        for (usize i = 0; i < world.cars.size(); i++) {
          const auto id = static_cast<CarID>(i);
          if (world.cars[i].alive && !world.cars[i].player_inside && world.cars[i].driver == PedID::Invalid) {
            world.teleport_car(id, me + glm::vec2(7.0f, 3.0f), 0.3f);
            self.target_car = id;
            break;
          }
        }
      }
      if (at(3.4f) && self.target_car != CarID::Invalid) {
        world.damage_car(self.target_car, 500.0f, true);
      }
      if (at(3.5f)) {
        self.screenshot("explosion");
      }
      if (at(6.0f)) {
        self.screenshot("aftermath");
        self.next(fmt::format("kills {}, combo score {}", world.stats.peds_killed, world.juice.score));
      }
      break;
    }
    case 22: { // settings panel: open it from the pause menu, turn sound effects off, check it's saved
      if (at(0.5f)) {
        world.set_state(GameState::Paused);
        world.hud.settings_open = true;
        world.hud.sfx_on = false;
        world.save_settings();
      }
      if (at(1.0f)) {
        self.screenshot("settings");
      }
      if (at(1.5f)) {
        world.hud.sfx_on = !world.hud.sfx_on;
        world.load_settings(); // back from the file: must still say off
        const auto saved_off = !world.hud.sfx_on;
        world.hud.sfx_on = true;
        world.save_settings();
        world.hud.settings_open = false;
        world.set_state(GameState::Playing);
        self.next(saved_off ? "settings: sound effects off, saved and read back" : "settings: the saved value didn't come back");
      }
      break;
    }
    default: {
      if (self.step_time > 2.0f) {
        world.quit_requested = true;
      }
      break;
    }
  }

  return input;
}

auto Autoplay::report(this const Autoplay& self, const World& world) -> bool {
  struct Check {
    std::string_view name;
    bool ok;
  };
  const Check checks[] = {
    {"started a game from the RmlUi menu", self.entered_game},
    {"stole a car", self.stole_car},
    {"drove it more than 20 m", self.drove},
    {"mugged a pedestrian and picked up the cash", self.mugged && self.cash_after_mug > self.cash_at_start},
    {"robbed the bank", self.robbed_bank},
    {"got a wanted level", self.max_stars > 0},
    {"police cars came after the player", self.cops_showed_up},
  };
  OX_LOG_INFO("OxCity autoplay report ({:.1f}s simulated)", self.total_time);
  for (const auto& line : self.log) {
    OX_LOG_INFO("  {}", line);
  }
  auto all = true;
  for (const auto& c : checks) {
    OX_LOG_INFO("  [{}] {}", c.ok ? "PASS" : "FAIL", c.name);
    all = all && c.ok;
  }
  OX_LOG_INFO(
    "  stats: cash ${} earned ${} | banks {} | cars stolen {} | robbed {} | killed {} | arrested {} | flatlined {} | max stars {}",
    world.player.cash,
    world.stats.cash_earned,
    world.stats.banks_robbed,
    world.stats.cars_stolen,
    world.stats.peds_robbed,
    world.stats.peds_killed,
    world.stats.times_arrested,
    world.stats.times_killed,
    self.max_stars
  );
  return all;
}
} // namespace oxcity
