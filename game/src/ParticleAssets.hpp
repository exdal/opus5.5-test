#pragma once

#include <filesystem>

namespace oxcity {
// builds the game's particle graphs with the engine's ParticleGraph API and writes them as .oxparticle files into
// `particles_dir` (normally game/assets/Particles). Run through `OxCity --write-particles <dir>`.
auto write_particle_assets(const std::filesystem::path& particles_dir) -> bool;
} // namespace oxcity
