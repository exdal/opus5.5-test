// Everything that makes the city fight back: wanted level, police cars and cops on foot, the bank heist, cash
// pickups, gunfire, and the FLATLINED / ARRESTED flow.

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
static constexpr f32 ARREST_RANGE = 1.4f;
static constexpr f32 PVP_BULLET = 0.7f; // players take this much of a bullet's damage from another player
static constexpr i32 MAX_POLICE_CARS = 5;
static constexpr i32 PVP_KILL_SCORE = 1000;

auto World::stars(this const World& self, PlayerID id) -> i32 {
  if (id == PlayerID::Invalid) {
    return 0;
  }
  return glm::clamp(static_cast<i32>(self.pl(id).wanted.heat), 0, 5);
}

auto World::total_stars(this const World& self) -> i32 {
  auto total = 0;
  for (auto id : ALL_PLAYERS) {
    if (self.in_play(id)) {
      total += self.stars(id);
    }
  }
  return total;
}

auto World::commit_crime(this World& self, PlayerID id, f32 heat, glm::vec2 where, std::string_view pager) -> void {
  if (!self.in_play(id)) {
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
    if (c.alive && c.role == CarRole::Police && c.player_driver == PlayerID::Invalid &&
        glm::distance(self.car_position(static_cast<CarID>(i)), where) < 35.0f) {
      witnessed = true;
    }
  }
  auto& wanted = self.pl(id).wanted;
  const auto before = self.stars(id);
  wanted.heat = glm::min(5.99f, wanted.heat + heat * (witnessed ? 2.0f : 1.0f));
  wanted.cooldown = 0.0f;
  if (!pager.empty()) {
    self.pager_to(id, pager);
  }
  if (self.stars(id) > before && before == 0) {
    self.pager_to(id, "THE COPS ARE ONTO YOU.");
  }
}

