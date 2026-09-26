// Sound through the engine's AudioEngine (miniaudio). Every effect is one audio asset, loaded once and kept
// referenced for the whole game, played by restarting its ma_sound.

#include <filesystem>
#include <fstream>
#include <glm/common.hpp>

#include "Asset/AssetManager.hpp"
#include "Asset/AudioSource.hpp"
#include "Audio/AudioEngine.hpp"
#include "Core/App.hpp"
#include "Core/VFS.hpp"
#include "Utils/Log.hpp"
#include "World.hpp"

namespace oxcity {
static auto sound_of(const ox::UUID& uuid) -> ma_sound* {
  if (!uuid) {
    return nullptr;
  }
  auto audio = ox::App::mod<ox::AssetManager>().get_audio(uuid);
  return audio ? audio->get_source() : nullptr;
}

auto World::init_audio(this World& self) -> void {
  auto& asset_man = ox::App::mod<ox::AssetManager>();
  auto& audio = ox::App::mod<ox::AudioEngine>();
  const auto& a = self.assets;
  for (const auto* uuid : {&a.sfx_engine, &a.sfx_siren, &a.sfx_horn, &a.sfx_gunshot, &a.sfx_punch, &a.sfx_cash,
                           &a.sfx_footstep, &a.sfx_door, &a.sfx_crash, &a.sfx_alarm, &a.sfx_pager, &a.sfx_death,
                           &a.sfx_radio, &a.sfx_knife_swing, &a.sfx_stab, &a.sfx_splat, &a.sfx_explosion, &a.music_menu}) {
    if (!*uuid || !asset_man.load_asset(*uuid)) {
      OX_LOG_WARN("OxCity: couldn't load a sound");
      continue;
    }
    if (auto* sound = sound_of(*uuid)) {
      // these are 2D game sounds: miniaudio spatializes by default, and nothing sets a sound's position
      // unless it sits on an AudioSourceComponent, so leaving it on puts every sound at the world origin
      audio.set_source_spatialization(sound, false);
    }
  }
  for (const auto* uuid : {&a.sfx_engine, &a.sfx_siren, &a.sfx_radio, &a.sfx_alarm, &a.music_menu}) {
    if (auto* sound = sound_of(*uuid)) {
      audio.set_source_looping(sound, true);
    }
  }
}

auto World::play(this World& self, const ox::UUID& sound_uuid, f32 volume, f32 pitch) -> void {
  if (!self.hud.sfx_on) {
    return;
  }
  auto* sound = sound_of(sound_uuid);
  if (!sound) {
    return;
  }
  auto& audio = ox::App::mod<ox::AudioEngine>();
  audio.set_source_volume(sound, volume);
  audio.set_source_pitch(sound, pitch);
  audio.seek_source(sound, 0.0f);
  audio.play_source(sound);
}

auto World::play_at(this World& self, const ox::UUID& sound, glm::vec2 where, f32 volume, f32 pitch, f32 range) -> void {
  self.record({
    .kind = net::EventKind::Sound,
    .asset = self.sound_index(sound),
    .f = {where.x, where.y, volume, pitch, range, 0.0f},
  });
  // one listener per machine: the local player (or the camera). Linear falloff, with a floor so a gunshot across
  // town is still a faint pop rather than nothing
  const auto d = glm::distance(where, self.listener_position());
  if (d > range * 1.5f) {
    return;
  }
  self.play(sound, volume * glm::clamp(1.0f - d / range, 0.1f, 1.0f), pitch);
}

static auto set_loop(ox::AudioEngine& audio, const ox::UUID& uuid, bool& playing, bool want, f32 volume, f32 pitch) {
  auto* sound = sound_of(uuid);
  if (!sound) {
    return;
  }
  if (want != playing) {
    playing = want;
    if (want) {
      audio.play_source(sound);
    } else {
      audio.stop_source(sound);
    }
  }
  if (want) {
    audio.set_source_volume(sound, volume);
    audio.set_source_pitch(sound, pitch);
  }
}

auto World::update_audio(this World& self, f32 dt) -> void {
  auto& audio = ox::App::mod<ox::AudioEngine>();
  const auto* me = self.local_player();
  const auto playing = self.state == GameState::Playing && me && me->active;
  const auto in_car = playing && me->car != CarID::Invalid && me->life == Life::Alive;

  // engine note follows the speed of the car you're in
  auto target_pitch = 0.6f;
  if (in_car) {
    const auto speed = glm::length(self.car_velocity(me->car));
    target_pitch = 0.6f + glm::fract(speed / 14.0f) * 0.9f + glm::floor(speed / 14.0f) * 0.12f;
  }
  self.engine_pitch += (target_pitch - self.engine_pitch) * glm::min(1.0f, dt * 6.0f);
  const auto sfx = self.hud.sfx_on;
  const auto music = self.hud.music_on;
  set_loop(audio, self.assets.sfx_engine, self.engine_playing, sfx && in_car, 0.35f, self.engine_pitch);
  set_loop(audio, self.assets.sfx_radio, self.radio_playing, music && in_car, 0.22f, 1.0f);
  const auto on_menu = self.state == GameState::MainMenu || self.state == GameState::Paused;
  set_loop(audio, self.assets.music_menu, self.menu_music_playing, music && on_menu, 0.3f, 1.0f);

  // siren gets louder as the closest chasing cop car gets closer
  auto nearest = 1000.0f;
  for (usize i = 0; i < self.cars.size(); i++) {
    const auto& c = self.cars[i];
    if (c.alive && c.role == CarRole::Police && c.player_driver == PlayerID::Invalid && c.driver != PedID::Invalid) {
      nearest = glm::min(nearest, glm::distance(self.car_position(static_cast<CarID>(i)), self.listener_position()));
    }
  }
  // any chase you can hear, yours or someone else's
  const auto siren = playing && self.total_stars() > 0 && nearest < 90.0f;
  set_loop(audio, self.assets.sfx_siren, self.siren_playing, sfx && siren, glm::clamp(1.0f - nearest / 90.0f, 0.1f, 0.8f), 1.0f);
  set_loop(audio, self.assets.sfx_alarm, self.alarm_playing, sfx && self.heist.alarm > 0.0f,
           glm::clamp(1.0f - glm::distance(self.listener_position(), self.heist.position) / 80.0f, 0.05f, 0.7f), 1.0f);
}
static auto settings_path() -> std::filesystem::path {
  return ox::App::get_vfs().resolve_physical_dir(ox::VFS::APP_DIR, "oxcity_settings.txt");
}

auto World::load_settings(this World& self) -> void {
  auto file = std::ifstream(settings_path());
  auto line = std::string{};
  while (std::getline(file, line)) {
    if (line.starts_with("sfx=")) {
      self.hud.sfx_on = line != "sfx=0";
    } else if (line.starts_with("music=")) {
      self.hud.music_on = line != "music=0";
    } else if (line.starts_with("name=") && line.size() > 5) {
      self.hud.player_name = line.substr(5, 16);
    } else if (line.starts_with("join=") && line.size() > 5) {
      self.hud.join_address = line.substr(5, 64);
    }
  }
}

auto World::save_settings(this const World& self) -> void {
  auto file = std::ofstream(settings_path());
  file << "sfx=" << (self.hud.sfx_on ? 1 : 0) << "\n" << "music=" << (self.hud.music_on ? 1 : 0) << "\n";
  file << "name=" << self.hud.player_name << "\n" << "join=" << self.hud.join_address << "\n";
}
} // namespace oxcity
