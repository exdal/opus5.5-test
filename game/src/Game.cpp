#include "Game.hpp"

#include <glm/common.hpp>
#include <stb_image_write.h>
#include <vuk/vsl/Core.hpp>

#include "Asset/AssetManager.hpp"
#include "Audio/AudioEngine.hpp"
#include "Core/App.hpp"
#include "Core/Input.hpp"
#include "Physics/Physics.hpp"
#include "Render/RenderContext.hpp"
#include "Render/Camera.hpp"
#include "Render/Renderer.hpp"
#include "Render/Window.hpp"
#include "Scene/Scene.hpp"
#include "UI/RmlUI.hpp"
#include "Utils/Log.hpp"
#include "Utils/Timestep.hpp"

namespace oxcity {
Game::Game(GameOptions options_) : options(std::move(options_)) {}

auto Game::init(this Game& self) -> std::expected<void, std::string> {
  self.world = std::make_unique<World>(self.options.seed);
  if (!self.world->init()) {
    return std::unexpected("OxCity failed to build its world, see the log above");
  }

  if (self.options.autoplay) {
    OX_LOG_INFO("OxCity: autoplay enabled");
    self.autoplay.start_step = self.options.autoplay_from;
  }

  self.net_autoplay.role = self.options.net_autoplay;
  if (self.options.host_port != 0) {
    self.world->host_requested = true;
    self.world->hud.host_port = self.options.host_port;
  }
  if (!self.options.join_address.empty()) {
    self.world->join_requested = true;
    self.world->hud.join_address = self.options.join_address;
  }

  return {};
}

auto Game::player_name(this const Game& self) -> std::string {
  if (!self.options.player_name.empty()) {
    return self.options.player_name;
  }
  return self.world && !self.world->hud.player_name.empty() ? std::string(self.world->hud.player_name) : "PLAYER";
}

auto Game::rebuild_world(this Game& self, u32 seed) -> bool {
  ZoneScoped;

  // the scene owns gpu resources, nothing can be in flight when it goes
  ox::App::get_rendercontext().wait();
  auto settings = self.world ? self.world->hud : HudData{};
  self.world.reset();
  self.world = std::make_unique<World>(seed);
  if (!self.world->init()) {
    OX_LOG_ERROR("OxCity: couldn't rebuild the world");
    return false;
  }
  // what the player typed and picked survives the new city
  self.world->hud.player_name = settings.player_name;
  self.world->hud.join_address = settings.join_address;
  return true;
}

auto Game::handle_net_requests(this Game& self) -> void {
  auto& w = *self.world;
  if (w.host_requested) {
    w.host_requested = false;
    const auto port = w.hud.host_port > 0 ? static_cast<u16>(w.hud.host_port) : net::DEFAULT_PORT;
    if (auto* me = w.local_player()) {
      me->name = self.player_name();
    }
    if (self.net.host(w, port, false)) {
      w.start_game();
      w.pager(fmt::format("{}. TELL YOUR FRIENDS.", self.net.status_text));
    }
    w.hud.net_status = self.net.status_text;
  }
  if (w.join_requested) {
    w.join_requested = false;
    self.net.join(std::string(w.hud.join_address), self.player_name());
    w.hud.net_status = self.net.status_text;
  }
  if (w.leave_requested) {
    w.leave_requested = false;
    self.net.leave();
    self.rebuild_world(self.options.seed);
  }
}

auto Game::deinit(this Game& self) -> std::expected<void, std::string> {
  // say goodbye before the engine's NetworkManager goes (it asserts every host is gone)
  self.net.leave();
  // the scene owns gpu resources, nothing can be in flight when it goes
  ox::App::get_rendercontext().wait();
  self.world.reset();
  return {};
}

auto Game::read_input(this Game& self) -> GameInput {
  auto& in = ox::App::mod<ox::Input>();
  using ox::ScanCode;

  auto held = [&in](ScanCode a, ScanCode b = ScanCode::Unknown) {
    return in.get_key_held(a) || (b != ScanCode::Unknown && in.get_key_held(b));
  };
  auto pressed = [&in](ScanCode a, ScanCode b = ScanCode::Unknown) {
    return in.get_key_pressed(a) || (b != ScanCode::Unknown && in.get_key_pressed(b));
  };

  auto input = GameInput{};
  const auto up = held(ScanCode::W, ScanCode::Up);
  const auto down = held(ScanCode::S, ScanCode::Down);
  const auto left = held(ScanCode::A, ScanCode::Left);
  const auto right = held(ScanCode::D, ScanCode::Right);

  input.move = {static_cast<f32>(right) - static_cast<f32>(left), static_cast<f32>(up) - static_cast<f32>(down)};
  if (glm::length(input.move) > 1.0f) {
    input.move = glm::normalize(input.move);
  }
  input.throttle = static_cast<f32>(up) - static_cast<f32>(down);
  input.steer = static_cast<f32>(right) - static_cast<f32>(left);
  input.handbrake = held(ScanCode::Space);
  input.sprint = held(ScanCode::LeftShift, ScanCode::RightShift);
  input.attack = held(ScanCode::LeftControl, ScanCode::RightControl) || in.get_mouse_held(ox::MouseCode::Left);
  input.enter_exit = pressed(ScanCode::F, ScanCode::Return);
  input.interact = held(ScanCode::E);
  input.switch_weapon = pressed(ScanCode::Q);
  input.horn = held(ScanCode::H);
  input.pause = pressed(ScanCode::Escape);
  input.confirm = pressed(ScanCode::Return, ScanCode::Space);

  // aim: the cursor's ray through the camera, cut at the height bullets fly at. The matrices are last frame's,
  // which is what the cursor was drawn over anyway
  const auto* camera = self.world->camera.try_get<ox::CameraComponent>();
  const auto window_size = glm::vec2(ox::App::get_window().get_logical_size());
  if (camera && window_size.x > 0.0f && window_size.y > 0.0f) {
    const auto ray = ox::Camera::get_screen_ray(*camera, in.get_mouse_position(), window_size);
    // with the engine's reversed-z the ray's direction points back at the camera (see ENGINE_FEEDBACK.md), so solve
    // for the line and don't care about the sign
    const auto dir = ray.get_direction();
    if (glm::abs(dir.y) > 0.0001f) {
      const auto hit = ray.get_point_on_ray((BULLET_HEIGHT - ray.get_origin().y) / dir.y);
      input.has_aim = true;
      input.aim = {hit.x, hit.z};
    }
  }

  return input;
}

auto Game::update(this Game& self, const ox::Timestep& timestep) -> void {
  ZoneScoped;

  // the scene is stepped with this same delta (Scene::runtime_step), physics catches up in fixed 1/60
  // substeps. clamped so a long hitch doesn't teleport everything
  auto dt = self.options.fixed_dt > 0.0f ? self.options.fixed_dt
                                         : glm::clamp(static_cast<f32>(timestep.get_seconds()), 0.001f, 1.0f / 15.0f);

  auto input = !self.options.net_autoplay.empty() ? self.net_autoplay.update(*self.world, self.net, dt)
               : self.options.autoplay               ? self.autoplay.update(*self.world, dt)
                                                     : self.read_input();

  self.handle_net_requests();
  self.net.poll(*self.world, timestep);
  if (self.net.rebuild_seed) {
    // the host's city isn't ours: build theirs, then step into it
    const auto seed = *self.net.rebuild_seed;
    self.net.rebuild_seed.reset();
    if (self.rebuild_world(seed)) {
      self.world->become_client(self.net.welcome_slot);
    }
  }
  if (self.net.lost_host) {
    self.net.lost_host = false;
    const auto why = self.net.status_text;
    self.net.leave();
    if (self.world->role == NetRole::Client) {
      self.rebuild_world(self.options.seed);
    }
    self.world->hud.net_status = why;
    self.world->pager(why);
  }
  if (self.net.status == NetSession::Status::Connecting || self.net.status == NetSession::Status::Joined) {
    self.world->hud.net_status = self.net.status_text;
  }

  self.world->update(input, dt);
  self.net.flush(*self.world);

  auto& pending = self.options.net_autoplay.empty() ? self.autoplay.pending_screenshot : self.net_autoplay.pending_screenshot;
  if (self.options.screenshot_dir.empty() || pending.empty()) {
    self.render_frame();
  } else {
    auto path = self.options.screenshot_dir / (pending + ".png");
    std::filesystem::create_directories(self.options.screenshot_dir);
    if (self.capture_screenshot(path)) {
      OX_LOG_INFO("OxCity: wrote screenshot {}", path.string());
    }
  }
  pending.clear();

  self.frame++;
  const auto out_of_frames = self.options.frame_limit > 0 && self.frame >= static_cast<u64>(self.options.frame_limit);
  if (self.world->quit_requested || out_of_frames) {
    if (!self.options.net_autoplay.empty()) {
      const auto passed = self.net_autoplay.report(*self.world, self.net);
      OX_LOG_INFO("OxCity: net autoplay ({}) {}", self.options.net_autoplay, passed ? "PASSED" : "FAILED");
    } else if (self.options.autoplay) {
      const auto passed = self.autoplay.report(*self.world);
      OX_LOG_INFO("OxCity: autoplay {}", passed ? "PASSED" : "FAILED");
    }
    ox::App::get()->should_stop();
  }
}

auto Game::render_frame(this Game& self) -> void {
  ZoneScoped;

  auto& render_context = ox::App::get_rendercontext();
  const auto& window = ox::App::get_window();

  auto target = render_context.new_frame();
  target = vuk::clear_image(std::move(target), vuk::Black<f32>);
  const auto extent = target->extent;
  const auto surface = glm::ivec2(extent.width, extent.height);

  // the viewport is in window (mouse) coordinates, the surface in pixels, RmlUi maps between the two
  target = self.world->scene->render(std::move(target), glm::ivec2(0), window.get_logical_size(), surface, true);

  render_context.end_frame(std::move(target));
}

auto Game::capture_screenshot(this Game& self, const std::filesystem::path& path) -> bool {
  ZoneScoped;

  auto& render_context = ox::App::get_rendercontext();
  const auto& window = ox::App::get_window();

  // same shape as the editor's thumbnail renderer: render into our own image, copy it into a host visible buffer
  // and wait for it, all inside a normal frame so the frame allocator is live
  auto swapchain = render_context.new_frame();
  const auto width = swapchain->extent.width;
  const auto height = swapchain->extent.height;

  auto image = vuk::declare_ia(
    "oxcity_screenshot",
    {
      .image_type = vuk::ImageType::e2D,
      .extent = vuk::Extent3D{width, height, 1u},
      .format = vuk::Format::eR8G8B8A8Srgb,
      .sample_count = vuk::Samples::e1,
      .base_level = 0,
      .level_count = 1,
      .base_layer = 0,
      .layer_count = 1,
    }
  );
  image = vuk::clear_image(std::move(image), vuk::Black<f32>);
  image = self.world->scene->render(
    std::move(image),
    glm::ivec2(0),
    window.get_logical_size(),
    glm::ivec2(width, height),
    true
  );

  const auto size = static_cast<usize>(width) * height * 4;
  auto readback = render_context.alloc_transient_buffer(vuk::MemoryUsage::eGPUtoCPU, size);
  readback = vuk::copy(std::move(image), std::move(readback));
  {
    auto lock = std::unique_lock(render_context.queue_mutex);
    readback.wait(*render_context.frame_allocator, render_context.get_compiler());
  }

  auto pixels = std::vector<u8>(size);
  std::memcpy(pixels.data(), readback->mapped_ptr, size);
  // the scene image is not guaranteed opaque (the editor thumbnails rely on that), force alpha for viewers
  for (usize i = 3; i < size; i += 4) {
    pixels[i] = 255;
  }

  swapchain = vuk::clear_image(std::move(swapchain), vuk::Black<f32>);
  render_context.end_frame(std::move(swapchain));

  const auto ok = stbi_write_png(
                    path.string().c_str(),
                    static_cast<int>(width),
                    static_cast<int>(height),
                    4,
                    pixels.data(),
                    static_cast<int>(width) * 4
                  ) != 0;
  return ok;
}
} // namespace oxcity
