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
#include "NetMessages.hpp"

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
constexpr i32 MAX_PLAYERS = 4; // multiplayer slots, slot 0 is the local player offline and the host on a listen server
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
  bool has_aim = false;       // the mouse cursor is over the city
  glm::vec2 aim = {};         // world x/z under the cursor, at bullet height
};

// --- gameplay objects -----------------------------------------------------------------------------------------------

enum class Weapon : u8 { Fists = 0, Knife, Pistol };
enum class PedKind : u8 { Civilian = 0, Cop, Guard };
enum class PedState : u8 { Wander = 0, Flee, Chase, Attack, Dead, Driving, Idle };
enum class CarRole : u8 { Parked = 0, Traffic, Police, Abandoned };
// what the local screen shows. Being dead or arrested is per player (Life), the city doesn't stop for it
enum class GameState : u8 { MainMenu = 0, Playing, Paused };
enum class Life : u8 { Alive = 0, Dead, Arrested };
// Offline: single player. Host: simulates the city and plays in it (listen server). Server: simulates, nobody plays
// locally (dedicated). Client: renders what the host sends and sends its input back
enum class NetRole : u8 { Offline = 0, Host, Server, Client };

enum class PedID : i32 { Invalid = -1 };
enum class CarID : i32 { Invalid = -1 };
enum class NodeID : i32 { Invalid = -1 };
enum class PlayerID : i32 { Invalid = -1 };

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
  f32 drip_timer = 0.0f; // wounded peds leave a blood trail
  u8 model = 0;      // AssetTable::peds index, then cop (5), then guard (6): what a client has to spawn
  u8 generation = 0; // bumped when the slot is reused, so a client knows it's someone new
  f32 anim_speed = 0.0f; // m/s the legs are walking at
  f32 punch = 0.0f;
};

struct Car {
  flecs::entity entity = {};
  std::array<flecs::entity, 4> wheels = {};
  flecs::entity siren_red = {};
  flecs::entity siren_blue = {};
  CarRole role = CarRole::Parked;
  std::string model = {};
  std::string display_name = {};
  PedID driver = PedID::Invalid;              // an AI driver
  PlayerID player_driver = PlayerID::Invalid; // a player at the wheel; both Invalid means empty
  NodeID from_node = NodeID::Invalid;
  NodeID to_node = NodeID::Invalid;
  f32 stuck_timer = 0.0f;
  f32 reverse_timer = 0.0f;
  f32 health = 100.0f;
  f32 cruise_speed = 9.0f;
  bool alive = true;
  bool exploded = false;
  PlayerID last_hit_by = PlayerID::Invalid; // who gets the credit (and the combo) if it blows up
  f32 smoke_timer = 0.0f;
  f32 tire_timer = 0.0f;
  glm::vec3 last_velocity = {};
  u8 generation = 0;
  // client side: what the host said, there's no rigid body to ask
  glm::vec3 net_velocity = {};
  f32 net_steer = 0.0f;
  f32 wheel_spin = 0.0f;
  bool net_siren = false;
};

struct Pickup {
  u16 id = 0;
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

// a few emitter entities per effect, used round robin so bursts in different places in the same frame don't
// drag each other's position around
struct FxPool {
  std::vector<flecs::entity> emitters = {};
  usize next = 0;
};

// a short lived point light, the flash of an explosion
struct FlashLight {
  flecs::entity entity = {};
  f32 life = 0.0f;
};

// the local "game feel" state: kills freeze the world for a beat (offline only, online the city can't stop for one
// player), shake the camera, flash the screen and pop the combo. The combo and the score themselves are per player
struct Juice {
  f32 hitstop = 0.0f;    // seconds of near-frozen simulation left
  f32 shake = 0.0f;      // trauma, 0..1; the camera offset goes with its square
  f32 flash = 0.0f;      // red screen flash, 0..1
  f32 combo_pop = 0.0f;  // 1 on a new kill, eases back to 0; drives the HUD text's punch
};

struct Wanted {
  f32 heat = 0.0f; // stars are floor(heat), 0..5
  f32 cooldown = 0.0f; // seconds since the last crime seen by a cop
  f32 arrest_timer = 0.0f;
};

struct Heist {
  glm::vec2 position = {}; // the vault marker in front of the bank
  f32 progress = 0.0f; // seconds held
  f32 restock = 0.0f;  // seconds until the bank can be robbed again
  f32 alarm = 0.0f;    // seconds the alarm keeps ringing
  PlayerID driller = PlayerID::Invalid; // who's cracking it right now, they get the money
  flecs::entity marker = {};
};

// "ALICE WASTED BOB" in the corner for a few seconds
struct KillFeedLine {
  std::string text = {};
  f32 age = 0.0f;
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
  flecs::entity knife = {}; // blade model in the right hand, parked out of sight unless the knife is out

