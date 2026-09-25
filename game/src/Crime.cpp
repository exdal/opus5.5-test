// Everything that makes the city fight back: wanted level, police cars and cops on foot, the bank heist, cash
// pickups, gunfire, and the WASTED / BUSTED flow.

#include <glm/common.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtx/norm.hpp>

#include "Scene/Components.hpp"
#include "Scene/Scene.hpp"
#include "Utils/Log.hpp"
#include "World.hpp"

namespace oxcity {
static constexpr f32 HEIST_TIME = 8.0f;
static constexpr f32 HEIST_RADIUS = 2.4f;
static constexpr f32 BUST_RANGE = 1.4f;

auto World::stars(this const World& self) -> i32 {
  return glm::clamp(static_cast<i32>(self.wanted.heat), 0, 5);
}

auto World::commit_crime(this World& self, f32 heat, glm::vec2 where, std::string_view pager) -> void {
  if (self.state != GameState::Playing) {
    return;
  }
  // crimes in front of a cop count double
  auto witnessed = false;
  for (const auto& p : self.peds) {
    if (p.alive && p.kind == PedKind::Cop && glm::distance(p.position, where) < 30.0f) {
      witnessed = true;
    }
  }
  for (usize i = 0; i < self.cars.size(); i++) {
    const auto& c = self.cars[i];
    if (c.alive && c.role == CarRole::Police && !c.player_inside &&
        glm::distance(self.car_position(static_cast<CarID>(i)), where) < 35.0f) {
      witnessed = true;
    }
  }
  const auto before = self.stars();
  self.wanted.heat = glm::min(5.99f, self.wanted.heat + heat * (witnessed ? 2.0f : 1.0f));
  self.wanted.cooldown = 0.0f;
  if (!pager.empty()) {
    self.pager(pager);
  }
  if (self.stars() > before && before == 0) {
    self.pager("THE COPS ARE ONTO YOU.");
  }
}

auto World::shoot(this World& self, glm::vec2 from, f32 heading, f32 damage, bool by_player) -> void {
  const auto dir = forward_of(heading);
  constexpr f32 RANGE = 38.0f;
  constexpr f32 STEP = 0.25f;

  auto end = from + dir * RANGE;
  for (f32 t = 0.0f; t < RANGE; t += STEP) {
    const auto p = from + dir * t;
    if (self.is_solid(p, 0.0f)) {
      end = p;
      break;
    }

    auto hit = false;
    for (usize i = 0; i < self.peds.size(); i++) {
      auto& ped = self.peds[i];
      if (!ped.alive || ped.state == PedState::Driving || glm::distance2(ped.position, p) > 0.45f * 0.45f) {
        continue;
      }
      // the shooter's own team doesn't shoot itself
      if (!by_player && ped.kind != PedKind::Civilian) {
        continue;
      }
      self.damage_ped(static_cast<PedID>(i), damage, from);
      if (by_player) {
        self.commit_crime(ped.kind == PedKind::Civilian ? 0.4f : 1.2f, from, "");
      }
      hit = true;
      break;
    }

    if (!hit && !by_player && self.player.car == CarID::Invalid && glm::distance2(self.player.position, p) < 0.5f * 0.5f) {
      self.damage_player(damage);
      hit = true;
    }

    if (!hit) {
      for (usize i = 0; i < self.cars.size(); i++) {
        const auto id = static_cast<CarID>(i);
        const auto& c = self.cars[i];
        if (!c.alive || (by_player && c.player_inside)) {
          continue;
        }
        const auto local = p - self.car_position(id);
        const auto fwd = forward_of(self.car_heading(id));
        if (glm::abs(glm::dot(local, fwd)) < 2.3f && glm::abs(glm::dot(local, right_of(fwd))) < 1.0f) {
          self.damage_car(id, damage * 0.3f);
          if (c.player_inside && !by_player) {
            self.damage_player(damage * 0.4f);
          }
          hit = true;
          break;
        }
      }
    }

    if (hit) {
      end = p;
      break;
    }
  }

  self.spawn_tracer(to3(from, 1.25f), to3(end, 1.1f));
  const auto dist = glm::distance(from, self.player_position());
  self.play(self.assets.sfx_gunshot, glm::clamp(1.0f - dist / 60.0f, 0.1f, 1.0f), self.random_float(0.92f, 1.08f));
  if (by_player) {
    self.panic_around(from, 22.0f);
    self.commit_crime(0.15f, from, "");
  }
}

auto World::bust_player(this World& self) -> void {
  if (self.state != GameState::Playing) {
    return;
  }
  self.stats.times_busted++;
  const auto fine = self.player.cash * 3 / 10;
  self.player.cash -= fine;
  self.player.ammo = 0;
  self.player.weapon = Weapon::Fists;
  self.set_state(GameState::Busted);
  self.hud.stats = fmt::format("FINE: ${}", fine);
  self.play(self.assets.sfx_wasted, 0.8f, 1.3f);
}

auto World::waste_player(this World& self) -> void {
  if (self.state != GameState::Playing) {
    return;
  }
  self.stats.times_wasted++;
  const auto bill = glm::min(self.player.cash, 750);
  self.player.cash -= bill;
  self.set_state(GameState::Wasted);
  self.hud.stats = fmt::format("HOSPITAL BILL: ${}", bill);
  self.play(self.assets.sfx_wasted, 1.0f);
  if (self.player.car == CarID::Invalid) {
    // fall over where you stand
    self.set_entity_pose(
      self.player.entity,
      to3(self.player.position, 0.25f),
      yaw_quat(self.player.heading) * glm::angleAxis(-glm::half_pi<f32>(), glm::vec3(1.0f, 0.0f, 0.0f))
    );
  }
}

auto World::respawn_player(this World& self) -> void {
  auto& p = self.player;
  const auto busted = self.state == GameState::Busted;
  if (p.car != CarID::Invalid) {
    auto& c = self.car(p.car);
    c.player_inside = false;
    c.role = c.model == "police" ? CarRole::Police : CarRole::Abandoned;
    p.car = CarID::Invalid;
  }
  p.position = busted ? self.police_station : self.hospital;
  p.heading = 0.0f;
  p.health = 100.0f;
  p.invulnerable = 3.0f;
  self.wanted = {};

  // re-create the character at the new spot: the observer builds it from the entity's world position
  p.entity.remove<ox::CharacterControllerComponent>();
  self.set_entity_pose(p.entity, to3(p.position, 0.4f), yaw_quat(0.0f));
  p.entity.set<ox::CharacterControllerComponent>({.character_height_standing = 1.1f, .character_radius_standing = 0.3f});

  self.pager(busted ? "YOU'RE OUT ON BAIL. TRY NOT TO DO THAT AGAIN." : "PATCHED UP AND BACK ON THE STREET.");
}

auto World::update_crime(this World& self, const GameInput& input, f32 dt) -> void {
  ZoneScoped;

  const auto player_pos = self.player_position();
  const auto playing = self.state == GameState::Playing;

  // --- wanted level decay ---
  self.wanted.cooldown += dt;
  auto cop_near = false;
  for (const auto& p : self.peds) {
    if (p.alive && p.kind == PedKind::Cop && glm::distance(p.position, player_pos) < 25.0f) {
      cop_near = true;
    }
  }
  for (usize i = 0; i < self.cars.size(); i++) {
    const auto& c = self.cars[i];
    if (c.alive && c.role == CarRole::Police && c.driver != PedID::Invalid &&
        glm::distance(self.car_position(static_cast<CarID>(i)), player_pos) < 30.0f) {
      cop_near = true;
    }
  }
  if (self.wanted.cooldown > 10.0f && !cop_near) {
    self.wanted.heat = glm::max(0.0f, self.wanted.heat - dt * 0.12f);
  }

  // --- police cars: one per star, spawned out of sight on the road graph ---
  auto police_cars = 0;
  for (const auto& c : self.cars) {
    if (c.alive && c.role == CarRole::Police && !c.player_inside) {
      police_cars++;
    }
  }
  self.wanted.spawn_timer -= dt;
  const auto wanted_cars = glm::min(self.stars(), 4);
  if (playing && police_cars < wanted_cars && self.wanted.spawn_timer <= 0.0f) {
    for (i32 attempt = 0; attempt < 12; attempt++) {
      const auto node = self.random_node();
      const auto pos = self.nodes[static_cast<usize>(node)].position;
      const auto d = glm::distance(pos, player_pos);
      if (d < 40.0f || d > 95.0f) {
        continue;
      }
      const auto id = self.spawn_car("police", pos, heading_of(player_pos - pos), CarRole::Police);
      if (id != CarID::Invalid) {
        const auto cop = self.spawn_ped(PedKind::Cop, pos);
        if (cop != PedID::Invalid) {
          self.ped(cop).state = PedState::Driving;
          self.ped(cop).car = id;
          self.hide_entity(self.ped(cop).entity);
          self.car(id).driver = cop;
        }
        self.car(id).cruise_speed = 12.0f;
      }
      self.wanted.spawn_timer = 5.0f;
      break;
    }
  }

  for (usize i = 0; i < self.cars.size(); i++) {
    const auto id = static_cast<CarID>(i);
    auto& c = self.cars[i];
    if (!c.alive || c.role != CarRole::Police || c.player_inside) {
      continue;
    }
    const auto d = glm::distance(self.car_position(id), player_pos);
    // lost the heat: far away patrol cars go home
    if (self.stars() == 0 && d > 70.0f) {
      self.despawn_car(id);
      continue;
    }
    // player on foot next to the car: the cop jumps out
    if (playing && c.driver != PedID::Invalid && self.stars() > 0 && self.player.car == CarID::Invalid && d < 14.0f &&
        glm::length(self.car_velocity(id)) < 3.0f) {
      auto& cop = self.ped(c.driver);
      cop.position = self.car_position(id) - right_of(forward_of(self.car_heading(id))) * 2.0f;
      cop.state = PedState::Chase;
      cop.car = CarID::Invalid;
      c.driver = PedID::Invalid;
    }
  }

  // cops on foot give up and leave once the heat is gone
  for (auto& p : self.peds) {
    if (p.alive && p.kind == PedKind::Cop && p.state != PedState::Driving && self.stars() == 0 &&
        glm::distance(p.position, player_pos) > 50.0f && p.entity.is_alive()) {
      p.entity.destruct();
      p.entity = {};
      p.alive = false;
      p.dead_time = 100.0f;
    }
  }

  // --- busted ---
  if (playing && self.stars() > 0) {
    auto cop_on_you = false;
    const auto reach = self.player.car == CarID::Invalid ? BUST_RANGE : 3.0f;
    const auto stopped = self.player.car == CarID::Invalid || glm::length(self.car_velocity(self.player.car)) < 1.0f;
    for (const auto& p : self.peds) {
      if (p.alive && p.kind == PedKind::Cop && p.state != PedState::Driving && glm::distance(p.position, player_pos) < reach) {
        cop_on_you = true;
      }
    }
    self.wanted.bust_timer = cop_on_you && stopped ? self.wanted.bust_timer + dt : 0.0f;
    if (self.wanted.bust_timer > 1.5f) {
      self.bust_player();
    }
  } else {
    self.wanted.bust_timer = 0.0f;
  }

  // --- the bank ---
  self.heist.restock = glm::max(0.0f, self.heist.restock - dt);
  self.heist.alarm = glm::max(0.0f, self.heist.alarm - dt);
  const auto at_vault = playing && self.player.car == CarID::Invalid &&
                        glm::distance(self.player.position, self.heist.position) < HEIST_RADIUS;
  self.hud.heist_active = false;
  if (at_vault && self.heist.restock <= 0.0f) {
    self.prompt_text = "HOLD E: CRACK THE VAULT";
    if (input.interact) {
      if (self.heist.progress == 0.0f) {
        self.heist.alarm = 60.0f;
        self.commit_crime(2.2f, self.heist.position, "THE ALARM IS RINGING. KEEP DRILLING!");
        for (auto& p : self.peds) {
          if (p.alive && p.kind == PedKind::Guard) {
            p.state = PedState::Attack;
          }
        }
      }
      self.heist.progress += dt;
      self.hud.heist_active = true;
      if (self.heist.progress >= HEIST_TIME) {
        const auto take = self.random_int(60, 120) * 100;
        self.player.cash += take;
        self.stats.cash_earned += take;
        self.stats.banks_robbed++;
        self.heist.progress = 0.0f;
        self.heist.restock = 120.0f;
        self.commit_crime(1.0f, self.heist.position, fmt::format("${} IN THE BAG! NOW LOSE THE HEAT.", take));
        self.play(self.assets.sfx_cash, 1.0f, 0.8f);
      }
    }
  } else if (!at_vault) {
    self.heist.progress = glm::max(0.0f, self.heist.progress - dt * 0.5f);
  }
  self.hud.heist_progress = static_cast<i32>(100.0f * self.heist.progress / HEIST_TIME);
  if (self.heist.marker && self.heist.marker.is_alive()) {
    const auto y = self.heist.restock > 0.0f ? -50.0f : 1.6f + std::sin(self.time * 3.0f) * 0.25f;
    self.set_entity_pose(self.heist.marker, to3(self.heist.position, y), yaw_quat(self.time * 1.5f));
  }

  // --- pickups ---
  for (auto& pk : self.pickups) {
    if (pk.taken) {
      continue;
    }
    pk.age += dt;
    const auto reach = self.player.car == CarID::Invalid ? 1.3f : 2.6f;
    if (playing && glm::distance(pk.position, player_pos) < reach) {
      pk.taken = true;
      self.player.cash += pk.cash;
      self.stats.cash_earned += pk.cash;
      if (pk.ammo > 0) {
        self.player.ammo += pk.ammo;
      }
      self.play(self.assets.sfx_cash, 0.8f);
    } else if (pk.age > 90.0f) {
      pk.taken = true;
    }
    if (pk.taken) {
      if (pk.entity.is_alive()) {
        pk.entity.destruct();
      }
      continue;
    }
    self.set_entity_pose(pk.entity, to3(pk.position, 0.2f + std::sin(self.time * 4.0f + pk.age) * 0.08f), yaw_quat(self.time * 2.0f + pk.age));
  }
  std::erase_if(self.pickups, [](const Pickup& pk) { return pk.taken; });

  for (auto& t : self.tracers) {
    t.life -= dt;
    if (t.life <= 0.0f && t.entity.is_alive()) {
      t.entity.destruct();
    }
  }
  std::erase_if(self.tracers, [](const Tracer& t) { return t.life <= 0.0f; });
}
} // namespace oxcity
