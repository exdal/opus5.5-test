#pragma once

// OxCity game state. One `World` owns the Oxylus scene plus every gameplay object in it. The logic is split
// across the .cpp files next to this header by topic (city, peds, vehicles, crime, hud, ...), the same way the
// engine splits `Scene`.

#include <RmlUi/Core/Types.h>
#include <array>
#include <flecs.h>
#include <glm/gtc/quaternion.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "Core/Types.hpp"
#include "Core/UUID.hpp"

namespace Rml {
class ElementDocument;
class DataModelHandle;
} // namespace Rml

namespace ox {
class Scene;
class Timestep;
} // namespace ox

namespace oxcity {
// --- tuning ---------------------------------------------------------------------------------------------------------

constexpr f32 TILE = 12.0f;  // metres per city tile, must match tools/assetgen/models.py
constexpr i32 CITY_TILES = 16; // city is CITY_TILES x CITY_TILES tiles
constexpr i32 ROAD_EVERY = 3;  // every third row/column of tiles is a road
constexpr f32 LANE_OFFSET = 2.8f;
constexpr f32 SIDEWALK_INSET = 4.6f; // where pedestrians walk, from a block tile's centre

constexpr i32 MAX_PEDS = 34;
constexpr i32 MAX_TRAFFIC = 14;
constexpr i32 PARKED_CARS = 8;

// --- input ----------------------------------------------------------------------------------------------------------

// What the player wants this frame, filled either from the keyboard or from the autoplay script
struct GameInput {
  glm::vec2 move = {}; // on foot, screen space: x right, y up
  f32 throttle = 0.0f; // in a car: [-1,1]
  f32 steer = 0.0f;    // in a car: [-1,1], positive is right
  bool handbrake = false;
  bool sprint = false;
  bool attack = false;        // held
  bool enter_exit = false;    // pressed
  bool interact = false;      // held (rob bank, mug)
  bool switch_weapon = false; // pressed
  bool horn = false;          // held
  bool pause = false;         // pressed
  bool confirm = false;       // pressed
};

// --- gameplay objects -----------------------------------------------------------------------------------------------

enum class Weapon : u8 { Fists = 0, Pistol };
enum class PedKind : u8 { Civilian = 0, Cop, Guard };
enum class PedState : u8 { Wander = 0, Flee, Chase, Attack, Dead, Driving, Idle };
enum class CarRole : u8 { Parked = 0, Traffic, Police, Abandoned };
enum class GameState : u8 { MainMenu = 0, Playing, Paused, Dead, Arrested };

enum class PedID : i32 { Invalid = -1 };
enum class CarID : i32 { Invalid = -1 };
enum class NodeID : i32 { Invalid = -1 };

struct Limbs {
  flecs::entity torso = {};
  flecs::entity leg_l = {};
  flecs::entity leg_r = {};
  flecs::entity arm_l = {};
  flecs::entity arm_r = {};
};

struct Ped {
  flecs::entity entity = {};
  Limbs limbs = {};
  PedKind kind = PedKind::Civilian;
  PedState state = PedState::Wander;
  glm::vec2 position = {};
  glm::vec2 velocity = {};
  f32 heading = 0.0f; // radians, 0 faces +z
  glm::vec2 target = {};
  f32 health = 100.0f;
  f32 walk_phase = 0.0f;
  f32 timer = 0.0f;
  f32 attack_cooldown = 0.0f;
  i32 cash = 0;
  CarID car = CarID::Invalid; // the car this ped drives, cops and traffic drivers
  bool alive = true;
  f32 dead_time = 0.0f;
  f32 fall = 0.0f; // 0 standing, 1 lying flat
};

struct Car {
  flecs::entity entity = {};
  std::array<flecs::entity, 4> wheels = {};
  flecs::entity siren_red = {};
  flecs::entity siren_blue = {};
  CarRole role = CarRole::Parked;
  std::string model = {};
  std::string display_name = {};
  PedID driver = PedID::Invalid; // Invalid and !player_inside means empty
  bool player_inside = false;
  NodeID from_node = NodeID::Invalid;
  NodeID to_node = NodeID::Invalid;
  f32 stuck_timer = 0.0f;
  f32 reverse_timer = 0.0f;
  f32 health = 100.0f;
  f32 cruise_speed = 9.0f;
  bool alive = true;
  glm::vec3 last_velocity = {};
};

struct Pickup {
  flecs::entity entity = {};
  glm::vec2 position = {};
  i32 cash = 0;
  i32 ammo = 0;
  f32 age = 0.0f;
  bool taken = false;
};

struct Tracer {
  flecs::entity entity = {};
  f32 life = 0.0f;
};

// intersection in the road graph, traffic drives node to node
struct RoadNode {
  glm::vec2 position = {};
  std::vector<NodeID> neighbours = {};
};

struct Player {
  flecs::entity entity = {};
  Limbs limbs = {};
  glm::vec2 position = {};
  f32 heading = 0.0f;
  f32 health = 100.0f;
  i32 cash = 0;
  i32 ammo = 60;
  Weapon weapon = Weapon::Pistol;
  CarID car = CarID::Invalid;
  f32 attack_cooldown = 0.0f;
  f32 walk_phase = 0.0f;
  f32 footstep_timer = 0.0f;
  f32 punch_anim = 0.0f;
  f32 invulnerable = 0.0f;
};

struct Wanted {
  f32 heat = 0.0f; // stars are floor(heat), 0..5
  f32 cooldown = 0.0f; // seconds since the last crime seen by a cop
  f32 arrest_timer = 0.0f;
  f32 spawn_timer = 0.0f;
};

struct Heist {
  glm::vec2 position = {}; // the vault marker in front of the bank
  f32 progress = 0.0f; // seconds held
  f32 restock = 0.0f;  // seconds until the bank can be robbed again
  f32 alarm = 0.0f;    // seconds the alarm keeps ringing
  flecs::entity marker = {};
};

struct Stats {
  i32 peds_killed = 0;
  i32 peds_robbed = 0;
  i32 cars_stolen = 0;
  i32 banks_robbed = 0;
  i32 times_arrested = 0;
  i32 times_killed = 0;
  i32 cash_earned = 0;
};

// flat struct the RmlUi data model binds to
struct HudData {
  i32 money = 0;
  i32 wanted = 0;
  i32 health = 100;
  i32 ammo = 0;
  Rml::String weapon = "PISTOL";
  Rml::String vehicle = "";
  Rml::String pager = "";
  Rml::String prompt = "";
  Rml::String big_text = "";
  i32 heist_progress = 0; // percent
  bool heist_active = false;
  bool menu_visible = true;
  bool paused = false;
  i32 speed = 0; // km/h
  i32 multiplier = 1;
  Rml::String stats = "";
};

// assets are looked up by source path once, then referenced by uuid
struct AssetTable {
  ox::UUID player = {};
  std::array<ox::UUID, 5> peds = {};
  ox::UUID cop = {};
  ox::UUID guard = {};
  ox::UUID sedan = {}, sports = {}, taxi = {}, police = {}, van = {}, wheel = {};
  ox::UUID road_straight = {}, road_cross = {}, block = {}, park = {}, bank = {}, vault_door = {};
  std::array<ox::UUID, 6> buildings = {};
  ox::UUID street_lamp = {}, tree = {}, ground = {};
  ox::UUID cash = {}, tracer = {}, marker = {};

