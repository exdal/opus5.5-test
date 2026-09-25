# Engine changes

Every edit made under `engine/`, relative to the vendored upstream commit.

Vendored base: `oxylusengine/Oxylus` `main` @ `af9f521f` (git subtree, squashed, prefix `engine/`),
plus upstream PR #144 merged on top (commit `2a333b0b`).

- Upstream + merged PRs: `engine/` at commit `2a333b0b`.
- Every local patch on top: `git diff 2a333b0b -- engine/`. The list below should match it one to one.
- Local patch commits are titled `engine: ...` so they can be cherry-picked (re-rooted onto the upstream
  repo with `git format-patch --relative=engine`).

## Upstream PRs merged locally

Open PRs were identified with `git ls-remote ... 'refs/pull/*/merge'` (GitHub only keeps merge refs
for open PRs; the REST API was not reachable from the sandbox).

| PR | Head | Status here | Why |
|---|---|---|---|
| [#144](https://github.com/oxylusengine/Oxylus/pull/144) asset cooking, game content mount, registry virtual paths | `fc3c6850` | **Merged** (commit `2a333b0b`), no conflicts | A standalone game needs to ship and mount content without the editor; also contains #143. |
| [#143](https://github.com/oxylusengine/Oxylus/pull/143) vehicle scripting APIs, lua module handling | `b45d2cc4` | **Merged** (as part of #144) | Vehicle speed/slip queries and nil-safe `get_body` are exactly what a driving game needs. |
| [#141](https://github.com/oxylusengine/Oxylus/pull/141) animations, skinned meshes, cinematics | `71df8383` | **Not merged** | Branched from `f2a45a9` (older than main). A proper 3-way merge onto #144 conflicts in 10 files: `Scene/Components.hpp`, `Scene/Scene.cpp` (3 hunks, physics debug draw + transform upload vs. skinning), `Render/RendererInstance.cpp` (`draw_debug_shapes` vs `draw_bounding_boxes`), `UI/AssetManagerViewer.*` (deleted on main, modified on #141) and the editor asset importer, which #144 moved into ResourceCompiler while #141 extended the old editor-side one. Needs a rebase by the author. |

Pitfall worth noting for anyone else vendoring Oxylus: `git subtree merge --squash` of #141 *after*
#144 silently produced a diff `fc3c6850..71df8383`, i.e. it **reverted #144** (deleted
`ResourceCompiler/public/AssetImport.hpp` etc.) without any conflict. Squashed subtree merges only
work for linear upstream history; sibling PR branches have to be 3-way merged on the upstream
history first.

## Local patches

Every patch here is something the game hit. Each lists the symptom, what changed, and what I'd suggest
as the proper upstream fix, when that differs from what I did.

### 1. Enable the window system's Vulkan instance extensions
- **Files:** `Oxylus/include/Render/Window.hpp`, `Oxylus/src/Render/Window.cpp`,
  `Oxylus/src/Render/RenderContext.cpp`
- **Symptom:** with SDL's offscreen video driver (any headless / CI run),
  `SDL_Vulkan_CreateSurface: VK_EXT_headless_surface extension is not enabled`, then
  `FATL no_surface_provided`. `create_context` only enables `VK_KHR_surface` and relies on vk-bootstrap
  to guess the platform surface extension. vk-bootstrap guesses the usual desktop ones and nothing
  else.
- **Change:** new `Window::get_vulkan_instance_extensions()` (wraps
  `SDL_Vulkan_GetInstanceExtensions`). `create_context` enables everything it returns.
- **Upstream suggestion:** take it as is. It's the documented way to create an SDL + Vulkan instance.

### 2. Size the swapchain from the window
- **File:** `Oxylus/src/Render/RenderContext.cpp` (`make_swapchain`, `handle_resize`, `new_frame`)
- **Symptom:** on surfaces without a fixed `currentExtent` (VK_EXT_headless_surface, and Wayland too),
  the swapchain was always **256x256** (vk-bootstrap's fallback), whatever the window size.
  `handle_resize(width, height)` ignored its arguments, and the vsync toggle calls
  `handle_resize(1, 1)`.
- **Change:** `make_swapchain` takes a desired extent and passes it to
  `SwapchainBuilder::set_desired_extent`. Callers pass `Window::get_size_in_pixels()`.
- **Upstream suggestion:** take it. Also consider not routing the present mode change through
  `handle_resize(1, 1)`.

### 3. Register AudioEngine before AssetManager
- **File:** `Oxylus/include/Core/DefaultModules.hpp`
- **Symptom:** SIGSEGV at shutdown whenever an audio asset is still referenced. Modules deinit in reverse
  order, so the miniaudio engine was destroyed first, and AssetManager then called `ma_sound_uninit` on
  sounds of a dead engine.
- **Change:** `DefaultModules` order is now `LuaManager, AudioEngine, AssetManager, ...`.
- **Upstream suggestion:** take it, or have AssetManager declare
  `module_dependencies = std::tuple<AudioEngine>` so the registry enforces the order.

### 4. Let the caller drive the scene clock, and let physics catch up
- **Files:** `Oxylus/include/Scene/Scene.hpp`, `Oxylus/src/Scene/Scene.cpp`
- **Symptom:** `runtime_update` called `world.progress()` with no delta (there was a
  `// TODO: Pass our delta_time?`), and `physics_step` runs on a flecs interval tick source. A tick
  source fires **at most once per frame**, so below 60 fps physics ran in slow motion: on the software
  rasterizer at 3 fps, cars moved at 1/20 speed while gameplay timers ran in real time. There was also no
  way to step a scene with a fixed delta for deterministic tests.
- **Change:**
  - New `Scene::runtime_step(f32 delta_seconds)`. `runtime_update(const Timestep&)` forwards to it, so
    existing callers behave the same.
  - `world.progress(delta_seconds)` now uses the caller's clock.
  - `physics_step` runs `round(delta_system_time / physics_interval)` substeps, clamped to 1..4, in one
    `PhysicsSystem::Update`.
  - It's a new name rather than an overload of `runtime_update`, because the Lua binding takes
    `&Scene::runtime_update` and an overload breaks that (`LuaSceneBindings.cpp`).
- **Upstream suggestion:** the substep loop is the important part. A proper fixed-step accumulator
  (with the interpolation alpha the engine already computes) would be better still.

### 5. AudioSourceComponent spatialization
- **File:** `Oxylus/src/Scene/Scene.cpp` (`audio_source_update` system)
- **Symptom:** spatialization was switched by `ac.looping`
  (`set_source_spatialization(..., ac.looping)`), a copy-paste slip. Nothing ever set a source's
  position, so every spatialized scene sound sat at the world origin.
- **Change:** use `ac.spatialization`, and when it's on, set the source position from the entity's
  world transform each frame.
- **Upstream suggestion:** take it. The game itself plays 2D sounds straight through `AudioEngine`, so
  this fix isn't exercised by OxCity. I only found it while reading the system.
