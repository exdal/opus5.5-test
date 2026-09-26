#include "Net.hpp"

#include <charconv>
#include <enet.h>

#include "Core/App.hpp"
#include "Networking/NetworkManager.hpp"
#include "Utils/Log.hpp"
#include "Utils/Timestep.hpp"

namespace oxcity {
using ox::RPCParameter;

// NetServer reports disconnects through a virtual (and the global event bus, which every server in the process
// shares); a subclass hands them to the session that owns it
struct OxServer : ox::NetServer {
  NetSession* session = nullptr;

  OxServer(ENetHost* host, NetSession* session_) : ox::NetServer(host), session(session_) {}
  auto on_client_disconnect(ox::NetClientID id) -> void override { session->dropped.push_back(id); }
};

static auto bytes_param(std::vector<u8> bytes) -> RPCParameter { return RPCParameter{.value = std::move(bytes)}; }
static auto int_param(i64 v) -> RPCParameter { return RPCParameter{.value = v}; }
static auto str_param(std::string v) -> RPCParameter { return RPCParameter{.value = std::move(v)}; }

static auto bytes_of(std::span<RPCParameter> params, usize i) -> std::span<const u8> {
  return i < params.size() ? params[i].as_span<u8>() : std::span<const u8>{};
}
static auto int_of(std::span<RPCParameter> params, usize i, i64 fallback) -> i64 {
  if (i < params.size()) {
    if (auto v = params[i].as_int64()) {
      return *v;
    }
  }
  return fallback;
}
static auto str_of(std::span<RPCParameter> params, usize i) -> std::string {
  return i < params.size() ? std::string(params[i].as_str()) : std::string{};
}

NetSession::~NetSession() { this->shutdown(); }

auto NetSession::host(this NetSession& self, World& world, u16 port, bool dedicated) -> bool {
  self.shutdown();
  auto& network = ox::App::mod<ox::NetworkManager>();
  // a listen server is one of the players itself
  const auto max_remotes = static_cast<u32>(dedicated ? MAX_PLAYERS : MAX_PLAYERS - 1);
  self.server = network.create_server<OxServer>(port, max_remotes, &self);
  if (!self.server) {
    self.status = Status::Failed;
    self.status_text = fmt::format("COULDN'T OPEN UDP PORT {}", port);
    return false;
  }
  self.server->set_tick_rate(net::SNAPSHOT_RATE);
  self.register_server_procs();

  world.role = dedicated ? NetRole::Server : NetRole::Host;
  if (dedicated) {
    world.local = PlayerID::Invalid;
  }
  self.server_name = dedicated ? "OXCITY DEDICATED" : fmt::format("{}'S CITY", world.local_player() ? world.local_player()->name : "SOMEONE");
  self.status = Status::Hosting;
  self.status_text = fmt::format("HOSTING ON UDP PORT {}", port);
  OX_LOG_INFO("OxCity net: hosting on port {} ({}), seed {}", port, dedicated ? "dedicated" : "listen server", world.seed);
  return true;
}

auto NetSession::join(this NetSession& self, std::string_view address, std::string_view name) -> bool {
  self.shutdown();
  auto host_name = std::string(address);
  auto port = net::DEFAULT_PORT;
  if (const auto colon = host_name.rfind(':'); colon != std::string::npos) {
    const auto port_text = std::string_view(host_name).substr(colon + 1);
    auto parsed = u16{0};
    if (std::from_chars(port_text.data(), port_text.data() + port_text.size(), parsed).ec == std::errc{} && parsed > 0) {
      port = parsed;
    }
    host_name.resize(colon);
  }
  if (host_name.empty()) {
    host_name = "127.0.0.1";
  }

  auto& network = ox::App::mod<ox::NetworkManager>();
  self.client = network.create_client();
  if (!self.client || !self.client->connect(host_name, port, 5000.0)) {
    self.status = Status::Failed;
    self.status_text = fmt::format("COULDN'T REACH {}:{}", host_name, port);
    self.shutdown();
    return false;
  }
  self.register_client_procs();
  self.player_name = std::string(name);
  self.join_sent = false;
  self.status = Status::Connecting;
  self.status_text = fmt::format("CONNECTING TO {}:{}...", host_name, port);
  return true;
}

auto NetSession::leave(this NetSession& self) -> void {
  self.shutdown();
  self.status = Status::Offline;
  self.status_text.clear();
}

auto NetSession::shutdown(this NetSession& self) -> void {
  if (!ox::App::get() || !ox::App::has_mod<ox::NetworkManager>()) {
    return;
  }
  auto& network = ox::App::mod<ox::NetworkManager>();
  if (self.server) {
    // say goodbye properly, or every client sits in a timeout for half a minute. The engine has no "close the
    // server" call, so: a reliable message, a graceful disconnect per peer, and one flush before the host goes
    self.server->broadcast_call(net::RPC_REJECT, std::array{str_param("THE HOST CLOSED THE GAME")}, true);
    for (auto id : self.server->client_ids()) {
      if (auto* c = self.server->client(id)) {
        c->disconnect(false);
      }
    }
    enet_host_flush(self.server->local_host);
    network.destroy_server(self.server);
    self.server = nullptr;
  }
  if (self.client) {
    network.destroy_client(self.client);
    self.client = nullptr;
  }
  self.remotes.clear();
  self.dropped.clear();
}

auto NetSession::register_server_procs(this NetSession& self) -> void {
  self.server->register_proc(net::RPC_JOIN, [&self](ox::NetClientID id, std::span<RPCParameter> params) {
    if (!self.world || !self.server) {
      return;
    }
    for (const auto& r : self.remotes) {
      if (r.id == id) {
        return; // joined already
      }
    }
    const auto version = int_of(params, 0, 0);
    if (version != net::PROTOCOL_VERSION) {
      self.server->call_client(id, net::RPC_REJECT, std::array{str_param("VERSION MISMATCH, UPDATE YOUR GAME")}, true);
      return;
    }
    auto name = str_of(params, 1);
    if (name.empty() || name.size() > 16) {
      name = fmt::format("PLAYER {}", self.remotes.size() + 2);
    }
    const auto slot = self.world->join_player(name);
    if (slot == PlayerID::Invalid) {
      self.server->call_client(id, net::RPC_REJECT, std::array{str_param("THE SERVER IS FULL")}, true);
      return;
    }
    self.remotes.push_back({.id = id, .slot = slot});
    self.server->call_client(
      id,
      net::RPC_WELCOME,
      std::array{int_param(static_cast<i64>(slot)), int_param(static_cast<i64>(self.world->seed)), str_param(self.server_name)},
      true
    );
    self.roster_dirty = true;
    self.world->kill_feed.push_back({.text = fmt::format("{} JOINED", name)});
    self.world->record({.kind = net::EventKind::KillFeed, .text = self.world->kill_feed.back().text});
    OX_LOG_INFO("OxCity net: '{}' joined as player {}", name, static_cast<i32>(slot));
  });

  self.server->register_proc(net::RPC_INPUT, [&self](ox::NetClientID id, std::span<RPCParameter> params) {
    if (!self.world) {
      return;
    }
    for (const auto& r : self.remotes) {
      if (r.id == id) {
        auto message = net::InputMessage{};
        if (net::unpack(bytes_of(params, 0), message)) {
          self.world->apply_input(r.slot, message);
        }
        return;
      }
    }
  });
}

auto NetSession::register_client_procs(this NetSession& self) -> void {
  self.client->register_proc(net::RPC_WELCOME, [&self](ox::NetClientID, std::span<RPCParameter> params) {
    if (!self.world) {
      return;
    }
    const auto slot = static_cast<PlayerID>(int_of(params, 0, -1));
    const auto seed = static_cast<u32>(int_of(params, 1, 0));
    if (slot == PlayerID::Invalid || static_cast<i32>(slot) >= MAX_PLAYERS) {
      return;
    }
    self.status = Status::Joined;
    self.status_text = fmt::format("JOINED {}", str_of(params, 2));
    self.welcome_slot = slot;
    if (seed != self.world->seed) {
      // a different city: the game rebuilds its world from the host's seed, then becomes a client in it
      self.rebuild_seed = seed;
    } else {
      self.world->become_client(slot);
    }
    OX_LOG_INFO("OxCity net: joined as player {} ({})", static_cast<i32>(slot), self.status_text);
  });

  self.client->register_proc(net::RPC_REJECT, [&self](ox::NetClientID, std::span<RPCParameter> params) {
    self.status = Status::Failed;
    self.status_text = str_of(params, 0);
    self.lost_host = true;
    OX_LOG_WARN("OxCity net: the host says '{}'", self.status_text);
  });

  self.client->register_proc(net::RPC_SNAPSHOT, [&self](ox::NetClientID, std::span<RPCParameter> params) {
    if (!self.world || self.world->role != NetRole::Client) {
      return;
    }
    auto snapshot = net::Snapshot{};
    if (net::unpack(bytes_of(params, 0), snapshot)) {
      self.snapshots_received++;
      self.world->apply_snapshot(std::move(snapshot));
    }
  });

  self.client->register_proc(net::RPC_EVENTS, [&self](ox::NetClientID, std::span<RPCParameter> params) {
    if (!self.world || self.world->role != NetRole::Client) {
      return;
    }
    auto batch = net::EventBatch{};
    if (net::unpack(bytes_of(params, 0), batch)) {
      self.world->apply_events(batch);
    }
  });

  self.client->register_proc(net::RPC_ROSTER, [&self](ox::NetClientID, std::span<RPCParameter> params) {
    if (!self.world) {
      return;
    }
    auto roster = net::Roster{};
    if (net::unpack(bytes_of(params, 0), roster)) {
      self.world->apply_roster(roster);
    }
  });
}

auto NetSession::handle_drops(this NetSession& self) -> void {
  for (auto id : self.dropped) {
    for (usize i = 0; i < self.remotes.size(); i++) {
      if (self.remotes[i].id != id) {
        continue;
      }
      const auto slot = self.remotes[i].slot;
      const auto name = self.world->pl(slot).name;
      self.world->despawn_player(slot);
      self.remotes.erase(self.remotes.begin() + static_cast<std::ptrdiff_t>(i));
      self.world->kill_feed.push_back({.text = fmt::format("{} LEFT", name)});
      self.world->record({.kind = net::EventKind::KillFeed, .text = self.world->kill_feed.back().text});
      self.roster_dirty = true;
      OX_LOG_INFO("OxCity net: '{}' (player {}) left", name, static_cast<i32>(slot));
      break;
    }
  }
  self.dropped.clear();
}

auto NetSession::send_roster(this NetSession& self) -> void {
  auto roster = net::Roster{};
  for (auto id : ALL_PLAYERS) {
    const auto& p = self.world->pl(id);
    if (p.active) {
      roster.players.push_back({.slot = static_cast<u8>(id), .generation = p.generation, .name = p.name});
    }
  }
  self.server->broadcast_call(net::RPC_ROSTER, std::array{bytes_param(net::pack(roster))}, true);
  self.roster_dirty = false;
}

auto NetSession::poll(this NetSession& self, World& world, const ox::Timestep& timestep) -> void {
  ZoneScoped;

  self.world = &world;
  if (self.server) {
    // the engine's NetworkManager doesn't pump its hosts, whoever made one ticks it. tick() also tells us when
    // the configured send rate says it's time for a snapshot
    self.send_tick = self.server->tick(timestep) || self.send_tick;
    self.handle_drops();
  }
  if (self.client) {
    self.client->tick(timestep);
    if (self.status == Status::Connecting && self.client->status == ox::NetClientStatus::Connected && !self.join_sent) {
      // the engine's handshake went out on this same reliable channel first, so the host knows us by now
      self.client->call_server(
        net::RPC_JOIN,
        std::array{int_param(net::PROTOCOL_VERSION), str_param(self.player_name)},
        true
      );
      self.join_sent = true;
    }
    const auto gone = self.client && (self.client->status == ox::NetClientStatus::Disconnected ||
                                      self.client->status == ox::NetClientStatus::TimedOut);
    if (gone || self.lost_host) {
      if (self.status_text.empty() || self.status == Status::Joined || self.status == Status::Connecting) {
        self.status_text = self.status == Status::Joined ? "LOST THE CONNECTION TO THE HOST" : "NOBODY ANSWERED";
      }
      self.status = Status::Failed;
      self.lost_host = true;
      self.shutdown();
    }
  }
}

auto NetSession::flush(this NetSession& self, World& world) -> void {
  ZoneScoped;

  self.world = &world;
  if (self.server) {
    // the one-shot presses that came in have been seen by this frame's World::update
    for (const auto& r : self.remotes) {
      auto& input = world.pl(r.slot).input;
      input.enter_exit = false;
      input.switch_weapon = false;
    }
    if (self.roster_dirty) {
      self.send_roster();
    }
    if (self.send_tick) {
      self.send_tick = false;
      if (!self.remotes.empty()) {
        const auto bytes = net::pack(world.build_snapshot(++self.snapshot_sequence));
        if (bytes.size() > self.largest_snapshot) {
          self.largest_snapshot = bytes.size();
          OX_LOG_INFO("OxCity net: largest snapshot so far {} bytes", bytes.size());
        }
        for (const auto& r : self.remotes) {
          auto batch = net::EventBatch{};
          for (const auto& e : world.net_events) {
            if (e.target == net::EVERYONE || e.target == static_cast<u8>(r.slot)) {
              batch.events.push_back(e);
            }
          }
          if (!batch.events.empty()) {
            self.server->call_client(r.id, net::RPC_EVENTS, std::array{bytes_param(net::pack(batch))}, true);
          }
          self.server->call_client(r.id, net::RPC_SNAPSHOT, std::array{bytes_param(bytes)}, false);
          self.snapshots_sent++;
        }
      }
      // shipped (or nobody to ship them to): start collecting the next tick's
      world.net_events.clear();
    }
  }
  if (self.client && self.status == Status::Joined && world.role == NetRole::Client) {
    const auto message = world.make_input(++self.input_sequence);
    self.client->call_server(net::RPC_INPUT, std::array{bytes_param(net::pack(message))}, false);
  }
}
} // namespace oxcity