  // multiplayer: every slot is a full player with its own wallet, wanted level and record
  bool active = false;
  std::string name = {};
  Life life = Life::Alive;
  f32 life_timer = 0.0f; // seconds since dying or getting arrested
  std::string life_note = {}; // "HOSPITAL BILL: $120", shown under the big text
  Wanted wanted = {};
  Stats stats = {};
  i32 score = 0;
  i32 kills = 0; // other players wasted
  i32 combo = 0; // kills in a row, each within COMBO_WINDOW of the last
  f32 combo_timer = 0.0f;
  GameInput input = {}; // what this player wants this frame, from the keyboard or from the network
  u8 generation = 0;    // bumped every time the slot is taken by someone new
  f32 anim_speed = 0.0f;

  // networking. The host moves a remote player to where their client says they walked (checked for
  // plausibility); every time the host moves someone itself (spawn, respawn, cars) `teleport_seq` changes and the
  // client snaps its own character to the host's position
  bool remote = false;
  u8 teleport_seq = 0;
  bool has_report = false;
  glm::vec2 reported_position = {};
  f32 reported_heading = 0.0f;
  u8 reported_teleport = 0;
  // one-shot presses sent as counters (see net::InputMessage). The client counts what it sent, the host what it saw
  u8 net_enter_count = 0;
  u8 net_switch_count = 0;
  u32 net_input_sequence = 0;
};

// rows of the RmlUi data model's arrays (data-for)
struct NameTag {
  Rml::String name = "";
  f32 x = 0.0f; // context pixels
  f32 y = 0.0f;
  i32 stars = 0;
};

struct ScoreRow {
  Rml::String name = "";
  i32 score = 0;
  i32 kills = 0;
  i32 stars = 0;
  bool me = false;
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
  f32 kill_flash = 0.0f;     // opacity of the red overlay
  Rml::String combo = "";    // "3X COMBO", empty hides it
  f32 combo_scale = 1.0f;
  f32 combo_tilt = 0.0f;     // degrees
  i32 score = 0;
  Rml::String score_popup = ""; // "+300"
  bool settings_open = false;
  bool sfx_on = true;
  bool music_on = true;

  // multiplayer menu
  bool online = false;         // hosting or joined: the pause menu offers LEAVE instead of NEW GAME
  bool join_open = false;      // the join panel replaces the main buttons
  Rml::String join_address = "127.0.0.1";
  Rml::String player_name = "PLAYER";
  i32 host_port = 7777;
  Rml::String net_status = ""; // "CONNECTING...", "THE SERVER IS FULL", ...
  std::vector<NameTag> tags = {};
  std::vector<ScoreRow> scoreboard = {};
  std::vector<Rml::String> kill_feed = {};
};

// assets are looked up by source path once, then referenced by uuid
struct AssetTable {
  std::array<ox::UUID, MAX_PLAYERS> players = {}; // one jacket colour per slot
  std::array<ox::UUID, 5> peds = {};
  ox::UUID cop = {};
  ox::UUID guard = {};
  ox::UUID sedan = {}, sports = {}, taxi = {}, police = {}, van = {}, wheel = {};
  ox::UUID road_straight = {}, road_cross = {}, block = {}, park = {}, bank = {}, vault_door = {};
  std::array<ox::UUID, 6> buildings = {};
  ox::UUID street_lamp = {}, tree = {}, ground = {};
  ox::UUID cash = {}, tracer = {}, marker = {}, knife = {}, fx = {};
  std::array<ox::UUID, 4> blood_decals = {};
  std::array<ox::UUID, 2> scorch_decals = {};
  std::array<ox::UUID, 4> blood_streaks = {}; // directional spatter, thrown along +z from the origin

