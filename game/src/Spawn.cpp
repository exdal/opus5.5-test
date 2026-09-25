#include <glm/gtc/constants.hpp>
#include <glm/gtx/norm.hpp>

#include "Scene/Components.hpp"
#include "Scene/Scene.hpp"
#include "Utils/Log.hpp"
#include "World.hpp"

namespace oxcity {
struct VehicleSpec {
  std::string_view name;
  std::string_view display;
  f32 width;
  f32 length;
  f32 body_height;
  f32 mass;
  f32 torque;
  ox::VehicleComponent::DriveMode drive;
};

// dimensions must match VEHICLE_STYLES in tools/assetgen/models.py
static constexpr VehicleSpec VEHICLE_SPECS[] = {
  {"sedan", "COSSIE", 1.9f, 4.4f, 0.62f, 1300.0f, 520.0f, ox::VehicleComponent::DriveMode::RearWheelDrive},
  {"sports", "FURORE GT", 1.95f, 4.3f, 0.5f, 1150.0f, 760.0f, ox::VehicleComponent::DriveMode::RearWheelDrive},
  {"taxi", "TAXI", 1.9f, 4.5f, 0.62f, 1350.0f, 500.0f, ox::VehicleComponent::DriveMode::FrontWheelDrive},
  {"police", "COP CAR", 1.95f, 4.6f, 0.64f, 1400.0f, 700.0f, ox::VehicleComponent::DriveMode::AllWheelDrive},
  {"van", "BOX VAN", 2.05f, 4.9f, 0.9f, 1900.0f, 620.0f, ox::VehicleComponent::DriveMode::RearWheelDrive},
};

static auto find_spec(std::string_view name) -> const VehicleSpec& {
  for (const auto& spec : VEHICLE_SPECS) {
    if (spec.name == name) {
      return spec;
    }
  }
  return VEHICLE_SPECS[0];
}

auto World::spawn_model(this World& self, const ox::UUID& model, glm::vec3 position, f32 yaw, glm::vec3 scale)
  -> flecs::entity {
  if (!model) {
    return {};
  }
  auto e = self.scene->create_model_entity(model);
  if (!e) {
    return {};
  }
  e.set<ox::TransformComponent>({.position = position, .rotation = yaw_quat(yaw), .scale = scale});
  return e;
}

auto World::set_entity_pose(this World& self, flecs::entity e, glm::vec3 position, glm::quat rotation) -> void {
  if (!e || !e.is_alive()) {
    return;
  }
  auto& tc = e.get_mut<ox::TransformComponent>();
  tc.position = position;
  tc.rotation = rotation;
  e.modified<ox::TransformComponent>();
}

auto World::hide_entity(this World& self, flecs::entity e) -> void {
  // the renderer has no per entity visibility toggle (the `Hidden` tag only affects the editor), so parking
  // things far under the map is the cheapest way to make them disappear
  if (!e || !e.is_alive()) {
    return;
  }
  auto& tc = e.get_mut<ox::TransformComponent>();
  tc.position.y = -500.0f;
  e.modified<ox::TransformComponent>();
}

// depth first search by entity name. glTF nodes don't come out at a fixed depth: the model compiler adds a
// group for the scene itself, so a node that is a root child in the .glb is a grandchild of the spawned entity
static auto find_descendant(flecs::entity root, std::string_view name) -> flecs::entity {
  auto found = flecs::entity{};
  root.children([&](flecs::entity child) {
    if (found) {
      return;
    }
    if (std::string_view(child.name()) == name) {
      found = child;
      return;
    }
    found = find_descendant(child, name);
  });
  return found;
}

auto World::find_limbs(this World& self, flecs::entity root) -> Limbs {
  auto limbs = Limbs{};
  limbs.torso = find_descendant(root, "torso");
  limbs.leg_l = find_descendant(root, "leg_l");
  limbs.leg_r = find_descendant(root, "leg_r");
  limbs.arm_l = find_descendant(root, "arm_l");
  limbs.arm_r = find_descendant(root, "arm_r");
  if (!limbs.leg_l || !limbs.arm_l) {
    OX_LOG_WARN("OxCity: character model '{}' has no limb nodes, it won't animate", root.name().c_str());
  }
  return limbs;
}

auto World::spawn_ped(this World& self, PedKind kind, glm::vec2 position) -> PedID {
  auto model = self.assets.peds[static_cast<usize>(self.random_int(0, static_cast<i32>(self.assets.peds.size()) - 1))];
  if (kind == PedKind::Cop) {
    model = self.assets.cop;
  } else if (kind == PedKind::Guard) {
    model = self.assets.guard;
  }

  // reuse the slot of a ped that has been dead long enough
  auto slot = PedID::Invalid;
  for (usize i = 0; i < self.peds.size(); i++) {
    const auto& p = self.peds[i];
    if (!p.alive && p.dead_time > 15.0f && !p.entity.is_valid()) {
      slot = static_cast<PedID>(i);
      break;
    }
  }

  const auto heading = self.random_float(-glm::pi<f32>(), glm::pi<f32>());
  auto e = self.spawn_model(model, to3(position, 0.15f), heading);
  if (!e) {
    return PedID::Invalid;
  }

  auto ped = Ped{};
  ped.entity = e;
  ped.limbs = self.find_limbs(e);
  ped.kind = kind;
  ped.state = kind == PedKind::Cop ? PedState::Chase : PedState::Wander;
  ped.position = position;
  ped.heading = heading;
  ped.target = position;
  ped.health = kind == PedKind::Civilian ? 40.0f : 80.0f;
  ped.cash = kind == PedKind::Civilian ? self.random_int(5, 90) : self.random_int(20, 60);
  ped.walk_phase = self.random_float(0.0f, 6.0f);

  if (slot != PedID::Invalid) {
    self.ped(slot) = ped;
    return slot;
  }
  self.peds.push_back(ped);
  return static_cast<PedID>(self.peds.size() - 1);
}

auto World::spawn_car(this World& self, std::string_view model, glm::vec2 position, f32 yaw, CarRole role) -> CarID {
  ZoneScoped;

  const auto& spec = find_spec(model);
  auto uuid = self.assets.sedan;
  if (model == "sports") {
    uuid = self.assets.sports;
  } else if (model == "taxi") {
    uuid = self.assets.taxi;
  } else if (model == "police") {
    uuid = self.assets.police;
  } else if (model == "van") {
    uuid = self.assets.van;
  }

  auto root = self.spawn_model(uuid, to3(position, 0.25f), yaw);
  if (!root) {
    return CarID::Invalid;
  }

  auto car = Car{};
  car.entity = root;
  car.role = role;
  car.model = std::string(spec.name);
  car.display_name = std::string(spec.display);

  // wheels are children, front axle first and left before right, that's the order the engine pairs them into
  // differentials. chassis space: +z forward, and seen from above the right hand side is -x
  const auto half_w = spec.width * 0.5f - 0.12f;
  const auto axle = spec.length * 0.5f - 0.85f;
  const glm::vec3 wheel_positions[] = {
    {half_w, 0.36f, axle},
    {-half_w, 0.36f, axle},
    {half_w, 0.36f, -axle},
    {-half_w, 0.36f, -axle},
  };
  for (usize i = 0; i < 4; i++) {
    auto wheel = self.scene->create_model_entity(self.assets.wheel);
    if (!wheel) {
      continue;
    }
    // create_model_entity names the root after the glTF node ("wheel") and only dedupes against other root
    // entities. once the first wheel is re-parented the name is free again at the root, so every wheel comes
    // out as "wheel" and flecs aborts on the second child_of. name them per axle before moving them
    static constexpr const char* WHEEL_NAMES[] = {"wheel_fl", "wheel_fr", "wheel_rl", "wheel_rr"};
    wheel.set_name(nullptr);
    wheel.child_of(root);
    wheel.set_name(WHEEL_NAMES[i]);
    wheel.set<ox::TransformComponent>({.position = wheel_positions[i]});
    const auto front = i < 2;
    wheel.set<ox::VehicleWheelComponent>({
      .attachment = wheel_positions[i] + glm::vec3(0.0f, 0.42f, 0.0f),
      .radius = 0.36f,
      .width = 0.26f,
      .suspension_min_length = 0.2f,
      .suspension_max_length = 0.5f,
      .suspension_frequency = 1.8f,
      .suspension_damping = 0.6f,
      .max_steer_angle = front ? 36.0f : 0.0f,
      .max_brake_torque = 2200.0f,
      .max_hand_brake_torque = front ? 0.0f : 5000.0f,
      .driven = true,
    });
    car.wheels[i] = wheel;
  }

  const auto top = 0.3f + spec.body_height + (spec.name == "van" ? 0.9f : 0.5f);
  root.set<ox::BoxColliderComponent>({
    .size = {spec.width * 0.5f, (top - 0.3f) * 0.5f, spec.length * 0.5f + 0.05f},
    .offset = {0.0f, 0.3f + (top - 0.3f) * 0.5f, 0.0f},
    .density = 200.0f,
    .friction = 0.4f,
  });
  root.set<ox::RigidBodyComponent>({
    .type = ox::RigidBodyComponent::BodyType::Dynamic,
    .mass = spec.mass,
    .linear_drag = 0.05f,
    .angular_drag = 0.3f,
    .friction = 0.4f,
    // keep the centre of mass low, arcade cars shouldn't roll over at every corner
    .center_of_mass_offset = {0.0f, -0.35f, 0.0f},
    .allow_sleep = true,
  });
  root.set<ox::VehicleComponent>({
    .drive_mode = spec.drive,
    .collision_mode = ox::VehicleComponent::CollisionMode::CylinderCast,
    .max_pitch_roll_angle = 50.0f,
    .max_engine_torque = spec.torque,
    .min_engine_rpm = 900.0f,
    .max_engine_rpm = 6500.0f,
  });

  if (spec.name == "police") {
    car.siren_red = find_descendant(root, "siren_red");
    car.siren_blue = find_descendant(root, "siren_blue");
  }

  for (usize i = 0; i < self.cars.size(); i++) {
    if (!self.cars[i].alive) {
      self.cars[i] = std::move(car);
      return static_cast<CarID>(i);
    }
  }
  self.cars.push_back(std::move(car));
  return static_cast<CarID>(self.cars.size() - 1);
}

auto World::despawn_car(this World& self, CarID id) -> void {
  auto& c = self.car(id);
  if (!c.alive) {
    return;
  }
  if (c.driver != PedID::Invalid) {
    auto& driver = self.ped(c.driver);
    if (driver.entity.is_alive()) {
      driver.entity.destruct();
    }
    driver.entity = {};
    driver.alive = false;
    driver.dead_time = 100.0f;
    c.driver = PedID::Invalid;
  }
  // OnRemove observers tear the vehicle constraint and the body down, children (wheels, meshes) go with it
  if (c.entity.is_alive()) {
    c.entity.destruct();
  }
  c = Car{};
  c.alive = false;
}

auto World::spawn_pickup(this World& self, glm::vec2 position, i32 cash, i32 ammo) -> void {
  auto e = self.spawn_model(self.assets.cash, to3(position, 0.2f), self.random_float(0.0f, glm::two_pi<f32>()));
  self.pickups.push_back(Pickup{.entity = e, .position = position, .cash = cash, .ammo = ammo});
}

auto World::spawn_tracer(this World& self, glm::vec3 from, glm::vec3 to) -> void {
  const auto delta = to - from;
  const auto length = glm::length(delta);
  if (length < 0.1f) {
    return;
  }
  auto e = self.spawn_model(self.assets.tracer, from, heading_of({delta.x, delta.z}), glm::vec3(1.0f, 1.0f, length));
  if (e) {
    self.tracers.push_back(Tracer{.entity = e, .life = 0.08f});
  }
}
} // namespace oxcity
