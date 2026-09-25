// Procedural city: a grid of tiles where every ROAD_EVERY-th row and column is road and the 2x2 tile blocks in
// between are "superblocks" holding buildings, a park or the bank. Intersections form the road graph traffic
// drives on. Everything static gets a Static rigidbody so the player and the cars collide with it.

#include <glm/common.hpp>
#include <glm/gtc/constants.hpp>

#include "Scene/Components.hpp"
#include "Scene/Scene.hpp"
#include "Utils/Log.hpp"
#include "World.hpp"

namespace oxcity {
// floors per building style, must match BUILDING_STYLES in tools/assetgen/models.py
static constexpr i32 BUILDING_FLOORS[] = {3, 6, 2, 9, 4, 5};
static constexpr f32 FLOOR_HEIGHT = 3.2f;
static constexpr f32 BUILDING_HALF = 4.5f;

static auto add_static_box(ox::Scene& scene, glm::vec3 center, glm::vec3 half_extents, std::string_view name)
  -> flecs::entity {
  auto e = scene.create_entity(std::string(name), true);
  e.set<ox::TransformComponent>({.position = center});
  e.set<ox::BoxColliderComponent>({.size = half_extents, .friction = 0.8f});
  e.set<ox::RigidBodyComponent>({.type = ox::RigidBodyComponent::BodyType::Static});
  return e;
}

auto World::is_road_tile(this const World& self, i32 x, i32 z) -> bool {
  return x % ROAD_EVERY == 0 || z % ROAD_EVERY == 0;
}

auto World::tile_center(this const World& self, i32 x, i32 z) -> glm::vec2 {
  const auto half = static_cast<f32>(CITY_TILES - 1) * 0.5f;
  return {(static_cast<f32>(x) - half) * TILE, (static_cast<f32>(z) - half) * TILE};
}

auto World::world_to_tile(this const World& self, glm::vec2 p) -> glm::ivec2 {
  const auto half = static_cast<f32>(CITY_TILES - 1) * 0.5f;
  return {static_cast<i32>(std::round(p.x / TILE + half)), static_cast<i32>(std::round(p.y / TILE + half))};
}

auto World::is_solid(this const World& self, glm::vec2 p, f32 radius) -> bool {
  for (const auto& b : self.solid_boxes) {
    if (p.x > b.x - radius && p.x < b.z + radius && p.y > b.y - radius && p.y < b.w + radius) {
      return true;
    }
  }
  const auto limit = static_cast<f32>(CITY_TILES) * TILE * 0.5f;
  return glm::abs(p.x) > limit || glm::abs(p.y) > limit;
}

auto World::nearest_node(this const World& self, glm::vec2 p) -> NodeID {
  auto best = NodeID::Invalid;
  auto best_d = std::numeric_limits<f32>::max();
  for (usize i = 0; i < self.nodes.size(); i++) {
    const auto d = glm::distance(self.nodes[i].position, p);
    if (d < best_d) {
      best_d = d;
      best = static_cast<NodeID>(i);
    }
  }
  return best;
}

auto World::random_node(this World& self) -> NodeID {
  return static_cast<NodeID>(self.random_int(0, static_cast<i32>(self.nodes.size()) - 1));
}

auto World::random_sidewalk_point(this World& self) -> glm::vec2 {
  // a point on the sidewalk ring of a random superblock
  const auto blocks = CITY_TILES / ROAD_EVERY;
  const auto bx = self.random_int(0, blocks - 1);
  const auto bz = self.random_int(0, blocks - 1);
  const auto center = (self.tile_center(bx * ROAD_EVERY + 1, bz * ROAD_EVERY + 1) +
                       self.tile_center(bx * ROAD_EVERY + 2, bz * ROAD_EVERY + 2)) *
                      0.5f;
  const auto ring = TILE - 0.9f;
  const auto t = self.random_float(-ring, ring);
  switch (self.random_int(0, 3)) {
    case 0 : return center + glm::vec2(t, -ring);
    case 1 : return center + glm::vec2(t, ring);
    case 2 : return center + glm::vec2(-ring, t);
    default: return center + glm::vec2(ring, t);
  }
}

auto World::build_city(this World& self) -> void {
  ZoneScoped;

  auto& scene = *self.scene;
  const auto extent = static_cast<f32>(CITY_TILES) * TILE;

  // ground: one big slab for rendering, one collider whose top face is the road surface at y = 0
  self.spawn_model(self.assets.ground, glm::vec3(0.0f), 0.0f, glm::vec3(extent + 200.0f, 1.0f, extent + 200.0f));
  add_static_box(scene, glm::vec3(0.0f, -0.5f, 0.0f), glm::vec3(extent * 0.5f + 100.0f, 0.5f, extent * 0.5f + 100.0f), "ground_collider");
  // invisible walls at the city limits so nothing drives off the map
  const auto wall = extent * 0.5f + 1.0f;
  add_static_box(scene, glm::vec3(wall, 5.0f, 0.0f), glm::vec3(1.0f, 5.0f, wall), "wall_e");
  add_static_box(scene, glm::vec3(-wall, 5.0f, 0.0f), glm::vec3(1.0f, 5.0f, wall), "wall_w");
  add_static_box(scene, glm::vec3(0.0f, 5.0f, wall), glm::vec3(wall, 5.0f, 1.0f), "wall_s");
  add_static_box(scene, glm::vec3(0.0f, 5.0f, -wall), glm::vec3(wall, 5.0f, 1.0f), "wall_n");

  // roads
  for (i32 z = 0; z < CITY_TILES; z++) {
    for (i32 x = 0; x < CITY_TILES; x++) {
      if (!self.is_road_tile(x, z)) {
        continue;
      }
      const auto c = self.tile_center(x, z);
      const auto cross = x % ROAD_EVERY == 0 && z % ROAD_EVERY == 0;
      if (cross) {
        self.spawn_model(self.assets.road_cross, to3(c), 0.0f);
      } else {
        // the straight model runs along z, rotate it for roads running along x
        const auto along_x = z % ROAD_EVERY == 0;
        self.spawn_model(self.assets.road_straight, to3(c), along_x ? glm::half_pi<f32>() : 0.0f);
      }
    }
  }

  // road graph: every intersection, linked to the next one along each axis
  const auto nodes_per_axis = (CITY_TILES - 1) / ROAD_EVERY + 1;
  auto node_index = [nodes_per_axis](i32 ix, i32 iz) { return static_cast<NodeID>(iz * nodes_per_axis + ix); };
  for (i32 iz = 0; iz < nodes_per_axis; iz++) {
    for (i32 ix = 0; ix < nodes_per_axis; ix++) {
      auto node = RoadNode{.position = self.tile_center(ix * ROAD_EVERY, iz * ROAD_EVERY)};
      if (ix > 0) {
        node.neighbours.push_back(node_index(ix - 1, iz));
      }
      if (ix < nodes_per_axis - 1) {
        node.neighbours.push_back(node_index(ix + 1, iz));
      }
      if (iz > 0) {
        node.neighbours.push_back(node_index(ix, iz - 1));
      }
      if (iz < nodes_per_axis - 1) {
        node.neighbours.push_back(node_index(ix, iz + 1));
      }
      self.nodes.push_back(std::move(node));
    }
  }

  // superblocks
  const auto blocks = CITY_TILES / ROAD_EVERY;
  const auto bank_block = glm::ivec2(blocks / 2, 0);             // top middle, "north side"
  const auto hospital_block = glm::ivec2(0, blocks - 1);         // bottom left
  const auto police_block = glm::ivec2(blocks - 1, blocks - 1);  // bottom right
  const auto park_blocks = std::array{glm::ivec2(1, 2), glm::ivec2(blocks - 2, 1)};

  for (i32 bz = 0; bz < blocks; bz++) {
    for (i32 bx = 0; bx < blocks; bx++) {
      const auto block = glm::ivec2(bx, bz);
      const auto tx = bx * ROAD_EVERY + 1;
      const auto tz = bz * ROAD_EVERY + 1;
      const auto center = (self.tile_center(tx, tz) + self.tile_center(tx + 1, tz + 1)) * 0.5f;
      const auto is_park = std::ranges::find(park_blocks, block) != park_blocks.end();

      // sidewalk slabs
      for (i32 dz = 0; dz < 2; dz++) {
        for (i32 dx = 0; dx < 2; dx++) {
          const auto c = self.tile_center(tx + dx, tz + dz);
          self.spawn_model(is_park ? self.assets.park : self.assets.block, to3(c), 0.0f);
        }
      }
      add_static_box(scene, to3(center, 0.075f), glm::vec3(TILE, 0.075f, TILE), "sidewalk");

      // lamps on two corners, alternating
      const auto lamp_offset = TILE - 0.5f;
      const auto flip = (bx + bz) % 2 == 0 ? 1.0f : -1.0f;
      self.spawn_model(self.assets.street_lamp, to3(center + glm::vec2(lamp_offset, lamp_offset * flip), 0.15f), glm::half_pi<f32>() * 2.0f);
      self.spawn_model(self.assets.street_lamp, to3(center + glm::vec2(-lamp_offset, -lamp_offset * flip), 0.15f), 0.0f);

      if (is_park) {
        for (i32 i = 0; i < 7; i++) {
          const auto p = center + glm::vec2(self.random_float(-9.0f, 9.0f), self.random_float(-9.0f, 9.0f));
          if (glm::abs(p.x - center.x) < 1.5f || glm::abs(p.y - center.y) < 1.5f) {
            continue; // keep the paths clear
          }
          self.spawn_model(self.assets.tree, to3(p, 0.15f), self.random_float(0.0f, glm::two_pi<f32>()));
        }
        continue;
      }

      if (block == bank_block) {
        // the bank takes the south row, its door faces the road on the south side
        const auto bank_center = center + glm::vec2(0.0f, TILE * 0.5f);
        auto bank = self.spawn_model(self.assets.bank, to3(bank_center), 0.0f);
        const auto bank_half = glm::vec3(TILE - 1.5f, 3.6f, 4.5f);
        add_static_box(scene, to3(bank_center + glm::vec2(0.0f, -0.5f), 3.6f), bank_half, "bank_collider");
        self.solid_boxes.emplace_back(
          bank_center.x - bank_half.x,
          bank_center.y - 0.5f - bank_half.z,
          bank_center.x + bank_half.x,
          bank_center.y - 0.5f + bank_half.z
        );
        self.heist.position = bank_center + glm::vec2(0.0f, 7.8f);
        self.heist.marker = self.spawn_model(self.assets.marker, to3(self.heist.position, 1.5f), 0.0f);
        // the north row behind the bank is a little plaza with trees
        for (i32 i = 0; i < 4; i++) {
          self.spawn_model(self.assets.tree, to3(center + glm::vec2(-9.0f + 6.0f * static_cast<f32>(i), -TILE * 0.6f), 0.15f), 0.0f);
        }
        continue;
      }

      if (block == hospital_block) {
        self.hospital = center + glm::vec2(TILE - 0.8f, 0.0f);
      }
      if (block == police_block) {
        self.police_station = center + glm::vec2(-TILE + 0.8f, 0.0f);
      }

      // one building per tile
      for (i32 dz = 0; dz < 2; dz++) {
        for (i32 dx = 0; dx < 2; dx++) {
          const auto c = self.tile_center(tx + dx, tz + dz);
          auto style = self.random_int(0, static_cast<i32>(std::size(BUILDING_FLOORS)) - 1);
          if (block == hospital_block || block == police_block) {
            style = block == hospital_block ? 2 : 1;
          }
          // face the building's door towards the nearest road
          const auto yaw = (dz == 1 ? 0.0f : glm::pi<f32>()) + (dx == 0 ? 0.0f : 0.0f);
          self.spawn_model(self.assets.buildings[static_cast<usize>(style)], to3(c), yaw);

          const auto height = static_cast<f32>(BUILDING_FLOORS[style]) * FLOOR_HEIGHT;
          add_static_box(scene, to3(c, 0.15f + height * 0.5f), glm::vec3(BUILDING_HALF, height * 0.5f, BUILDING_HALF), "building_collider");
          self.solid_boxes.emplace_back(c.x - BUILDING_HALF, c.y - BUILDING_HALF, c.x + BUILDING_HALF, c.y + BUILDING_HALF);
        }
      }
    }
  }

  if (self.hospital == glm::vec2(0.0f)) {
    self.hospital = self.nodes.front().position + glm::vec2(4.0f, 4.0f);
  }
  if (self.police_station == glm::vec2(0.0f)) {
    self.police_station = self.nodes.back().position - glm::vec2(4.0f, 4.0f);
  }
}
} // namespace oxcity