  // particle systems (.oxparticle)
  ox::UUID fx_blood = {}, fx_muzzle = {}, fx_sparks = {};
  ox::UUID fx_explosion = {}, fx_smoke = {}, fx_tire = {}, fx_casings = {}, fx_sparkle = {};

  // audio
  ox::UUID sfx_engine = {}, sfx_siren = {}, sfx_horn = {}, sfx_gunshot = {}, sfx_punch = {}, sfx_cash = {};
  ox::UUID sfx_footstep = {}, sfx_door = {}, sfx_crash = {}, sfx_alarm = {}, sfx_pager = {}, sfx_death = {};
  ox::UUID sfx_radio = {}, sfx_knife_swing = {}, sfx_stab = {}, sfx_splat = {}, sfx_explosion = {};
  ox::UUID music_menu = {};
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

  // --- players (Player.cpp) ---
  auto spawn_player(this World& self, PlayerID id, glm::vec2 position) -> void;
  auto despawn_player(this World& self, PlayerID id) -> void;
  auto update_players(this World& self, f32 dt) -> void;
  auto update_player(this World& self, PlayerID id, f32 dt) -> void;
  auto move_on_foot(this World& self, PlayerID id, f32 dt) -> void; // walking, facing, walk cycle
  auto show_knife(this World& self, PlayerID id) -> void;
  auto follow_report(this World& self, PlayerID id, f32 dt) -> void; // host: a remote player's own movement
  auto player_position(this const World& self, PlayerID id) -> glm::vec2;
  auto player_attack(this World& self, PlayerID id) -> void;
  auto enter_or_exit_car(this World& self, PlayerID id) -> void;
  auto eject_player(this World& self, PlayerID id) -> void; // out of their car onto the pavement
  // `attacker` is the player who did it, Invalid for the city (cops, guards, crashes)
  auto damage_player(this World& self, PlayerID id, f32 amount, PlayerID attacker = PlayerID::Invalid) -> void;
  auto teleport_player(this World& self, PlayerID id, glm::vec2 position) -> void; // on foot only, used by autoplay
  auto in_play(this const World& self, PlayerID id) -> bool; // in the game, alive and out of the menus
  auto on_foot(this const World& self, PlayerID id) -> bool;
  // closest player in play within `range`, optionally only the ones the police want
  auto nearest_player(this const World& self, glm::vec2 position, f32 range, bool wanted_only = false) -> PlayerID;
  auto distance_to_players(this const World& self, glm::vec2 position) -> f32; // to the closest one in the game
  auto listener_position(this const World& self) -> glm::vec2; // where the local screen and ears are

  // --- vehicles (Vehicles.cpp) ---
  auto update_vehicles(this World& self, f32 dt) -> void;
  auto car_position(this const World& self, CarID id) -> glm::vec2;
  auto car_heading(this const World& self, CarID id) -> f32;
  auto car_velocity(this const World& self, CarID id) -> glm::vec3;
  auto drive_car(this World& self, CarID id, f32 throttle, f32 steer, f32 brake, f32 handbrake) -> void;
  auto teleport_car(this World& self, CarID id, glm::vec2 position, f32 yaw) -> void;
  auto car_ai_follow_roads(this World& self, CarID id, f32 dt) -> void;
  auto car_ai_chase(this World& self, CarID id, PlayerID suspect, f32 dt) -> void;
  auto damage_car(this World& self, CarID id, f32 amount, PlayerID attacker = PlayerID::Invalid) -> void;

