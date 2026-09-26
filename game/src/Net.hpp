#pragma once

#include <optional>
#include <string>
#include <vector>

#include "Networking/Fwd.hpp"
#include "World.hpp"

namespace ox {
class Timestep;
struct NetServer;
struct NetClient;
} // namespace ox

namespace oxcity {
// The multiplayer transport, on top of the engine's NetworkManager / NetServer / NetClient (ENet). One session per
// game: hosting (listen server or dedicated), joining, or nothing. It moves bytes between machines and hands them
// to the World (Replication.cpp); it knows nothing about cars or cops.
//
// Call order every frame: poll() before World::update (receive inputs / snapshots), flush() after it (send them).
class NetSession {
public:
  enum class Status : u8 { Offline = 0, Hosting, Connecting, Joined, Failed };

  NetSession() = default;
  ~NetSession();
  NetSession(const NetSession&) = delete;
  auto operator=(const NetSession&) -> NetSession& = delete;

  // start a server on `port`. `world` becomes the host's (or, dedicated, nobody's) city
  auto host(this NetSession& self, World& world, u16 port, bool dedicated) -> bool;
  // connect to `address` ("1.2.3.4", "localhost", optionally ":port")
  auto join(this NetSession& self, std::string_view address, std::string_view name) -> bool;
  auto leave(this NetSession& self) -> void;

  auto poll(this NetSession& self, World& world, const ox::Timestep& timestep) -> void;
  auto flush(this NetSession& self, World& world) -> void;

  Status status = Status::Offline;
  std::string status_text = {}; // for the menu: "CONNECTING TO ...", "SERVER IS FULL", ...

  // a client got its welcome from a host whose city seed differs from ours: the game has to rebuild its world with
  // this seed and then call World::become_client(welcome_slot)
  std::optional<u32> rebuild_seed = std::nullopt;
  PlayerID welcome_slot = PlayerID::Invalid;
  // a client lost the host; the game goes back to an offline world and shows `status_text`
  bool lost_host = false;

  // stats for the log and the net test
  u32 snapshots_sent = 0;
  u32 snapshots_received = 0;
  usize largest_snapshot = 0;

private:
  struct Remote {
    ox::NetClientID id = ox::NetClientID::Invalid;
    PlayerID slot = PlayerID::Invalid;
  };

  ox::NetServer* server = nullptr;
  ox::NetClient* client = nullptr;
  World* world = nullptr; // valid inside poll(), the RPC handlers run from there
  std::vector<Remote> remotes = {};
  std::vector<ox::NetClientID> dropped = {}; // disconnects seen by the server, handled in poll
  std::string server_name = {};
  std::string player_name = {};
  u32 snapshot_sequence = 0;
  u32 input_sequence = 0;
  bool join_sent = false;
  bool roster_dirty = false;
  bool send_tick = false;
  f32 since_snapshot = 0.0f;

  auto register_server_procs(this NetSession& self) -> void;
  auto register_client_procs(this NetSession& self) -> void;
  auto handle_drops(this NetSession& self) -> void;
  auto send_roster(this NetSession& self) -> void;
  auto shutdown(this NetSession& self) -> void;

  friend struct OxServer;
};
} // namespace oxcity
