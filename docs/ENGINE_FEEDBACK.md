# Oxylus engine feedback

What I ran into building OxCity (a top-down open-city crime game) on Oxylus `main` @ `af9f521f` + PR #144,
ranked by how much it would help the next person making a game. Every item says whether it's **fixed
locally** (see [ENGINE_CHANGES.md](ENGINE_CHANGES.md)), **worked around** in the game, or just
**noted**.

Context: I built everything as a code-first game (no editor): C++ gameplay, the engine's Jolt, RmlUi,
miniaudio and asset cooking. It ran headless on llvmpipe with 4 CPU cores.

---

## Bugs

| # | Severity | What | Where | Status |
|---|---|---|---|---|
| B1 | High | **A module that fails `init()` silently leaves every later module uninitialized.** `ModuleRegistry::init` stops at the first failure and returns `false`, but `App::init` ignores the return value and the game carries on. On a machine where `ma_engine_init` fails (no audio device), Physics, Input, Renderer, RmlUI and the game module would all be skipped and crash later somewhere unrelated. | `Core/App.cpp` (`self.registry.init();`) | noted. Suggest `OX_LOG_FATAL` or stopping the app on `false` |
| B2 | High | Physics runs in **slow motion below 60 fps**: the physics system uses a flecs interval tick source, which fires at most once per `progress()`, and `progress()` didn't take the frame delta. | `Scene.cpp` | **fixed locally** (patch 4) |
| B3 | High | Headless / offscreen SDL can't create a surface: the instance never enables the surface extensions SDL asks for. | `RenderContext::create_context` | **fixed locally** (patch 1) |
| B4 | Medium | Swapchain is **256x256** on surfaces without a fixed extent (headless, Wayland); `handle_resize` ignores its size arguments. | `RenderContext.cpp` | **fixed locally** (patch 2) |
| B5 | Medium | SIGSEGV at shutdown if any audio asset is still referenced: AudioEngine deinits before AssetManager. | `DefaultModules.hpp` | **fixed locally** (patch 3) |
| B6 | Medium | `AudioSourceComponent`: spatialization toggled by `looping`, and the source position is never set, so every 3D sound sits at the origin. | `Scene.cpp` `audio_source_update` | **fixed locally** (patch 5) |
| B7 | Medium | `create_model_entity(uuid)` + `entity.child_of(parent)` **aborts inside flecs** as soon as two instances of the same model are parented to the same entity. The root is named after the glTF node, names are only deduplicated among root entities, and re-parenting frees the name again, so the second `child_of` collides. This happens to anyone attaching wheels to a car. | `Scene::create_model_entity` | worked around (rename before `child_of`). Suggest `create_model_entity(uuid, parent)` |
| B8 | Low | `BoxColliderComponent` ignores the entity's world scale, while Sphere/Capsule/Cylinder use it. Sphere/Capsule/Cylinder also use `2 * radius * scale` as the radius, so a "radius 0.5" sphere is 1 m in radius. The components are inconsistent with each other and with their names. | `Scene::create_shape` | noted |
| B9 | Low | `Scene::cast_ray` only runs a **broad phase** query (AABB hits, no hit fraction or normal). Anything like a bullet needs `get_physics_system()->GetNarrowPhaseQuery()` directly. | `Scene::cast_ray` | worked around (game does its own hitscan) |
| B10 | Low | One Material asset with refcount 1 is reported "still alive" at every shutdown, even with nothing else leaked. | AssetManager deinit | noted, not investigated |
| B12 | High | **Vulkan validation errors in the renderer**: invalid SPIR-V in `scene.slang` (`!` on a uint), 13 images cleared without `TRANSFER_DST` usage, and FSR3 history images sampled in the wrong layout every frame (wrong `last_access` on `acquire_ia`). The project owner hit a device lost on a real GPU. | `scene.slang`, `RendererInstance.cpp`, `Passes/*` | **fixed locally** (patches 6-8) |
| B13 | High | A parked (sleeping) Jolt vehicle **can never be driven off**: waking it through `MotionProperties` doesn't activate the body. | `Scene.cpp` `vehicle_input` | **fixed locally** (patch 9) |
| B14 | Low | Culling shaders fail the 1.3.275 spirv-val (`AliasedPointer`/`RestrictPointer` missing on function-local PSB pointer variables), and one descriptor set layout leaks at shutdown. | Slang codegen, RenderContext | noted |
| B15 | High | **GPU hang (AMD) the first time a model with new materials is spawned from game code.** Materials are uploaded in `Renderer::update`, which runs before later modules, so the frame that spawns the model draws with material indices the GPU buffer doesn't have yet. In OxCity that's the first cash pickup (robbing a ped) or the first tracer. Lavapipe tolerates the stale read, radv hangs in the VSM mesh-shader draw. | `RendererInstance::update`, `Renderer::sync_materials` | **fixed locally** (patch 10) |
| B16 | High | **Model refcounts are unbalanced, and unloading unregisters the asset.** `create_model_entity` takes one ref per spawned model, but every `MeshComponent` removal releases one. Destroying one multi-mesh instance unloads the model under all the other instances (null deref in `prepare_render`). And at refcount zero the asset is erased from the registry, so it can never be loaded again. In OxCity: a crash on respawn after an arrest (all cops despawn), and no more tracers after the first one faded. | `Scene.cpp` model spawning, `AssetManager::release_ref` | **fixed locally** (patch 11) |
| B17 | Low | `Camera::get_screen_ray` returns a ray whose direction points **back at the camera**. It unprojects NDC z=0 as "near" and z=1 as "far", but the engine's projection is reversed-z, so those are swapped. Anything that walks the ray forward from its origin (picking, `t > 0` tests) misses. The Lua binding `get_screen_ray_from_camera` inherits it. | `Render/Camera.cpp` | worked around (OxCity solves for the line, sign-agnostic) |
| B18 | Medium | Destroyed meshes leave their **shadow baked into the VSM**: page invalidation only runs for moved/added instances. Every bullet tracer left a shadow on the street. | RMVSM invalidation | **fixed locally** (patch 13) |
| B19 | Low | `~Texture` on a never-created texture calls `App::get_rendercontext()`, so `ParticleSystem` (or anything holding a `Texture`) can't be destroyed without a running renderer. | `Texture::destroy` | **fixed locally** (patch 12) |
| B20 | Medium | **A particle system that can't be read silently becomes the default one**, which loops at 32/s forever. `load_particle_system` falls back to `ParticleSystem::make_default()` when the file is missing, logs one error, and the game gets white dots that never stop. Fine for the editor's "new asset", wrong for a shipped asset. | `AssetManager::load_particle_system` | worked around (the game checks spawn_rate). Suggest failing the load |
| B21 | Medium | **The cooker registers `.oxparticle` (and audio) in the manifest by source path but doesn't pack or install them**, so a game has to know to copy them next to the binary with `ox.install_resources`. Nothing warns when a registered file is missing at runtime until something loads it. | `ox.cook_assets` rule | worked around (install rule). Suggest the cooker packs them, or installs what it registers |
| B11 | Cosmetic | First run logs `ERR File error: Unknown, Path: context_config.toml`. A missing config on first launch is normal, it shouldn't be an error. | `ContextCVar::load` | noted |