  // --- peds (Peds.cpp) ---
  auto update_peds(this World& self, f32 dt) -> void;
  auto kill_ped(this World& self, PedID id, glm::vec2 impulse, PlayerID killer = PlayerID::Invalid) -> void;
  auto damage_ped(this World& self, PedID id, f32 amount, glm::vec2 from, PlayerID attacker = PlayerID::Invalid) -> void;
  auto panic_around(this World& self, glm::vec2 position, f32 radius) -> void;
  auto animate_limbs(this World& self, Limbs& limbs, f32 phase, f32 amount, f32 punch) -> void;
  auto ground_height(this const World& self, glm::vec2 p) -> f32; // sidewalks are a kerb above the road
  auto ped_model(this const World& self, u8 model) -> ox::UUID;

  // --- crime, police, bank, pickups (Crime.cpp) ---
  auto commit_crime(this World& self, PlayerID id, f32 heat, glm::vec2 where, std::string_view pager) -> void;
  auto update_crime(this World& self, f32 dt) -> void;
  auto stars(this const World& self, PlayerID id) -> i32;
  auto total_stars(this const World& self) -> i32; // everyone's, the police budget
  auto arrest_player(this World& self, PlayerID id) -> void;
  auto kill_player(this World& self, PlayerID id, PlayerID killer = PlayerID::Invalid) -> void;
  auto respawn_player(this World& self, PlayerID id) -> void;
  auto on_player_killed(this World& self, PlayerID victim, PlayerID killer) -> void; // kill feed, pager, score juice
  // `shooter` Invalid means a cop or a guard
  auto shoot(this World& self, glm::vec2 from, f32 heading, f32 damage, PlayerID shooter) -> void;

  // --- blood, particles and game feel (Fx.cpp) ---
  auto init_fx(this World& self) -> void;
  auto update_fx(this World& self, f32 real_dt) -> void;
  auto emit(this World& self, FxPool& pool, glm::vec3 position, glm::vec3 velocity, u32 count) -> void;
  auto blood_burst(this World& self, glm::vec2 position, glm::vec2 direction, u32 count) -> void;
  auto blood_decal(this World& self, glm::vec2 position, f32 size) -> void;
  auto blood_streak(this World& self, glm::vec2 position, glm::vec2 direction, f32 length, f32 width) -> void;
  auto place_decal(this World& self, const ox::UUID& model, glm::vec2 position, f32 yaw, glm::vec3 scale) -> void;
  auto muzzle_flash(this World& self, glm::vec2 muzzle, f32 heading) -> void;
  auto impact_sparks(this World& self, glm::vec3 position, glm::vec2 direction) -> void;
  auto on_kill(this World& self, glm::vec2 position, glm::vec2 direction, PlayerID killer, bool big) -> void;
  auto sim_delta(this World& self, f32 real_dt) -> f32;
  auto local_kill_juice(this World& self, i32 points, bool big) -> void; // the local player scored a kill
  auto explode(this World& self, glm::vec2 position, PlayerID attacker) -> void;
  auto explode_fx(this World& self, glm::vec2 position) -> void; // what everyone sees and hears of it
  auto shell_casing(this World& self, glm::vec2 position, f32 heading) -> void;
  auto cash_sparkle(this World& self, glm::vec2 position) -> void;
  auto drill_sparks(this World& self, glm::vec2 position) -> void;
  auto update_car_fx(this World& self, f32 dt) -> void;

