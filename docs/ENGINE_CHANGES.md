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

### 6. Invalid SPIR-V in `scene.slang`
- **File:** `Oxylus/src/Render/Shaders/scene.slang` (`sample_tangent_normal`)
- **Symptom (reported on a real GPU with validation on):** `VUID-VkShaderModuleCreateInfo-pCode-08737`,
  `Expected bool scalar or vector type as Result Type: LogicalNot`. The code was
  `if (!(this.flags & MaterialFlag::HasNormalImage))`, which Slang lowers to `OpLogicalNot` on a `u32`.
- **Change:** `if ((this.flags & MaterialFlag::HasNormalImage) == MaterialFlag::None)`.
- **Upstream suggestion:** take it. It was the only `!` applied to an integer in the shaders
  (grepped). A spirv-val step in the shader build (`rcli`) would catch the next one at compile time.

### 7. `TRANSFER_DST` usage on every image that `vuk::clear_image` touches
- **Files:** `Oxylus/src/Render/RendererInstance.cpp`, `Passes/PBR.cpp`, `Passes/PostProcess.cpp`
- **Symptom:** `VUID-vkCmdClearColorImage-image-00002` and `VUID-VkImageMemoryBarrier2-oldLayout-01213`.
  `vuk::clear_image` records `vkCmdClearColorImage`, but these images were created `Storage | Sampled`
  only. That's undefined behaviour, and a likely device-lost candidate on real drivers.
- **Change:** `eTransferDst` added to the sky transmittance/multiscatter LUTs, sky cubemap, sky view
  LUT, sky aerial perspective, both VSM virtual page tables, hiz, the three vbgtao images, and bloom
  downsampled/upsampled (including the 1x1 "bloom disabled" image). Color/depth attachments didn't
  need it: vuk clears those through the render pass load op.
- **Upstream suggestion:** take it. Better still, have vuk (or an engine helper around `clear_image`)
  add `eTransferDst` itself, since this is easy to forget.

### 8. FSR3 history: transfer usage and the right `last_access`
- **File:** `Oxylus/src/Render/Passes/FSR3.cpp`
- **Symptom:**
  - The same missing `TRANSFER_DST` on the history targets, which are cleared on reset.
  - Every frame, `UNASSIGNED-CoreValidation-DrawState-InvalidImageLayout` on three images: expected
    `READ_ONLY_OPTIMAL`, actual `GENERAL`. `vuk::acquire_ia(name, ia, last_access)` must describe how
    the *previous* frame left the image. All four histories were acquired as `eComputeSampled`, but
    color, accumulation and luma history are last *written* as storage (`GENERAL`). vuk therefore
    skipped the transition and they were sampled in the wrong layout every frame. Luma is last read,
    so `eComputeSampled` is right for it.
- **Change:** `eTransferDst` added to the history usage and to `new_locks`. `acquire_or_clear` takes
  a per-image `last_access`: `eComputeWrite` for color, accumulation and luma history,
  `eComputeSampled` for luma.
- **Upstream suggestion:** take it. FSR3 is the default upscaler, so this runs on every frame of
  every scene, and it's my best guess for the device lost the project owner saw.

### 9. Wake sleeping vehicles properly
- **File:** `Oxylus/src/Scene/Scene.cpp` (`vehicle_input` system)
- **Symptom:** a car that had been standing still long enough for Jolt to put it to sleep could
  never be driven again. The system tried to wake it with
  `GetMotionProperties()->SetLinearVelocity(GetLinearVelocity())`, which doesn't activate a body. In
  OxCity every parked car you steal was stuck at 0 km/h, while traffic (never asleep) drove fine.
- **Change:** `BodyInterface::ActivateBody` when there's throttle or steering input, which is what
  Jolt's own vehicle samples do. Brake or handbrake alone doesn't wake it, so parked cars holding the
  handbrake still sleep.

### 10. Sync materials before the frame reads them
- **File:** `Oxylus/src/Render/RendererInstance.cpp` (`RendererInstance::update`)
- **Symptom:** on the project owner's AMD GPU (radv), the game hung the GPU as soon as the player
  interacted with a pedestrian. The radv hang dump showed the GPU stuck in the VSM mesh-shader draw
  (`rmvsm_draw_physical_pages_ms`, pixel stage reading `materials[]`). Lavapipe never showed it.
