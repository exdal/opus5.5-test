// Blood, particles and "game feel". Particles go through the engine's particle systems (built in
// ParticleAssets.cpp, cooked like any other asset), fired as bursts from a few pooled emitter entities. Blood pools on
// the ground are plain alpha masked models. Kills get the Hotline Miami treatment: a few frames of hit-stop, camera
// shake, a red flash and a combo counter.

#include <glm/common.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtx/norm.hpp>

#include "Scene/Scene.hpp"
#include "Utils/Log.hpp"
#include "World.hpp"

namespace oxcity {
static constexpr usize BLOOD_EMITTERS = 6;
static constexpr usize MUZZLE_EMITTERS = 3;
static constexpr usize SPARK_EMITTERS = 4;
static constexpr usize MAX_DECALS = 64;
static constexpr f32 COMBO_WINDOW = 3.0f;
static constexpr f32 PARKED_Y = -600.0f; // idle emitters and unused decals wait under the map

static auto make_pool(World& self, const ox::UUID& system, usize count) -> FxPool {
  auto pool = FxPool{};
  if (!system) {
    return pool;
  }
  for (usize i = 0; i < count; i++) {
    auto e = self.scene->create_particle_system_entity(system);
    if (!e) {
      OX_LOG_WARN("OxCity: couldn't create a particle emitter for {}", system.str());
      break;
    }
    self.set_entity_pose(e, glm::vec3(0.0f, PARKED_Y, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    pool.emitters.push_back(e);
  }
  return pool;
}

auto World::init_fx(this World& self) -> void {
  self.fx_blood = make_pool(self, self.assets.fx_blood, BLOOD_EMITTERS);
  self.fx_muzzle = make_pool(self, self.assets.fx_muzzle, MUZZLE_EMITTERS);
  self.fx_sparks = make_pool(self, self.assets.fx_sparks, SPARK_EMITTERS);
  OX_LOG_INFO(
    "OxCity: particle emitters: {} blood, {} muzzle, {} sparks",
    self.fx_blood.emitters.size(),
    self.fx_muzzle.emitters.size(),
    self.fx_sparks.emitters.size()
  );
}

auto World::emit(this World& self, FxPool& pool, glm::vec3 position, glm::vec3 velocity, u32 count) -> void {
  if (pool.emitters.empty() || count == 0) {
    return;
  }
  auto e = pool.emitters[pool.next];
  pool.next = (pool.next + 1) % pool.emitters.size();
  if (!e.is_alive()) {
    return;
  }
  self.set_entity_pose(e, position, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
  // the push the burst inherits (away from the hit). Written without `modified`: the component's OnSet observer
  // is for swapping the asset, the renderer reads the offset every frame anyway
  e.get_mut<ox::ParticleSystemComponent>().velocity_offset = velocity;
  self.scene->emit_particle_burst(e, count);
}

auto World::blood_burst(this World& self, glm::vec2 position, glm::vec2 direction, u32 count) -> void {
  const auto dir = glm::length2(direction) > 0.0001f ? glm::normalize(direction) : glm::vec2(0.0f);
  self.emit(self.fx_blood, to3(position, 1.0f), glm::vec3(dir.x, 0.4f, dir.y) * 3.0f, count);
}

auto World::blood_decal(this World& self, glm::vec2 position, f32 size) -> void {
  const auto variant = self.assets.blood_decals[static_cast<usize>(self.random_int(0, 3))];
  if (!variant) {
    return;
  }
  // a pool on the ground: slightly above it, and each decal a hair higher than the last so overlapping pools don't
  // z-fight
  const auto lift = 0.012f + static_cast<f32>(self.next_decal % 16) * 0.0006f;
  const auto yaw = self.random_float(0.0f, glm::two_pi<f32>());
  const auto scale = glm::vec3(size, 1.0f, size);
  if (self.decals.size() < MAX_DECALS) {
    auto e = self.spawn_model(variant, to3(position, lift), yaw, scale);
    if (e) {
      self.decals.push_back(e);
    }
    self.next_decal++;
    return;
  }
  // full: the oldest pool moves here
  auto& e = self.decals[self.next_decal % MAX_DECALS];
  self.next_decal++;
  if (!e.is_alive()) {
    return;
  }
  auto& tc = e.get_mut<ox::TransformComponent>();
  tc.position = to3(position, lift);
  tc.rotation = glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f));
  tc.scale = scale;
  e.modified<ox::TransformComponent>();
}

auto World::muzzle_flash(this World& self, glm::vec2 muzzle, f32 heading) -> void {
  const auto fwd = forward_of(heading);
  self.emit(self.fx_muzzle, to3(muzzle, BULLET_HEIGHT), glm::vec3(fwd.x, 0.0f, fwd.y) * 4.0f, 10);
}

auto World::impact_sparks(this World& self, glm::vec3 position, glm::vec2 direction) -> void {
  const auto dir = glm::length2(direction) > 0.0001f ? glm::normalize(direction) : glm::vec2(0.0f);
  self.emit(self.fx_sparks, position, glm::vec3(dir.x, 0.2f, dir.y) * 2.0f, 16);
}

auto World::on_kill(this World& self, glm::vec2 position, glm::vec2 direction, bool by_player, bool big) -> void {
  self.blood_burst(position, direction, big ? 90 : 60);
  // the pool is thrown a little in the direction of the hit
  const auto dir = glm::length2(direction) > 0.0001f ? glm::normalize(direction) : glm::vec2(0.0f);
  self.blood_decal(position + dir * 0.5f, self.random_float(1.3f, 2.2f) * (big ? 1.4f : 1.0f));
  self.play(self.assets.sfx_splat, 0.7f, self.random_float(0.85f, 1.1f));

  if (!by_player) {
    return;
  }
  auto& j = self.juice;
  j.hitstop = glm::max(j.hitstop, big ? 0.11f : 0.07f);
  j.shake = glm::min(1.0f, j.shake + (big ? 0.55f : 0.4f));
  j.flash = 1.0f;
  j.combo = j.combo_timer > 0.0f ? j.combo + 1 : 1;
  j.combo_timer = COMBO_WINDOW;
  j.combo_pop = 1.0f;
  const auto points = 100 * j.combo * (big ? 2 : 1);
  j.score += points;
  self.hud.score_popup = fmt::format("+{}", points);
}

auto World::sim_delta(this World& self, f32 real_dt) -> f32 {
  // hit-stop: the world crawls at 5% for a few frames. Not zero: a zero delta makes flecs measure its own frame time
  if (self.juice.hitstop > 0.0f) {
    self.juice.hitstop -= real_dt;
    return real_dt * 0.05f;
  }
  return real_dt;
}

auto World::update_fx(this World& self, f32 real_dt) -> void {
  auto& j = self.juice;
  j.shake = glm::max(0.0f, j.shake - real_dt * 1.8f);
  j.flash = glm::max(0.0f, j.flash - real_dt * 4.0f);
  j.combo_pop = glm::max(0.0f, j.combo_pop - real_dt * 5.0f);
  if (j.combo_timer > 0.0f) {
    j.combo_timer -= real_dt;
    if (j.combo_timer <= 0.0f) {
      j.combo = 0;
      self.hud.score_popup = "";
    }
  }

  auto& h = self.hud;
  h.kill_flash = j.flash * 0.35f;
  h.score = j.score;
  h.combo = j.combo >= 2 ? fmt::format("{}X COMBO", j.combo) : "";
  h.combo_scale = 1.0f + j.combo_pop * 0.6f;
  h.combo_tilt = -6.0f + std::sin(self.time * 7.0f) * 3.0f;
}
} // namespace oxcity