  // --- multiplayer (Replication.cpp) ---
  // host side
  auto record(this World& self, net::Event event) -> void; // an effect the clients should see too
  auto build_snapshot(this World& self, u32 sequence) -> net::Snapshot;
  auto apply_input(this World& self, PlayerID id, const net::InputMessage& message) -> void;
  auto join_player(this World& self, std::string_view name) -> PlayerID; // Invalid when the server is full
  auto sound_index(this const World& self, const ox::UUID& sound) -> u16;
  auto sound_by_index(this const World& self, u16 index) -> ox::UUID;
  // client side
  auto become_client(this World& self, PlayerID slot) -> void;
  auto apply_snapshot(this World& self, net::Snapshot&& snapshot) -> void;
  auto apply_events(this World& self, const net::EventBatch& batch) -> void;
  auto apply_roster(this World& self, const net::Roster& roster) -> void;
  auto client_update(this World& self, f32 dt) -> void;
  auto make_input(this World& self, u32 sequence) -> net::InputMessage;

  // --- camera (Camera.cpp) ---
  auto update_camera(this World& self, f32 dt) -> void;

  // --- hud (Hud.cpp) ---
  auto init_hud(this World& self) -> bool;
  auto update_hud(this World& self) -> void;
  auto update_multiplayer_hud(this World& self) -> void;
  auto pager(this World& self, std::string_view message) -> void;
  auto pager_to(this World& self, PlayerID id, std::string_view message) -> void; // only that player's pager beeps

  // --- audio (Sfx.cpp) ---
  auto init_audio(this World& self) -> void;
  auto play(this World& self, const ox::UUID& sound, f32 volume = 1.0f, f32 pitch = 1.0f) -> void;
  // a sound somewhere in the city, quieter the further it is from the local listener
  auto play_at(this World& self, const ox::UUID& sound, glm::vec2 where, f32 volume = 1.0f, f32 pitch = 1.0f, f32 range = 60.0f) -> void;
  auto update_audio(this World& self, f32 dt) -> void;
  // settings menu: sound effects and music on/off, kept in oxcity_settings.txt next to the game
  auto load_settings(this World& self) -> void;
  auto save_settings(this const World& self) -> void;

  std::unique_ptr<ox::Scene> scene;
  AssetTable assets = {};
  bool holding_models = false; // runtime_models() refs taken in init, handed back in the destructor
  std::mt19937 rng;
  u32 seed = 0; // the city layout comes from it: host and clients must build theirs from the same one

  GameState state = GameState::MainMenu;
  f32 state_timer = 0.0f;
  f32 time = 0.0f;
  NetRole role = NetRole::Offline;

  std::array<Player, MAX_PLAYERS> players = {};
  PlayerID local = PlayerID{0}; // whose eyes the screen is, Invalid on a dedicated server
  std::vector<Ped> peds = {};
  std::vector<Car> cars = {};
  std::vector<Pickup> pickups = {};
  std::vector<Tracer> tracers = {};
  std::vector<RoadNode> nodes = {};
  std::vector<glm::ivec4> building_rects = {}; // solid areas in tile space (x0, z0, x1, z1), 1 tile = 1 unit
  std::vector<glm::vec4> solid_boxes = {};     // world space xz boxes (min x, min z, max x, max z)
  Heist heist = {};
  f32 police_spawn_timer = 0.0f;
  std::vector<KillFeedLine> kill_feed = {};
  u16 next_pickup_id = 1;

  // multiplayer state (Replication.cpp). The host collects events while it simulates, the session ships them
  std::vector<net::Event> net_events = {};
  // client: the last few snapshots, drawn INTERPOLATION_DELAY behind the host's clock
  std::vector<net::Snapshot> snapshots = {};
  f32 net_clock = 0.0f;
  bool headless = false; // dedicated server: no renderer, no audio, no ui

  glm::vec2 hospital = {};
  glm::vec2 police_station = {};

  flecs::entity camera = {};
  glm::vec3 camera_position = {};
  f32 camera_height = 26.0f;

  FxPool fx_blood = {};
  FxPool fx_muzzle = {};
  FxPool fx_sparks = {};
  FxPool fx_explosion = {};
  FxPool fx_smoke = {};
  FxPool fx_tire = {};
  FxPool fx_casings = {};
  FxPool fx_sparkle = {};
  std::vector<FlashLight> flashes = {};
  usize next_flash = 0;
  // decals are recycled per model, the oldest splat of a kind moves to where the newest one goes
  struct DecalRing {
    ox::UUID model = {};
    std::vector<flecs::entity> entities = {};
    usize next = 0;
  };
  std::vector<DecalRing> decal_rings = {};
  usize decals_placed = 0;
  Juice juice = {};