## Missing features / API friction (ranked)

1. **No "make a game" path in the docs.** AGENTS.md is excellent, the best onboarding document in the
   repo. But the only example of hosting a scene outside the editor is reading `Editor::update`: you
   `new_frame()`, call `Scene::render(target, viewport_origin, viewport_size, surface_size)` and then
   `end_frame()`. A 50-line `examples/minimal_game` target would have saved me an hour. It would cover
   App + DefaultModules + a module that owns a Scene, renders it and loads an RmlUi document.
2. **Engine shaders are declared in `OxylusEditor/Assets/engine.toml`.** A game built with `--editor=n`
   still has to reach into the editor's folder (`add_files("../engine/OxylusEditor/Assets/engine.toml")`)
   or it has no renderer. The engine's shader manifest belongs with the engine library, e.g.
   `Oxylus/Assets/engine.toml` plus an `ox.engine_resources` rule.
3. **No per-entity visibility.** The `Hidden` tag only affects the editor. Hiding the player while
   they're in a car meant moving the entity 500 m under the map. `MeshComponent::visible` or a render
   tag would do.
4. **`VehicleComponent` mixes settings and per-frame input.** Writing `input_forward` with `set<>` or
   `modified<>` fires the OnSet observer, which **destroys and rebuilds the whole Jolt constraint**
   every frame. The only safe way is `get_mut` without notifying, which is exactly the opposite of what
   flecs users are taught. Split it into `VehicleComponent` (settings) and `VehicleInputComponent`
   (driver input, read by the `vehicle_input` system).