  // audio
  ox::UUID sfx_engine = {}, sfx_siren = {}, sfx_horn = {}, sfx_gunshot = {}, sfx_punch = {}, sfx_cash = {};
  ox::UUID sfx_footstep = {}, sfx_door = {}, sfx_crash = {}, sfx_alarm = {}, sfx_pager = {}, sfx_death = {};
  ox::UUID sfx_radio = {};
};

class World {
public:
  explicit World(u32 seed);
  ~World();

  World(const World&) = delete;
  auto operator=(const World&) -> World& = delete;

  auto init(this World& self) -> bool;
  auto update(this World& self, const GameInput& input, f32 dt) -> void;

  // --- state machine (Game.cpp) ---
  auto start_game(this World& self) -> void;
  auto set_state(this World& self, GameState state) -> void;

  // --- city (City.cpp) ---
  auto build_city(this World& self) -> void;
  auto is_road_tile(this const World& self, i32 x, i32 z) -> bool;
  auto tile_center(this const World& self, i32 x, i32 z) -> glm::vec2;
  auto world_to_tile(this const World& self, glm::vec2 p) -> glm::ivec2;
  auto is_solid(this const World& self, glm::vec2 p, f32 radius) -> bool; // buildings
  auto nearest_node(this const World& self, glm::vec2 p) -> NodeID;
  auto random_sidewalk_point(this World& self) -> glm::vec2;
  auto random_node(this World& self) -> NodeID;

