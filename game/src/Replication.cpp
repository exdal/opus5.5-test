// Multiplayer, the World's side of it. The host (listen or dedicated) runs the whole city exactly like single
// player and, once per snapshot tick, writes it down as a quantized net::Snapshot; the one-off things that happened
// in between (shots, blood, sounds, pager messages) are recorded as net::Events by the same functions that play them
// locally. A client runs none of the simulation: it poses what the snapshots say, drawn a little in the past so there
// are always two snapshots to blend between, and replays the events. The one thing a client does simulate is its own
// character on foot, so walking has no lag; the host takes the client's word for that position (within reason).

#include <glm/common.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtx/norm.hpp>

#include "Scene/Components.hpp"
#include "Scene/Scene.hpp"
#include "Utils/Log.hpp"
#include "World.hpp"

namespace oxcity {
// a third of a second at the normal rate; more when the host runs faster than we draw (see run_net_test.sh)
static constexpr usize MAX_SNAPSHOTS = 32;
static constexpr f32 MAX_SPEED_Q = 8.0f; // m/s that maps to 255 in the animation speed fields
static constexpr f32 WHEEL_RADIUS = 0.36f;
static constexpr f32 MAX_STEER = glm::radians(36.0f);

// every sound goes over the wire as its position in this list
static constexpr auto SOUNDS = std::array{
  &AssetTable::sfx_engine, &AssetTable::sfx_siren,  &AssetTable::sfx_horn,        &AssetTable::sfx_gunshot,
  &AssetTable::sfx_punch,  &AssetTable::sfx_cash,   &AssetTable::sfx_footstep,    &AssetTable::sfx_door,
  &AssetTable::sfx_crash,  &AssetTable::sfx_alarm,  &AssetTable::sfx_pager,       &AssetTable::sfx_death,
  &AssetTable::sfx_radio,  &AssetTable::sfx_knife_swing, &AssetTable::sfx_stab,   &AssetTable::sfx_splat,
  &AssetTable::sfx_explosion,
};

auto World::record(this World& self, net::Event event) -> void {
  if (self.role == NetRole::Host || self.role == NetRole::Server) {
    self.net_events.push_back(std::move(event));
  }
}

auto World::sound_index(this const World& self, const ox::UUID& sound) -> u16 {
  for (usize i = 0; i < SOUNDS.size(); i++) {
    if (self.assets.*SOUNDS[i] == sound) {
      return static_cast<u16>(i);
    }
  }
  return 0xffff;
}

auto World::sound_by_index(this const World& self, u16 index) -> ox::UUID {
  return index < SOUNDS.size() ? self.assets.*SOUNDS[index] : ox::UUID{};
}

// --- host -----------------------------------------------------------------------------------------------------------

auto World::join_player(this World& self, std::string_view name) -> PlayerID {
  for (auto id : ALL_PLAYERS) {
    auto& p = self.pl(id);
    if (id == self.local || p.active) {
      continue;
    }
    const auto generation = static_cast<u8>(p.generation + 1);
    if (p.entity && p.entity.is_alive()) {
      p.entity.destruct();
    }
    p = Player{};
    p.generation = generation;
    // a few steps apart outside the hospital, so nobody spawns inside somebody else
    self.spawn_player(id, self.hospital + glm::vec2(0.0f, 1.5f * static_cast<f32>(static_cast<i32>(id))));
    p.active = true;
    p.remote = true;
    p.name = std::string(name);
    p.invulnerable = 3.0f;
    return id;
  }
  return PlayerID::Invalid;
}

auto World::apply_input(this World& self, PlayerID id, const net::InputMessage& m) -> void {
  auto& p = self.pl(id);
  if (!p.active || m.sequence <= p.net_input_sequence) {
    return; // unreliable: late ones are older than what we have
  }
  p.net_input_sequence = m.sequence;

  auto in = GameInput{};
  in.move = {net::dq_snorm(m.move_x), net::dq_snorm(m.move_y)};
  in.throttle = net::dq_snorm(m.throttle);
  in.steer = net::dq_snorm(m.steer);
  in.handbrake = (m.buttons & net::BUTTON_HANDBRAKE) != 0;
  in.sprint = (m.buttons & net::BUTTON_SPRINT) != 0;
  in.attack = (m.buttons & net::BUTTON_ATTACK) != 0;
  in.interact = (m.buttons & net::BUTTON_INTERACT) != 0;
  in.horn = (m.buttons & net::BUTTON_HORN) != 0;
  in.has_aim = (m.buttons & net::BUTTON_HAS_AIM) != 0;
  in.aim = {net::dq_pos(m.aim_x), net::dq_pos(m.aim_z)};
  // presses stay latched until the next World::update has seen them (the session clears them afterwards), several
  // input packets can arrive between two updates
  in.enter_exit = p.input.enter_exit || m.enter_exit_count != p.net_enter_count;
  in.switch_weapon = p.input.switch_weapon || m.switch_weapon_count != p.net_switch_count;
  p.net_enter_count = m.enter_exit_count;
  p.net_switch_count = m.switch_weapon_count;
  p.input = in;

  if ((m.buttons & net::BUTTON_ON_FOOT) != 0) {
    p.has_report = true;
    p.reported_position = {net::dq_pos(m.x), net::dq_pos(m.z)};
    p.reported_heading = net::dq_angle(m.heading);
    p.reported_teleport = m.teleport_seen;
  }
}

static auto quantize_speed(f32 speed) -> u8 { return net::q_unit(speed / MAX_SPEED_Q); }
static auto speed_of(u8 v) -> f32 { return net::dq_unit(v) * MAX_SPEED_Q; }

auto World::build_snapshot(this World& self, u32 sequence) -> net::Snapshot {
  ZoneScoped;

  auto s = net::Snapshot{};
  s.sequence = sequence;
  s.time = self.time;
  s.heist_progress = net::q_unit(self.heist.progress / 8.0f);
  s.heist_alarm = static_cast<u16>(glm::clamp(self.heist.alarm * 10.0f, 0.0f, 65535.0f));
  s.heist_restock = static_cast<u16>(glm::clamp(self.heist.restock * 10.0f, 0.0f, 65535.0f));
  s.heist_driller = static_cast<i8>(self.heist.driller);

  s.players.reserve(MAX_PLAYERS);
  for (auto id : ALL_PLAYERS) {
    const auto& p = self.pl(id);
    auto sp = net::SnapPlayer{};
    sp.generation = p.generation;
    sp.flags = p.active ? net::PLAYER_ACTIVE : 0;
    if (p.active) {
      const auto pos = self.player_position(id);
      sp.life = static_cast<u8>(p.life);
      sp.weapon = static_cast<u8>(p.weapon);
      sp.x = net::q_pos(pos.x);
      sp.z = net::q_pos(pos.y);
      sp.heading = net::q_angle(p.heading);
      sp.car = static_cast<i16>(p.car);
      sp.health = static_cast<u8>(glm::clamp(p.health, 0.0f, 255.0f));
      sp.stars = static_cast<u8>(self.stars(id));
      sp.cash = p.cash;
      sp.ammo = static_cast<i16>(p.ammo);
      sp.score = p.score;
      sp.kills = static_cast<u16>(p.kills);
      sp.combo = static_cast<u8>(glm::min(p.combo, 255));
      sp.speed = quantize_speed(p.anim_speed);
      sp.punch = net::q_unit(p.punch_anim);
      sp.teleport = p.teleport_seq;
      sp.invulnerable = p.invulnerable > 0.0f ? 1 : 0;
      sp.banks_robbed = static_cast<u16>(p.stats.banks_robbed);
      sp.cars_stolen = static_cast<u16>(p.stats.cars_stolen);
      sp.peds_robbed = static_cast<u16>(p.stats.peds_robbed);
      sp.peds_killed = static_cast<u16>(p.stats.peds_killed);
    }
    s.players.push_back(sp);
  }

  s.peds.reserve(self.peds.size());
  for (const auto& p : self.peds) {
    auto sp = net::SnapPed{};
    sp.generation = p.generation;
    sp.model = p.model;
    const auto visible = p.entity.is_valid() && p.entity.is_alive() && p.state != PedState::Driving;
    sp.flags = static_cast<u8>((p.alive ? net::PED_ALIVE : 0) | (visible ? net::PED_VISIBLE : 0) |
                               (p.cash > 0 ? net::PED_HAS_CASH : 0));
    sp.heading = static_cast<u8>(net::q_angle(p.heading) >> 8);
    sp.x = net::q_pos(p.position.x);
    sp.z = net::q_pos(p.position.y);
    sp.fall = net::q_unit(p.fall);
    sp.speed = quantize_speed(p.anim_speed);
    sp.punch = net::q_unit(p.punch);
    s.peds.push_back(sp);
  }

  s.cars.reserve(self.cars.size());
  for (usize i = 0; i < self.cars.size(); i++) {
    const auto& c = self.cars[i];
    auto sc = net::SnapCar{};
    sc.generation = c.generation;
    if (c.alive && c.entity.is_alive()) {
      const auto id = static_cast<CarID>(i);
      const auto& tc = c.entity.get<ox::TransformComponent>();
      const auto fwd = tc.rotation * glm::vec3(0.0f, 0.0f, 1.0f);
      const auto side = tc.rotation * glm::vec3(1.0f, 0.0f, 0.0f);
      const auto chasing = c.role == CarRole::Police && (self.total_stars() > 0 || c.player_driver != PlayerID::Invalid);
      sc.model = car_model_index(c.model);
      sc.flags = static_cast<u8>(net::CAR_ALIVE | (c.exploded ? net::CAR_EXPLODED : 0) | (chasing ? net::CAR_SIREN : 0) |
                                 (c.driver != PedID::Invalid ? net::CAR_AI_DRIVER : 0));
      sc.health = static_cast<u8>(glm::clamp(c.health, 0.0f, 255.0f));
      sc.player_driver = static_cast<i8>(c.player_driver);
      sc.x = net::q_pos(tc.position.x);
      sc.y = net::q_pos(tc.position.y);
      sc.z = net::q_pos(tc.position.z);
      sc.yaw = net::q_angle(heading_of({fwd.x, fwd.z}));
      sc.pitch = static_cast<i8>(glm::clamp(glm::degrees(-std::asin(glm::clamp(fwd.y, -1.0f, 1.0f))), -127.0f, 127.0f));
      sc.roll = static_cast<i8>(glm::clamp(glm::degrees(std::asin(glm::clamp(side.y, -1.0f, 1.0f))), -127.0f, 127.0f));
      if (const auto* v = c.entity.try_get<ox::VehicleComponent>()) {
        sc.steer = net::q_snorm(v->input_right);
      }
      const auto velocity = self.car_velocity(id);
      sc.vx = static_cast<i8>(glm::clamp(velocity.x * 2.0f, -127.0f, 127.0f));
      sc.vz = static_cast<i8>(glm::clamp(velocity.z * 2.0f, -127.0f, 127.0f));
    }
    s.cars.push_back(sc);
  }

  s.pickups.reserve(self.pickups.size());
  for (const auto& pk : self.pickups) {
    if (!pk.taken) {
      s.pickups.push_back({.id = pk.id, .x = net::q_pos(pk.position.x), .z = net::q_pos(pk.position.y)});
    }
  }
  return s;
}

// --- client ---------------------------------------------------------------------------------------------------------

auto World::become_client(this World& self, PlayerID slot) -> void {
  // the static city stays (it's built from the same seed on both ends), everything that moves now comes from the
  // host: clear out the local traffic, crowd, pickups and the offline player
  for (usize i = 0; i < self.cars.size(); i++) {
    self.despawn_car(static_cast<CarID>(i));
  }
  self.cars.clear();
  for (auto& p : self.peds) {
    if (p.entity && p.entity.is_alive()) {
      p.entity.destruct();
    }
  }
  self.peds.clear();
  for (auto& pk : self.pickups) {
    if (pk.entity && pk.entity.is_alive()) {
      pk.entity.destruct();
    }
  }
  self.pickups.clear();
  for (auto& t : self.tracers) {
    if (t.entity && t.entity.is_alive()) {
      t.entity.destruct();
    }
  }
  self.tracers.clear();
  for (auto id : ALL_PLAYERS) {
    self.despawn_player(id);
    self.pl(id).generation = 0;
  }
  self.heist.progress = 0.0f;
  self.heist.driller = PlayerID::Invalid;
  self.kill_feed.clear();
  self.snapshots.clear();
  self.net_clock = 0.0f;
  self.juice = {};

  self.role = NetRole::Client;
  self.local = slot;
  self.set_state(GameState::Playing);
}

static auto ped_kind_of(u8 model, usize civilian_models) -> PedKind {
  if (model == civilian_models) {
    return PedKind::Cop;
  }
  return model > civilian_models ? PedKind::Guard : PedKind::Civilian;
}

// spawn, respawn and remove entities so the client's slots hold what the host's do
static auto sync_players(World& self, const net::Snapshot& s) -> void {
  for (usize i = 0; i < glm::min(s.players.size(), self.players.size()); i++) {
    const auto id = static_cast<PlayerID>(i);
    const auto& sp = s.players[i];
    auto& p = self.pl(id);
    const auto active = (sp.flags & net::PLAYER_ACTIVE) != 0;
    if (!active) {
      if (p.active || (p.entity && p.entity.is_alive())) {
        self.despawn_player(id);
      }
      continue;
    }
    const auto pos = glm::vec2(net::dq_pos(sp.x), net::dq_pos(sp.z));
    const auto fresh = !p.active || p.generation != sp.generation || !p.entity || !p.entity.is_alive();
    if (fresh) {
      const auto name = p.generation == sp.generation ? p.name : std::string{};
      self.despawn_player(id);
      self.spawn_player(id, pos);
      p.active = true;
      p.generation = sp.generation;
      p.name = name;
      p.teleport_seq = sp.teleport;
    }

    const auto was_alive = p.life == Life::Alive;
    const auto old_health = p.health;
    p.life = static_cast<Life>(sp.life);
    p.weapon = static_cast<Weapon>(sp.weapon);
    p.health = static_cast<f32>(sp.health);
    p.wanted.heat = static_cast<f32>(sp.stars);
    p.cash = sp.cash;
    p.ammo = sp.ammo;
    p.score = sp.score;
    p.kills = sp.kills;
    p.combo = sp.combo;
    p.invulnerable = sp.invulnerable ? 1.0f : 0.0f;
    p.stats.banks_robbed = sp.banks_robbed;
    p.stats.cars_stolen = sp.cars_stolen;
    p.stats.peds_robbed = sp.peds_robbed;
    p.stats.peds_killed = sp.peds_killed;

    const auto car = static_cast<CarID>(sp.car);
    if (!self.is_local(id)) {
      p.car = car;
      continue;
    }

    // the local player: what happened to you on the host
    if (!fresh && p.health < old_health && p.life == Life::Alive) {
      self.juice.flash = glm::max(self.juice.flash, 0.6f);
      self.juice.shake = glm::min(1.0f, self.juice.shake + 0.25f);
    }
    if (was_alive && p.life != Life::Alive) {
      self.play(self.assets.sfx_death, p.life == Life::Dead ? 1.0f : 0.8f, p.life == Life::Dead ? 1.0f : 1.3f);
      p.life_timer = 0.0f;
    }
    // the host moved you (spawned, got in or out of a car, respawned): drop the prediction and go where it says
    if (sp.teleport != p.teleport_seq || car != p.car) {
      p.teleport_seq = sp.teleport;
      p.car = car;
      p.heading = net::dq_angle(sp.heading);
      OX_LOG_INFO(
        "OxCity net: the host moved us to ({:.1f}, {:.1f}){}",
        pos.x,
        pos.y,
        car != CarID::Invalid ? " into a car" : (p.life == Life::Alive ? "" : " (not alive)")
      );
      p.entity.remove<ox::CharacterControllerComponent>();
      if (car == CarID::Invalid && p.life == Life::Alive) {
        p.position = pos;
        self.set_entity_pose(p.entity, to3(pos, 0.4f), yaw_quat(p.heading));
        p.entity.set<ox::CharacterControllerComponent>({.character_height_standing = 1.1f, .character_radius_standing = 0.3f});
      } else if (car != CarID::Invalid) {
        self.hide_entity(p.entity);
      }
    }
  }
}

static auto sync_peds(World& self, const net::Snapshot& s) -> void {
  if (self.peds.size() > s.peds.size()) {
    for (usize i = s.peds.size(); i < self.peds.size(); i++) {
      if (self.peds[i].entity && self.peds[i].entity.is_alive()) {
        self.peds[i].entity.destruct();
      }
    }
  }
  self.peds.resize(s.peds.size());
  for (usize i = 0; i < s.peds.size(); i++) {
    const auto& sp = s.peds[i];
    auto& p = self.peds[i];
    const auto visible = (sp.flags & net::PED_VISIBLE) != 0;
    const auto has_entity = p.entity && p.entity.is_alive();
    const auto someone_else = p.generation != sp.generation || p.model != sp.model;
    if (has_entity && (someone_else || !visible)) {
      p.entity.destruct();
      p.entity = {};
    }
    const auto pos = glm::vec2(net::dq_pos(sp.x), net::dq_pos(sp.z));
    if (visible && (!p.entity || !p.entity.is_alive())) {
      const auto heading = net::dq_angle(static_cast<u16>(sp.heading << 8));
      p.entity = self.spawn_model(self.ped_model(sp.model), to3(pos, self.ground_height(pos)), heading);
      p.limbs = self.find_limbs(p.entity);
      p.position = pos;
      p.heading = heading;
    }
    p.generation = sp.generation;
    p.model = sp.model;
    p.kind = ped_kind_of(sp.model, self.assets.peds.size());
    p.alive = (sp.flags & net::PED_ALIVE) != 0;
    p.state = !visible && p.alive ? PedState::Driving : (p.alive ? PedState::Wander : PedState::Dead);
    p.cash = (sp.flags & net::PED_HAS_CASH) != 0 ? 1 : 0;
  }
}

static auto sync_cars(World& self, const net::Snapshot& s) -> void {
  if (self.cars.size() > s.cars.size()) {
    for (usize i = s.cars.size(); i < self.cars.size(); i++) {
      self.despawn_car(static_cast<CarID>(i));
    }
  }
  const auto known = self.cars.size();
  self.cars.resize(s.cars.size());
  for (usize i = known; i < self.cars.size(); i++) {
    self.cars[i].alive = false; // an empty slot until the host says there's a car in it
  }
  for (usize i = 0; i < s.cars.size(); i++) {
    const auto id = static_cast<CarID>(i);
    const auto& sc = s.cars[i];
    const auto alive = (sc.flags & net::CAR_ALIVE) != 0;
    const auto model = std::string(car_model_name(sc.model));
    {
      auto& c = self.car(id);
      const auto someone_else = c.generation != sc.generation || c.model != model;
      if (c.alive && (!alive || someone_else || !c.entity || !c.entity.is_alive())) {
        self.despawn_car(id);
      }
    }
    if (!alive) {
      self.car(id).generation = sc.generation;
      continue;
    }
    if (!self.car(id).alive) {
      // spawn_car appends; move the new car into the host's slot
      const auto pos = glm::vec2(net::dq_pos(sc.x), net::dq_pos(sc.z));
      const auto spawned = self.spawn_car(model, pos, net::dq_angle(sc.yaw), model == "police" ? CarRole::Police : CarRole::Traffic);
      if (spawned == CarID::Invalid) {
        continue;
      }
      auto car = std::move(self.car(spawned));
      self.cars.pop_back();
      self.car(id) = std::move(car);
    }
    auto& c = self.car(id);
    c.generation = sc.generation;
    c.health = static_cast<f32>(sc.health);
    c.player_driver = static_cast<PlayerID>(sc.player_driver);
    // there is no AI on a client, but the siren and the "CARJACK" prompt want to know if someone's at the wheel
    c.driver = (sc.flags & net::CAR_AI_DRIVER) != 0 ? PedID{0} : PedID::Invalid;
    c.net_siren = (sc.flags & net::CAR_SIREN) != 0;
    c.net_steer = net::dq_snorm(sc.steer);
    c.net_velocity = {static_cast<f32>(sc.vx) * 0.5f, 0.0f, static_cast<f32>(sc.vz) * 0.5f};
    const auto exploded = (sc.flags & net::CAR_EXPLODED) != 0;
    if (exploded && !c.exploded) {
      char_wreck(self, c.entity);
    }
    c.exploded = exploded;
  }
}

static auto sync_pickups(World& self, const net::Snapshot& s) -> void {
  for (auto& pk : self.pickups) {
    const auto still_there = std::ranges::any_of(s.pickups, [&](const net::SnapPickup& sp) { return sp.id == pk.id; });
    if (!still_there) {
      pk.taken = true;
      if (pk.entity && pk.entity.is_alive()) {
        pk.entity.destruct();
      }
    }
  }
  std::erase_if(self.pickups, [](const Pickup& pk) { return pk.taken; });
  for (const auto& sp : s.pickups) {
    const auto known = std::ranges::any_of(self.pickups, [&](const Pickup& pk) { return pk.id == sp.id; });
    if (!known) {
      const auto pos = glm::vec2(net::dq_pos(sp.x), net::dq_pos(sp.z));
      auto e = self.spawn_model(self.assets.cash, to3(pos, 0.2f), 0.0f);
      self.pickups.push_back(Pickup{.id = sp.id, .entity = e, .position = pos});
    }
  }
}

auto World::apply_snapshot(this World& self, net::Snapshot&& snapshot) -> void {
  ZoneScoped;

  if (self.role != NetRole::Client) {
    return;
  }
  if (!self.snapshots.empty() && snapshot.sequence <= self.snapshots.back().sequence) {
    return; // arrived out of order, we already have something newer
  }

  // the clock the city is drawn at follows the host's, gently: jumps only when it's way off (first snapshot, a stall)
  if (self.snapshots.empty() || glm::abs(snapshot.time - self.net_clock) > 0.5f) {
    self.net_clock = snapshot.time;
  } else {
    self.net_clock += (snapshot.time - self.net_clock) * 0.1f;
  }

  sync_players(self, snapshot);
  sync_peds(self, snapshot);
  sync_cars(self, snapshot);
  sync_pickups(self, snapshot);
  self.heist.progress = net::dq_unit(snapshot.heist_progress) * 8.0f;
  self.heist.alarm = static_cast<f32>(snapshot.heist_alarm) / 10.0f;
  self.heist.restock = static_cast<f32>(snapshot.heist_restock) / 10.0f;
  self.heist.driller = static_cast<PlayerID>(snapshot.heist_driller);

  self.snapshots.push_back(std::move(snapshot));
  if (self.snapshots.size() > MAX_SNAPSHOTS) {
    self.snapshots.erase(self.snapshots.begin());
  }
}

auto World::apply_events(this World& self, const net::EventBatch& batch) -> void {
  for (const auto& e : batch.events) {
    if (e.target != net::EVERYONE && static_cast<PlayerID>(e.target) != self.local) {
      continue;
    }
    const auto& f = e.f;
    switch (e.kind) {
      case net::EventKind::Sound      : self.play_at(self.sound_by_index(e.asset), {f[0], f[1]}, f[2], f[3], f[4]); break;
      case net::EventKind::BloodBurst : self.blood_burst({f[0], f[1]}, {f[2], f[3]}, e.asset); break;
      case net::EventKind::BloodDecal : self.blood_decal({f[0], f[1]}, f[2]); break;
      case net::EventKind::BloodStreak: self.blood_streak({f[0], f[1]}, {f[2], f[3]}, f[4], f[5]); break;
      case net::EventKind::Muzzle     : self.muzzle_flash({f[0], f[1]}, f[2]); break;
      case net::EventKind::Sparks     : self.impact_sparks({f[0], f[1], f[2]}, {f[3], f[4]}); break;
      case net::EventKind::Casing     : self.shell_casing({f[0], f[1]}, f[2]); break;
      case net::EventKind::Sparkle    : self.cash_sparkle({f[0], f[1]}); break;
      case net::EventKind::Drill      : self.drill_sparks({f[0], f[1]}); break;
      case net::EventKind::Explosion  : self.explode_fx({f[0], f[1]}); break;
      case net::EventKind::Tracer     : self.spawn_tracer({f[0], f[1], f[2]}, {f[3], f[4], f[5]}); break;
      case net::EventKind::KillJuice  : self.local_kill_juice(e.asset, f[0] > 0.5f); break;
      case net::EventKind::Pager      : self.pager(e.text); break;
      case net::EventKind::KillFeed   : {
        self.kill_feed.push_back({.text = e.text});
        if (self.kill_feed.size() > 4) {
          self.kill_feed.erase(self.kill_feed.begin());
        }
        break;
      }
      case net::EventKind::LifeNote: {
        if (auto* me = self.local_player()) {
          me->life_note = e.text;
        }
        break;
      }
    }
  }
}

auto World::apply_roster(this World& self, const net::Roster& roster) -> void {
  for (const auto& entry : roster.players) {
    if (entry.slot < MAX_PLAYERS) {
      auto& p = self.pl(static_cast<PlayerID>(entry.slot));
      p.name = entry.name;
      // the roster can beat the first snapshot with this player in it; sync_players keeps the name if it matches
      p.generation = p.active ? p.generation : entry.generation;
    }
  }
}

// the two snapshots around the time the city is drawn at, and how far between them
struct Blend {
  const net::Snapshot* a = nullptr;
  const net::Snapshot* b = nullptr;
  f32 t = 1.0f;
};

static auto blend_at(const std::vector<net::Snapshot>& snapshots, f32 time) -> Blend {
  if (snapshots.empty()) {
    return {};
  }
  if (snapshots.size() == 1 || time <= snapshots.front().time) {
    return {&snapshots.front(), &snapshots.front(), 1.0f};
  }
  for (usize i = 1; i < snapshots.size(); i++) {
    const auto& a = snapshots[i - 1];
    const auto& b = snapshots[i];
    if (time <= b.time) {
      const auto span = glm::max(b.time - a.time, 0.0001f);
      return {&a, &b, glm::clamp((time - a.time) / span, 0.0f, 1.0f)};
    }
  }
  // ran out of snapshots (one got lost, or the host stalled): hold the newest rather than guess
  return {&snapshots.back(), &snapshots.back(), 1.0f};
}

static auto lerp_angle(f32 a, f32 b, f32 t) -> f32 { return a + wrap_angle(b - a) * t; }

auto World::client_update(this World& self, f32 dt) -> void {
  ZoneScoped;

  self.net_clock += dt;
  const auto blend = blend_at(self.snapshots, self.net_clock - net::INTERPOLATION_DELAY);
  if (!blend.b) {
    return;
  }
  const auto& a = *blend.a;
  const auto& b = *blend.b;
  const auto t = blend.t;

  // --- peds ---
  for (usize i = 0; i < self.peds.size() && i < b.peds.size(); i++) {
    auto& p = self.peds[i];
    if (!p.entity || !p.entity.is_alive()) {
      continue;
    }
    const auto& sb = b.peds[i];
    const auto same = i < a.peds.size() && a.peds[i].generation == sb.generation;
    const auto& sa = same ? a.peds[i] : sb;
    p.position = glm::mix(glm::vec2(net::dq_pos(sa.x), net::dq_pos(sa.z)), glm::vec2(net::dq_pos(sb.x), net::dq_pos(sb.z)), t);
    p.heading = lerp_angle(net::dq_angle(static_cast<u16>(sa.heading << 8)), net::dq_angle(static_cast<u16>(sb.heading << 8)), t);
    if (!p.alive) {
      p.fall = net::dq_unit(sb.fall);
      const auto lying = yaw_quat(p.heading) * glm::angleAxis(-glm::half_pi<f32>() * p.fall, glm::vec3(1.0f, 0.0f, 0.0f));
      self.set_entity_pose(p.entity, to3(p.position, self.ground_height(p.position) + 0.12f * p.fall), lying);
      continue;
    }
    const auto speed = speed_of(sb.speed);
    p.walk_phase += dt * speed * 2.6f;
    self.animate_limbs(p.limbs, p.walk_phase, glm::min(1.0f, speed / 1.4f), net::dq_unit(sb.punch));
    self.set_entity_pose(p.entity, to3(p.position, self.ground_height(p.position)), yaw_quat(p.heading));
  }

  // --- cars ---
  for (usize i = 0; i < self.cars.size() && i < b.cars.size(); i++) {
    auto& c = self.cars[i];
    if (!c.alive || !c.entity || !c.entity.is_alive()) {
      continue;
    }
    const auto& sb = b.cars[i];
    const auto same = i < a.cars.size() && a.cars[i].generation == sb.generation && (a.cars[i].flags & net::CAR_ALIVE);
    const auto& sa = same ? a.cars[i] : sb;
    const auto pos = glm::mix(
      glm::vec3(net::dq_pos(sa.x), net::dq_pos(sa.y), net::dq_pos(sa.z)),
      glm::vec3(net::dq_pos(sb.x), net::dq_pos(sb.y), net::dq_pos(sb.z)),
      t
    );
    const auto yaw = lerp_angle(net::dq_angle(sa.yaw), net::dq_angle(sb.yaw), t);
    const auto rotation = yaw_quat(yaw) * glm::angleAxis(glm::radians(static_cast<f32>(sb.pitch)), glm::vec3(1.0f, 0.0f, 0.0f)) *
                          glm::angleAxis(glm::radians(static_cast<f32>(sb.roll)), glm::vec3(0.0f, 0.0f, 1.0f));
    self.set_entity_pose(c.entity, pos, rotation);

    // wheels roll with the car and the front ones steer (seen from above the driver's right is -x, a negative turn)
    const auto forward_speed = glm::dot(glm::vec2(c.net_velocity.x, c.net_velocity.z), forward_of(yaw));
    c.wheel_spin = glm::mod(c.wheel_spin + forward_speed / WHEEL_RADIUS * dt, glm::two_pi<f32>());
    const auto spin = glm::angleAxis(c.wheel_spin, glm::vec3(1.0f, 0.0f, 0.0f));
    const auto steer = glm::angleAxis(-c.net_steer * MAX_STEER, glm::vec3(0.0f, 1.0f, 0.0f));
    for (usize w = 0; w < c.wheels.size(); w++) {
      if (c.wheels[w] && c.wheels[w].is_alive()) {
        auto& tc = c.wheels[w].get_mut<ox::TransformComponent>();
        tc.rotation = w < 2 ? steer * spin : spin;
        c.wheels[w].modified<ox::TransformComponent>();
      }
    }

    if (c.siren_red && c.siren_blue) {
      const auto phase = glm::fract(self.time * 3.0f) < 0.5f;
      const auto red = !c.net_siren || phase ? glm::vec3(1.0f) : glm::vec3(0.01f);
      const auto blue = !c.net_siren || !phase ? glm::vec3(1.0f) : glm::vec3(0.01f);
      c.siren_red.get_mut<ox::TransformComponent>().scale = red;
      c.siren_red.modified<ox::TransformComponent>();
      c.siren_blue.get_mut<ox::TransformComponent>().scale = blue;
      c.siren_blue.modified<ox::TransformComponent>();
    }
  }

  // --- players ---
  for (auto id : ALL_PLAYERS) {
    auto& p = self.pl(id);
    const auto slot = static_cast<usize>(id);
    if (!p.active || !p.entity || !p.entity.is_alive() || slot >= b.players.size()) {
      continue;
    }
    self.show_knife(id);
    if (p.car != CarID::Invalid) {
      self.hide_entity(p.entity);
      continue;
    }
    // your own feet are yours: walk now, tell the host afterwards
    if (self.is_local(id) && p.life == Life::Alive) {
      p.attack_cooldown = glm::max(0.0f, p.attack_cooldown - dt);
      p.punch_anim = glm::max(0.0f, p.punch_anim - dt * 4.0f);
      if (p.input.attack && p.attack_cooldown <= 0.0f) {
        p.punch_anim = 1.0f; // the swing shows at once, the host decides what it hit
        p.attack_cooldown = 0.28f;
      }
      self.move_on_foot(id, dt);
      continue;
    }
    const auto& sb = b.players[slot];
    const auto& sa = slot < a.players.size() && a.players[slot].generation == sb.generation ? a.players[slot] : sb;
    p.position = glm::mix(glm::vec2(net::dq_pos(sa.x), net::dq_pos(sa.z)), glm::vec2(net::dq_pos(sb.x), net::dq_pos(sb.z)), t);
    p.heading = lerp_angle(net::dq_angle(sa.heading), net::dq_angle(sb.heading), t);
    if (p.life == Life::Dead) {
      const auto lying = yaw_quat(p.heading) * glm::angleAxis(-glm::half_pi<f32>(), glm::vec3(1.0f, 0.0f, 0.0f));
      self.set_entity_pose(p.entity, to3(p.position, 0.25f), lying);
      continue;
    }
    const auto speed = speed_of(sb.speed);
    p.walk_phase += dt * speed * 2.2f;
    self.animate_limbs(p.limbs, p.walk_phase, glm::min(1.0f, speed / 4.2f), net::dq_unit(sb.punch));
    self.set_entity_pose(p.entity, to3(p.position, self.ground_height(p.position)), yaw_quat(p.heading));
  }

  // --- the rest of the street ---
  for (auto& pk : self.pickups) {
    pk.age += dt;
    self.set_entity_pose(pk.entity, to3(pk.position, 0.2f + std::sin(self.time * 4.0f + pk.age) * 0.08f), yaw_quat(self.time * 2.0f + pk.age));
  }
  for (auto& tr : self.tracers) {
    tr.life -= dt;
    if (tr.life <= 0.0f && tr.entity.is_alive()) {
      tr.entity.destruct();
    }
  }
  std::erase_if(self.tracers, [](const Tracer& tr) { return tr.life <= 0.0f; });
  if (self.heist.marker && self.heist.marker.is_alive()) {
    const auto y = self.heist.restock > 0.0f ? -50.0f : 1.6f + std::sin(self.time * 3.0f) * 0.25f;
    self.set_entity_pose(self.heist.marker, to3(self.heist.position, y), yaw_quat(self.time * 1.5f));
  }
  self.hud.heist_active = self.heist.driller != PlayerID::Invalid && self.heist.driller == self.local;
  self.hud.heist_progress = static_cast<i32>(100.0f * self.heist.progress / 8.0f);

  // the prompt line, worked out from what's on screen (the host doesn't send it)
  self.prompt_text.clear();
  if (auto* me = self.local_player(); me && self.on_foot(self.local)) {
    for (const auto& ped : self.peds) {
      if (ped.alive && ped.kind == PedKind::Civilian && ped.state != PedState::Driving && ped.cash > 0 &&
          glm::distance(ped.position, me->position) < 1.9f) {
        self.prompt_text = "HOLD E: ROB";
        break;
      }
    }
    if (self.prompt_text.empty()) {
      for (usize i = 0; i < self.cars.size(); i++) {
        const auto& c = self.cars[i];
        if (c.alive && glm::distance(self.car_position(static_cast<CarID>(i)), me->position) < 4.2f) {
          self.prompt_text = c.driver != PedID::Invalid || c.player_driver != PlayerID::Invalid ? "F: CARJACK" : "F: GET IN";
          break;
        }
      }
    }
    if (glm::distance(me->position, self.heist.position) < 2.4f && self.heist.restock <= 0.0f) {
      self.prompt_text = "HOLD E: CRACK THE VAULT";
    }
  }
  if (auto* me = self.local_player(); me && me->life != Life::Alive) {
    me->life_timer += dt;
  }
}

auto World::make_input(this World& self, u32 sequence) -> net::InputMessage {
  auto m = net::InputMessage{};
  m.sequence = sequence;
  auto* me = self.local_player();
  if (!me) {
    return m;
  }
  const auto& in = me->input;
  if (in.enter_exit) {
    me->net_enter_count++;
  }
  if (in.switch_weapon) {
    me->net_switch_count++;
  }
  m.buttons = static_cast<u16>((in.handbrake ? net::BUTTON_HANDBRAKE : 0) | (in.sprint ? net::BUTTON_SPRINT : 0) |
                               (in.attack ? net::BUTTON_ATTACK : 0) | (in.interact ? net::BUTTON_INTERACT : 0) |
                               (in.horn ? net::BUTTON_HORN : 0) | (in.has_aim ? net::BUTTON_HAS_AIM : 0));
  m.move_x = net::q_snorm(in.move.x);
  m.move_y = net::q_snorm(in.move.y);
  m.throttle = net::q_snorm(in.throttle);
  m.steer = net::q_snorm(in.steer);
  m.aim_x = net::q_pos(in.aim.x);
  m.aim_z = net::q_pos(in.aim.y);
  m.enter_exit_count = me->net_enter_count;
  m.switch_weapon_count = me->net_switch_count;
  if (self.on_foot(self.local) && me->entity.has<ox::CharacterControllerComponent>()) {
    m.buttons |= net::BUTTON_ON_FOOT;
    m.x = net::q_pos(me->position.x);
    m.z = net::q_pos(me->position.y);
    m.heading = net::q_angle(me->heading);
    m.teleport_seen = me->teleport_seq;
  }
  return m;
}
} // namespace oxcity