5. **The character controller is `JPH::Character`, driven from Lua.** In C++ there's no API: you cast
   `CharacterControllerComponent::character` to `JPH::Character*` and call `SetLinearVelocity` /
   `SetRotation` yourself. To teleport one you remove and re-add the component, because the observer
   only builds it from the entity's position. The component's `max_ground_speed`, `jump_force`,
   `friction`, ... fields aren't used by the C++ side at all. A small `CharacterMove{velocity, yaw}`
   input component, with the engine applying it, would fit the ECS design.
6. **Model hierarchy depth is not what the glTF says.** The compiler adds a group for the glTF
   *scene*, so a root node's children end up as grandchildren of the spawned entity, and
   `root.lookup("leg_l")` finds nothing. I wrote a recursive `find_descendant`. A helper like
   `Scene::find_node(root, name)` would help everyone who animates parts of a model.
7. **RmlUi has no VFS-aware file interface.** Documents and fonts load through RmlUi's default
   `fopen`, so the game resolves real paths itself (`vfs.resolve_physical_dir(APP_DIR, ...)`) and loads
   fonts itself (`Rml::LoadFontFace`). An engine `RmlFileInterface` over the VFS, plus a default font,
   would make `context->LoadDocument("ui/hud.rml")` just work.
8. **Audio for games is manual.** There's `AudioSourceComponent` for 3D sounds on entities. For
   one-shots, UI sounds and looping 2D sounds, you get the `ma_sound*` from the asset, then
   seek/play/stop yourself, and turn **off** miniaudio's default spatialization or everything plays
   from the world origin. A `AudioEngine::play_oneshot(uuid, volume, pitch)` would be welcome. One
   `ma_sound` per asset also means the same effect can't overlap itself (two gunshots).
9. **`create_model_entity` spawns synchronously and loads the asset on the calling thread.** That's
   fine for a city built at load time, but there's no pooling or instancing API. 20 police cars means
   20 full hierarchy spawns and 20 x 5 flecs entities.
10. **No "keep this resident" API.** Once refcounts are right (patch 11), a model unloads the moment
    its last instance goes away. Anything spawned briefly (a tracer that lives 80 ms, a cash pickup, a
    police car that comes and goes) would be read from disk and re-uploaded every time. A game has to
    know to call `load_asset` once at startup and `unload_asset` at shutdown to pin it, and nothing
    documents that. A `Scene::preload(uuid)` or an `AssetHandle` RAII type would say it in the API.
