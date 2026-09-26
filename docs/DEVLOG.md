# OxCity dev log

A running journal of building a top-down open-city crime game ("OxCity") on the Oxylus engine. Written as I go,
in the voice of a game developer picking the engine up for the first time. Pain points are
summarised and ranked in [`ENGINE_FEEDBACK.md`](ENGINE_FEEDBACK.md); every change to `engine/` is in
[`ENGINE_CHANGES.md`](ENGINE_CHANGES.md).

Setup for this project: a cloud container with 4 cores, 15 GB RAM and **no GPU**. It has a
restrictive egress proxy (GitHub `git clone` works, GitHub release/archive tarballs, apt.llvm.org,
lua.org and xmake.io do not). House rule: nothing may be written outside the repo, so every tool,
cache and temp file lives in the gitignored `.sandbox/`.

---

## Day 1: getting the engine

### Vendoring
`git subtree add --prefix=engine ... main --squash` worked on the first try, and the engine is small
to vendor (9 MB). Three PRs were open upstream (#141, #143, #144). #144 (asset cooking, game content
mount) already contains #143 (vehicle scripting APIs), so I merged #144 and got both. Both matter to
me: a standalone game has to ship content without the editor, and I'm making a driving game.

#141 (animations/skinned meshes) was the one I really wanted, for walking pedestrians. It is
branched off an older main. My first attempt was a second `git subtree merge --squash`, and it
**silently reverted #144** with no conflict at all: squash merges only diff "last squashed commit ->
new commit". Once I did a real 3-way merge on upstream history, it conflicted in 10 files, deep in
`Scene.cpp` and `RendererInstance.cpp`. That's not something a game dev should resolve blind, so I
left #141 out. Characters get procedural limb swing instead (see later).

### Reading AGENTS.md
The engine ships a very thorough `AGENTS.md`. As onboarding documentation for a newcomer it's the
best thing in the repo: architecture, module registration order, asset ref counting rules, render
stages, the cooking pipeline. The README, by contrast, is a feature list and a build command. **A
human-facing "Getting started: make a game" doc would be the single most valuable addition.** Right
now the only example of "how to host a scene and draw it to the window" is the editor itself
(`Editor::update` -> `RenderContext::new_frame` -> `Scene::render` -> `end_frame`).

Note: AGENTS.md says AI agents must never create commits "in this repository". That's upstream's
policy for the Oxylus repository. This is a separate project repo that *vendors* the engine, and its
owner asked for commits, so engine patches here are committed in this repo and listed in
ENGINE_CHANGES.md for upstream to take (or not).

### Toolchain: the long way round
The engine needs **xmake** and a **very new clang**. CI uses clang/libc++ 23, and the engine uses
`std::views::enumerate`, which libc++ 20 doesn't have. Neither is on the box, and the usual download
sites are blocked. How I got there:

1. **xmake**: built from source via `git clone --recurse-submodules` of the release tag (about 2
   min). Running it as root needs `XMAKE_ROOT=y`.
2. **clang 23**: apt.llvm.org and the GitHub LLVM releases are blocked, but conda-forge is reachable
   and has LLVM 23.1.2. A sandboxed micromamba installed clang/lld/libc++ 23 into `.sandbox/llvm23`.
3. conda's clang has three surprises for anyone using it as a plain host compiler:
   - Its default triple is `x86_64-conda-linux-gnu`, and it auto-loads
     `x86_64-conda-linux-gnu-clang++.cfg`. That file forces conda's sysroot, so `<X11/Xlib.h>` goes
     missing.
   - Even with `--target=x86_64-linux-gnu`, it still picks up conda's gcc 16 install for headers,
     so I needed `--gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/13`.
   - `libc++.a` doesn't contain libc++abi, so xmake's `c++_static` runtime (`-static-libstdc++`)
     fails to link. I replaced `libc++.a` with a linker script
     `GROUP ( libc++_real.a libc++abi.a )`.