auto World::shoot(this World& self, glm::vec2 from, f32 heading, f32 damage, PlayerID shooter) -> void {
  const auto dir = forward_of(heading);
  const auto by_player = shooter != PlayerID::Invalid;
  constexpr f32 RANGE = 38.0f;
  constexpr f32 STEP = 0.25f;

  self.muzzle_flash(from + dir * 0.2f, heading);

  auto end = from + dir * RANGE;
  auto sparks = false; // walls and cars throw sparks, people bleed (damage_ped)
  for (f32 t = 0.0f; t < RANGE; t += STEP) {
    const auto p = from + dir * t;
    if (self.is_solid(p, 0.0f)) {
      end = p;
      sparks = true;
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
      self.damage_ped(static_cast<PedID>(i), damage, from, shooter);
      if (by_player) {
        self.commit_crime(shooter, ped.kind == PedKind::Civilian ? 0.4f : 1.2f, from, "");
      }
      hit = true;
      break;
    }

    // players on foot: the police's targets, and everyone else's
    for (auto id : ALL_PLAYERS) {
      if (hit || id == shooter || !self.on_foot(id) || glm::distance2(self.pl(id).position, p) > 0.5f * 0.5f) {
        continue;
      }
      self.damage_player(id, by_player ? damage * PVP_BULLET : damage, shooter);
      if (by_player) {
        self.commit_crime(shooter, 0.6f, from, "");
      }
      hit = true;
    }

    if (!hit) {
      for (usize i = 0; i < self.cars.size(); i++) {
        const auto id = static_cast<CarID>(i);
        const auto& c = self.cars[i];
        if (!c.alive || (by_player && c.player_driver == shooter)) {
          continue;
        }
        const auto local = p - self.car_position(id);
        const auto fwd = forward_of(self.car_heading(id));
        if (glm::abs(glm::dot(local, fwd)) < 2.3f && glm::abs(glm::dot(local, right_of(fwd))) < 1.0f) {
          const auto driver = c.player_driver;
          self.damage_car(id, damage * 0.3f, shooter);
          sparks = true;
          if (driver != PlayerID::Invalid) {
            // through the door
            self.damage_player(driver, damage * 0.4f * (by_player ? PVP_BULLET : 1.0f), shooter);
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
  if (sparks) {
    self.impact_sparks(to3(end, BULLET_HEIGHT), -dir);
  }
  self.play_at(self.assets.sfx_gunshot, from, 1.0f, self.random_float(0.92f, 1.08f));
  if (by_player) {
    self.panic_around(from, 22.0f);
    self.commit_crime(shooter, 0.15f, from, "");
  }
}

auto World::arrest_player(this World& self, PlayerID id) -> void {
  if (!self.in_play(id)) {
    return;
  }
  auto& p = self.pl(id);
  p.stats.times_arrested++;
  const auto fine = p.cash * 3 / 10;
  p.cash -= fine;
  p.ammo = 0;
  p.weapon = Weapon::Fists;
  p.life = Life::Arrested;
  p.life_timer = 0.0f;
  p.life_note = fmt::format("FINE: ${}", fine);
  self.record({.kind = net::EventKind::LifeNote, .target = static_cast<u8>(id), .text = p.life_note});
  if (self.is_local(id)) {
    self.play(self.assets.sfx_death, 0.8f, 1.3f);
  }
}

auto World::kill_player(this World& self, PlayerID id, PlayerID killer) -> void {
  if (!self.in_play(id)) {
    return;
  }
  auto& p = self.pl(id);
  p.stats.times_killed++;
  const auto bill = glm::min(p.cash, 750);
  p.cash -= bill;
  p.life = Life::Dead;
  p.life_timer = 0.0f;
  if (killer != PlayerID::Invalid && killer != id) {
    // wasted by another player: the money you had on you is on the pavement, for whoever gets there first
    auto& k = self.pl(killer);
    k.kills++;
    k.score += PVP_KILL_SCORE;
    p.life_note = fmt::format("WASTED BY {}. DROPPED ${}", k.name, bill);
    if (bill > 0) {
      self.spawn_pickup(p.position, bill, 0);
    }
    self.on_player_killed(id, killer);
  } else {
    p.life_note = fmt::format("HOSPITAL BILL: ${}", bill);
  }
  if (p.car == CarID::Invalid) {
    const auto away = killer != PlayerID::Invalid ? p.position - self.player_position(killer) : glm::vec2(0.0f);
    self.on_kill(p.position, glm::length2(away) > 0.001f ? glm::normalize(away) * 5.0f : glm::vec2(0.0f), killer, false);
  }
  self.record({.kind = net::EventKind::LifeNote, .target = static_cast<u8>(id), .text = p.life_note});
  if (self.is_local(id)) {
    self.play(self.assets.sfx_death, 1.0f);
  }
  if (p.car == CarID::Invalid) {
    // fall over where you stand
    self.set_entity_pose(
      p.entity,
      to3(p.position, 0.25f),
      yaw_quat(p.heading) * glm::angleAxis(-glm::half_pi<f32>(), glm::vec3(1.0f, 0.0f, 0.0f))
    );
  }
}

auto World::on_player_killed(this World& self, PlayerID victim, PlayerID killer) -> void {
  const auto& k = self.pl(killer);
  const auto& v = self.pl(victim);
  self.kill_feed.push_back({.text = fmt::format("{} WASTED {}", k.name, v.name)});
  self.record({.kind = net::EventKind::KillFeed, .text = self.kill_feed.back().text});
  if (self.kill_feed.size() > 4) {
    self.kill_feed.erase(self.kill_feed.begin());
  }
  self.pager_to(killer, fmt::format("YOU WASTED {}. +{} PTS", v.name, PVP_KILL_SCORE));
  OX_LOG_INFO("OxCity: {} wasted {}", k.name, v.name);
}

auto World::respawn_player(this World& self, PlayerID id) -> void {
  auto& p = self.pl(id);
  const auto arrested = p.life == Life::Arrested;
  if (p.car != CarID::Invalid) {
    auto& c = self.car(p.car);
    c.player_driver = PlayerID::Invalid;
    c.role = c.model == "police" ? CarRole::Police : CarRole::Abandoned;
    p.car = CarID::Invalid;
  }
  p.position = arrested ? self.police_station : self.hospital;
  p.heading = 0.0f;
  p.health = 100.0f;
  p.invulnerable = 3.0f;
  p.wanted = {};
  p.life = Life::Alive;
  p.life_timer = 0.0f;
  p.life_note.clear();
  p.teleport_seq++;

  // re-create the character at the new spot: the observer builds it from the entity's world position
  p.entity.remove<ox::CharacterControllerComponent>();
  self.set_entity_pose(p.entity, to3(p.position, 0.4f), yaw_quat(0.0f));
  p.entity.set<ox::CharacterControllerComponent>({.character_height_standing = 1.1f, .character_radius_standing = 0.3f});

  self.pager_to(id, arrested ? "YOU'RE OUT ON BAIL. TRY NOT TO DO THAT AGAIN." : "PATCHED UP AND BACK ON THE STREET.");
}

auto World::update_crime(this World& self, f32 dt) -> void {
  ZoneScoped;

  // --- wanted level decay, per player: lying low means no cop in sight for a while ---
  for (auto id : ALL_PLAYERS) {
    if (!self.in_play(id)) {
      continue;
    }
    const auto pos = self.player_position(id);
    auto& wanted = self.pl(id).wanted;
    wanted.cooldown += dt;
    auto cop_near = false;
    for (const auto& p : self.peds) {
      if (p.alive && p.kind == PedKind::Cop && glm::distance(p.position, pos) < 25.0f) {
        cop_near = true;
      }
    }
    for (usize i = 0; i < self.cars.size(); i++) {
      const auto& c = self.cars[i];
      if (c.alive && c.role == CarRole::Police && c.driver != PedID::Invalid &&
          glm::distance(self.car_position(static_cast<CarID>(i)), pos) < 30.0f) {
        cop_near = true;
      }
    }
    if (wanted.cooldown > 10.0f && !cop_near) {
      wanted.heat = glm::max(0.0f, wanted.heat - dt * 0.12f);
    }
  }

  // --- police cars: one per star (four at most per player, five in the whole city), spawned out of sight on the
  // road graph near whoever has the most heat ---
  auto police_cars = 0;
  for (const auto& c : self.cars) {
    if (c.alive && c.role == CarRole::Police && c.player_driver == PlayerID::Invalid) {
      police_cars++;
    }
  }
  auto wanted_cars = 0;
  auto most_wanted = PlayerID::Invalid;
  for (auto id : ALL_PLAYERS) {
    if (!self.in_play(id)) {
      continue;
    }
    wanted_cars += glm::min(self.stars(id), 4);
    if (self.stars(id) > 0 && (most_wanted == PlayerID::Invalid || self.stars(id) > self.stars(most_wanted))) {
      most_wanted = id;
    }
  }
  wanted_cars = glm::min(wanted_cars, MAX_POLICE_CARS);
  auto& spawn_timer = self.police_spawn_timer;
  spawn_timer -= dt;
  if (most_wanted != PlayerID::Invalid && police_cars < wanted_cars && spawn_timer <= 0.0f) {
    const auto target = self.player_position(most_wanted);
    for (i32 attempt = 0; attempt < 12; attempt++) {
      const auto node = self.random_node();
      const auto pos = self.nodes[static_cast<usize>(node)].position;
      const auto d = glm::distance(pos, target);
      if (d < 40.0f || d > 95.0f || self.distance_to_players(pos) < 40.0f) {
        continue;
      }
      const auto id = self.spawn_car("police", pos, heading_of(target - pos), CarRole::Police);
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
      spawn_timer = 5.0f;
      break;
    }
  }

  const auto anyone_wanted = self.total_stars() > 0;
  for (usize i = 0; i < self.cars.size(); i++) {
    const auto id = static_cast<CarID>(i);
    auto& c = self.cars[i];
    if (!c.alive || c.role != CarRole::Police || c.player_driver != PlayerID::Invalid) {
      continue;
    }
    const auto pos = self.car_position(id);
    // lost the heat: far away patrol cars go home
    if (!anyone_wanted && self.distance_to_players(pos) > 70.0f) {
      self.despawn_car(id);
      continue;
    }
    // a wanted player on foot next to the car: the cop jumps out
    const auto suspect = self.nearest_player(pos, 14.0f, true);
    if (c.driver != PedID::Invalid && suspect != PlayerID::Invalid && self.on_foot(suspect) &&
        glm::length(self.car_velocity(id)) < 3.0f) {
      auto& cop = self.ped(c.driver);
      cop.position = pos - right_of(forward_of(self.car_heading(id))) * 2.0f;
      cop.state = PedState::Chase;
      cop.car = CarID::Invalid;
      c.driver = PedID::Invalid;
    }
  }

  // cops on foot give up and leave once the heat is gone
  for (auto& p : self.peds) {
    if (p.alive && p.kind == PedKind::Cop && p.state != PedState::Driving && !anyone_wanted &&
        self.distance_to_players(p.position) > 50.0f && p.entity.is_alive()) {
      p.entity.destruct();
      p.entity = {};
      p.alive = false;
      p.dead_time = 100.0f;
    }
  }

  // --- arrests, per player ---
  for (auto id : ALL_PLAYERS) {
    if (!self.in_play(id)) {
      continue;
    }
    auto& pl = self.pl(id);
    if (self.stars(id) == 0) {
      pl.wanted.arrest_timer = 0.0f;
      continue;
    }
    const auto pos = self.player_position(id);
    auto cop_on_you = false;
    const auto reach = pl.car == CarID::Invalid ? ARREST_RANGE : 3.0f;
    const auto stopped = pl.car == CarID::Invalid || glm::length(self.car_velocity(pl.car)) < 1.0f;
    for (const auto& p : self.peds) {
      if (p.alive && p.kind == PedKind::Cop && p.state != PedState::Driving && glm::distance(p.position, pos) < reach) {
        cop_on_you = true;
      }
    }
    pl.wanted.arrest_timer = cop_on_you && stopped ? pl.wanted.arrest_timer + dt : 0.0f;
    if (pl.wanted.arrest_timer > 1.5f) {
      self.arrest_player(id);
    }
  }

  // --- the bank: one vault, whoever holds E at the door drills it and takes the money ---
  self.heist.restock = glm::max(0.0f, self.heist.restock - dt);
  self.heist.alarm = glm::max(0.0f, self.heist.alarm - dt);
  auto at_vault = [&](PlayerID id) {
    return self.on_foot(id) && glm::distance(self.pl(id).position, self.heist.position) < HEIST_RADIUS;
  };
  if (self.heist.driller != PlayerID::Invalid &&
      (!at_vault(self.heist.driller) || !self.pl(self.heist.driller).input.interact)) {
    self.heist.driller = PlayerID::Invalid;
  }
  if (self.heist.driller == PlayerID::Invalid && self.heist.restock <= 0.0f) {
    for (auto id : ALL_PLAYERS) {
      if (at_vault(id) && self.pl(id).input.interact) {
        self.heist.driller = id;
        break;
      }
    }
  }
  if (self.local != PlayerID::Invalid && at_vault(self.local) && self.heist.restock <= 0.0f) {
    self.prompt_text = self.heist.driller == PlayerID::Invalid || self.heist.driller == self.local
                         ? "HOLD E: CRACK THE VAULT"
                         : "SOMEONE'S ALREADY DRILLING";
  }
  const auto driller = self.heist.driller;
  if (driller != PlayerID::Invalid) {
    if (self.heist.progress == 0.0f) {
      self.heist.alarm = 60.0f;
      self.commit_crime(driller, 2.2f, self.heist.position, "THE ALARM IS RINGING. KEEP DRILLING!");
      for (auto& p : self.peds) {
        if (p.alive && p.kind == PedKind::Guard) {
          p.state = PedState::Attack;
        }
      }
    }
    self.heist.progress += dt;
    // the drill bites: sparks off the vault door every few frames
    if (glm::fract(self.heist.progress * 12.0f) < glm::fract((self.heist.progress - dt) * 12.0f)) {
      self.drill_sparks(self.heist.position + glm::vec2(0.0f, -0.6f));
    }
    if (self.heist.progress >= HEIST_TIME) {
      auto& pl = self.pl(driller);
      const auto take = self.random_int(60, 120) * 100;
      pl.cash += take;
      pl.stats.cash_earned += take;
      pl.stats.banks_robbed++;
      self.heist.progress = 0.0f;
      self.heist.restock = 120.0f;
      self.heist.driller = PlayerID::Invalid;
      self.commit_crime(driller, 1.0f, self.heist.position, fmt::format("${} IN THE BAG! NOW LOSE THE HEAT.", take));
      self.play_at(self.assets.sfx_cash, self.heist.position, 1.0f, 0.8f);
    }
  } else {
    self.heist.progress = glm::max(0.0f, self.heist.progress - dt * 0.5f);
  }
  self.hud.heist_active = driller != PlayerID::Invalid && self.is_local(driller);
  self.hud.heist_progress = static_cast<i32>(100.0f * self.heist.progress / HEIST_TIME);
  if (self.heist.marker && self.heist.marker.is_alive()) {
    const auto y = self.heist.restock > 0.0f ? -50.0f : 1.6f + std::sin(self.time * 3.0f) * 0.25f;
    self.set_entity_pose(self.heist.marker, to3(self.heist.position, y), yaw_quat(self.time * 1.5f));
  }

  // --- pickups: first come, first served ---
  for (auto& pk : self.pickups) {
    if (pk.taken) {
      continue;
    }
    pk.age += dt;
    for (auto id : ALL_PLAYERS) {
      if (!self.in_play(id)) {
        continue;
      }
      auto& pl = self.pl(id);
      const auto reach = pl.car == CarID::Invalid ? 1.3f : 2.6f;
      if (glm::distance(pk.position, self.player_position(id)) < reach) {
        pk.taken = true;
        pl.cash += pk.cash;
        pl.stats.cash_earned += pk.cash;
        if (pk.ammo > 0) {
          pl.ammo += pk.ammo;
        }
        self.play_at(self.assets.sfx_cash, pk.position, 0.8f);
        self.cash_sparkle(pk.position);
        break;
      }
    }
    if (!pk.taken && pk.age > 90.0f) {
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
