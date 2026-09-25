// The player on foot: movement through the engine's JPH::Character, melee and pistol, mugging, and getting in
// and out of cars (including throwing the driver out).

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

auto World::spawn_player(this World& self, glm::vec2 position) -> void {
  self.player.entity = self.spawn_model(self.assets.player, to3(position, 0.3f), 0.0f);
  self.player.limbs = self.find_limbs(self.player.entity);
  self.player.position = position;
  add_character(self.player.entity);

  // the knife rides on the right arm, so it swings with the slash animation for free
  if (self.assets.knife && self.player.limbs.arm_r) {
    self.player.knife = self.spawn_model(self.assets.knife, KNIFE_IN_HAND, 0.0f, glm::vec3(0.001f));
    if (self.player.knife) {
      self.player.knife.child_of(self.player.limbs.arm_r);
      self.player.knife.modified<ox::TransformComponent>();
    }
  }
}

auto World::teleport_player(this World& self, glm::vec2 position) -> void {
  if (self.player.car != CarID::Invalid) {
    return;
  }
  // a JPH::Character is created from the entity's position, moving it means re-creating it
  self.player.entity.remove<ox::CharacterControllerComponent>();
  self.player.position = position;
  self.set_entity_pose(self.player.entity, to3(position, 0.4f), yaw_quat(self.player.heading));
  add_character(self.player.entity);
}

auto World::player_position(this const World& self) -> glm::vec2 {
  if (self.player.car != CarID::Invalid) {
    return self.car_position(self.player.car);
  }
  return self.player.position;
}

auto World::damage_player(this World& self, f32 amount) -> void {
  if (self.state != GameState::Playing || self.player.invulnerable > 0.0f) {
    return;
  }
  self.player.health -= amount;
  // getting hurt reads on screen too: a smaller flash and a jolt, and you bleed like anyone else
  self.juice.flash = glm::max(self.juice.flash, 0.6f);
  self.juice.shake = glm::min(1.0f, self.juice.shake + 0.25f);
  if (self.player.car == CarID::Invalid) {
    self.blood_burst(self.player.position, {}, 14);
  }
  if (self.player.health <= 0.0f) {
    self.player.health = 0.0f;
    self.kill_player();
  }
}

auto World::update_player(this World& self, const GameInput& input, f32 dt) -> void {
  ZoneScoped;

  auto& p = self.player;
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
  // show the blade only while it's out (a 1 mm knife is as good as none, and keeps the transform well formed)
  if (p.knife && p.knife.is_alive()) {
    const auto want = p.weapon == Weapon::Knife ? glm::vec3(1.0f) : glm::vec3(0.001f);
    if (p.knife.get<ox::TransformComponent>().scale != want) {
      auto& tc = p.knife.get_mut<ox::TransformComponent>();
      tc.scale = want;
      tc.position = KNIFE_IN_HAND;
      p.knife.modified<ox::TransformComponent>();
    }
  }

  if (input.enter_exit) {
    self.enter_or_exit_car();
  }

  if (p.car != CarID::Invalid) {
    self.prompt_text.clear();
    // driving is handled with the rest of the vehicles, only the horn lives here
    if (input.horn && glm::fract(self.time * 2.0f) < 0.1f) {
      self.play(self.assets.sfx_horn, 0.7f);
    }
    return;
  }

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

  const auto current = character->GetLinearVelocity();
  // JPH::Character is a real dynamic body, gravity keeps working as long as y is left alone
  character->SetLinearVelocity(JPH::Vec3(velocity.x, current.GetY(), velocity.y));
  const auto q = yaw_quat(p.heading);
  character->SetRotation(JPH::Quat(q.x, q.y, q.z, q.w));

  // walk cycle and footsteps
  const auto moving = glm::length(velocity);
  p.walk_phase += dt * moving * 2.2f;
  if (moving > 0.5f) {
    p.footstep_timer -= dt * moving / WALK_SPEED;
    if (p.footstep_timer <= 0.0f) {
      p.footstep_timer = 0.38f;
      self.play(self.assets.sfx_footstep, 0.35f, self.random_float(0.9f, 1.1f));
    }
  }
  self.animate_limbs(p.limbs, p.walk_phase, glm::min(1.0f, moving / WALK_SPEED), p.punch_anim);

  if (input.attack && p.attack_cooldown <= 0.0f) {
    // shoot (or swing) exactly at the cursor: the shot leaves along the heading, so point it at the aim first
    if (input.has_aim && glm::distance2(input.aim, p.position) > 0.01f) {
      p.heading = heading_of(input.aim - p.position);
    }
    self.player_attack();
  }

  // mugging: walk up to someone and hold E
  self.prompt_text.clear();
  for (usize i = 0; i < self.peds.size(); i++) {
    auto& ped = self.peds[i];
    if (!ped.alive || ped.kind != PedKind::Civilian || ped.state == PedState::Driving || ped.cash <= 0) {
      continue;
    }
    if (glm::distance(ped.position, p.position) > 1.9f) {
      continue;
    }
    self.prompt_text = "HOLD E: ROB";
    if (input.interact) {
      self.spawn_pickup(ped.position + forward_of(ped.heading) * 0.6f, ped.cash, 0);
      ped.cash = 0;
      ped.state = PedState::Flee;
      ped.timer = 8.0f;
      self.stats.peds_robbed++;
      self.commit_crime(0.6f, p.position, "MUGGING! THAT'S ONE WAY TO MAKE A LIVING.");
    }
    break;
  }

  if (self.prompt_text.empty()) {
    for (usize i = 0; i < self.cars.size(); i++) {
      const auto& c = self.cars[i];
      if (c.alive && glm::distance(self.car_position(static_cast<CarID>(i)), p.position) < ENTER_RANGE) {
        self.prompt_text = c.driver != PedID::Invalid ? "F: CARJACK" : "F: GET IN";
        break;
      }
    }
  }
}