- **Cause:** `Renderer::sync_materials` (which grows the global materials buffer and uploads dirty
  materials) only runs in `Renderer::update`. Modules update in registration order, so it runs
  **before** any game module registered after `DefaultModules`. Robbing or killing a ped spawns the
  first cash pickup, the first shot spawns the first tracer, and each loads a model with new
  materials during the game's update. That same frame, `Scene::prepare_render` hands the GPU mesh
  instances whose `material_index` points past the end of the materials buffer, or into its
  uninitialized slack. The garbage flags and texture indices hang radv. The materials only reach the
  GPU a frame later.
- **Change:** `RendererInstance::update` calls `renderer.sync_materials()` right before it acquires
  the materials buffer. It's a no-op when nothing is dirty. The comment that said "already synced by
  the renderer" was the assumption that broke.
- **Upstream suggestion:** take it, or sync inside `Renderer::get_materials_buffer`. The general
  problem is that GPU-side state is synced in module `update`, which depends on module order. Any
  asset loaded from a later module (the editor included) has the same one-frame gap.

### 11. Model refcounts: one ref per MeshComponent, and unloading doesn't unregister
- **Files:** `Oxylus/src/Scene/Scene.cpp` (`spawn_model_mesh_entity`, `create_model_entity`,
  `update_pending_model_spawns`), `Oxylus/src/Asset/AssetManager.cpp` (`release_ref`)
- **Symptom:** the game crashed on the project owner's machine right after being arrested, just as
  the player respawned. The respawn clears the wanted level, and the next frames despawn the police
  cars and cops that are far enough away. Older headless logs also showed `Cannot import an invalid
  model '<tracer uuid>'` for every shot after the first tracer had faded out; that part is confirmed.
  The crash itself is **not reproduced**: a headless arrest → respawn on the unpatched build ran
  cleanly. Part 1 below only crashes when *some* instances of a model are destroyed while others stay
  alive. In the scripted run every cop and police car was out of range and went in the same frame,
  so no instance was left to dereference the unloaded model. It's the prime suspect, not a proven cause.
