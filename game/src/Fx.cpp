// Blood, particles and "game feel". Particles go through the engine's particle systems (built in
// ParticleAssets.cpp, cooked like any other asset), fired as bursts from a few pooled emitter entities. Blood pools on
// the ground are plain alpha masked models. Kills get the Hotline Miami treatment: a few frames of hit-stop, camera
// shake, a red flash and a combo counter.

#include <glm/common.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtx/norm.hpp>

#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"
#include "Scene/Scene.hpp"
#include "Utils/Log.hpp"
#include "World.hpp"

namespace oxcity {
static constexpr usize BLOOD_EMITTERS = 6;
static constexpr usize MUZZLE_EMITTERS = 3;
static constexpr usize SPARK_EMITTERS = 4;
static constexpr usize DECALS_PER_KIND = 20;
static constexpr f32 COMBO_WINDOW = 3.0f;
static constexpr f32 PARKED_Y = -600.0f; // idle emitters and unused decals wait under the map
static constexpr f32 EXPLOSION_RADIUS = 4.5f;
static constexpr usize FLASH_LIGHTS = 3;

static auto make_pool(World& self, const ox::UUID& system, usize count) -> FxPool {
  auto pool = FxPool{};
  if (!system) {
    return pool;
  }
  // every system here is burst only (spawn rate 0). When the .oxparticle can't be read the engine quietly swaps in its
  // default system, which loops at 32/s forever, and the game would fill the city with white dots. Say so loudly
  if (!ox::App::mod<ox::AssetManager>().load_asset(system)) {
    OX_LOG_ERROR("OxCity: particle system {} doesn't load", system.str());
    return pool;
  }
  if (auto ps = ox::App::mod<ox::AssetManager>().get_particle_system(system); ps && ps->emitter.spawn_rate > 0.0f) {
    OX_LOG_ERROR("OxCity: particle system {} came back as the engine default, is the .oxparticle installed?", system.str());
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
  self.fx_explosion = make_pool(self, self.assets.fx_explosion, 2);
  self.fx_smoke = make_pool(self, self.assets.fx_smoke, 6);
  self.fx_tire = make_pool(self, self.assets.fx_tire, 6);
  self.fx_casings = make_pool(self, self.assets.fx_casings, 2);
  self.fx_sparkle = make_pool(self, self.assets.fx_sparkle, 2);
  OX_LOG_INFO(
    "OxCity: particle emitters: {} blood, {} muzzle, {} sparks, {} explosion, {} smoke, {} tire, {} casings, {} sparkle",
    self.fx_blood.emitters.size(),
    self.fx_muzzle.emitters.size(),
    self.fx_sparks.emitters.size(),
    self.fx_explosion.emitters.size(),
    self.fx_smoke.emitters.size(),
    self.fx_tire.emitters.size(),
    self.fx_casings.emitters.size(),
    self.fx_sparkle.emitters.size()
  );

  // explosions light up the street for a moment: a few shadowless point lights, parked until needed
  for (usize i = 0; i < FLASH_LIGHTS; i++) {
    auto e = self.scene->create_entity(fmt::format("explosion_flash_{}", i));
    self.set_entity_pose(e, glm::vec3(0.0f, PARKED_Y, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    e.set<ox::LightComponent>({
      .type = ox::LightComponent::LightType::Point,
      .color = {1.0f, 0.55f, 0.2f},
      .intensity = 0.0f,
      .radius = 16.0f,
      .cast_shadows = false,
    });
    self.flashes.push_back({.entity = e});
  }
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
  // thrown hard along the hit, like it came out the other side
  self.emit(self.fx_blood, to3(position, 1.0f), glm::vec3(dir.x * 5.5f, 1.2f, dir.y * 5.5f), count);
}

auto World::place_decal(this World& self, const ox::UUID& model, glm::vec2 position, f32 yaw, glm::vec3 scale) -> void {
  if (!model) {
    return;
  }
  // slightly above the ground, and each decal a hair higher than the last so overlapping splats don't z-fight
  const auto lift = 0.012f + static_cast<f32>(self.decals_placed % 24) * 0.0006f;
  self.decals_placed++;

  auto* ring = static_cast<DecalRing*>(nullptr);
  for (auto& r : self.decal_rings) {
    if (r.model == model) {
      ring = &r;
    }
  }
  if (!ring) {
    ring = &self.decal_rings.emplace_back(DecalRing{.model = model});
  }

  if (ring->entities.size() < DECALS_PER_KIND) {
    if (auto e = self.spawn_model(model, to3(position, lift), yaw, scale)) {
      ring->entities.push_back(e);
    }
    return;
  }
  // full: the oldest of this kind moves here
  auto e = ring->entities[ring->next];
  ring->next = (ring->next + 1) % ring->entities.size();
  if (!e.is_alive()) {
    return;
  }
  auto& tc = e.get_mut<ox::TransformComponent>();
  tc.position = to3(position, lift);
  tc.rotation = yaw_quat(yaw);
  tc.scale = scale;
  e.modified<ox::TransformComponent>();
}

auto World::blood_decal(this World& self, glm::vec2 position, f32 size) -> void {
  const auto model = self.assets.blood_decals[static_cast<usize>(self.random_int(0, 3))];
  self.place_decal(model, position, self.random_float(0.0f, glm::two_pi<f32>()), glm::vec3(size, 1.0f, size));
}

auto World::blood_streak(this World& self, glm::vec2 position, glm::vec2 direction, f32 length, f32 width) -> void {
  if (glm::length2(direction) < 0.0001f) {
    direction = forward_of(self.random_float(0.0f, glm::two_pi<f32>()));
  }
  // a little wobble so repeated hits from the same side don't stamp the same shape
  const auto yaw = heading_of(direction) + self.random_float(-0.15f, 0.15f);
  const auto model = self.assets.blood_streaks[static_cast<usize>(self.random_int(0, 3))];
  // the model is 1 m wide and 2 m long
  self.place_decal(model, position, yaw, glm::vec3(width, 1.0f, length * 0.5f));
}

auto World::muzzle_flash(this World& self, glm::vec2 muzzle, f32 heading) -> void {
  const auto fwd = forward_of(heading);
  self.emit(self.fx_muzzle, to3(muzzle, BULLET_HEIGHT), glm::vec3(fwd.x, 0.0f, fwd.y) * 4.0f, 10);
}

auto World::impact_sparks(this World& self, glm::vec3 position, glm::vec2 direction) -> void {
  const auto dir = glm::length2(direction) > 0.0001f ? glm::normalize(direction) : glm::vec2(0.0f);
  self.emit(self.fx_sparks, position, glm::vec3(dir.x, 0.2f, dir.y) * 2.0f, 16);
}

auto World::explode(this World& self, glm::vec2 position, bool by_player) -> void {
  // above the roof: particles are depth tested against the scene, emitted inside the car body they'd stay hidden
  const auto at = to3(position, 2.2f);
  self.emit(self.fx_explosion, at, glm::vec3(0.0f, 2.0f, 0.0f), 120);
  self.emit(self.fx_smoke, at, glm::vec3(0.0f, 1.5f, 0.0f), 50);
  self.emit(self.fx_sparks, at, glm::vec3(0.0f, 3.0f, 0.0f), 80);
  {
    const auto size = self.random_float(6.5f, 8.0f);
    self.place_decal(self.assets.scorch_decals[static_cast<usize>(self.random_int(0, 1))], position,
                     self.random_float(0.0f, glm::two_pi<f32>()), glm::vec3(size, 1.0f, size));
  }
  self.play(self.assets.sfx_explosion, 1.0f, self.random_float(0.9f, 1.05f));

  if (!self.flashes.empty()) {
    auto& flash = self.flashes[self.next_flash % self.flashes.size()];
    self.next_flash++;
    flash.life = 0.6f;
    self.set_entity_pose(flash.entity, to3(position, 2.5f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
  }

  // everything close by gets hurt: people die, other cars take enough damage to chain react
  auto& j = self.juice;
  j.shake = 1.0f;
  j.hitstop = glm::max(j.hitstop, 0.09f);
  j.flash = glm::max(j.flash, 0.5f);
  for (usize i = 0; i < self.peds.size(); i++) {
    auto& ped = self.peds[i];
    if (ped.alive && ped.state != PedState::Driving && glm::distance(ped.position, position) < EXPLOSION_RADIUS) {
      self.kill_ped(static_cast<PedID>(i), (ped.position - position) * 3.0f, by_player);
    }
  }
  const auto player_distance = glm::distance(self.player_position(), position);
  if (player_distance < EXPLOSION_RADIUS + 1.0f) {
    self.damage_player(90.0f * (1.0f - player_distance / (EXPLOSION_RADIUS + 1.0f)) + 10.0f);
  }
  for (usize i = 0; i < self.cars.size(); i++) {
    const auto id = static_cast<CarID>(i);
    const auto& c = self.cars[i];
    if (c.alive && !c.exploded && glm::distance(self.car_position(id), position) < EXPLOSION_RADIUS + 2.0f) {
      self.damage_car(id, 60.0f, by_player);
    }
  }
}

auto World::shell_casing(this World& self, glm::vec2 position, f32 heading) -> void {
  // ejected to the right of the gun, a little back
  const auto fwd = forward_of(heading);
  const auto right = right_of(fwd);
  const auto push = right * self.random_float(2.0f, 3.0f) - fwd * 0.5f;
  self.emit(self.fx_casings, to3(position + fwd * 0.3f, BULLET_HEIGHT), glm::vec3(push.x, 2.0f, push.y), 1);
}

auto World::cash_sparkle(this World& self, glm::vec2 position) -> void {
  self.emit(self.fx_sparkle, to3(position, 0.3f), glm::vec3(0.0f, 1.0f, 0.0f), 40);
}

auto World::drill_sparks(this World& self, glm::vec2 position) -> void {
  self.emit(self.fx_sparks, to3(position, 1.0f), glm::vec3(0.0f, 0.5f, 0.0f), 6);
}

// smoke from damaged and wrecked cars, tyre smoke from sliding ones
auto World::update_car_fx(this World& self, f32 dt) -> void {
  for (usize i = 0; i < self.cars.size(); i++) {
    const auto id = static_cast<CarID>(i);
    auto& c = self.cars[i];
    if (!c.alive || !c.entity.is_alive()) {
      continue;
    }
    const auto pos = self.car_position(id);
    // only where the camera could plausibly see it
    if (glm::distance(pos, self.player_position()) > 60.0f) {
      continue;
    }
    const auto fwd = forward_of(self.car_heading(id));

    if (c.health < 40.0f) {
      c.smoke_timer -= dt;
      if (c.smoke_timer <= 0.0f) {
        // the engine bay: black and thick once wrecked, grey while it still runs
        const auto hood = pos + fwd * 1.3f;
        c.smoke_timer = c.exploded ? 0.12f : 0.3f;
        self.emit(self.fx_smoke, to3(hood, 1.7f), glm::vec3(0.0f, 1.0f, 0.0f), c.exploded ? 3 : 1);
      }
    }

    const auto velocity = to2(self.car_velocity(id));
    const auto lateral = glm::abs(glm::dot(velocity, right_of(fwd)));
    const auto speed = glm::length(velocity);
    if (lateral > 3.0f && speed > 4.0f) {
      c.tire_timer -= dt;
      if (c.tire_timer <= 0.0f) {
        c.tire_timer = 0.05f;
        const auto rear = pos - fwd * 1.4f;
        const auto side = right_of(fwd) * 0.8f;
        const auto count = static_cast<u32>(glm::clamp(lateral * 0.6f, 1.0f, 5.0f));
        self.emit(self.fx_tire, to3(rear + side, 0.25f), glm::vec3(0.0f), count);
        self.emit(self.fx_tire, to3(rear - side, 0.25f), glm::vec3(0.0f), count);
      }
    }
  }
}

auto World::on_kill(this World& self, glm::vec2 position, glm::vec2 direction, bool by_player, bool big) -> void {
  self.blood_burst(position, direction, big ? 180 : 130);
  // a pool where they drop, and the spatter thrown out behind them along the hit, longer for harder hits
  self.blood_decal(position, self.random_float(1.1f, 1.7f) * (big ? 1.3f : 1.0f));
  self.blood_streak(position, direction, self.random_float(3.5f, 5.5f) * (big ? 1.5f : 1.0f), self.random_float(1.6f, 2.4f));
  if (big) {
    // run over or blown up: it goes everywhere
    for (auto k = 0; k < 2; k++) {
      self.blood_streak(position, glm::vec2(direction.y, -direction.x) * (k == 0 ? 1.0f : -1.0f) + direction * 0.5f,
                        self.random_float(1.5f, 2.5f), 1.0f);
    }
  }
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
  for (auto& flash : self.flashes) {
    if (flash.life <= 0.0f || !flash.entity.is_alive()) {
      continue;
    }
    flash.life -= real_dt;
    const auto k = glm::max(0.0f, flash.life / 0.6f);
    auto light = flash.entity.get<ox::LightComponent>();
    light.intensity = 400.0f * k * k;
    flash.entity.set<ox::LightComponent>(light);
    if (flash.life <= 0.0f) {
      self.set_entity_pose(flash.entity, glm::vec3(0.0f, PARKED_Y, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    }
  }

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
