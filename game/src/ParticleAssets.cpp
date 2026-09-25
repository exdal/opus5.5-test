// Authoring for the game's particle systems. The engine's particle systems are node graphs (emitter, spawn and
// update) that it compiles to bytecode when the asset loads. The graphs are normally built in the editor's node
// canvas; this game has no editor workflow, so they're built here with the same `ParticleGraph` API the editor uses
// and written out as .oxparticle files:
//
//     OxCity --write-particles game/assets/Particles
//
// The JSON stores node types as raw enum values, so generating it from anything but C++ would silently break the day
// the enum is reordered. Going through the engine's own types and `ParticleSystem::write` keeps it honest.

#include "ParticleAssets.hpp"

#include <fstream>
#include <sstream>

#include "Asset/ParticleSystem.hpp"
#include "Utils/Log.hpp"

namespace oxcity {
using ox::ParticleNodeID;
using ox::ParticleNodeType;

namespace {
// a tiny wrapper so a graph reads as a list of nodes and links
struct GraphBuilder {
  ox::ParticleGraph& graph;
  f32 x = -600.0f;

  auto node(ParticleNodeType type, std::initializer_list<glm::vec4> params = {}, u32 index = 0) -> ParticleNodeID {
    const auto id = this->graph.add_node(type, {this->x, 0.0f});
    this->x += 180.0f;
    for (auto& n : this->graph.nodes) {
      if (n.id == id) {
        for (usize i = 0; i < params.size() && i < n.params.size(); i++) {
          n.params[i] = *(params.begin() + i);
        }
        n.index = index;
      }
    }
    return id;
  }

