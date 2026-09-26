#pragma once

// What OxCity sends over the wire. Everything rides the engine's RPC packets (NetServer::call_client /
// NetClient::call_server), each message packed into one `std::vector<u8>` RPC parameter with zpp::bits, the
// serializer the engine itself uses for its packets. Positions and angles are quantized so a whole city snapshot
// stays around one UDP datagram.

#include <glm/common.hpp>
#include <glm/gtc/constants.hpp>
#include <span>
#include <string>
#include <vector>
#include <zpp_bits.h>

#include "Core/Types.hpp"

namespace oxcity::net {
constexpr u16 PROTOCOL_VERSION = 1;
constexpr u16 DEFAULT_PORT = 7777;
constexpr f64 SNAPSHOT_RATE = 30.0; // per second, the host's NetServer tick rate
constexpr f32 INTERPOLATION_DELAY = 0.1f; // clients draw the city this far in the past, between two snapshots

// procedure names, hashed by the engine
constexpr auto RPC_JOIN = "oxcity.join";         // C->S reliable:   [version:i64, name:string]
constexpr auto RPC_WELCOME = "oxcity.welcome";   // S->C reliable:   [slot:i64, seed:i64, server name:string]
constexpr auto RPC_REJECT = "oxcity.reject";     // S->C reliable:   [reason:string]
constexpr auto RPC_INPUT = "oxcity.input";       // C->S unreliable: [InputMessage bytes]
constexpr auto RPC_SNAPSHOT = "oxcity.snapshot"; // S->C unreliable: [Snapshot bytes]
constexpr auto RPC_EVENTS = "oxcity.events";     // S->C reliable:   [EventBatch bytes]
constexpr auto RPC_ROSTER = "oxcity.roster";     // S->C reliable:   [Roster bytes]

// --- quantization ---------------------------------------------------------------------------------------------------

// 2 cm steps, +-655 m: the city is 192 m across
inline auto q_pos(f32 v) -> i16 { return static_cast<i16>(glm::clamp(glm::round(v * 50.0f), -32767.0f, 32767.0f)); }
inline auto dq_pos(i16 v) -> f32 { return static_cast<f32>(v) / 50.0f; }
inline auto q_angle(f32 radians) -> u16 {
  const auto turns = glm::fract(radians / glm::two_pi<f32>());
  return static_cast<u16>(glm::round(turns * 65535.0f));
}
inline auto dq_angle(u16 v) -> f32 { return static_cast<f32>(v) / 65535.0f * glm::two_pi<f32>(); }
inline auto q_unit(f32 v) -> u8 { return static_cast<u8>(glm::round(glm::clamp(v, 0.0f, 1.0f) * 255.0f)); }
inline auto dq_unit(u8 v) -> f32 { return static_cast<f32>(v) / 255.0f; }
inline auto q_snorm(f32 v) -> i8 { return static_cast<i8>(glm::round(glm::clamp(v, -1.0f, 1.0f) * 127.0f)); }
inline auto dq_snorm(i8 v) -> f32 { return static_cast<f32>(v) / 127.0f; }

// --- client -> server -----------------------------------------------------------------------------------------------

enum InputButtons : u16 {
  BUTTON_HANDBRAKE = 1 << 0,
  BUTTON_SPRINT = 1 << 1,
  BUTTON_ATTACK = 1 << 2,
  BUTTON_INTERACT = 1 << 3,
  BUTTON_HORN = 1 << 4,
  BUTTON_HAS_AIM = 1 << 5,
  BUTTON_ON_FOOT = 1 << 6, // the position below is the client's own, predicted one
};

struct InputMessage {
  u32 sequence = 0;
  u16 buttons = 0;
  i8 move_x = 0, move_y = 0;
  i8 throttle = 0, steer = 0;
  i16 aim_x = 0, aim_z = 0;
  // one-shot presses travel as counters: an unreliable packet can get lost, a counter that moved can't be missed
  u8 enter_exit_count = 0;
  u8 switch_weapon_count = 0;
  // where the client's own character is. On foot the client moves itself, the host checks it's plausible
  i16 x = 0, z = 0;
  u16 heading = 0;
  u8 teleport_seen = 0; // the last host teleport the client applied, stale positions are ignored
};

// --- server -> client -----------------------------------------------------------------------------------------------

enum PlayerFlags : u8 {
  PLAYER_ACTIVE = 1 << 0,
};

struct SnapPlayer {
  u8 generation = 0;
  u8 flags = 0;
  u8 life = 0;
  u8 weapon = 0;
  i16 x = 0, z = 0;
  u16 heading = 0;
  i16 car = -1;
  u8 health = 0;
  u8 stars = 0;
  i32 cash = 0;
  i16 ammo = 0;
  i32 score = 0;
  u16 kills = 0;
  u8 combo = 0;
  u8 walk = 0;  // walk cycle phase, 0..255 is one stride
  u8 speed = 0; // how much the legs swing, 0..1
  u8 punch = 0;
  u8 teleport = 0; // bumped whenever the host moves the player itself: spawn, respawn, cars
  u8 invulnerable = 0;
  u16 banks_robbed = 0, cars_stolen = 0, peds_robbed = 0, peds_killed = 0;
};

enum PedFlags : u8 {
  PED_ALIVE = 1 << 0,
  PED_VISIBLE = 1 << 1, // has an entity: not driving, not cleared away
  PED_HAS_CASH = 1 << 2,
};

struct SnapPed {
  u8 generation = 0;
  u8 model = 0; // AssetTable::peds index, then cop, then guard
  u8 flags = 0;
  u8 heading = 0;
  i16 x = 0, z = 0;
  u8 fall = 0;
  u8 walk = 0;
  u8 speed = 0;
  u8 punch = 0;
};

enum CarFlags : u8 {
  CAR_ALIVE = 1 << 0,
  CAR_EXPLODED = 1 << 1,
  CAR_SIREN = 1 << 2,
  CAR_AI_DRIVER = 1 << 3,
};

struct SnapCar {
  u8 generation = 0;
  u8 model = 0; // index into the vehicle specs
  u8 flags = 0;
  u8 health = 0;
  i8 player_driver = -1;
  i16 x = 0, y = 0, z = 0;
  u16 yaw = 0;
  i8 pitch = 0, roll = 0; // degrees, a car on its roof is rare enough
  i8 steer = 0;
  i8 vx = 0, vz = 0; // half metres per second, engine pitch and tyre smoke on the client
};

struct SnapPickup {
  u16 id = 0;
  i16 x = 0, z = 0;
};

struct Snapshot {
  u32 sequence = 0;
  f32 time = 0.0f; // host World::time
  u8 heist_progress = 0;
  u16 heist_alarm = 0;   // tenths of a second left
  u16 heist_restock = 0; // tenths of a second left
  i8 heist_driller = -1;
  std::vector<SnapPlayer> players = {};
  std::vector<SnapPed> peds = {};
  std::vector<SnapCar> cars = {};
  std::vector<SnapPickup> pickups = {};
};

// one-off things that happened: effects, sounds, messages. The host records them while it simulates and ships
// them reliably once per snapshot tick; clients replay them through the same World functions
enum class EventKind : u8 {
  Sound = 0,  // asset: sound index, f: x, z, volume, pitch, range
  BloodBurst, // f: x, z, dir x, dir z; count in asset
  BloodDecal, // f: x, z, size
  BloodStreak,// f: x, z, dir x, dir z, length, width
  Muzzle,     // f: x, z, heading
  Sparks,     // f: x, y, z, dir x, dir z
  Casing,     // f: x, z, heading
  Sparkle,    // f: x, z
  Drill,      // f: x, z
  Explosion,  // f: x, z
  Tracer,     // f: from xyz, to xyz
  KillJuice,  // for the killer's screen only: asset = points / 100, f0 = big
  Pager,      // text, to one player
  KillFeed,   // text, to everyone
  LifeNote,   // text, to the one who died or got arrested
};

constexpr u8 EVERYONE = 0xff;

struct Event {
  EventKind kind = EventKind::Sound;
  u8 target = EVERYONE; // a player slot, or everyone
  u16 asset = 0;
  std::array<f32, 6> f = {};
  std::string text = {};
};

struct EventBatch {
  std::vector<Event> events = {};
};

struct RosterEntry {
  u8 slot = 0;
  u8 generation = 0;
  std::string name = {};
};

struct Roster {
  std::vector<RosterEntry> players = {};
};

// --- packing --------------------------------------------------------------------------------------------------------

template <typename T>
auto pack(const T& message) -> std::vector<u8> {
  auto [data, out] = zpp::bits::data_out<u8>();
  if (zpp::bits::failure(out(message))) {
    return {};
  }
  return data;
}

template <typename T>
auto unpack(std::span<const u8> bytes, T& message) -> bool {
  auto in = zpp::bits::in(bytes);
  return zpp::bits::success(in(message));
}
} // namespace oxcity::net
