#pragma once

#include <string>

namespace ox {
struct TilemapComponent;
class TilemapSerializer {
public:
  TilemapSerializer() {}

  void serialize(const std::string& path);
  void deserialize(const std::string& path);
};
} // namespace ox
