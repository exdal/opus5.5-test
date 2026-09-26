#include <cstdlib>
#include <string>

#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"
#include "Core/DefaultModules.hpp"
#include "Game.hpp"
#include "Networking/NetworkManager.hpp"
#include "ParticleAssets.hpp"
#include "Physics/Physics.hpp"
#include "Scripting/LuaManager.hpp"
#include "Server.hpp"

// OxCity
//   --autoplay            drive the player with the scripted test run in Autoplay.cpp
//   --frames N            quit after N frames
//   --screenshots DIR     write the autoplay screenshots into DIR
//   --autoplay-from N     autoplay, but skip ahead to step N after the menu (7 = police chase, arrest, respawn)
//   --fixed-dt S          step the simulation with a fixed delta time
//   --seed N              city / traffic seed
//   --width W --height H  window size
//   --write-particles DIR build the particle graphs and write them as .oxparticle files, then exit
//   --host [PORT]         start a listen server (default port 7777) and play
//   --join ADDR[:PORT]    join a game
//   --name NAME           your name in multiplayer
//   --server [PORT]       dedicated server: no window, no renderer, just the city and the network
//   --net-autoplay ROLE   scripted multiplayer test player: host, shooter or target (tools/run_net_test.sh)
auto main(int argc, char** argv) -> int {
  // authoring mode: no window, no engine modules, just the particle graph API
  for (int i = 1; i + 1 < argc; i++) {
    if (std::string_view(argv[i]) == "--write-particles") {
      return oxcity::write_particle_assets(argv[i + 1]) ? EXIT_SUCCESS : EXIT_FAILURE;
    }
  }

  auto app = ox::App(argc, argv);
  const auto& args = app.get_command_line_args();

  auto options = oxcity::GameOptions{};
  options.autoplay = args.contains("--autoplay");
  auto value_of = [&args](std::string_view flag) -> std::string {
    if (auto index = args.get_index(flag)) {
      if (auto value = args.get(*index + 1)) {
        return value->arg_str;
      }
    }
    return {};
  };
  if (auto frames = value_of("--frames"); !frames.empty()) {
    options.frame_limit = std::atoi(frames.c_str());
  }
  if (auto dir = value_of("--screenshots"); !dir.empty()) {
    options.screenshot_dir = dir;
  }
  if (auto dt = value_of("--fixed-dt"); !dt.empty()) {
    options.fixed_dt = static_cast<f32>(std::atof(dt.c_str()));
  }
  if (auto from = value_of("--autoplay-from"); !from.empty()) {
    options.autoplay = true;
    options.autoplay_from = std::atoi(from.c_str());
  }
  if (args.contains("--host")) {
    const auto port = value_of("--host");
    options.host_port = !port.empty() && port[0] != '-' ? static_cast<u16>(std::atoi(port.c_str())) : 7777;
  }
  options.join_address = value_of("--join");
  options.player_name = value_of("--name");
  options.net_autoplay = value_of("--net-autoplay");
  if (auto seed = value_of("--seed"); !seed.empty()) {
    options.seed = static_cast<u32>(std::atoi(seed.c_str()));
  }
  if (args.contains("--server")) {
    const auto port = value_of("--server");
    options.host_port = !port.empty() && port[0] != '-' ? static_cast<u16>(std::atoi(port.c_str())) : 7777;
    // no window and no Renderer: App then has no render context to read its frame limit from, so give it one
    // (it also keeps the server from spinning a core flat out)
    app.with_name("OxCity Server")
      .with_frame_limit(60)
      .with<ox::LuaManager>()
      .with<ox::AssetManager>()
      .with<ox::Physics>()
      .with<ox::NetworkManager>()
      .with<oxcity::Server>(options)
      .run();
    return 0;
  }

  auto width = 1280u;
  auto height = 720u;
  if (auto w = value_of("--width"); !w.empty()) {
    width = static_cast<u32>(std::atoi(w.c_str()));
  }
  if (auto h = value_of("--height"); !h.empty()) {
    height = static_cast<u32>(std::atoi(h.c_str()));
  }

  app.with_name("OxCity")
    .with_window(
      ox::WindowInfo{
        .title = "OxCity",
        .width = width,
        .height = height,
        .flags = ox::WindowFlag::Centered | ox::WindowFlag::Resizable,
      }
    )
    .with(ox::DefaultModules{})
    .with<oxcity::Game>(options)
    .run();

  return 0;
}