4. **lavapipe** (Mesa's software Vulkan) came out of the Ubuntu archive with `apt-get download` +
   `dpkg -x`, and an ICD json points at the sandboxed `.so`.

All of this is scripted in `tools/sandbox/bootstrap.sh` and loaded with `. ./env.sh`.

### First configure: packages
`package.precompiled=false` means xmake builds about 30 libraries from source. With GitHub archive
downloads blocked, every package falls back to `git clone`, which mostly just works. It's slower,
but xmake's fallback chain is genuinely good. The exceptions:
- **vulkan-headers**: the recipe's git fallback does `git checkout 1.4.335+0`, which isn't a tag.
  I fixed it with an override recipe in `tools/xmake-repo` that adds `add_versions("git:1.4.335+0",
  "vulkan-sdk-1.4.335.0")`. Engine-agnostic, but worth upstreaming to xmake-repo.
- **lua**: lua.org is blocked. The git fallback built fine, but the install test runs the freshly
  built `lua` binary, which links libc++ *dynamically* and couldn't find `libc++.so.1`. Fixed on the
  sandbox side.

None of this is Oxylus's fault. It's the cost of "build everything from source" plus a locked-down
network, and a real studio build machine would hit the same wall. A **prebuilt dependency bundle**
(or `package.precompiled=true` with a binary mirror) would make first-time setup far less fragile.

---

## Day 1, afternoon: writing the game

### Shape of the code
Everything is one engine module, `oxcity::Game`, registered after `DefaultModules`. It owns a `World`,
which owns an `ox::Scene` plus plain C++ vectors of gameplay objects (peds, cars, pickups) that hold
`flecs::entity` handles for their visuals and physics. The engine's own systems do the heavy work:
Jolt stepping, vehicle constraints, transform propagation, rendering, RmlUi. My code writes inputs
into components and reads positions back. I deliberately stayed in C++ rather than the Lua scripting
layer, to exercise the engine's public headers.

Per frame, `Game::update`:
1. reads input, or autoplay input
2. calls `World::update(input, dt)`, which runs the gameplay and then `scene->runtime_step(dt)`
3. calls `render_context.new_frame()`, `scene->render(...)`, `end_frame()`

Finding step 3 meant reading `Editor::update`. It's four lines once you know them.

### Assets
I wrote a ~500-line dependency-free glTF writer (`tools/assetgen/models.py`): boxes, wedges and
cylinders, flat normals, one primitive per material, and emissive strength for windows, lamps and
sirens. Characters are node hierarchies (`torso`, `leg_l`, `arm_r`, ...) with each node's origin at
its joint, so limbs can swing without skinning. Sounds are synthesized with the Python stdlib
(`tools/assetgen/sounds.py`): engine loop, siren, gunshot, cash register, alarm bell, and a little
chiptune radio loop.

The engine's cooker accepted all of it on the first try and wrote `.oxasset` sidecars next to the
sources (committed, as AGENTS.md asks). At runtime `AssetManager::find_asset(physical path)` returns
the UUID. The one catch: audio isn't packed, so the manifest points at the source `.wav` under the
assets dir, and the build has to copy the `.wav` files next to the binary too.

### Things that bit me, in the order they did
1. **Explicit object parameters + `const`**: I wrote `auto f(this const World& self) const`, and clang
   rightly refuses it. Engine-style C++23 takes a moment to get used to.
2. **`unique_ptr<ox::Scene>` with a forward declaration** needs the default member initializer
   dropped. Otherwise every TU that includes `World.hpp` instantiates the deleter.
3. **Headless Vulkan**: `SDL_VIDEO_DRIVER=offscreen` + lavapipe failed with
   `no_surface_provided`, because the engine never enabled `VK_EXT_headless_surface`. Engine patch 1.
4. **flecs abort** the first time a car got its second wheel: `create_model_entity` names the entity
   after the glTF node ("wheel"), and `child_of` collides with the first wheel's name. Renaming before
   re-parenting fixed it (feedback B7).
5. **Limbs not found**: the model compiler adds a group node for the glTF scene, so my limb nodes are
   grandchildren. A recursive lookup fixed it.
6. **llvmpipe JIT crashes** (`LLVM ERROR: Cannot emit physreg copy instruction`). This is Mesa 25.2's
   bug, not Oxylus's, but it's triggered by the mesh shader / ray tracing paths the engine turns on
   whenever the device advertises them. `context_config.toml` with `mesh_shaders = false` and
   `ray_tracing = false` fixed it. Mesa 24.0 (LLVM 17) doesn't have the headless WSI at all, so
   downgrading wasn't an option.
7. **Shutdown segfault**, from audio assets outliving the audio engine. Engine patch 3, and the game
   now gives its sound refs back.
8. **256x256 screenshots**: the headless swapchain ignored the window size. Engine patch 2.
9. **Pitch-black city**: my sun pointed at the ground. In Oxylus the directional light's forward
   axis points *towards* the sun (the editor's default scene uses +45 deg pitch). A comment on
   `LightComponent` would have saved a build.
10. **Cars in slow motion**: physics ticks at most once per frame. Engine patch 4 adds catch-up
    substeps and lets the game step the scene with its own (fixed) delta.

After that, the first real frame came out looking like the game in my head. The city is lit, with
shadows, buildings with lit windows and rooftop clutter, crosswalks, street lamps and traffic, and
the RmlUi HUD on top: the pager, the rolling cash counter, the wanted diamonds, health, weapon, and
the vehicle name and speed.

### RmlUi
This was the smoothest part. The scene already owns an `Rml::Context`
(`scene->get_rml_context()`), so the HUD is two `.rml` documents and one data model:

```cpp
auto ctor = context->CreateDataModel("hud");
ctor.Bind("money", &hud.money);
ctor.BindEventCallback("start", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { ... });
```

`data-if`, `data-class-lit="wanted >= 3"` and `data-style-width="health + '%'"` covered the whole
HUD, the heist progress bar and the menus without a single DOM lookup. The engine routes mouse
input to the view under the cursor, and the menu buttons just work. Two small gaps: no VFS-aware
file interface (I resolve real paths) and no default font (the game loads FiraSans itself).

---

## Day 1, evening: the validation pass

The project owner built the repo on a machine with a real GPU and got a validation error from
`scene.slang` (`OpLogicalNot` on a `uint`) and a **device lost**. Lavapipe had happily run all of it.
I pulled Ubuntu's Khronos validation layers into the sandbox (`tools/run_headless.sh --validation`)
and went through every message:

- **The shader error** was a one-line fix: `!(flags & X)` becomes `(flags & X) == MaterialFlag::None`.
- **13 images were cleared without `TRANSFER_DST` usage.** `vuk::clear_image` is a transfer command,
  and images created `Storage | Sampled` "just work" on lavapipe. A small script that pairs every
  `clear_image(x)` with `x`'s declaration found them all. Color and depth attachments are fine,
  because vuk clears those through load ops.
- **FSR3 sampled three history images in the wrong layout, every frame.** The history is re-acquired
  with `acquire_ia(..., eComputeSampled)`, but the previous frame left it as a storage write
  (`GENERAL`). Finding it meant logging `Texture::create` handles to rule out persistent textures,
  then matching the ping-pong handle pattern to FSR3's allocation order. That was the most
  interesting bug of the day, and FSR3 is on by default. It's my prime suspect for the device lost.

After that, 200 validated frames show no usage or layout errors. The only remaining messages are
Slang-generated SPIR-V the old spirv-val dislikes and a descriptor set layout leaked at shutdown
(ENGINE_CHANGES.md, "Validation status").

The same session's full autoplay run also caught a gameplay-breaking engine bug: every **parked car
was undrivable**. The autoplay stole one, floored it, and logged "drove 0 m". Jolt had put the
parked chassis to sleep, and the engine's wake-up (writing the velocity through
`MotionProperties`) doesn't activate a body. Traffic never noticed because it never stops long
enough to sleep. Fixed with `BodyInterface::ActivateBody` (patch 9). A rerun of the same script then drove the
stolen parked car 17 m in its 14 s driving slot, where it had moved 0 m before.

## Day 2: the pedestrian that hung the GPU

With validation off, the project owner got into the game on their AMD card, and then "when I touch
any peds, I get a GPU crash". They sent a `RADV_DEBUG=hang` dump: the command stream, the bound
pipeline's NIR/ISA and its SPIR-V.

Reading it:
- The compute (ACE) stream ends in `DISPATCH_TASKMESH_INDIRECT_MULTI_ACE` with `COUNT 10, STRIDE 12`:
  a mesh-shader draw with an indirect count of up to 10. That's the VSM "draw dirty clipmaps" pass,
  one draw per directional clipmap.
- `pipeline.log` confirms it: the task stage binds `draw_clipmaps`, `visibility`, `meshlet_instances`,
  `hpb`, and the pixel stage binds `page_tables`, `physical_pages`, `clipmaps` and `materials`. That's
  `rmvsm_draw_physical_pages_ms.slang` exactly.

None of that is ped-specific, so the question was what "touching a ped" changes on the scene side.
Peds have no physics bodies in OxCity; walking up to one only shows the "HOLD E: ROB" prompt, and
robbing (or killing) one **spawns the first cash pickup of the session**. That's the first model
loaded after startup. The chain from there:

1. The game module loads `cash.glb` inside its `update`, which creates new Material assets and marks
   them dirty.
2. The Renderer uploads dirty materials in `Renderer::update`, but that module already ran this frame,
   because modules update in registration order and the game comes last.
3. The same frame renders the pickup with a `material_index` past the end of the GPU materials buffer
   (or into the uninitialized half of a buffer that grew by doubling).
4. The VSM pixel shader reads garbage flags, sees `HasAlbedoImage`, and samples a garbage bindless
   index. Radv hangs. Lavapipe shrugs and draws the next frame correctly.

The fix is one line in `RendererInstance::update`: sync materials right before acquiring the buffer
(patch 10). A headless run with a log line in that spot confirmed the timing: on the frame of the
mugging, the global material count went from 128 to 131 *inside the render path*. Before the patch,
those three materials would have reached the GPU one frame late. The comment above it said "already synced by the renderer", which is only true if nothing
loads a model after the Renderer's update. That's what every game does.

What I'd take away as an engine user: **ordering by module registration is invisible API.** Nothing in
`App::with<>` tells you that a module registered after `Renderer` can't load assets and draw them in
the same frame. Either state sync belongs in the render path (as patched), or the engine needs a
documented "pre-render" phase.

## Day 2, later: "only the first bullet shows"

The project owner then reported that shooting only drew the first tracer: after that, just the gunshot
sound. The hitscan and the damage never depended on the tracer, but the tracer model did. When the first
tracer faded, the engine dropped the model to refcount zero and **erased it from the asset registry**, so
every later `spawn_tracer` logged `Cannot import an invalid model` (it had been in my own logs all along;
I had read it as noise). That's the second half of patch 11. Once that was fixed, a new problem showed
up: an 80 ms tracer now meant loading `tracer.glb` from disk on nearly every shot. The game now holds one
ref on every model it spawns at runtime for the whole session (`runtime_models()` in `World.cpp`). That's
a pattern the engine should make obvious (ENGINE_FEEDBACK.md, friction item 10).

## Day 2: aiming with the mouse

Next request: "I want to shoot where I am aiming". Until now the pistol fired along the player's heading,
which is the direction you last walked. The engine already has `Camera::get_screen_ray(camera, mouse,
window_size)`, and the camera component carries last frame's view/projection, so the game casts the cursor
ray onto a plane at bullet height (1.15 m). With the pistol out, the player turns to face the cursor; on
the frame you fire, the heading snaps to it, so the shot line goes exactly through the point under the
cursor.

One thing to watch out for: the ray comes back **pointing at the camera**. The function unprojects NDC
z=0 and z=1 as near and far, but the projection is reversed-z (`glm::perspective(fov, aspect, far,
near)`), so they're the wrong way round (B17). A plane intersection solved for the whole line doesn't
care; a "march forward from the origin" pick would find nothing.

## Day 2: the sounds were too shrill

"Sound effects are hurting my ears, they are too high pitched." Fair. I had designed them to be
readable on a laptop speaker: a 2.2 kHz square-wave pager, a 1.2/2.75 kHz bell, a siren sweeping up to
1.2 kHz with harmonics, square-wave horns, white-noise gunshots, all normalized to a 0.9 peak. On
headphones that's harsh. The generator now keeps fundamentals below ~900 Hz, uses a few band-limited
harmonics instead of square waves, low-passes everything (~1–3.5 kHz depending on the sound), fades in
over 3 ms, and gives each sound its own peak level. Measured on the old and new files, the spectral
centroid roughly halved (pager 3.4 kHz → 1.4 kHz, gunshot 3.9 kHz → 1.4 kHz) and the loud ones are
5–11 dB quieter. Nothing about this is engine-specific; I hadn't listened with headphones, and
headless I can't listen at all, so I measured instead.

## Day 2: particles, blood and a settings menu

The project owner asked for the engine's particle system ("it's node based but it provides bytecode,
not sure how you're gonna handle that"). The bytecode part is the engine's job: `ParticleSystem`
holds three node graphs (emitter, spawn, update) and `compile_particle_graphs` turns them into a small
register-VM program when the asset loads. The interesting part was authoring without the editor.
OxCity builds the graphs with the same `ParticleGraph` API the editor's canvas uses, and
`OxCity --write-particles game/assets/Particles` saves them with `ParticleSystem::write`. The JSON
stores node types as raw enum values, so a Python generator (like the models and sounds use) would
break silently the day that enum is reordered.

Two things went wrong on the way:
- The first tool run segfaulted in `~Texture`: an empty texture still asked for the render context
  (patch 12).
- The first version the owner played showed "default particles that never disappear". The cooker had
  registered my `.oxparticle` files in the manifest, but nothing installed them next to the binary.
  Every load failed, and the engine quietly substituted its default system, which loops forever
  (B20, B21). One line in `game/xmake.lua` fixed it, and `init_fx` now refuses a system that comes
  back looking like the default.

Effects now in the game, all burst-only emitters fired from small pools:
- **Blood:** a spray on every hit. Kills leave a pool plus a directional spatter decal thrown along the
  hit and stretched with its force. Wounded peds leave a trail.
- **Guns and cars:** muzzle flashes, sparks off walls and cars, shell casings bouncing off the
  pavement, tyre smoke when sliding, grey smoke from damaged cars and black smoke from wrecks.
- **Explosions:** a fireball, smoke, debris, a scorch mark, a short point-light flash and chain
  reactions.
- **Heist and pickups:** drill sparks at the vault, a cash sparkle on pickup.

One lesson from the first headless screenshots: particles are depth tested against the scene, so a
fireball emitted at the centre of a car stays hidden inside it. Emit above the roof.

The kill feedback leans on RmlUi: a full-screen red flash bound to a float, and a "3X COMBO" whose
`transform` is a data expression (`'rotate(' + combo_tilt + 'deg) scale(' + combo_scale + ')'`), which
RmlUi handled without complaint. Hit-stop is done game-side by stepping the scene with 5% of the frame
time; a delta of exactly zero would make flecs measure its own frame time. The settings panel (sound
effects and music on/off, saved to `oxcity_settings.txt`) is plain RmlUi data binding plus two event
callbacks. The menu got a short synthwave theme, so the music toggle has something to switch off
outside the car.

One more engine bug came from the owner: every bullet tracer left its shadow behind. RMVSM caches
shadow pages and only invalidated pages for moved or added meshes, never for removed ones (patch 13).
`docs/screenshots/vsm_ghost_shadow_{unpatched,patched}.png` show a van-shaped ghost shadow and the
clean road after the fix, from the same scripted run with the new pass switched off and on.

## Day 3: multiplayer

The ask: "we have a networking module, implement multiplayer". The owner picked **free-for-all PvP**
(everyone has their own wanted level and can kill each other), **listen server and a dedicated one**,
and **up to 4 players**.

### What the engine has

`Oxylus/{include,src}/Networking` is a thin, readable layer over ENet:
- `NetworkManager::create_server(port, max_clients)` and `create_client()`.
- `NetServer` and `NetClient`. Each has a `tick(timestep)`, reliable and unreliable channels, and an
  automatic handshake.
- An RPC system: `register_proc("name", callback)`, `call_server`, `call_client`, `broadcast_call`.
  Parameters are a variant of ints, floats, strings, a UUID and a byte array.
- `SceneSnapshotBuilder`, which reflects every `Networked` flecs entity's components into a byte blob
  and diffs it against the last acked one.

Two things I only learned by reading the source:
- `NetworkManager::update` is empty. Nothing talks until you call `tick()` yourself every frame.
- `tick()` returns true on the send rate you set with `set_tick_rate`. That turned out to be exactly the
  snapshot clock I wanted.

### Why not the snapshot builder

I read `SceneSnapshotBuilder` first, because "sync the scene" is what it promises. It doesn't fit a game
built like this one:
- It memcpys raw component bytes. `RigidBodyComponent` holds a Jolt pointer, names are strings, and
  neither survives a trip to another process.
- Entities are keyed by local flecs ids, and nothing maps those between host and client.
- OxCity's real state isn't in components anyway. A pedestrian is a position, a heading and a state
  machine in a game struct; the flecs entity only draws it.

Reading it also turned up a bug: `find_last_acked` never used its loop counter (patch 16). So OxCity
sends its own snapshot through the RPC layer, packed with zpp::bits into the byte-array parameter. The
whole protocol is seven RPCs: join, welcome, reject, input, snapshot, events and roster
(`game/src/NetMessages.hpp`).

### The design

- **The host simulates everything.** That's AI, Jolt vehicles, crime, damage and scoring, exactly as in
  single player. A dedicated server is a host whose local player slot is empty.
- **Snapshots.** Every tick (30 Hz) the host writes a snapshot of the whole city: 4 players, around 50
  peds, around 25 cars, pickups and the heist. Positions are 2 cm fixed point and angles 16 bits,
  which comes to 1.2–1.3 KB, one UDP datagram. It's a full snapshot, no deltas; at this size, deltas
  would buy complexity and nothing else.
- **Clients draw the past.** Each client draws the city 100 ms behind the host's clock, blending the two
  snapshots around that time. Slots carry a generation byte, so a reused ped or car slot respawns the
  entity instead of sliding the old one across town.
- **Events.** Anything that happens once (a shot, blood, a sound, a pager message, a kill feed line)
  is recorded by the same function that plays it on the host. It's shipped reliably with the next
  snapshot and replayed on the clients through the same function. Sounds became `play_at(sound,
  where)`, so each machine attenuates against its own listener.
- **Your own feet are yours.** A client walks its own Jolt character locally and sends its position with
  its input. The host moves that player's character there if it's plausible: at most sprint speed
  × 1.5, and not into a wall. When the host moves a player itself (spawn, respawn, into and out of
  cars), it bumps a teleport counter, and the client snaps to the host's position.
- **Everything else is the host's call.** That covers driving, shooting, stabbing, carjacking and
  pickups. Over the internet it's a round trip late; on a LAN you can't tell.
- **Presses as counters.** One-shot presses (get in the car, switch weapon) travel as counters in the
  unreliable input stream, so a lost packet can't eat a keypress.

Going from one player to four touched most gameplay files: `World::player` became
`players[MAX_PLAYERS]`, `by_player` became `PlayerID killer`, and "dead" and "arrested" became
per-player states instead of screen states. I did that first, alone, and reran the single-player
autoplay before writing any networking. It passed exactly as before, including a driving check that
was already failing on this machine.

### Headless, for real this time

The dedicated server is `OxCity --server`: an `App` with no window and only `LuaManager`,
`AssetManager`, `Physics` and `NetworkManager`. I expected a fight. It builds the city, 22 Jolt
vehicles and 50 pedestrians in **14 ms** and runs. `Scene` already checks for the renderer and RmlUi,
and entities spawned from models become bare transforms. The one crash was `App::step` reading the frame
limit from a render context that doesn't exist (patch 15). The server calls `with_frame_limit(60)` in
any case, so it doesn't burn a core.

### ENet and IPv6

The first real test failed before any packet was sent: `Failed to create new NetServer for port 7777!`.
The ENet fork the engine uses only opens IPv6 sockets (dual stack), and this container has no IPv6 at
all. `socket(AF_INET6)` returns `EAFNOSUPPORT`, so no server and no client can exist (B23). Desktops
almost always have an IPv6 stack, so players probably won't hit this, but Docker and some CI boxes will.
For testing I wrote a small `LD_PRELOAD` shim (`tools/netshim/ipv4_fallback.c`). When the IPv6
socket fails, it hands ENet an IPv4 socket and translates the IPv4-mapped addresses at the edges. It
never ships; `tools/run_net_test.sh` only loads it when the machine has no IPv6.

### Testing four players on one CPU

`tools/run_net_test.sh` starts a dedicated server and two scripted clients on 127.0.0.1
(`--net-autoplay shooter|target`, `game/src/NetAutoplay.cpp`), or a scripted listen-server host and
one client. Each prints a PASS/FAIL list.

The awkward part is time. The clients render through lavapipe at 1–2 fps, and a server that doesn't
render runs the city in real time around them: in the first run the cops arrested the shooter before
it had finished turning around. The test now slows the server's clock to the clients' pace
(`--fixed-dt`).

In the first joined frames (screenshots in `captures/net/`):
- the scoreboard, the kill feed ("ALICE JOINED") and the other player's name tag, drawn by RmlUi
  `data-for` over arrays of structs;
- replicated traffic and a police car with its lights going;
- a pedestrian killed by the client, credited by the host;
- an arrest: the fine note, then bail and a teleport to the police station.

The full dedicated run (server + ALICE the shooter + BOB the target, 25 minutes of wall clock for about
80 s of game time) passed every check:
- ALICE killed a pedestrian (the host credited it).
- ALICE wasted BOB: the server logged "ALICE wasted BOB", her pager said "YOU WASTED BOB. +1000 PTS", a 2X
  COMBO popped, and BOB's screen said "WASTED BY ALICE" (`docs/screenshots/mp_shooter_wasted_them.png`,
  `mp_target_wasted.png`).
- BOB respawned at the hospital (`mp_target_respawned.png`).
- ALICE stole a car and drove it 136 m. The car is a Jolt vehicle on the server, driven by her
  input over the network.
- ALICE was then shot dead by the police she'd been collecting stars from, which is how it should be.

Snapshots peaked at 1282 bytes, under ENet's 1392-byte MTU, so they never fragment.

The listen-server run (a scripted host, HOSTESS, and ALICE as the client) passed too:
- ALICE joined "HOSTESS'S CITY" and walked around; the host saw the movement.
- She killed a pedestrian and wasted HOSTESS.
- She stole a MERIDIAN and drove it (`docs/screenshots/mp_listen_driving.png`).
- When her process quit, the host logged "ALICE left" and freed the slot.

The menu got a MULTIPLAYER panel with name and address fields bound to the data model
(`mp_menu.png`). One RmlUi layout lesson: an `<input>`'s padding and border count towards its width,
so a label and an input sized to exactly fill a row wrap onto two lines.
