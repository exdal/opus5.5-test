// Players on foot: movement through the engine's JPH::Character, melee and pistol, mugging, and getting in and out
// of cars (including throwing the driver out, AI or human). Every function works on one player slot: offline that's
// slot 0, online the host runs all of them from each player's own input.

// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Character/Character.h>
// clang-format on

#include <glm/common.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtx/norm.hpp>

#include <fmt/format.h>

#include "Scene/Components.hpp"
#include "Scene/Scene.hpp"
#include "World.hpp"

namespace oxcity {
static constexpr f32 WALK_SPEED = 4.2f;
static constexpr f32 SPRINT_SPEED = 7.5f;
static constexpr f32 PUNCH_RANGE = 1.7f;
static constexpr f32 KNIFE_RANGE = 1.9f;
static constexpr f32 KNIFE_ARC = 0.35f; // cos of the half angle the slash covers, about 70 degrees each way
static constexpr glm::vec3 KNIFE_IN_HAND = {0.0f, -0.6f, 0.06f};
static constexpr f32 ENTER_RANGE = 4.2f;
// damage one player does to another, per weapon. Peds fold faster, players are meant to trade a few blows
static constexpr f32 PVP_PUNCH = 12.0f;
static constexpr f32 PVP_KNIFE = 100.0f; // it's a knife

static auto character_of(flecs::entity e) -> JPH::Character* {
  if (!e || !e.is_alive()) {
    return nullptr;
  }
  const auto* cc = e.try_get<ox::CharacterControllerComponent>();
  return cc ? static_cast<JPH::Character*>(cc->character) : nullptr;
}

static auto add_character(flecs::entity e) -> void {
  e.set<ox::CharacterControllerComponent>({
    .character_height_standing = 1.1f,
    .character_radius_standing = 0.3f,
  });
}

auto World::spawn_player(this World& self, PlayerID id, glm::vec2 position) -> void {
  auto& p = self.pl(id);
  const auto model = self.assets.players[static_cast<usize>(id)];
  p.entity = self.spawn_model(model ? model : self.assets.players[0], to3(position, 0.3f), 0.0f);
  p.limbs = self.find_limbs(p.entity);
  p.position = position;
  p.car = CarID::Invalid;
  p.teleport_seq++;
  if (self.authority() || self.is_local(id)) {
    // the host moves everyone through Jolt; a client only runs its own character (the rest are posed from snapshots)
    add_character(p.entity);
  }

  // the knife rides on the right arm, so it swings with the slash animation for free
  if (self.assets.knife && p.limbs.arm_r) {
    p.knife = self.spawn_model(self.assets.knife, KNIFE_IN_HAND, 0.0f, glm::vec3(0.001f));
    if (p.knife) {
      p.knife.child_of(p.limbs.arm_r);
      p.knife.modified<ox::TransformComponent>();
    }
  }
}

auto World::despawn_player(this World& self, PlayerID id) -> void {
  auto& p = self.pl(id);
  if (p.car != CarID::Invalid && self.authority() && static_cast<usize>(p.car) < self.cars.size()) {
    auto& c = self.car(p.car);
    c.player_driver = PlayerID::Invalid;
    c.role = c.model == "police" ? CarRole::Police : CarRole::Abandoned;
    self.drive_car(p.car, 0.0f, 0.0f, 1.0f, 1.0f);
  }
  if (self.heist.driller == id) {
    self.heist.driller = PlayerID::Invalid;
  }
  // the knife is a child of the arm, it goes with the rest of the hierarchy
  if (p.entity && p.entity.is_alive()) {
    p.entity.destruct();
  }
  const auto generation = p.generation;
  p = Player{};
  p.generation = generation;
}

auto World::teleport_player(this World& self, PlayerID id, glm::vec2 position) -> void {
  auto& p = self.pl(id);
  if (p.car != CarID::Invalid) {
    return;
  }
  // a JPH::Character is created from the entity's position, moving it means re-creating it
  const auto had_character = p.entity.has<ox::CharacterControllerComponent>();
  p.entity.remove<ox::CharacterControllerComponent>();
  p.position = position;
  p.teleport_seq++;
  self.set_entity_pose(p.entity, to3(position, 0.4f), yaw_quat(p.heading));
  if (had_character) {
    add_character(p.entity);
  }
}

auto World::player_position(this const World& self, PlayerID id) -> glm::vec2 {
  if (id == PlayerID::Invalid) {
    return {};
  }
  const auto& p = self.pl(id);
  if (p.car != CarID::Invalid) {
    return self.car_position(p.car);
  }
  return p.position;
}

auto World::in_play(this const World& self, PlayerID id) -> bool {
  if (id == PlayerID::Invalid) {
    return false;
  }
  const auto& p = self.pl(id);
  return p.active && p.life == Life::Alive;
}

auto World::on_foot(this const World& self, PlayerID id) -> bool {
  return self.in_play(id) && self.pl(id).car == CarID::Invalid;
}

auto World::nearest_player(this const World& self, glm::vec2 position, f32 range, bool wanted_only) -> PlayerID {
  auto best = PlayerID::Invalid;
  auto best_d = range;
  for (auto id : ALL_PLAYERS) {
    if (!self.in_play(id) || (wanted_only && self.stars(id) == 0)) {
      continue;
    }
    const auto d = glm::distance(self.player_position(id), position);
    if (d < best_d) {
      best_d = d;
      best = id;
    }
  }
  return best;
}

auto World::distance_to_players(this const World& self, glm::vec2 position) -> f32 {
  auto best = std::numeric_limits<f32>::max();
  for (auto id : ALL_PLAYERS) {
    if (self.pl(id).active) {
      best = glm::min(best, glm::distance(self.player_position(id), position));
    }
  }
  return best;
}

auto World::listener_position(this const World& self) -> glm::vec2 {
  if (self.local != PlayerID::Invalid && self.pl(self.local).active) {
    return self.player_position(self.local);
  }
  return to2(self.camera_position);
}

auto World::damage_player(this World& self, PlayerID id, f32 amount, PlayerID attacker) -> void {
  if (!self.in_play(id) || (self.local == id && self.state != GameState::Playing && self.role == NetRole::Offline)) {
    return;
  }
  auto& p = self.pl(id);
  if (p.invulnerable > 0.0f) {
    return;
  }
  p.health -= amount;
  if (self.is_local(id)) {
    // getting hurt reads on screen too: a smaller flash and a jolt
    self.juice.flash = glm::max(self.juice.flash, 0.6f);
    self.juice.shake = glm::min(1.0f, self.juice.shake + 0.25f);
  }
  if (p.car == CarID::Invalid) {
    // and you bleed like anyone else
    const auto from = attacker != PlayerID::Invalid ? p.position - self.player_position(attacker) : glm::vec2(0.0f);
    self.blood_burst(p.position, from, 14);
  }
  if (p.health <= 0.0f) {
    p.health = 0.0f;
    self.kill_player(id, attacker);
  }
}

auto World::update_players(this World& self, f32 dt) -> void {
  for (auto id : ALL_PLAYERS) {
    auto& p = self.pl(id);
    if (!p.active) {
      continue;
    }
    if (p.combo_timer > 0.0f) {
      p.combo_timer -= dt;
      if (p.combo_timer <= 0.0f) {
        p.combo = 0;
      }
    }
    if (p.life != Life::Alive) {
      // FLATLINED / ARRESTED on screen for a few seconds, then back to the hospital or out on bail
      p.life_timer += dt;
      if (p.life_timer > 4.0f && self.authority()) {
        self.respawn_player(id);
      }
      continue;
    }
    // offline the menus pause the player; online the city (and you in it) keeps going behind them
    if (self.is_local(id) && self.role == NetRole::Offline && self.state != GameState::Playing) {
      continue;
    }
    self.update_player(id, dt);
  }
}

auto World::update_player(this World& self, PlayerID id, f32 dt) -> void {
  ZoneScoped;

  auto& p = self.pl(id);
  const auto& input = p.input;
  const auto local = self.is_local(id);
  p.attack_cooldown = glm::max(0.0f, p.attack_cooldown - dt);
  p.invulnerable = glm::max(0.0f, p.invulnerable - dt);
  p.punch_anim = glm::max(0.0f, p.punch_anim - dt * 4.0f);

  if (input.switch_weapon) {
    // fists -> knife -> pistol (while there's ammo) -> fists
    switch (p.weapon) {
      case Weapon::Fists : p.weapon = Weapon::Knife; break;
      case Weapon::Knife : p.weapon = p.ammo > 0 ? Weapon::Pistol : Weapon::Fists; break;
      case Weapon::Pistol: p.weapon = Weapon::Fists; break;
    }
  }
  self.show_knife(id);

  if (input.enter_exit) {
    self.enter_or_exit_car(id);
  }

  if (local) {
    self.prompt_text.clear();
  }
  if (p.car != CarID::Invalid) {
    // driving is handled with the rest of the vehicles, only the horn lives here
    if (input.horn && glm::fract(self.time * 2.0f) < 0.1f) {
      self.play_at(self.assets.sfx_horn, self.car_position(p.car), 0.7f);
    }
    return;
  }

  if (!character_of(p.entity)) {
    return;
  }
  if (p.remote) {
    self.follow_report(id, dt);
  } else {
    self.move_on_foot(id, dt);
  }

  if (input.attack && p.attack_cooldown <= 0.0f) {
    // shoot (or swing) exactly at the cursor: the shot leaves along the heading, so point it at the aim first
    if (input.has_aim && glm::distance2(input.aim, p.position) > 0.01f) {
      p.heading = heading_of(input.aim - p.position);
    }
    self.player_attack(id);
  }

  // mugging: walk up to someone and hold E
  auto prompt = std::string{};
  for (usize i = 0; i < self.peds.size(); i++) {
    auto& ped = self.peds[i];
    if (!ped.alive || ped.kind != PedKind::Civilian || ped.state == PedState::Driving || ped.cash <= 0) {
      continue;
    }
    if (glm::distance(ped.position, p.position) > 1.9f) {
      continue;
    }
    prompt = "HOLD E: ROB";
    if (input.interact) {
      self.spawn_pickup(ped.position + forward_of(ped.heading) * 0.6f, ped.cash, 0);
      ped.cash = 0;
      ped.state = PedState::Flee;
      ped.timer = 8.0f;
      p.stats.peds_robbed++;
      self.commit_crime(id, 0.6f, p.position, "MUGGING! THAT'S ONE WAY TO MAKE A LIVING.");
    }
    break;
  }

  if (prompt.empty()) {
    for (usize i = 0; i < self.cars.size(); i++) {
      const auto& c = self.cars[i];
      if (c.alive && glm::distance(self.car_position(static_cast<CarID>(i)), p.position) < ENTER_RANGE) {
        prompt = c.driver != PedID::Invalid || c.player_driver != PlayerID::Invalid ? "F: CARJACK" : "F: GET IN";
        break;
      }
    }
  }
  if (local) {
    self.prompt_text = prompt;
  }
}

auto World::show_knife(this World& self, PlayerID id) -> void {
  // show the blade only while it's out (a 1 mm knife is as good as none, and keeps the transform well formed)
  auto& p = self.pl(id);
  if (p.knife && p.knife.is_alive()) {
    const auto want = p.weapon == Weapon::Knife ? glm::vec3(1.0f) : glm::vec3(0.001f);
    if (p.knife.get<ox::TransformComponent>().scale != want) {
      auto& tc = p.knife.get_mut<ox::TransformComponent>();
      tc.scale = want;
      tc.position = KNIFE_IN_HAND;
      p.knife.modified<ox::TransformComponent>();
    }
  }
}

auto World::move_on_foot(this World& self, PlayerID id, f32 dt) -> void {
  auto& p = self.pl(id);
  const auto& input = p.input;
  auto* character = character_of(p.entity);
  if (!character) {
    return;
  }

  const auto pos = character->GetPosition();
  p.position = {pos.GetX(), pos.GetZ()};

  // screen relative: the camera looks down with -z towards the top of the screen
  auto move = glm::vec2(input.move.x, -input.move.y);
  const auto speed = input.sprint ? SPRINT_SPEED : WALK_SPEED;
  auto velocity = move * speed;
  // with the pistol out you face the cursor (and can walk backwards while shooting), with fists you face where
  // you're walking
  const auto aiming = input.has_aim && p.weapon != Weapon::Fists && glm::distance2(input.aim, p.position) > 0.01f;
  if (aiming) {
    p.heading += wrap_angle(heading_of(input.aim - p.position) - p.heading) * glm::min(1.0f, dt * 20.0f);
  } else if (glm::length2(move) > 0.01f) {
    const auto target = heading_of(move);
    p.heading += wrap_angle(target - p.heading) * glm::min(1.0f, dt * 14.0f);
  }
  // swinging turns you towards the cursor at once, on the client too so the arm and the shot agree
  if (input.attack && p.attack_cooldown <= 0.0f && input.has_aim && glm::distance2(input.aim, p.position) > 0.01f) {
    p.heading = heading_of(input.aim - p.position);
  }

  const auto current = character->GetLinearVelocity();
  // JPH::Character is a real dynamic body, gravity keeps working as long as y is left alone
  character->SetLinearVelocity(JPH::Vec3(velocity.x, current.GetY(), velocity.y));
  const auto q = yaw_quat(p.heading);
  character->SetRotation(JPH::Quat(q.x, q.y, q.z, q.w));

  // walk cycle, and footsteps for your own feet only (everyone else's would be a drumroll)
  const auto moving = glm::length(velocity);
  p.anim_speed = moving;
  p.walk_phase += dt * moving * 2.2f;
  if (moving > 0.5f && self.is_local(id)) {
    p.footstep_timer -= dt * moving / WALK_SPEED;
    if (p.footstep_timer <= 0.0f) {
      p.footstep_timer = 0.38f;
      self.play(self.assets.sfx_footstep, 0.35f, self.random_float(0.9f, 1.1f));
    }
  }
  self.animate_limbs(p.limbs, p.walk_phase, glm::min(1.0f, moving / WALK_SPEED), p.punch_anim);
}

auto World::follow_report(this World& self, PlayerID id, f32 dt) -> void {
  // a remote player walks on their own machine; here their character goes where they say, as long as that's
  // somewhere they could have walked to since the last report. A stale report (from before the host last moved
  // them) is ignored until the client has caught up with the teleport
  auto& p = self.pl(id);
  auto* character = character_of(p.entity);
  if (!character || !p.has_report || p.reported_teleport != p.teleport_seq) {
    p.anim_speed = 0.0f;
    self.animate_limbs(p.limbs, p.walk_phase, 0.0f, p.punch_anim);
    return;
  }
  const auto pos = character->GetPosition();
  p.position = {pos.GetX(), pos.GetZ()};
  auto step = p.reported_position - p.position;
  const auto max_step = SPRINT_SPEED * 1.5f * glm::max(dt, 1.0f / 30.0f) + 0.05f;
  if (glm::length(step) > max_step) {
    step = glm::normalize(step) * max_step;
  }
  auto next = p.position + step;
  if (self.is_solid(next, 0.25f)) {
    next = p.position;
  }
  character->SetPosition(JPH::RVec3(next.x, pos.GetY(), next.y));
  character->SetLinearVelocity(JPH::Vec3(0.0f, character->GetLinearVelocity().GetY(), 0.0f));
  p.heading = p.reported_heading;
  const auto q = yaw_quat(p.heading);
  character->SetRotation(JPH::Quat(q.x, q.y, q.z, q.w));
  const auto moving = glm::length(next - p.position) / glm::max(dt, 0.0001f);
  p.position = next;
  p.anim_speed = moving;
  p.walk_phase += dt * moving * 2.2f;
  self.animate_limbs(p.limbs, p.walk_phase, glm::min(1.0f, moving / WALK_SPEED), p.punch_anim);
}

auto World::player_attack(this World& self, PlayerID id) -> void {
  auto& p = self.pl(id);
  const auto fwd = forward_of(p.heading);
  // melee targets: anyone standing within `range` inside the arc in front
  auto in_reach = [&](glm::vec2 target, f32 range, f32 arc) {
    const auto to = target - p.position;
    const auto d = glm::length(to);
    return d <= range && d > 0.001f && glm::dot(to / d, fwd) >= arc;
  };
  auto other_players_in_reach = [&](f32 range, f32 arc) {
    auto hits = std::vector<PlayerID>{};
    for (auto other : ALL_PLAYERS) {
      if (other != id && self.on_foot(other) && in_reach(self.pl(other).position, range, arc)) {
        hits.push_back(other);
      }
    }
    return hits;
  };

  if (p.weapon == Weapon::Pistol && p.ammo > 0) {
    p.attack_cooldown = 0.28f;
    p.ammo--;
    p.punch_anim = 0.6f;
    if (self.is_local(id)) {
      self.juice.shake = glm::min(1.0f, self.juice.shake + 0.12f);
    }
    self.shell_casing(p.position, p.heading);
    self.shoot(p.position + fwd * 0.5f, p.heading, 50.0f, id);
    if (p.ammo == 0) {
      p.weapon = Weapon::Fists;
      self.pager_to(id, "OUT OF AMMO. FISTS IT IS.");
    }
    return;
  }

  if (p.weapon == Weapon::Knife) {
    // one quick slash across an arc in front: anyone in it goes down, it's a knife
    p.attack_cooldown = 0.3f;
    p.punch_anim = 1.0f;
    self.play_at(self.assets.sfx_knife_swing, p.position, 0.6f, self.random_float(0.9f, 1.15f), 25.0f);
    auto hits = 0;
    for (usize i = 0; i < self.peds.size(); i++) {
      auto& ped = self.peds[i];
      if (!ped.alive || ped.state == PedState::Driving || !in_reach(ped.position, KNIFE_RANGE, KNIFE_ARC)) {
        continue;
      }
      self.damage_ped(static_cast<PedID>(i), 500.0f, p.position, id);
      self.commit_crime(id, ped.kind == PedKind::Civilian ? 0.5f : 1.4f, p.position, "");
      hits++;
    }
    for (auto other : other_players_in_reach(KNIFE_RANGE, KNIFE_ARC)) {
      self.damage_player(other, PVP_KNIFE, id);
      self.commit_crime(id, 0.6f, p.position, "");
      hits++;
    }
    if (hits > 0) {
      self.play_at(self.assets.sfx_stab, p.position, 0.9f, self.random_float(0.9f, 1.1f), 30.0f);
    }
    return;
  }

  p.attack_cooldown = 0.45f;
  p.punch_anim = 1.0f;
  auto hit_anyone = false;
  for (usize i = 0; i < self.peds.size(); i++) {
    auto& ped = self.peds[i];
    if (!ped.alive || ped.state == PedState::Driving || !in_reach(ped.position, PUNCH_RANGE, 0.4f)) {
      continue;
    }
    self.damage_ped(static_cast<PedID>(i), 22.0f, p.position, id);
    self.commit_crime(id, ped.kind == PedKind::Civilian ? 0.25f : 1.0f, p.position, "");
    hit_anyone = true;
    break;
  }
  if (!hit_anyone) {
    if (auto others = other_players_in_reach(PUNCH_RANGE, 0.4f); !others.empty()) {
      self.damage_player(others.front(), PVP_PUNCH, id);
      self.commit_crime(id, 0.2f, p.position, "");
      hit_anyone = true;
    }
  }
  if (hit_anyone) {
    self.play_at(self.assets.sfx_punch, p.position, 0.9f, self.random_float(0.9f, 1.1f), 30.0f);
  }
}

auto World::eject_player(this World& self, PlayerID id) -> void {
  auto& p = self.pl(id);
  if (p.car == CarID::Invalid) {
    return;
  }
  auto& c = self.car(p.car);
  const auto cpos = self.car_position(p.car);
  const auto heading = self.car_heading(p.car);
  const auto left = -right_of(forward_of(heading));
  auto exit_pos = cpos + left * 1.9f;
  if (self.is_solid(exit_pos, 0.4f)) {
    exit_pos = cpos - left * 1.9f;
  }
  c.player_driver = PlayerID::Invalid;
  c.role = CarRole::Abandoned;
  self.drive_car(p.car, 0.0f, 0.0f, 1.0f, 1.0f);
  p.car = CarID::Invalid;
  p.position = exit_pos;
  p.heading = heading;
  p.teleport_seq++;
  self.set_entity_pose(p.entity, to3(exit_pos, 0.4f), yaw_quat(heading));
  // the observer creates the JPH::Character from the entity's current world position
  add_character(p.entity);
}

auto World::enter_or_exit_car(this World& self, PlayerID id) -> void {
  auto& p = self.pl(id);

  if (p.car != CarID::Invalid) {
    self.eject_player(id);
    self.play_at(self.assets.sfx_door, p.position, 0.8f);
    return;
  }

  auto best = CarID::Invalid;
  auto best_d = ENTER_RANGE;
  for (usize i = 0; i < self.cars.size(); i++) {
    if (!self.cars[i].alive) {
      continue;
    }
    const auto d = glm::distance(self.car_position(static_cast<CarID>(i)), p.position);
    if (d < best_d) {
      best_d = d;
      best = static_cast<CarID>(i);
    }
  }
  if (best == CarID::Invalid) {
    return;
  }

  auto& c = self.car(best);
  const auto cpos = self.car_position(best);
  const auto heading = self.car_heading(best);
  if (c.driver != PedID::Invalid) {
    // carjack: drag the driver out and throw them on the pavement
    auto& driver = self.ped(c.driver);
    const auto out = cpos - right_of(forward_of(heading)) * 2.2f;
    driver.position = out;
    driver.car = CarID::Invalid;
    driver.heading = heading;
    if (driver.kind == PedKind::Cop) {
      driver.state = PedState::Chase;
      self.commit_crime(id, 2.0f, p.position, "YOU JUST CARJACKED A COP. BOLD.");
    } else {
      driver.state = PedState::Flee;
      driver.timer = 10.0f;
      self.commit_crime(id, 0.8f, p.position, "");
    }
    self.set_entity_pose(driver.entity, to3(out, 0.15f), yaw_quat(heading));
    c.driver = PedID::Invalid;
    self.play_at(self.assets.sfx_punch, cpos, 0.8f);
  }
  if (c.player_driver != PlayerID::Invalid) {
    // someone else's ride: they end up on the kerb
    const auto victim = c.player_driver;
    self.eject_player(victim);
    self.commit_crime(id, 0.5f, p.position, "");
    self.pager_to(victim, fmt::format("{} JUST TOOK YOUR CAR.", p.name));
    self.play_at(self.assets.sfx_punch, cpos, 0.8f);
  }

  if (c.role != CarRole::Abandoned || c.model == "police") {
    p.stats.cars_stolen++;
    self.pager_to(id, fmt::format("NICE WHEELS. YOU STOLE A {}.", c.display_name));
  }
  c.player_driver = id;
  if (c.role != CarRole::Police) {
    c.role = CarRole::Abandoned;
  }
  p.car = best;
  p.teleport_seq++;
  // the JPH::Character goes away with the component (observer), the body stays out of the car's way
  p.entity.remove<ox::CharacterControllerComponent>();
  self.hide_entity(p.entity);
  self.play_at(self.assets.sfx_door, cpos, 0.8f);
}
} // namespace oxcity