11. **The particle system is powerful but only reachable through the editor.** The node graph compiled
    to bytecode is a good design, and the VM is fast and easy to read. But there's no way to author a
    system in code except building `ParticleGraph`s by hand (as OxCity does in `ParticleAssets.cpp`), and
    the file format stores node types as raw enum integers, so it can't be written by anything but C++.
    Small helpers would make it a pleasure: named node types in JSON, a `ParticleGraphBuilder`, and a
    "burst only" preset. Also: `Random` is per component, so `direction * Random(a, b)` (the default
    graph's own pattern) skews the direction as well as the speed.
12. **Particles are depth tested against the scene**, which is right, but it means a burst emitted at
    an object's centre (an explosion inside a car) is invisible until it leaves the mesh. Worth a line
    in the docs, or an option to test against the depth *before* the emitter's own entity.
13. **`Scene` exposes Jolt in its public header** (`Scene.hpp` includes six Jolt headers). AGENTS.md
    already calls this debt, and I agree: every game TU pays for it.

## Things that were genuinely good

- **Asset cooking (PR #144)** is exactly right for a code-first game. You drop `.glb` and `.wav` files
  in a folder, add `add_rules("ox.cook_assets", ...)`, and look them up at runtime by source path
  (`AssetManager::find_asset`). My hand-written glTF files (flat normals, per-primitive materials,
  `KHR_materials_emissive_strength`) went through the meshlet compiler without a single complaint.
- **The renderer** looks great even on llvmpipe with half the features off: the sun, sky, shadows,
  bloom on emissive windows, sirens and street lamps. It was also robust to 1000+ small mesh entities.
- **RmlUi integration**: every scene has its own `RmlView`, and input routing works without any setup.
  Data models (`context->CreateDataModel`, `Bind`, `BindEventCallback`, `data-if`, `data-class-*`,
  `data-style-width`) drove the whole HUD and menus with zero glue code. `Scene::render` composites
  the UI for you.
- **Jolt wheeled vehicles out of the box**: `VehicleComponent` + four `VehicleWheelComponent`
  children gave me drivable cars, traffic AI and police rams in an afternoon. PR #143's forward-speed
  and slip queries show the direction is right.
- **The module system** (`App::with<T>(args...)`, `module_dependencies`, `update(Timestep)`) is small
  and pleasant. The whole game is one module.

## Debugging notes

- `--vulkan-validation` is built into the engine, which is great. With validation on, the engine's
  own shaders and renderer produced errors on the very first frame (B12). It would pay to run the
  editor with validation in CI, even on lavapipe: everything in B12 reproduces there with no GPU.
- **Lavapipe hides out-of-bounds GPU reads.** Both GPU crashes the project owner hit on AMD (B12, B15)
  ran for hundreds of frames on lavapipe without a hitch. The radv hang dump (`RADV_DEBUG=hang`) was the
  useful artifact: `pipeline.log` names the bound pipeline, and its NIR lists the bindings by their
  Slang names, so it maps straight back to a `.slang` file. Enabling `robustBufferAccess2` in a debug
  build, or a debug assert that every `material_index` is below the uploaded material count, would
  have turned B15 into a readable error.
- `acquire_ia(name, image, last_access)` is the easiest vuk API to get wrong: `last_access` means
  "how the previous frame left it", not "how I'm about to use it". A comment, or a helper that
  records the real last access at the end of the frame, would prevent the FSR3 class of bug.

## Build and platform notes

- CI pins **clang/libc++ 23**. libc++ 20 lacks `std::views::enumerate` (used in ResourceCompiler), so
  anything older than 21 fails. The README only says "a C++23 compiler".
- `package.precompiled=false` builds about 30 dependencies from source on first configure. With
  GitHub archive downloads blocked, most xmake-repo recipes fall back to `git clone` and work. The ones
  that don't are generic xmake-repo recipe issues, not Oxylus ones, and are fixed in `tools/xmake-repo`:
  vulkan-headers git tags, sol2 fetching Lua from lua.org, tracy's patch URL, SDL3 not finding X11 libs
  outside the default paths. A prebuilt dependency bundle would make first-time setup far less fragile.
- **llvmpipe**: with Mesa 25.2 (LLVM 20), llvmpipe advertises mesh shaders and ray tracing, and the
  engine uses both whenever they're present. The Mesa JIT then aborts (`LLVM ERROR: Cannot emit physreg
  copy instruction`) or segfaults. Setting `mesh_shaders = false` and `ray_tracing = false` in
  `context_config.toml` makes everything work. Since the engine already has an `llvmpipe` build option,
  it could default those two cvars off when the device is a CPU.