  HudData hud = {};
  std::unique_ptr<Rml::DataModelHandle> hud_model;
  Rml::ElementDocument* hud_document = nullptr;
  Rml::ElementDocument* menu_document = nullptr;
  bool start_requested = false;
  bool quit_requested = false;
  bool host_requested = false;  // the menu's HOST GAME, picked up by the Game module
  bool join_requested = false;  // CONNECT
  bool leave_requested = false; // LEAVE GAME
  std::string pager_text = {};
  f32 pager_timer = 0.0f;
  std::string prompt_text = {};

  f32 engine_pitch = 1.0f;
  bool engine_playing = false;
  bool siren_playing = false;
  bool radio_playing = false;
  bool alarm_playing = false;
  bool menu_music_playing = false;
  bool phases_disabled = false;

  auto random_float(this World& self, f32 lo, f32 hi) -> f32;
  auto random_int(this World& self, i32 lo, i32 hi) -> i32;
  auto ped(this World& self, PedID id) -> Ped& { return self.peds[static_cast<usize>(id)]; }
  auto car(this World& self, CarID id) -> Car& { return self.cars[static_cast<usize>(id)]; }
  auto car(this const World& self, CarID id) -> const Car& { return self.cars[static_cast<usize>(id)]; }
  auto pl(this World& self, PlayerID id) -> Player& { return self.players[static_cast<usize>(id)]; }
  auto pl(this const World& self, PlayerID id) -> const Player& { return self.players[static_cast<usize>(id)]; }
  auto local_player(this World& self) -> Player* { return self.local == PlayerID::Invalid ? nullptr : &self.pl(self.local); }
  auto is_local(this const World& self, PlayerID id) -> bool { return id != PlayerID::Invalid && id == self.local; }
  // offline the whole simulation lives here; online only the host (or dedicated server) runs it
  auto authority(this const World& self) -> bool { return self.role != NetRole::Client; }
};

// --- small helpers shared by every file ---------------------------------------------------------------------------

inline auto forward_of(f32 heading) -> glm::vec2 { return {std::sin(heading), std::cos(heading)}; }
// shots and tracers fly at roughly chest height, the mouse aim is taken at the same height
inline constexpr f32 BULLET_HEIGHT = 1.15f;
inline auto heading_of(glm::vec2 dir) -> f32 { return std::atan2(dir.x, dir.y); }
inline auto yaw_quat(f32 heading) -> glm::quat { return glm::angleAxis(heading, glm::vec3(0.0f, 1.0f, 0.0f)); }
inline auto to3(glm::vec2 p, f32 y = 0.0f) -> glm::vec3 { return {p.x, y, p.y}; }
inline auto to2(glm::vec3 p) -> glm::vec2 { return {p.x, p.z}; }
// seen from above (+y), the right hand side of something travelling along `dir` (x, z)
inline auto right_of(glm::vec2 dir) -> glm::vec2 { return {-dir.y, dir.x}; }
// every player slot, used or not: `for (auto id : ALL_PLAYERS) { if (!world.pl(id).active) continue; ... }`
inline constexpr auto ALL_PLAYERS = std::array{PlayerID{0}, PlayerID{1}, PlayerID{2}, PlayerID{3}};
static_assert(ALL_PLAYERS.size() == MAX_PLAYERS);
auto car_model_index(std::string_view name) -> u8; // position in the vehicle specs, what goes over the wire
auto car_model_name(u8 index) -> std::string_view;
auto char_wreck(World& self, flecs::entity root) -> void; // burnt-out material on every mesh of a wrecked car
auto wrap_angle(f32 a) -> f32;
auto approach(f32 current, f32 target, f32 max_delta) -> f32;
} // namespace oxcity
