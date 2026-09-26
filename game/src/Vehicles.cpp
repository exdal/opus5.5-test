// Cars: the player's driving, traffic following the road graph, police chasing, run overs and crash damage.
// Physics is the engine's Jolt wheeled vehicle (VehicleComponent + VehicleWheelComponent children), the game
// only writes driver input into the component and reads the body back.

// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/PhysicsSystem.h>
// clang-format on

#include <glm/common.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtx/norm.hpp>

#include "Scene/Components.hpp"
#include "Scene/Scene.hpp"
#include "World.hpp"

namespace oxcity {
static auto body_of(flecs::entity e) -> JPH::Body* {
  if (!e || !e.is_alive()) {
    return nullptr;
  }
  const auto* rb = e.try_get<ox::RigidBodyComponent>();
  return rb ? static_cast<JPH::Body*>(rb->runtime_body) : nullptr;
}

auto World::car_position(this const World& self, CarID id) -> glm::vec2 {
  const auto& c = self.car(id);
  if (auto* body = body_of(c.entity)) {
    const auto p = body->GetPosition();
    return {p.GetX(), p.GetZ()};
  }
  if (c.entity && c.entity.is_alive()) {
    return to2(c.entity.get<ox::TransformComponent>().position);
  }
  return {};
}

auto World::car_heading(this const World& self, CarID id) -> f32 {
  const auto& c = self.car(id);
  if (auto* body = body_of(c.entity)) {
    const auto f = body->GetRotation() * JPH::Vec3::sAxisZ();
    return heading_of({f.GetX(), f.GetZ()});
  }
  if (c.entity && c.entity.is_alive()) {
    const auto f = c.entity.get<ox::TransformComponent>().rotation * glm::vec3(0.0f, 0.0f, 1.0f);
    return heading_of({f.x, f.z});
  }
  return 0.0f;
}

auto World::car_velocity(this const World& self, CarID id) -> glm::vec3 {
  if (auto* body = body_of(self.car(id).entity)) {
    const auto v = body->GetLinearVelocity();
    return {v.GetX(), v.GetY(), v.GetZ()};
  }
  // a client's cars have no body, the host told us how fast they go
  return self.car(id).net_velocity;
}

auto World::drive_car(this World& self, CarID id, f32 throttle, f32 steer, f32 brake, f32 handbrake) -> void {
  auto& c = self.car(id);
  if (!c.entity || !c.entity.is_alive() || !c.entity.has<ox::VehicleComponent>()) {
    return;
  }
  // get_mut and no modified(): VehicleComponent's OnSet observer rebuilds the whole constraint, which is what
  // `set<>` would trigger every frame. the engine's vehicle_input system picks these up on the next tick
  auto& v = c.entity.get_mut<ox::VehicleComponent>();
  v.input_forward = glm::clamp(throttle, -1.0f, 1.0f);
  v.input_right = glm::clamp(steer, -1.0f, 1.0f);
  v.input_brake = glm::clamp(brake, 0.0f, 1.0f);
  v.input_hand_brake = glm::clamp(handbrake, 0.0f, 1.0f);
}

auto World::teleport_car(this World& self, CarID id, glm::vec2 position, f32 yaw) -> void {
  auto* body = body_of(self.car(id).entity);
  if (!body) {
    return;
  }
  auto& bi = self.scene->get_physics_system()->GetBodyInterface();
  const auto q = yaw_quat(yaw);
  bi.SetPositionAndRotation(
    body->GetID(),
    JPH::RVec3(position.x, 0.6f, position.y),
    JPH::Quat(q.x, q.y, q.z, q.w),
    JPH::EActivation::Activate
  );
  bi.SetLinearAndAngularVelocity(body->GetID(), JPH::Vec3::sZero(), JPH::Vec3::sZero());
}

auto World::damage_car(this World& self, CarID id, f32 amount, PlayerID attacker) -> void {
  auto& c = self.car(id);
  if (!c.alive) {
    return;
  }
  c.health -= amount;
  if (attacker != PlayerID::Invalid) {
    c.last_hit_by = attacker;
  }
  if (c.health <= 0.0f && !c.exploded) {
    // shot or smashed to pieces: it goes up. Marked first, the blast can reach this car again through a chain
    c.exploded = true;
    if (c.player_driver != PlayerID::Invalid) {
      self.damage_player(c.player_driver, 80.0f, c.last_hit_by);
    }
    self.explode(self.car_position(id), c.last_hit_by);
  }
  if (c.health <= 0.0f && c.role != CarRole::Abandoned) {
    // wrecked: engine dies, the driver bails
    c.health = 0.0f;
    if (c.driver != PedID::Invalid) {
      auto& driver = self.ped(c.driver);
      driver.state = driver.kind == PedKind::Cop ? PedState::Chase : PedState::Flee;
      driver.timer = 8.0f;
      driver.car = CarID::Invalid;
      driver.position = self.car_position(id) - right_of(forward_of(self.car_heading(id))) * 2.2f;
      self.set_entity_pose(driver.entity, to3(driver.position, 0.15f), yaw_quat(driver.heading));
      c.driver = PedID::Invalid;
    }
    if (c.player_driver == PlayerID::Invalid) {
      c.role = CarRole::Abandoned;
    }
  }
}

// throttle/brake to hold `target_speed` (m/s, negative reverses), steering towards `target`
static auto steer_to(World& self, CarID id, glm::vec2 target, f32 target_speed, f32 dt) -> void {
  auto& c = self.car(id);
  const auto pos = self.car_position(id);
  const auto heading = self.car_heading(id);
  const auto velocity = self.car_velocity(id);
  const auto forward_speed = glm::dot(to2(velocity), forward_of(heading));

  const auto to_target = target - pos;
  const auto angle = wrap_angle(heading_of(to_target) - heading);
  // heading grows turning towards +x, which is the driver's left (see right_of), so steer against it
  auto steer = glm::clamp(-angle * 2.2f, -1.0f, 1.0f);

  // stuck against something: back out for a moment with the wheel turned the other way
  if (c.reverse_timer > 0.0f) {
    c.reverse_timer -= dt;
    self.drive_car(id, -0.7f, -steer, 0.0f, 0.0f);
    return;
  }
  if (target_speed > 1.0f && glm::abs(forward_speed) < 0.6f) {
    c.stuck_timer += dt;
    if (c.stuck_timer > 2.5f) {
      c.stuck_timer = 0.0f;
      c.reverse_timer = 1.4f;
    }
  } else {
    c.stuck_timer = 0.0f;
  }

  // slow down for sharp turns
  const auto turn_speed = glm::mix(target_speed, glm::min(target_speed, 5.0f), glm::min(1.0f, glm::abs(angle) * 1.2f));
  auto throttle = 0.0f;
  auto brake = 0.0f;
  if (forward_speed < turn_speed - 0.5f) {
    throttle = glm::clamp((turn_speed - forward_speed) * 0.3f, 0.25f, 1.0f);
  } else if (forward_speed > turn_speed + 1.5f) {
    brake = glm::clamp((forward_speed - turn_speed) * 0.2f, 0.1f, 1.0f);
  }
  self.drive_car(id, throttle, steer, brake, 0.0f);
}

auto World::car_ai_follow_roads(this World& self, CarID id, f32 dt) -> void {
  auto& c = self.car(id);
  if (c.from_node == NodeID::Invalid || c.to_node == NodeID::Invalid) {
    c.from_node = self.nearest_node(self.car_position(id));
    const auto& n = self.nodes[static_cast<usize>(c.from_node)];
    c.to_node = n.neighbours.empty() ? c.from_node : n.neighbours.front();
  }

  const auto from = self.nodes[static_cast<usize>(c.from_node)].position;
  const auto to = self.nodes[static_cast<usize>(c.to_node)].position;
  const auto dir = glm::length2(to - from) > 0.01f ? glm::normalize(to - from) : glm::vec2(0.0f, 1.0f);
  const auto lane = right_of(dir) * LANE_OFFSET;
  const auto pos = self.car_position(id);

  // aim a little ahead along the lane rather than at the node itself, it gives smoother lines
  const auto along = glm::clamp(glm::dot(pos - from, dir) + 8.0f, 0.0f, glm::distance(from, to));
  const auto aim = from + dir * along + lane;

  if (glm::distance(pos, to + lane) < 7.5f) {
    // at the intersection: pick where to go next, no u-turns unless it's a dead end
    const auto& n = self.nodes[static_cast<usize>(c.to_node)];
    auto options = std::vector<NodeID>{};
    for (auto next : n.neighbours) {
      if (next != c.from_node) {
        options.push_back(next);
      }
    }
    if (options.empty()) {
      options = n.neighbours;
    }
    c.from_node = c.to_node;
    c.to_node = options[static_cast<usize>(self.random_int(0, static_cast<i32>(options.size()) - 1))];
  }

  // brake for whatever is in front: other cars, peds, the player
  auto speed = c.cruise_speed;
  const auto fwd = forward_of(self.car_heading(id));
  auto blocked = [&](glm::vec2 other, f32 range) {
    const auto to_other = other - pos;
    const auto d = glm::length(to_other);
    return d < range && d > 0.1f && glm::dot(to_other / d, fwd) > 0.85f;
  };
  for (usize i = 0; i < self.cars.size(); i++) {
    if (static_cast<CarID>(i) != id && self.cars[i].alive && blocked(self.car_position(static_cast<CarID>(i)), 9.0f)) {
      speed = 0.0f;
    }
  }
  for (const auto& p : self.peds) {
    if (p.alive && p.state != PedState::Driving && blocked(p.position, 7.0f)) {
      speed = 0.0f;
    }
  }
  for (auto pid : ALL_PLAYERS) {
    if (self.on_foot(pid) && blocked(self.pl(pid).position, 7.0f)) {
      speed = 0.0f;
    }
  }
  if (glm::distance(pos, to) < 16.0f) {
    speed = glm::min(speed, 6.0f);
  }

  steer_to(self, id, aim, speed, dt);
}

auto World::car_ai_chase(this World& self, CarID id, PlayerID suspect, f32 dt) -> void {
  const auto target = self.player_position(suspect);
  auto& c = self.car(id);
  const auto pos = self.car_position(id);
  auto aim = target;

  // far away and no line of sight: go via the road graph, driving through buildings is not an option
  auto line_clear = true;
  for (i32 i = 1; i < 8; i++) {
    if (self.is_solid(glm::mix(pos, target, static_cast<f32>(i) / 8.0f), 1.0f)) {
      line_clear = false;
      break;
    }
  }
  if (!line_clear) {
    const auto here = self.nearest_node(pos);
    auto best = here;
    auto best_d = glm::distance(self.nodes[static_cast<usize>(here)].position, target);
    for (auto n : self.nodes[static_cast<usize>(here)].neighbours) {
      const auto d = glm::distance(self.nodes[static_cast<usize>(n)].position, target);
      if (d < best_d) {
        best_d = d;
        best = n;
      }
    }
    aim = self.nodes[static_cast<usize>(best)].position;
    if (glm::distance(pos, aim) < 6.0f && best == here) {
      aim = target;
    }
  }

  const auto dist = glm::distance(pos, target);
  const auto on_foot = self.pl(suspect).car == CarID::Invalid;
  // on foot the cops pull up next to you, in a car they ram you
  const auto speed = on_foot && dist < 14.0f ? 0.0f : 16.0f;
  steer_to(self, id, aim, speed, dt);
  if (speed == 0.0f) {
    self.drive_car(id, 0.0f, 0.0f, 1.0f, 0.0f);
  }
  c.cruise_speed = speed;
}

auto World::update_vehicles(this World& self, f32 dt) -> void {
  ZoneScoped;

  for (usize i = 0; i < self.cars.size(); i++) {
    const auto id = static_cast<CarID>(i);
    auto& c = self.cars[i];
    if (!c.alive || !c.entity || !c.entity.is_alive()) {
      continue;
    }

    const auto pos = self.car_position(id);
    const auto heading = self.car_heading(id);
    const auto velocity = self.car_velocity(id);
    const auto speed = glm::length(velocity);
    const auto forward_speed = glm::dot(to2(velocity), forward_of(heading));

    const auto driver = c.player_driver;
    if (driver != PlayerID::Invalid) {
      // offline the pause menu takes the wheel away; online the car keeps doing what its driver says
      const auto driving = self.in_play(driver) && (self.role != NetRole::Offline || self.state == GameState::Playing);
      if (driving) {
        const auto& input = self.pl(driver).input;
        auto throttle = input.throttle;
        auto brake = 0.0f;
        // the brake pedal and reverse share a key, like every arcade racer
        if (throttle < 0.0f && forward_speed > 1.0f) {
          brake = -throttle;
          throttle = 0.0f;
        } else if (throttle > 0.0f && forward_speed < -1.0f) {
          brake = throttle;
          throttle = 0.0f;
        }
        // tighter steering at low speed, gentler at high speed
        const auto steer = input.steer * glm::mix(1.0f, 0.45f, glm::clamp(speed / 30.0f, 0.0f, 1.0f));
        self.drive_car(id, throttle, steer, brake, input.handbrake ? 1.0f : 0.0f);
      } else {
        self.drive_car(id, 0.0f, 0.0f, 1.0f, 1.0f);
      }
    } else if (c.health > 0.0f && c.driver != PedID::Invalid) {
      const auto suspect = c.role == CarRole::Police ? self.nearest_player(pos, 250.0f, true) : PlayerID::Invalid;
      if (suspect != PlayerID::Invalid) {
        self.car_ai_chase(id, suspect, dt);
      } else {
        self.car_ai_follow_roads(id, dt);
      }
    } else {
      self.drive_car(id, 0.0f, 0.0f, 0.0f, 1.0f);
    }

    // sirens blink while chasing
    if (c.siren_red && c.siren_blue) {
      const auto chasing = c.role == CarRole::Police && (self.total_stars() > 0 || driver != PlayerID::Invalid);
      const auto phase = glm::fract(self.time * 3.0f) < 0.5f;
      const auto red = !chasing || phase ? glm::vec3(1.0f) : glm::vec3(0.01f);
      const auto blue = !chasing || !phase ? glm::vec3(1.0f) : glm::vec3(0.01f);
      c.siren_red.get_mut<ox::TransformComponent>().scale = red;
      c.siren_red.modified<ox::TransformComponent>();
      c.siren_blue.get_mut<ox::TransformComponent>().scale = blue;
      c.siren_blue.modified<ox::TransformComponent>();
    }

    // crashes: a big change in velocity between frames
    const auto dv = glm::length(velocity - c.last_velocity);
    if (dv > 7.0f && glm::length(c.last_velocity) > 5.0f) {
      self.damage_car(id, dv * 2.5f, driver);
      if (dv > 12.0f) {
        self.impact_sparks(to3(pos, 0.6f), -to2(c.last_velocity));
      }
      self.play_at(self.assets.sfx_crash, pos, glm::clamp(dv / 20.0f, 0.3f, 1.0f), 1.0f, 40.0f);
      if (driver != PlayerID::Invalid) {
        self.damage_player(driver, glm::max(0.0f, dv - 12.0f) * 2.0f);
      }
    }
    c.last_velocity = velocity;

    if (speed < 3.0f) {
      continue;
    }

    // run overs, in chassis space so long cars hit along their whole length
    const auto fwd = forward_of(heading);
    const auto side = right_of(fwd);
    auto inside = [&](glm::vec2 p, f32 margin) {
      const auto local = p - pos;
      return glm::abs(glm::dot(local, fwd)) < 2.4f + margin && glm::abs(glm::dot(local, side)) < 1.0f + margin;
    };
    for (usize k = 0; k < self.peds.size(); k++) {
      auto& ped = self.peds[k];
      if (!ped.alive || ped.state == PedState::Driving || !inside(ped.position, 0.3f)) {
        continue;
      }
      self.kill_ped(static_cast<PedID>(k), to2(velocity) * 0.6f, driver);
      self.play_at(self.assets.sfx_punch, ped.position, 1.0f, 0.6f);
      if (driver != PlayerID::Invalid) {
        self.commit_crime(driver, ped.kind == PedKind::Cop ? 2.0f : 0.7f, pos, "");
      }
    }
    // players on foot in the way, including the other players' bumpers
    for (auto pid : ALL_PLAYERS) {
      if (pid == driver || !self.on_foot(pid) || !inside(self.pl(pid).position, 0.2f)) {
        continue;
      }
      self.damage_player(pid, speed * 3.0f, driver);
      self.play_at(self.assets.sfx_punch, self.pl(pid).position, 1.0f, 0.5f);
      if (driver != PlayerID::Invalid) {
        self.commit_crime(driver, 0.7f, pos, "");
      }
    }
  }
}
} // namespace oxcity
