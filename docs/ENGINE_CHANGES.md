# Engine changes

Every edit made under `engine/`, relative to the vendored upstream commit.

Vendored base: `oxylusengine/Oxylus` `main` @ `af9f521f` (git subtree, squashed, prefix `engine/`).
To see every local engine change: `git diff e8912c42 -- engine/` (e8912c42 = the squashed vendor commit).

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

_None yet._