  // --- spawning (Spawn.cpp) ---
  auto spawn_model(this World& self, const ox::UUID& model, glm::vec3 position, f32 yaw, glm::vec3 scale = glm::vec3(1.0f))
    -> flecs::entity;
  auto find_limbs(this World& self, flecs::entity root) -> Limbs;
  auto spawn_ped(this World& self, PedKind kind, glm::vec2 position) -> PedID;
  auto spawn_car(this World& self, std::string_view model, glm::vec2 position, f32 yaw, CarRole role) -> CarID;
  auto spawn_pickup(this World& self, glm::vec2 position, i32 cash, i32 ammo) -> void;
  auto spawn_tracer(this World& self, glm::vec3 from, glm::vec3 to) -> void;
  auto despawn_car(this World& self, CarID id) -> void;
  auto set_entity_pose(this World& self, flecs::entity e, glm::vec3 position, glm::quat rotation) -> void;
  auto hide_entity(this World& self, flecs::entity e) -> void;

  // --- player (Player.cpp) ---
  auto spawn_player(this World& self, glm::vec2 position) -> void;
  auto update_player(this World& self, const GameInput& input, f32 dt) -> void;
  auto player_position(this const World& self) -> glm::vec2;
  auto player_attack(this World& self) -> void;
  auto enter_or_exit_car(this World& self) -> void;
  auto damage_player(this World& self, f32 amount) -> void;
  auto teleport_player(this World& self, glm::vec2 position) -> void; // on foot only, used by autoplay

  // --- vehicles (Vehicles.cpp) ---
  auto update_vehicles(this World& self, const GameInput& input, f32 dt) -> void;
  auto car_position(this const World& self, CarID id) -> glm::vec2;
  auto car_heading(this const World& self, CarID id) -> f32;
  auto car_velocity(this const World& self, CarID id) -> glm::vec3;
  auto drive_car(this World& self, CarID id, f32 throttle, f32 steer, f32 brake, f32 handbrake) -> void;
  auto teleport_car(this World& self, CarID id, glm::vec2 position, f32 yaw) -> void;
  auto car_ai_follow_roads(this World& self, CarID id, f32 dt) -> void;
  auto car_ai_chase(this World& self, CarID id, glm::vec2 target, f32 dt) -> void;
  auto damage_car(this World& self, CarID id, f32 amount) -> void;

  // --- peds (Peds.cpp) ---
  auto update_peds(this World& self, f32 dt) -> void;
  auto kill_ped(this World& self, PedID id, glm::vec2 impulse) -> void;
  auto damage_ped(this World& self, PedID id, f32 amount, glm::vec2 from) -> void;
  auto panic_around(this World& self, glm::vec2 position, f32 radius) -> void;
  auto animate_limbs(this World& self, Limbs& limbs, f32 phase, f32 amount, f32 punch) -> void;

  // --- crime, police, bank, pickups (Crime.cpp) ---
  auto commit_crime(this World& self, f32 heat, glm::vec2 where, std::string_view pager) -> void;
  auto update_crime(this World& self, const GameInput& input, f32 dt) -> void;
  auto stars(this const World& self) -> i32;
  auto arrest_player(this World& self) -> void;
  auto kill_player(this World& self) -> void;
  auto respawn_player(this World& self) -> void;
  auto shoot(this World& self, glm::vec2 from, f32 heading, f32 damage, bool by_player) -> void;

