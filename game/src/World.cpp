#include "World.hpp"

#include <RmlUi/Core/DataModelHandle.h>
#include <glm/common.hpp>
#include <glm/gtc/constants.hpp>

#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"
#include "Core/VFS.hpp"
#include "Scene/Components.hpp"
#include "Scene/Scene.hpp"
#include "Utils/Log.hpp"

namespace oxcity {
auto wrap_angle(f32 a) -> f32 {
  while (a > glm::pi<f32>()) {
    a -= glm::two_pi<f32>();
  }
  while (a < -glm::pi<f32>()) {
    a += glm::two_pi<f32>();
  }
  return a;
}

auto approach(f32 current, f32 target, f32 max_delta) -> f32 {
  if (current < target) {
    return glm::min(current + max_delta, target);
  }
  return glm::max(current - max_delta, target);
}

static auto find_asset(std::string_view relative) -> ox::UUID {
  auto& asset_man = ox::App::mod<ox::AssetManager>();
  auto& vfs = ox::App::get_vfs();
  // the manifest stores source paths virtual (`assets_dir/Models/...`), find_asset wants a real path and
  // converts it back, so go through the vfs rather than spelling the virtual path by hand
  const auto physical = vfs.resolve_physical_dir(ox::VFS::ASSETS_DIR, std::string(relative));
  auto uuid = asset_man.find_asset(physical);
  if (!uuid) {
    OX_LOG_ERROR("OxCity: asset '{}' is not in the cooked manifest", relative);
  }
  return uuid;
}

// models that come and go at runtime. The engine unloads a model as soon as its last instance is destroyed, so a
// tracer (alive for 0.08 s) would be read from disk and uploaded again on every shot. Holding one ref for the
// whole session keeps them resident.
static auto runtime_models(const AssetTable& a) -> std::vector<ox::UUID> {
  auto models = std::vector<ox::UUID>(a.peds.begin(), a.peds.end());
  models.insert(models.end(), {a.cop, a.guard, a.sedan, a.sports, a.taxi, a.police, a.van, a.wheel, a.cash, a.tracer, a.marker});
  return models;
}

World::World(u32 seed) : rng(seed) {}

World::~World() {
  // the sounds were acquired once in init_audio, give the refs back or the asset manager frees them after the
  // audio engine is gone at shutdown
  if (ox::App::has_mod<ox::AssetManager>()) {
    auto& asset_man = ox::App::mod<ox::AssetManager>();
    const auto& a = this->assets;
    for (const auto* uuid : {&a.sfx_engine, &a.sfx_siren, &a.sfx_horn, &a.sfx_gunshot, &a.sfx_punch, &a.sfx_cash,
                             &a.sfx_footstep, &a.sfx_door, &a.sfx_crash, &a.sfx_alarm, &a.sfx_pager, &a.sfx_death,
                             &a.sfx_radio}) {
      if (*uuid) {
        asset_man.unload_asset(*uuid);
      }
    }
    if (this->holding_models) {
      for (const auto& uuid : runtime_models(a)) {
        if (uuid) {
          asset_man.unload_asset(uuid);
        }
      }
    }
  }

  // entities reference the scene, drop them before the scene goes
  this->peds.clear();
  this->cars.clear();
  this->pickups.clear();
  this->tracers.clear();
  this->hud_model.reset();
  this->scene.reset();
}

auto World::random_float(this World& self, f32 lo, f32 hi) -> f32 {
  return std::uniform_real_distribution<f32>(lo, hi)(self.rng);
}

auto World::random_int(this World& self, i32 lo, i32 hi) -> i32 {
  return std::uniform_int_distribution<i32>(lo, hi)(self.rng);
}

auto World::init(this World& self) -> bool {
  ZoneScoped;

  auto& a = self.assets;
  a.player = find_asset("Models/Characters/player.glb");
  for (usize i = 0; i < a.peds.size(); i++) {
    a.peds[i] = find_asset(fmt::format("Models/Characters/ped_{}.glb", i));
  }
  a.cop = find_asset("Models/Characters/cop.glb");
  a.guard = find_asset("Models/Characters/guard.glb");
  a.sedan = find_asset("Models/Vehicles/sedan.glb");
  a.sports = find_asset("Models/Vehicles/sports.glb");
  a.taxi = find_asset("Models/Vehicles/taxi.glb");
  a.police = find_asset("Models/Vehicles/police.glb");
  a.van = find_asset("Models/Vehicles/van.glb");
  a.wheel = find_asset("Models/Vehicles/wheel.glb");
  a.road_straight = find_asset("Models/City/road_straight.glb");
  a.road_cross = find_asset("Models/City/road_cross.glb");
  a.block = find_asset("Models/City/block.glb");
  a.park = find_asset("Models/City/park.glb");
  a.bank = find_asset("Models/City/bank.glb");
  a.vault_door = find_asset("Models/City/vault_door.glb");
  for (usize i = 0; i < a.buildings.size(); i++) {
    a.buildings[i] = find_asset(fmt::format("Models/City/building_{}.glb", i));
  }
  a.street_lamp = find_asset("Models/City/street_lamp.glb");
  a.tree = find_asset("Models/City/tree.glb");
  a.ground = find_asset("Models/City/ground.glb");
  a.cash = find_asset("Models/Props/cash.glb");
  a.tracer = find_asset("Models/Props/tracer.glb");
  a.marker = find_asset("Models/Props/marker.glb");

  a.sfx_engine = find_asset("Audio/engine_loop.wav");
  a.sfx_siren = find_asset("Audio/siren.wav");
  a.sfx_horn = find_asset("Audio/horn.wav");
  a.sfx_gunshot = find_asset("Audio/gunshot.wav");
  a.sfx_punch = find_asset("Audio/punch.wav");
  a.sfx_cash = find_asset("Audio/cash.wav");
  a.sfx_footstep = find_asset("Audio/footstep.wav");
  a.sfx_door = find_asset("Audio/door.wav");
  a.sfx_crash = find_asset("Audio/crash.wav");
  a.sfx_alarm = find_asset("Audio/alarm.wav");
  a.sfx_pager = find_asset("Audio/pager.wav");
  a.sfx_death = find_asset("Audio/death.wav");
  a.sfx_radio = find_asset("Audio/radio.wav");

  if (!a.player || !a.sedan || !a.road_straight) {
    OX_LOG_ERROR("OxCity: core assets are missing, was the game built with the ox.cook_assets rule?");
    return false;
  }

  {
    auto& asset_man = ox::App::mod<ox::AssetManager>();
    for (const auto& uuid : runtime_models(a)) {
      if (uuid && !asset_man.load_asset(uuid)) {
        OX_LOG_ERROR("OxCity: couldn't load model {}", uuid.str());
      }
    }
    self.holding_models = true;
  }

  self.scene = std::make_unique<ox::Scene>("OxCity");

  // a top down city under a software rasterizer: keep the expensive screen space and gi passes off, the look
  // comes from the sun, the sky and bloom on the emissive lights
  auto& cvar = self.scene->renderer_cvar;
  cvar.cvar_ddgi_enable.set(false);
  cvar.cvar_rtao_enable.set(false);
  cvar.cvar_vbgtao_enable.set(false);
  cvar.cvar_contact_shadows_enabled.set(false);
  cvar.cvar_bloom_enable.set(true);
  cvar.cvar_fxaa_enable.set(true);

  // sun + sky, same recipe the editor uses for a new scene
  const auto sun = self.scene->create_entity("sun");
  sun.set<ox::TransformComponent>({
    // the light's forward axis points *at* the sun (the editor's default scene uses a positive pitch too),
    // a negative pitch puts the sun under the horizon and the whole city goes black
    .rotation = glm::quat(glm::vec3(glm::radians(58.0f), glm::radians(30.0f), 0.0f)),
  });
  sun.set<ox::LightComponent>({.type = ox::LightComponent::LightType::Directional, .intensity = 10.0f})
    .add<ox::AtmosphereComponent>();
  sun.set<ox::AutoExposureComponent>({});

  self.camera = self.scene->create_entity("camera");
  self.camera.set<ox::CameraComponent>({.fov = 50.0f, .far_clip = 400.0f, .near_clip = 0.5f});
  self.camera.add<ox::AudioListenerComponent>();
  self.camera.set<ox::AudioListenerComponent>({.active = true});

  self.build_city();

  // traffic, parked cars and pedestrians before the scene starts, so physics_init creates every body in one go
  for (i32 i = 0; i < MAX_TRAFFIC; i++) {
    const auto node = self.random_node();
    const auto& n = self.nodes[static_cast<usize>(node)];
    if (n.neighbours.empty()) {
      continue;
    }
    const auto next = n.neighbours[static_cast<usize>(self.random_int(0, static_cast<i32>(n.neighbours.size()) - 1))];
    const auto dir = glm::normalize(self.nodes[static_cast<usize>(next)].position - n.position);
    const auto right = right_of(dir);
    const auto start = glm::mix(n.position, self.nodes[static_cast<usize>(next)].position, self.random_float(0.3f, 0.6f)) +
                       right * LANE_OFFSET;
    static constexpr std::string_view traffic_models[] = {"sedan", "taxi", "van", "sedan", "sports"};
    const auto model = traffic_models[static_cast<usize>(i) % std::size(traffic_models)];
    const auto car_id = self.spawn_car(model, start, heading_of(dir), CarRole::Traffic);
    if (car_id == CarID::Invalid) {
      continue;
    }
    auto& c = self.car(car_id);
    c.from_node = node;
    c.to_node = next;
    c.cruise_speed = self.random_float(7.0f, 11.0f);
    // traffic has a driver, carjacking throws them out
    const auto driver = self.spawn_ped(PedKind::Civilian, start);
    if (driver != PedID::Invalid) {
      auto& p = self.ped(driver);
      p.state = PedState::Driving;
      p.car = car_id;
      self.hide_entity(p.entity);
      c.driver = driver;
    }
  }

  for (i32 i = 0; i < PARKED_CARS; i++) {
    // parked by the curb, facing along the road
    const auto node = self.random_node();
    const auto& n = self.nodes[static_cast<usize>(node)];
    if (n.neighbours.empty()) {
      continue;
    }
    const auto next = n.neighbours.front();
    const auto dir = glm::normalize(self.nodes[static_cast<usize>(next)].position - n.position);
    const auto right = right_of(dir);
    const auto pos = glm::mix(n.position, self.nodes[static_cast<usize>(next)].position, 0.5f) + right * (TILE * 0.5f - 1.4f);
    static constexpr std::string_view parked_models[] = {"sports", "sedan", "van", "taxi"};
    self.spawn_car(parked_models[static_cast<usize>(i) % std::size(parked_models)], pos, heading_of(dir), CarRole::Parked);
  }

  for (i32 i = 0; i < MAX_PEDS; i++) {
    self.spawn_ped(PedKind::Civilian, self.random_sidewalk_point());
  }
  // two guards at the bank door
  for (i32 i = 0; i < 2; i++) {
    const auto guard = self.spawn_ped(PedKind::Guard, self.heist.position + glm::vec2(i == 0 ? -3.0f : 3.0f, -1.0f));
    if (guard != PedID::Invalid) {
      self.ped(guard).state = PedState::Idle;
      self.ped(guard).heading = 0.0f;
    }
  }

  self.spawn_player(self.hospital);

  if (!self.init_hud()) {
    return false;
  }
  self.init_audio();

  self.scene->runtime_start();
  self.set_state(GameState::MainMenu);

  OX_LOG_INFO(
    "OxCity: world ready, {} road nodes, {} cars, {} peds",
    self.nodes.size(),
    self.cars.size(),
    self.peds.size()
  );
  return true;
}

auto World::start_game(this World& self) -> void {
  self.player.cash = 0;
  self.player.health = 100.0f;
  self.player.ammo = 60;
  self.player.weapon = Weapon::Pistol;
  self.wanted = {};
  self.stats = {};
  self.set_state(GameState::Playing);
  self.pager("WELCOME TO OXCITY. THE BANK ON THE NORTH SIDE IS RIPE. STEAL A CAR, GET RICH.");
}

auto World::set_state(this World& self, GameState state) -> void {
  self.state = state;
  self.state_timer = 0.0f;
  self.hud.menu_visible = state == GameState::MainMenu || state == GameState::Paused;
  self.hud.paused = state == GameState::Paused;
  switch (state) {
    case GameState::Dead: self.hud.big_text = "FLATLINED"; break;
    case GameState::Arrested: self.hud.big_text = "ARRESTED"; break;
    default               : self.hud.big_text = ""; break;
  }
}

auto World::update(this World& self, const GameInput& input, f32 dt) -> void {
  ZoneScoped;

  self.time += dt;
  self.state_timer += dt;

  switch (self.state) {
    case GameState::MainMenu: {
      if (input.confirm || self.start_requested) {
        self.start_requested = false;
        self.start_game();
      }
      break;
    }
    case GameState::Paused: {
      if (input.pause || input.confirm || self.start_requested) {
        self.start_requested = false;
        self.set_state(GameState::Playing);
      }
      break;
    }
    case GameState::Playing: {
      if (input.pause) {
        self.set_state(GameState::Paused);
        break;
      }
      self.update_player(input, dt);
      break;
    }
    case GameState::Dead:
    case GameState::Arrested: {
      if (self.state_timer > 4.0f) {
        self.respawn_player();
        self.set_state(GameState::Playing);
      }
      break;
    }
  }

  // the city keeps living behind the menus, it doubles as the title screen's attract mode
  if (self.state != GameState::Paused) {
    self.update_vehicles(input, dt);
    self.update_peds(dt);
    self.update_crime(input, dt);
  }
  self.update_camera(dt);
  self.update_audio(dt);
  self.update_hud();

  // the paused world still has to reach the renderer (and RmlUi still needs its update), it just doesn't
  // advance: the gameplay and physics phases are switched off rather than stepping with a zero delta, which
  // flecs would read as "measure the frame time yourself"
  const auto paused = self.state == GameState::Paused;
  if (paused != self.phases_disabled) {
    self.phases_disabled = paused;
    if (paused) {
      self.scene->disable_phases({flecs::PreUpdate, flecs::OnUpdate});
    } else {
      self.scene->enable_all_phases();
    }
  }
  self.scene->runtime_step(dt);
}
} // namespace oxcity