auto World::player_attack(this World& self) -> void {
  auto& p = self.player;
  if (p.weapon == Weapon::Pistol && p.ammo > 0) {
    p.attack_cooldown = 0.28f;
    p.ammo--;
    p.punch_anim = 0.6f;
    self.juice.shake = glm::min(1.0f, self.juice.shake + 0.12f);
    self.shell_casing(p.position, p.heading);
    self.shoot(p.position + forward_of(p.heading) * 0.5f, p.heading, 50.0f, true);
    if (p.ammo == 0) {
      p.weapon = Weapon::Fists;
      self.pager("OUT OF AMMO. FISTS IT IS.");
    }
    return;
  }

  if (p.weapon == Weapon::Knife) {
    // one quick slash across an arc in front: anyone in it goes down, it's a knife
    p.attack_cooldown = 0.3f;
    p.punch_anim = 1.0f;
    self.play(self.assets.sfx_knife_swing, 0.6f, self.random_float(0.9f, 1.15f));
    const auto fwd = forward_of(p.heading);
    auto hits = 0;
    for (usize i = 0; i < self.peds.size(); i++) {
      auto& ped = self.peds[i];
      if (!ped.alive || ped.state == PedState::Driving) {
        continue;
      }
      const auto to = ped.position - p.position;
      const auto d = glm::length(to);
      if (d > KNIFE_RANGE || d < 0.001f || glm::dot(to / d, fwd) < KNIFE_ARC) {
        continue;
      }
      self.damage_ped(static_cast<PedID>(i), 500.0f, p.position, true);
      self.commit_crime(ped.kind == PedKind::Civilian ? 0.5f : 1.4f, p.position, "");
      hits++;
    }
    if (hits > 0) {
      self.play(self.assets.sfx_stab, 0.9f, self.random_float(0.9f, 1.1f));
    }
    return;
  }

  p.attack_cooldown = 0.45f;
  p.punch_anim = 1.0f;
  auto hit_anyone = false;
  for (usize i = 0; i < self.peds.size(); i++) {
    auto& ped = self.peds[i];
    if (!ped.alive || ped.state == PedState::Driving) {
      continue;
    }
    const auto to = ped.position - p.position;
    const auto d = glm::length(to);
    if (d > PUNCH_RANGE || d < 0.001f || glm::dot(to / d, forward_of(p.heading)) < 0.4f) {
      continue;
    }
    self.damage_ped(static_cast<PedID>(i), 22.0f, p.position, true);
    self.commit_crime(ped.kind == PedKind::Civilian ? 0.25f : 1.0f, p.position, "");
    hit_anyone = true;
    break;
  }
  if (hit_anyone) {
    self.play(self.assets.sfx_punch, 0.9f, self.random_float(0.9f, 1.1f));
  }
}

auto World::enter_or_exit_car(this World& self) -> void {
  auto& p = self.player;

  if (p.car != CarID::Invalid) {
    auto& c = self.car(p.car);
    const auto cpos = self.car_position(p.car);
    const auto heading = self.car_heading(p.car);
    const auto left = -right_of(forward_of(heading));
    auto exit_pos = cpos + left * 1.9f;
    if (self.is_solid(exit_pos, 0.4f)) {
      exit_pos = cpos - left * 1.9f;
    }
    c.player_inside = false;
    c.role = CarRole::Abandoned;
    self.drive_car(p.car, 0.0f, 0.0f, 1.0f, 1.0f);
    p.car = CarID::Invalid;
    p.position = exit_pos;
    p.heading = heading;
    self.set_entity_pose(p.entity, to3(exit_pos, 0.4f), yaw_quat(heading));
    // the observer creates the JPH::Character from the entity's current world position
    add_character(p.entity);
    self.play(self.assets.sfx_door, 0.8f);
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
      self.commit_crime(2.0f, p.position, "YOU JUST CARJACKED A COP. BOLD.");
    } else {
      driver.state = PedState::Flee;
      driver.timer = 10.0f;
      self.commit_crime(0.8f, p.position, "");
    }
    self.set_entity_pose(driver.entity, to3(out, 0.15f), yaw_quat(heading));
    c.driver = PedID::Invalid;
    self.play(self.assets.sfx_punch, 0.8f);
  }

  if (c.role != CarRole::Abandoned || c.model == "police") {
    self.stats.cars_stolen++;
    self.pager(fmt::format("NICE WHEELS. YOU STOLE A {}.", c.display_name));
  }
  c.player_inside = true;
  if (c.role != CarRole::Police) {
    c.role = CarRole::Abandoned;
  }
  p.car = best;
  // the JPH::Character goes away with the component (observer), the body stays out of the car's way
  p.entity.remove<ox::CharacterControllerComponent>();
  self.hide_entity(p.entity);
  self.play(self.assets.sfx_door, 0.8f);
}
} // namespace oxcity
