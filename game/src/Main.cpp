#include <cstdlib>
#include <string>

#include "Core/App.hpp"
#include "Core/DefaultModules.hpp"
#include "Game.hpp"

// OxCity
//   --autoplay            drive the player with the scripted test run in Autoplay.cpp
//   --frames N            quit after N frames
//   --screenshots DIR     write the autoplay screenshots into DIR
//   --fixed-dt S          step the simulation with a fixed delta time
//   --seed N              city / traffic seed
//   --width W --height H  window size
auto main(int argc, char** argv) -> int {
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
  if (auto seed = value_of("--seed"); !seed.empty()) {
    options.seed = static_cast<u32>(std::atoi(seed.c_str()));
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