- **Cause, part 1 (unbalanced refs):** `create_model_entity` takes **one** ref on the model (through
  `load_asset`), and the async path takes one per hierarchy, but the `MeshComponent` OnRemove
  observer releases **one per mesh entity**. A cop has a mesh per limb, so destroying the first cop
  drops the cop model to zero and unloads it while the other cops still draw it.
  `Scene::prepare_render` then does `asset_man.get_model(uuid)->gpu_meshes[...]` on a null model
  (its own comment in `attach_mesh` says "rendering assumes every mesh instance has a loaded
  model").
- **Cause, part 2 (unload = delete):** when a refcount reached zero, `release_ref` **erased the
  registry entry**, not just the loaded data. The asset then no longer exists: `load_asset` fails,
  and `find_asset` still returns the uuid because the source index isn't cleaned up. So even a
  balanced model (the single-mesh tracer) couldn't be spawned again once its last instance was gone.
- **Change:**
  - `spawn_model_mesh_entity` acquires a ref for the `MeshComponent` it writes, matching the
    observer. That's the convention the particle observer already documents: "whoever writes the
    uuid owns the ref".
  - `create_model_entity` hands back its `load_asset` ref once mesh entities hold theirs. It keeps
    the ref if the model spawned no meshes, since dropping it would unload the model under the new
    hierarchy.
  - The async path no longer takes a per-hierarchy ref.
  - `release_ref` resets the entry to "not loaded" (`model_id = Invalid`, which is the union's id)
    instead of erasing it. `delete_asset` is still the one that erases, and it also unindexes the
    source.
- **Upstream suggestion:** take both parts. Also consider making `prepare_render` skip (and log) a
  mesh instance whose model isn't loaded instead of dereferencing null; that would have turned this
  into a missing mesh plus a log line instead of a crash.

### 12. Destroying an empty `Texture` needs no renderer
- **File:** `Oxylus/src/Asset/Texture.cpp` (`Texture::destroy`)
- **Symptom:** OxCity builds its particle graphs with `ParticleGraph` and writes them with
  `ParticleSystem::write` from a tool mode that doesn't start the App. It segfaulted as soon as the
  first `ParticleSystem` went out of scope: `~ParticleSystem` → `~Texture` (the never-created
  `curve_atlas`) → `destroy()` → `App::get_rendercontext()` on a null App.
- **Change:** `destroy()` returns early when the texture holds no image, view or sampler.
- **Upstream suggestion:** take it. Any asset type with a `Texture` member (particle systems, materials)
  becomes usable in tools, tests and after-renderer teardown.

### 13. VSM: invalidate the pages of destroyed mesh instances
- **Files:** `Oxylus/src/Render/Shaders/passes/rmvsm_invalidate_pages.slang`,
  `rmvsm_pointspot_invalidate_pages.slang`, `Oxylus/src/Render/Passes/Shadowmaps.cpp`,
  `Oxylus/src/Scene/Scene.cpp` (MeshComponent OnRemove observer, `prepare_render`),
  `Scene.hpp`, `RendererInstance.hpp/.cpp`
- **Symptom (reported by the project owner):** bullet tracer shadows never went away. Each 80 ms
  tracer left its shadow baked into the sun's shadow map (and the street lamps').
- **Cause:** RMVSM caches shadow pages and only redraws pages that are invalidated. Invalidation is
  driven by *dirty* mesh instances (moved, or newly attached through `set_dirty`), using their previous
  and current transforms. A destroyed instance is in no GPU buffer anymore and never becomes dirty,
  so the pages its shadow was drawn into are never redrawn.
- **Change:** when a `MeshComponent` is removed, the Scene records the entity's last world AABB
  (`removed_mesh_bounds`, as center/size pairs). `prepare_render` hands them to the renderer instance,
  which uploads them. Both invalidate shaders gained a specialization constant
  (`INVALIDATE_FROM_BOUNDS`, id 60) and a bounds buffer; in that mode they project the world box with
  the clipmap's (or light view's) own matrix instead of reading an instance. `Shadowmaps.cpp` runs one
  extra dispatch per light type when anything was removed. The instance passes bind a placeholder to
  the new slot.
- **Upstream suggestion:** take it, or fold removal into the existing pass by giving it a list of
  world boxes for both cases (moved instances could contribute their previous box the same way).

### 14. Particles run on the scene's clock
- **Files:** `Oxylus/src/Render/RendererInstance.cpp` (`update`), `Oxylus/src/Scene/Scene.cpp`
  (`runtime_step`), `Oxylus/include/Scene/Scene.hpp`
- **Symptom:** particles ignored pause, hit-stop and fixed time steps. Headless on lavapipe (about 1 s
  of real time per frame) every short-lived effect (muzzle flash, sparks, fireball) was born and
  dead within one frame and never appeared. Only long-lived smoke showed.
- **Cause:** the particle simulation's delta was `App::get_timestep()` (wall clock), while everything
  else in a running scene advances by what the game passes to `runtime_step`.
- **Change:** `runtime_step` records `last_step_delta` (0 while the gameplay phases are disabled, i.e.
  paused). A running scene simulates its particles with it; outside play mode (the editor) particles
  keep real time.
- **Upstream suggestion:** take it. It also makes particles deterministic for replays and tests.

## Validation status

Run on lavapipe with Khronos validation 1.3.275 (`tools/run_headless.sh --validation`), 200 frames
covering the menu, gameplay and driving. After patches 6–8 no image usage or layout errors remain.
What's left:
- `VUID-VkPipelineShaderStageCreateInfo-pSpecializationInfo-06849` x5 on the culling compute
  pipelines: `OpVariable ... expected AliasedPointer or RestrictPointer for PhysicalStorageBuffer
  pointer`. These are function-local variables holding buffer device address pointers (one is a
  `u16x4*`). They come from Slang's codegen rather than from something written in the engine source,
  so they're **not patched here**. Worth checking with the current SDK's spirv-val and a current Slang.
- `VUID-vkDestroyDevice-device-05137`: one `VkDescriptorSetLayout` is never destroyed at shutdown.
- `VUID-VkDeviceCreateInfo-pNext-pNext` (unknown struct type 55): these validation layers are older
  than the Vulkan headers the engine uses. Not an engine bug.