  auto link(ParticleNodeID from, ParticleNodeID to, u32 pin = 0) -> void { this->graph.add_link(from, to, pin); }
};

auto splat(f32 v) -> glm::vec4 { return glm::vec4(v); }
auto size2(f32 v) -> glm::vec4 { return {v, v, 0.0f, 0.0f}; }

// the shared update: gravity (negative floats up, for smoke), drag, colour over life from gradient 0, size over life
// from curve 0 times `base_size`
auto add_common_update(ox::ParticleSystem& system, f32 gravity, f32 base_size, f32 drag = 0.0f) -> void {
  auto g = GraphBuilder{system.update_graph};
  if (drag != 0.0f) {
    // velocity += velocity * -drag * dt
    const auto velocity = g.node(ParticleNodeType::ReadVelocity);
    const auto k = g.node(ParticleNodeType::Constant, {splat(-drag)});
    const auto dt = g.node(ParticleNodeType::ReadDeltaTime);
    const auto scaled = g.node(ParticleNodeType::Multiply);
    const auto step = g.node(ParticleNodeType::Multiply);
    const auto add_velocity = g.node(ParticleNodeType::AddVelocity);
    g.link(velocity, scaled, 0);
    g.link(k, scaled, 1);
    g.link(scaled, step, 0);
    g.link(dt, step, 1);
    g.link(step, add_velocity);
  }
  if (gravity != 0.0f) {
    const auto accel = g.node(ParticleNodeType::Constant, {{0.0f, -gravity, 0.0f, 0.0f}});
    const auto dt = g.node(ParticleNodeType::ReadDeltaTime);
    const auto step = g.node(ParticleNodeType::Multiply);
    const auto add_velocity = g.node(ParticleNodeType::AddVelocity);
    g.link(accel, step, 0);
    g.link(dt, step, 1);
    g.link(step, add_velocity);
  }

  const auto age = g.node(ParticleNodeType::ReadAge);
  const auto color = g.node(ParticleNodeType::Gradient, {}, 0);
  const auto set_color = g.node(ParticleNodeType::SetColor);
  g.link(age, color);
  g.link(color, set_color);

  const auto curve = g.node(ParticleNodeType::Curve, {}, 0);
  const auto base = g.node(ParticleNodeType::Constant, {size2(base_size)});
  const auto scaled = g.node(ParticleNodeType::Multiply);
  const auto set_size = g.node(ParticleNodeType::SetSize);
  g.link(age, curve);
  g.link(curve, scaled, 0);
  g.link(base, scaled, 1);
  g.link(scaled, set_size);
}

// spawn: velocity = emission direction * random speed, and a starting size
auto add_common_spawn(ox::ParticleSystem& system, f32 speed_min, f32 speed_max, f32 size) -> void {
  auto g = GraphBuilder{system.spawn_graph};
  const auto direction = g.node(ParticleNodeType::ReadVelocity);
  const auto speed = g.node(ParticleNodeType::Random, {splat(speed_min), splat(speed_max)}, 0);
  const auto velocity = g.node(ParticleNodeType::Multiply);
  const auto set_velocity = g.node(ParticleNodeType::SetVelocity);
  g.link(direction, velocity, 0);
  g.link(speed, velocity, 1);
  g.link(velocity, set_velocity);

  const auto start_size = g.node(ParticleNodeType::Constant, {size2(size)});
  const auto set_size = g.node(ParticleNodeType::SetSize);
  g.link(start_size, set_size);
}

// burst-only emitters: nothing spawns on its own, the game fires `Scene::emit_particle_burst`
auto burst_emitter(ox::ParticleSystem& system, u32 capacity, glm::vec2 lifetime, ox::ParticleEmissionShape shape, f32 shape_size)
  -> void {
  system.emitter.capacity = capacity;
  system.emitter.spawn_rate = 0.0f;
  system.emitter.looping = true;
  system.emitter.duration = 1.0f;
  system.emitter.lifetime = lifetime;
  system.emitter.shape = shape;
  system.emitter.shape_size = glm::vec3(shape_size);
  system.emitter.simulation_space = ox::ParticleSimulationSpace::World;
}

auto blood_spray(const ox::UUID& material) -> ox::ParticleSystem {
  auto system = ox::ParticleSystem{};
  burst_emitter(system, 768, {0.35f, 0.75f}, ox::ParticleEmissionShape::Hemisphere, 0.12f);
  system.render.material = material;
  system.render.billboard = ox::ParticleBillboardMode::VelocityStretched;
  system.render.velocity_stretch = 0.05f;
  system.render.blend = ox::ParticleBlendMode::AlphaBlend;
  system.render.sort = false;
  system.render.depth_collision = true;
  system.render.restitution = 0.05f;

  system.curves.push_back({.name = "Size", .points = {{0.0f, 1.0f}, {0.7f, 0.8f}, {1.0f, 0.35f}}});
  system.gradients.push_back({
    .name = "Blood",
    .keys = {{0.0f, {0.55f, 0.015f, 0.01f, 1.0f}}, {0.6f, {0.4f, 0.0f, 0.0f, 1.0f}}, {1.0f, {0.25f, 0.0f, 0.0f, 0.0f}}},
  });
  add_common_spawn(system, 1.2f, 4.0f, 0.1f);
  add_common_update(system, 14.0f, 0.12f);
  return system;
}

auto muzzle_flash(const ox::UUID& material) -> ox::ParticleSystem {
  auto system = ox::ParticleSystem{};
  burst_emitter(system, 256, {0.05f, 0.09f}, ox::ParticleEmissionShape::Sphere, 0.05f);
  system.render.material = material;
  system.render.billboard = ox::ParticleBillboardMode::FaceCamera;
  system.render.blend = ox::ParticleBlendMode::Additive;
  system.render.sort = false;

  system.curves.push_back({.name = "Size", .points = {{0.0f, 1.0f}, {1.0f, 0.3f}}});
  // above 1.0 on purpose: HDR, so the flash feeds the bloom
  system.gradients.push_back({
    .name = "Flash",
    .keys = {{0.0f, {6.0f, 4.5f, 1.6f, 1.0f}}, {1.0f, {3.0f, 1.0f, 0.2f, 0.0f}}},
  });
  add_common_spawn(system, 0.2f, 1.2f, 0.35f);
  add_common_update(system, 0.0f, 0.4f);
  return system;
}

auto sparks(const ox::UUID& material) -> ox::ParticleSystem {
  auto system = ox::ParticleSystem{};
  burst_emitter(system, 512, {0.15f, 0.35f}, ox::ParticleEmissionShape::Hemisphere, 0.05f);
  system.render.material = material;
  system.render.billboard = ox::ParticleBillboardMode::VelocityStretched;
  system.render.velocity_stretch = 0.04f;
  system.render.blend = ox::ParticleBlendMode::Additive;
  system.render.sort = false;

  system.curves.push_back({.name = "Size", .points = {{0.0f, 1.0f}, {1.0f, 0.4f}}});
  system.gradients.push_back({
    .name = "Hot",
    .keys = {{0.0f, {5.0f, 3.0f, 1.0f, 1.0f}}, {1.0f, {2.0f, 0.5f, 0.05f, 0.0f}}},
  });
  add_common_spawn(system, 2.0f, 6.5f, 0.05f);
  add_common_update(system, 9.8f, 0.06f);
  return system;
}

auto explosion(const ox::UUID& material) -> ox::ParticleSystem {
  // the fireball: big additive blobs thrown out fast, braked hard by drag, growing as they cool
  auto system = ox::ParticleSystem{};
  burst_emitter(system, 512, {0.35f, 0.8f}, ox::ParticleEmissionShape::Sphere, 0.8f);
  system.render.material = material;
  system.render.billboard = ox::ParticleBillboardMode::FaceCamera;
  system.render.blend = ox::ParticleBlendMode::Additive;
  system.render.sort = false;

  system.curves.push_back({.name = "Size", .points = {{0.0f, 0.6f}, {0.25f, 1.2f}, {1.0f, 1.6f}}});
  system.gradients.push_back({
    .name = "Fire",
    .keys = {{0.0f, {9.0f, 7.0f, 3.0f, 1.0f}},
             {0.25f, {6.0f, 2.2f, 0.3f, 1.0f}},
             {0.7f, {1.2f, 0.25f, 0.02f, 0.6f}},
             {1.0f, {0.2f, 0.05f, 0.0f, 0.0f}}},
  });
  add_common_spawn(system, 3.0f, 9.0f, 1.0f);
  add_common_update(system, -2.0f, 1.8f, 3.5f);
  return system;
}

auto smoke(const ox::UUID& material) -> ox::ParticleSystem {
  // thick, slow, rising; alpha blended and sorted so it layers over the street
  auto system = ox::ParticleSystem{};
  burst_emitter(system, 1024, {1.6f, 3.2f}, ox::ParticleEmissionShape::Sphere, 0.4f);
  system.render.material = material;
  system.render.billboard = ox::ParticleBillboardMode::FaceCamera;
  system.render.blend = ox::ParticleBlendMode::AlphaBlend;
  system.render.sort = true;

  system.curves.push_back({.name = "Size", .points = {{0.0f, 0.4f}, {1.0f, 1.8f}}});
  system.gradients.push_back({
    .name = "Smoke",
    .keys = {{0.0f, {0.05f, 0.045f, 0.04f, 0.0f}},
             {0.1f, {0.06f, 0.055f, 0.05f, 0.75f}},
             {1.0f, {0.18f, 0.18f, 0.18f, 0.0f}}},
  });
  add_common_spawn(system, 0.2f, 0.8f, 0.8f);
  add_common_update(system, -1.6f, 1.6f, 0.8f);
  return system;
}

auto tire_smoke(const ox::UUID& material) -> ox::ParticleSystem {
  // pale, low, quick to spread and fade
  auto system = ox::ParticleSystem{};
  burst_emitter(system, 1024, {0.7f, 1.3f}, ox::ParticleEmissionShape::Hemisphere, 0.2f);
  system.render.material = material;
  system.render.billboard = ox::ParticleBillboardMode::FaceCamera;
  system.render.blend = ox::ParticleBlendMode::AlphaBlend;
  system.render.sort = true;

  system.curves.push_back({.name = "Size", .points = {{0.0f, 0.4f}, {1.0f, 1.6f}}});
  system.gradients.push_back({
    .name = "Tire",
    .keys = {{0.0f, {0.7f, 0.7f, 0.7f, 0.0f}}, {0.1f, {0.75f, 0.75f, 0.75f, 0.45f}}, {1.0f, {0.8f, 0.8f, 0.8f, 0.0f}}},
  });
  add_common_spawn(system, 0.3f, 1.2f, 0.6f);
  add_common_update(system, -0.4f, 1.1f, 1.5f);
  return system;
}

auto shell_casings(const ox::UUID& material) -> ox::ParticleSystem {
  // brass flicked out to the side, bouncing off the pavement (depth collision)
  auto system = ox::ParticleSystem{};
  burst_emitter(system, 256, {1.2f, 1.8f}, ox::ParticleEmissionShape::Point, 0.0f);
  system.render.material = material;
  system.render.billboard = ox::ParticleBillboardMode::FaceCamera;
  system.render.blend = ox::ParticleBlendMode::AlphaBlend;
  system.render.sort = false;
  system.render.depth_collision = true;
  system.render.restitution = 0.35f;

  system.curves.push_back({.name = "Size", .points = {{0.0f, 1.0f}, {1.0f, 1.0f}}});
  system.gradients.push_back({
    .name = "Brass",
    .keys = {{0.0f, {1.6f, 1.1f, 0.3f, 1.0f}}, {0.8f, {0.9f, 0.6f, 0.15f, 1.0f}}, {1.0f, {0.9f, 0.6f, 0.15f, 0.0f}}},
  });
  add_common_spawn(system, 0.5f, 1.0f, 0.07f);
  add_common_update(system, 9.8f, 0.07f);
  return system;
}

auto cash_sparkle(const ox::UUID& material) -> ox::ParticleSystem {
  // green glitter popping up out of a pickup
  auto system = ox::ParticleSystem{};
  burst_emitter(system, 256, {0.4f, 0.9f}, ox::ParticleEmissionShape::Hemisphere, 0.3f);
  system.render.material = material;
  system.render.billboard = ox::ParticleBillboardMode::FaceCamera;
  system.render.blend = ox::ParticleBlendMode::Additive;
  system.render.sort = false;

  system.curves.push_back({.name = "Size", .points = {{0.0f, 1.0f}, {1.0f, 0.1f}}});
  system.gradients.push_back({
    .name = "Money",
    .keys = {{0.0f, {1.5f, 6.0f, 1.8f, 1.0f}}, {1.0f, {0.3f, 2.0f, 0.4f, 0.0f}}},
  });
  add_common_spawn(system, 2.0f, 4.5f, 0.12f);
  add_common_update(system, 5.0f, 0.14f, 1.0f);
  return system;
}

// the particle material lives in fx.glb; its uuid is whatever the cooker assigned, read from the sidecar
auto fx_material_uuid(const std::filesystem::path& assets_dir) -> ox::UUID {
  auto file = std::ifstream(assets_dir / "Models" / "Props" / "fx.glb.oxasset");
  auto contents = (std::stringstream() << file.rdbuf()).str();
  const auto materials = contents.find("\"materials\"");
  const auto key = contents.find("\"uuid\": \"", materials == std::string::npos ? 0 : materials);
  if (materials == std::string::npos || key == std::string::npos) {
    return {};
  }
  const auto start = key + 9;
  return ox::UUID::from_string(contents.substr(start, 36)).value_or(ox::UUID{});
}
} // namespace

auto write_particle_assets(const std::filesystem::path& particles_dir) -> bool {
  const auto assets_dir = particles_dir.parent_path();
  const auto material = fx_material_uuid(assets_dir);
  if (!material) {
    OX_LOG_ERROR("OxCity: no material uuid in {}/Models/Props/fx.glb.oxasset, build once to cook fx.glb", assets_dir);
    return false;
  }

  std::filesystem::create_directories(particles_dir);
  auto ok = true;
  auto write = [&](std::string_view name, ox::ParticleSystem system) {
    // compile once here as well, so a broken graph fails now rather than at load time in the game
    auto compiled = ox::compile_particle_graphs(system.emitter_graph, system.spawn_graph, system.update_graph);
    if (!compiled) {
      OX_LOG_ERROR("OxCity: particle system {} doesn't compile: {}", name, compiled.error());
      ok = false;
      return;
    }
    const auto path = particles_dir / fmt::format("{}.oxparticle", name);
    if (!system.write(path)) {
      OX_LOG_ERROR("OxCity: couldn't write {}", path);
      ok = false;
      return;
    }
    OX_LOG_INFO(
      "OxCity: wrote {} ({} spawn + {} update instructions)",
      path,
      compiled->spawn_count,
      compiled->update_count
    );
  };

  write("blood_spray", blood_spray(material));
  write("muzzle_flash", muzzle_flash(material));
  write("sparks", sparks(material));
  write("explosion", explosion(material));
  write("smoke", smoke(material));
  write("tire_smoke", tire_smoke(material));
  write("shell_casings", shell_casings(material));
  write("cash_sparkle", cash_sparkle(material));
  return ok;
}
} // namespace oxcity
