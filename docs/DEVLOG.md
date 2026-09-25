# OxCity dev log

A running journal of building a GTA 2 style game ("OxCity") on the Oxylus engine. Written as I go,
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
