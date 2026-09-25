# OxCity

A top-down open-city crime game built on the [Oxylus engine](https://github.com/oxylusengine/Oxylus),
written as a field test of the engine. The main output of this repo is the experience report in
[`docs/`](docs/); the game is the vehicle for it.

You wake up outside the hospital with a pistol and no money. Steal cars, drive them, mug people on the
sidewalk, rob the bank on the north side, and try to lose the cops before they arrest you.

![driving](docs/screenshots/03_driving.png)

## What's in the game

- A procedural city: 16x16 tiles of roads, sidewalks, parks, six building styles, street lamps and a
  bank. Intersections form a road graph.
- On foot: screen-relative movement on the engine's Jolt character controller, fists and a pistol,
  and mugging (hold **E** next to someone).
- Cars: every car is a Jolt wheeled vehicle (engine `VehicleComponent`). Get in, steal a parked car,
  or pull a driver out (carjack). There are five models with different handling.
- Traffic that follows the road graph and brakes for things in front of it. Pedestrians walk the
  sidewalks, cross streets and flee from violence.
- A wanted level of 0 to 5 diamonds: police cars spawn out of sight and chase you (they ram you if
  you're in a car, and cops jump out if you're on foot), cops shoot from 3 stars, and you get
  **ARRESTED** if they catch you standing still. **FLATLINED** sends you to the hospital.
- The bank heist: stand on the green marker and hold **E** for 8 seconds while the alarm rings and
  the guards open fire. The payout is $6,000 to $12,000.
- The HUD, pager messages, prompts, heist progress bar and main/pause menus are all RmlUi documents
  bound to one data model.
- Procedural low-poly models and synthesized sound effects, both generated from scripts in
  `tools/assetgen`.

Controls: **WASD/arrows** move and drive, **Shift** sprint, **Space** handbrake, **F/Enter** get in
and out, **Ctrl/LMB** attack, **Q** switch weapon, **E** rob, **H** horn, **Esc** pause.

## Repo layout

| Path | What |
|---|---|
| `engine/` | Oxylus, vendored with `git subtree` (upstream `main` + PR #144), with a few local patches |
| `game/src` | the game (C++23, one engine module) |
| `game/assets` | cooked by the engine at build time: models (`.glb`), sounds (`.wav`), RmlUi documents, fonts |
| `tools/assetgen` | generators for the models and sounds |
| `tools/sandbox` | bootstraps the toolchain inside `.sandbox/` (nothing is installed system-wide) |
| `tools/xmake-repo` | xmake package overrides for downloads the sandbox's network blocks |
| `docs/DEVLOG.md` | the journal of building the game on Oxylus |
| `docs/ENGINE_CHANGES.md` | every change made to `engine/`, why, and what to upstream |
| `docs/ENGINE_FEEDBACK.md` | ranked bugs, friction and wins |

## Building

On a normal Linux dev box with the engine's usual requirements (xmake, clang/libc++ 23, the Vulkan
SDK):

```sh
cd engine
xmake f --toolchain=clang --runtimes=c++_static -m release --editor=n
xmake b OxCity
xmake r OxCity
```

xmake picks up the root `xmake.lua`, which includes the engine and `game/xmake.lua`.

In the locked-down sandbox this was developed in (no GPU, no system installs, restricted network):

```sh
tools/sandbox/bootstrap.sh          # xmake, clang 23 (conda-forge), lavapipe, into .sandbox/
. ./env.sh
cd engine && xmake f --toolchain=clang --runtimes=c++_static -m release --llvmpipe=y --editor=n -y
xmake b OxCity && cd ..
tools/run_headless.sh --autoplay --frames 3000 --fixed-dt 0.0333 --screenshots captures
```

`--autoplay` plays the game from a script: menu, steal a car, drive, mug someone, shoot, rob the bank,
run from the cops, get arrested, respawn. It takes screenshots on the way and prints a pass/fail
checklist at the end.
