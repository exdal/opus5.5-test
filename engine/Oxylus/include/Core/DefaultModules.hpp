#pragma once

#include <tuple>

#include "Asset/AssetManager.hpp"
#include "Audio/AudioEngine.hpp"
#include "Core/Input.hpp"
#include "Networking/NetworkManager.hpp"
#include "Physics/Physics.hpp"
#include "Render/Renderer.hpp"
#include "Scripting/LuaManager.hpp"
#include "UI/ImGuiRenderer.hpp"
#include "UI/RmlUI.hpp"

namespace ox {
// AudioEngine comes before AssetManager: modules deinit in reverse order, and audio assets own ma_sounds that have
// to be uninitialized while the engine they belong to is still alive
using DefaultModules =
  std::tuple<LuaManager, AudioEngine, AssetManager, Physics, Input, NetworkManager, Renderer, ImGuiRenderer, RmlUI>;
}