  // --- camera (Camera.cpp) ---
  auto update_camera(this World& self, f32 dt) -> void;

  // --- hud (Hud.cpp) ---
  auto init_hud(this World& self) -> bool;
  auto update_hud(this World& self) -> void;
  auto pager(this World& self, std::string_view message) -> void;

  // --- audio (Sfx.cpp) ---
  auto init_audio(this World& self) -> void;
  auto play(this World& self, const ox::UUID& sound, f32 volume = 1.0f, f32 pitch = 1.0f) -> void;
  auto update_audio(this World& self, f32 dt) -> void;

  std::unique_ptr<ox::Scene> scene;
  AssetTable assets = {};
  bool holding_models = false; // runtime_models() refs taken in init, handed back in the destructor
  std::mt19937 rng;

  GameState state = GameState::MainMenu;
  f32 state_timer = 0.0f;
  f32 time = 0.0f;

  Player player = {};
  std::vector<Ped> peds = {};
  std::vector<Car> cars = {};
  std::vector<Pickup> pickups = {};
  std::vector<Tracer> tracers = {};
  std::vector<RoadNode> nodes = {};
  std::vector<glm::ivec4> building_rects = {}; // solid areas in tile space (x0, z0, x1, z1), 1 tile = 1 unit
  std::vector<glm::vec4> solid_boxes = {};     // world space xz boxes (min x, min z, max x, max z)
  Wanted wanted = {};
  Heist heist = {};
  Stats stats = {};

  glm::vec2 hospital = {};
  glm::vec2 police_station = {};

  flecs::entity camera = {};
  glm::vec3 camera_position = {};
  f32 camera_height = 26.0f;

  HudData hud = {};
  std::unique_ptr<Rml::DataModelHandle> hud_model;
  Rml::ElementDocument* hud_document = nullptr;
  Rml::ElementDocument* menu_document = nullptr;
  bool start_requested = false;
  bool quit_requested = false;
  std::string pager_text = {};
  f32 pager_timer = 0.0f;
  std::string prompt_text = {};

  f32 engine_pitch = 1.0f;
  bool engine_playing = false;
  bool siren_playing = false;
  bool radio_playing = false;
  bool alarm_playing = false;
  bool phases_disabled = false;

  auto random_float(this World& self, f32 lo, f32 hi) -> f32;
  auto random_int(this World& self, i32 lo, i32 hi) -> i32;
  auto ped(this World& self, PedID id) -> Ped& { return self.peds[static_cast<usize>(id)]; }
  auto car(this World& self, CarID id) -> Car& { return self.cars[static_cast<usize>(id)]; }
  auto car(this const World& self, CarID id) -> const Car& { return self.cars[static_cast<usize>(id)]; }
};

// --- small helpers shared by every file ---------------------------------------------------------------------------

inline auto forward_of(f32 heading) -> glm::vec2 { return {std::sin(heading), std::cos(heading)}; }
inline auto heading_of(glm::vec2 dir) -> f32 { return std::atan2(dir.x, dir.y); }
inline auto yaw_quat(f32 heading) -> glm::quat { return glm::angleAxis(heading, glm::vec3(0.0f, 1.0f, 0.0f)); }
inline auto to3(glm::vec2 p, f32 y = 0.0f) -> glm::vec3 { return {p.x, y, p.y}; }
inline auto to2(glm::vec3 p) -> glm::vec2 { return {p.x, p.z}; }
// seen from above (+y), the right hand side of something travelling along `dir` (x, z)
inline auto right_of(glm::vec2 dir) -> glm::vec2 { return {-dir.y, dir.x}; }
auto wrap_angle(f32 a) -> f32;
auto approach(f32 current, f32 target, f32 max_delta) -> f32;
} // namespace oxcity
