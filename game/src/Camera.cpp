// classic top-down camera: straight down from high above, zooming out the faster you drive. Behind the menus it slowly
// drifts over the city as an attract mode.

#include <glm/common.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include "Scene/Components.hpp"
#include "World.hpp"

namespace oxcity {
auto World::update_camera(this World& self, f32 dt) -> void {
  auto target = glm::vec3(0.0f);
  auto height = 24.0f;

  if (self.state == GameState::MainMenu) {
    const auto t = self.time * 0.05f;
    target = glm::vec3(std::cos(t) * 40.0f, 0.0f, std::sin(t * 1.3f) * 40.0f);
    height = 70.0f;
  } else {
    target = to3(self.player_position());
    if (self.player.car != CarID::Invalid) {
      const auto velocity = self.car_velocity(self.player.car);
      const auto speed = glm::length(velocity);
      height = 27.0f + glm::min(speed, 30.0f) * 0.75f;
      // look ahead of the car so you see what you are about to hit
      target += glm::vec3(velocity.x, 0.0f, velocity.z) * 0.35f;
    }
  }

  const auto smooth = 1.0f - std::exp(-dt * 4.0f);
  self.camera_height += (height - self.camera_height) * (1.0f - std::exp(-dt * 1.5f));
  const auto desired = target + glm::vec3(0.0f, self.camera_height, self.camera_height * 0.22f);
  if (self.camera_position == glm::vec3(0.0f)) {
    self.camera_position = desired;
  }
  self.camera_position += (desired - self.camera_position) * smooth;

  // screen shake: trauma squared, so small hits barely move it and kills kick hard. Sines at unrelated frequencies
  // stand in for noise and don't touch the gameplay rng. A kill also punches the camera in a little
  const auto trauma = self.juice.shake * self.juice.shake;
  const auto t = self.time;
  const auto shake = glm::vec3(std::sin(t * 47.0f) + std::sin(t * 83.0f) * 0.5f, 0.0f, std::sin(t * 59.0f + 1.3f) + std::sin(t * 97.0f) * 0.5f) *
                     trauma * 0.55f;
  const auto punch = glm::vec3(0.0f, -self.juice.combo_pop * 2.0f, 0.0f);
  const auto eye = self.camera_position + shake + punch;

  // almost straight down, tilted just enough that screen up is world -z (see update_player)
  const auto look = glm::normalize(target + shake * 0.5f - eye);
  const auto roll = glm::angleAxis(std::sin(t * 71.0f) * trauma * 0.04f, look);
  const auto rotation = roll * glm::quatLookAt(look, glm::vec3(0.0f, 1.0f, 0.0f));
  self.set_entity_pose(self.camera, eye, rotation);
}
} // namespace oxcity
