// Pedestrians, cops on foot and bank guards. Peds are kinematic: they walk the sidewalk rings of the city
// blocks and steer around the building footprints themselves instead of going through Jolt, a crowd of
// JPH::Characters would cost more than the rest of the game together on a software rasterizer box.

#include <glm/common.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtx/norm.hpp>

#include "Scene/Components.hpp"
#include "Scene/Scene.hpp"
#include "World.hpp"

namespace oxcity {
static constexpr f32 PED_WALK = 1.4f;
static constexpr f32 PED_RUN = 5.0f;
static constexpr f32 COP_RUN = 5.6f;
static constexpr f32 PED_RADIUS = 0.35f;

static auto block_center_of(const World& world, glm::vec2 p) -> glm::vec2 {
  // the superblock whose sidewalk ring is closest to p
  const auto tile = world.world_to_tile(p);
  const auto bx = glm::clamp(tile.x / ROAD_EVERY, 0, CITY_TILES / ROAD_EVERY - 1);
  const auto bz = glm::clamp(tile.y / ROAD_EVERY, 0, CITY_TILES / ROAD_EVERY - 1);
  return (world.tile_center(bx * ROAD_EVERY + 1, bz * ROAD_EVERY + 1) + world.tile_center(bx * ROAD_EVERY + 2, bz * ROAD_EVERY + 2)) *
         0.5f;
}

static auto ground_height(const World& world, glm::vec2 p) -> f32 {
  const auto t = world.world_to_tile(p);
  if (t.x < 0 || t.y < 0 || t.x >= CITY_TILES || t.y >= CITY_TILES) {
    return 0.0f;
  }
  return world.is_road_tile(t.x, t.y) ? 0.0f : 0.15f;
}

static auto next_wander_target(World& world, const Ped& ped) -> glm::vec2 {
  auto center = block_center_of(world, ped.position);
  // now and then cross the street to the next block
  if (world.random_int(0, 9) == 0) {
    const auto dirs = std::array{glm::vec2(1, 0), glm::vec2(-1, 0), glm::vec2(0, 1), glm::vec2(0, -1)};
    const auto next = center + dirs[static_cast<usize>(world.random_int(0, 3))] * (TILE * static_cast<f32>(ROAD_EVERY));
    if (glm::abs(next.x) < TILE * CITY_TILES * 0.5f - TILE && glm::abs(next.y) < TILE * CITY_TILES * 0.5f - TILE) {
      center = next;
    }
  }
  // go to a corner of the ring, or the middle of one of its sides
  const auto ring = TILE - 0.9f;
  const auto corner = glm::vec2(world.random_int(0, 1) ? ring : -ring, world.random_int(0, 1) ? ring : -ring);
  if (world.random_int(0, 1)) {
    return center + corner;
  }
  return center + (world.random_int(0, 1) ? glm::vec2(corner.x, 0.0f) : glm::vec2(0.0f, corner.y));
}

auto World::animate_limbs(this World& self, Limbs& limbs, f32 phase, f32 amount, f32 punch) -> void {
  // limbs rotate around their pivots (hip, shoulder), negative x rotation swings them forward (+z)
  const auto swing = std::sin(phase) * 0.75f * amount;
  auto rotate = [&self](flecs::entity e, f32 angle) {
    if (!e || !e.is_alive()) {
      return;
    }
    auto& tc = e.get_mut<ox::TransformComponent>();
    tc.rotation = glm::angleAxis(angle, glm::vec3(1.0f, 0.0f, 0.0f));
    e.modified<ox::TransformComponent>();
  };
  rotate(limbs.leg_l, swing);
  rotate(limbs.leg_r, -swing);
  rotate(limbs.arm_l, -swing * 0.8f);
  rotate(limbs.arm_r, punch > 0.0f ? -1.6f * punch : swing * 0.8f);
}

auto World::panic_around(this World& self, glm::vec2 position, f32 radius) -> void {
  for (auto& p : self.peds) {
    if (p.alive && p.kind == PedKind::Civilian && p.state != PedState::Driving &&
        glm::distance(p.position, position) < radius) {
      p.state = PedState::Flee;
      p.timer = self.random_float(4.0f, 9.0f);
    }
  }
}

auto World::kill_ped(this World& self, PedID id, glm::vec2 impulse, bool by_player) -> void {
  auto& p = self.ped(id);
  if (!p.alive) {
    return;
  }
  // run overs (big impulse) get the bigger splash
  self.on_kill(p.position, impulse, by_player, glm::length(impulse) > 6.0f);
  p.alive = false;
  p.state = PedState::Dead;
  p.health = 0.0f;
  p.dead_time = 0.0f;
  p.velocity = impulse;
  if (glm::length2(impulse) > 0.01f) {
    p.heading = heading_of(-impulse); // fall backwards, away from whatever hit them
  }
  if (p.cash > 0) {
    self.spawn_pickup(p.position, p.cash, p.kind == PedKind::Civilian ? 0 : 12);
    p.cash = 0;
  }
  self.stats.peds_killed++;
  self.panic_around(p.position, 18.0f);
}

auto World::damage_ped(this World& self, PedID id, f32 amount, glm::vec2 from, bool by_player) -> void {
  auto& p = self.ped(id);
  if (!p.alive) {
    return;
  }
  p.health -= amount;
  const auto away = p.position - from;
  const auto dir = glm::length2(away) > 0.001f ? glm::normalize(away) : glm::vec2(0.0f, 1.0f);
  if (p.health <= 0.0f) {
    self.kill_ped(id, dir * 5.0f, by_player);
    return;
  }
  // wounded: a spurt, and a few drops on the pavement
  self.blood_burst(p.position, dir, 45);
  self.blood_streak(p.position, dir, self.random_float(1.2f, 2.0f), self.random_float(0.6f, 0.9f));
  // knock back a step
  const auto pushed = p.position + dir * 0.4f;
  if (!self.is_solid(pushed, PED_RADIUS)) {
    p.position = pushed;
  }
  if (p.kind == PedKind::Civilian) {
    p.state = PedState::Flee;
    p.timer = 8.0f;
  } else {
    p.state = PedState::Attack;
  }
}

auto World::update_peds(this World& self, f32 dt) -> void {
  ZoneScoped;

  const auto player_pos = self.player_position();
  const auto player_on_foot = self.player.car == CarID::Invalid && self.state == GameState::Playing;
  auto civilians_alive = 0;

  for (usize i = 0; i < self.peds.size(); i++) {
    const auto id = static_cast<PedID>(i);
    auto& p = self.peds[i];
    if (!p.entity.is_valid() || !p.entity.is_alive()) {
      continue;
    }
    if (p.state == PedState::Driving) {
      continue;
    }

    p.attack_cooldown = glm::max(0.0f, p.attack_cooldown - dt);

    if (!p.alive) {
      p.dead_time += dt;
      p.fall = glm::min(1.0f, p.fall + dt * 5.0f);
      p.velocity *= glm::max(0.0f, 1.0f - dt * 3.0f);
      const auto slid = p.position + p.velocity * dt;
      if (!self.is_solid(slid, PED_RADIUS)) {
        p.position = slid;
      }
      const auto lying = yaw_quat(p.heading) * glm::angleAxis(-glm::half_pi<f32>() * p.fall, glm::vec3(1.0f, 0.0f, 0.0f));
      self.set_entity_pose(p.entity, to3(p.position, ground_height(self, p.position) + 0.12f * p.fall), lying);
      // bodies are cleared away once nobody is looking
      if (p.dead_time > 20.0f && glm::distance(p.position, player_pos) > 45.0f) {
        p.entity.destruct();
        p.entity = {};
      }
      continue;
    }

    if (p.kind == PedKind::Civilian) {
      civilians_alive++;
    }

    // wounded: a trail of drops wherever they go
    if (p.health < (p.kind == PedKind::Civilian ? 40.0f : 80.0f)) {
      p.drip_timer -= dt;
      if (p.drip_timer <= 0.0f) {
        p.drip_timer = self.random_float(0.25f, 0.5f);
        self.blood_burst(p.position, p.velocity * 0.2f, 3);
        if (self.random_float(0.0f, 1.0f) < 0.35f) {
          self.blood_decal(p.position, self.random_float(0.25f, 0.45f));
        }
      }
    }

    auto desired = glm::vec2(0.0f);
    auto speed = PED_WALK;
    const auto to_player = player_pos - p.position;
    const auto player_dist = glm::length(to_player);

    switch (p.state) {
      case PedState::Idle: {
        if (p.kind == PedKind::Guard && self.heist.alarm > 0.0f && player_dist < 30.0f) {
          p.state = PedState::Attack;
        }
        break;
      }
      case PedState::Wander: {
        if (glm::distance(p.position, p.target) < 0.8f || p.timer <= 0.0f) {
          p.target = next_wander_target(self, p);
          p.timer = 25.0f;
        }
        p.timer -= dt;
        desired = p.target - p.position;
        break;
      }
      case PedState::Flee: {
        p.timer -= dt;
        speed = PED_RUN;
        desired = player_dist > 0.1f ? -to_player : glm::vec2(1.0f, 0.0f);
        if (p.timer <= 0.0f || player_dist > 40.0f) {
          p.state = PedState::Wander;
          p.target = next_wander_target(self, p);
        }
        break;
      }
      case PedState::Chase:
      case PedState::Attack: {
        speed = p.kind == PedKind::Cop ? COP_RUN : PED_RUN * 0.8f;
        const auto hunting = self.stars() > 0 || p.state == PedState::Attack;
        if (!hunting || self.state != GameState::Playing) {
          p.state = p.kind == PedKind::Guard ? PedState::Idle : PedState::Wander;
          break;
        }
        if (player_dist > 1.1f) {
          desired = to_player;
        }
        // cops shoot from three stars up, guards whenever the alarm rings
        const auto armed = p.kind == PedKind::Guard || self.stars() >= 3;
        if (armed && player_dist < 16.0f && p.attack_cooldown <= 0.0f) {
          // guards are nervous shots, the heist is meant to be survivable if you keep drilling
          const auto guard = p.kind == PedKind::Guard;
          p.attack_cooldown = guard ? self.random_float(1.2f, 2.2f) : self.random_float(0.8f, 1.5f);
          const auto spread = guard ? 0.2f : 0.12f;
          const auto aim_error = self.random_float(-spread, spread);
          self.shoot(p.position + glm::normalize(to_player) * 0.5f, heading_of(to_player) + aim_error, guard ? 4.0f : 6.0f, false);
          if (player_dist < 5.0f) {
            desired = {}; // stand and shoot
          }
        }
        break;
      }
      default: break;
    }

    if (glm::length2(desired) > 0.0001f) {
      const auto dir = glm::normalize(desired);
      p.heading += wrap_angle(heading_of(dir) - p.heading) * glm::min(1.0f, dt * 10.0f);
      auto step = forward_of(p.heading) * speed * dt;
      auto next = p.position + step;
      if (self.is_solid(next, PED_RADIUS)) {
        // slide along the wall, otherwise give up on this target
        const auto slide_x = p.position + glm::vec2(step.x, 0.0f);
        const auto slide_z = p.position + glm::vec2(0.0f, step.y);
        if (!self.is_solid(slide_x, PED_RADIUS)) {
          next = slide_x;
        } else if (!self.is_solid(slide_z, PED_RADIUS)) {
          next = slide_z;
        } else {
          next = p.position;
          p.target = next_wander_target(self, p);
        }
      }
      p.velocity = (next - p.position) / glm::max(dt, 0.0001f);
      p.position = next;
      p.walk_phase += dt * speed * 2.6f;
    } else {
      p.velocity = {};
    }

    self.animate_limbs(p.limbs, p.walk_phase, glm::min(1.0f, glm::length(p.velocity) / PED_WALK), p.attack_cooldown > 0.5f ? 1.0f : 0.0f);
    self.set_entity_pose(p.entity, to3(p.position, ground_height(self, p.position)), yaw_quat(p.heading));
    (void)id;
  }

  // keep the streets busy: top the population back up out of the player's sight
  if (civilians_alive < MAX_PEDS - 4) {
    for (i32 attempt = 0; attempt < 8; attempt++) {
      const auto p = self.random_sidewalk_point();
      if (glm::distance(p, player_pos) > 45.0f) {
        self.spawn_ped(PedKind::Civilian, p);
        break;
      }
    }
  }
  (void)player_on_foot;
}
} // namespace oxcity
