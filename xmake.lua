-- OxCity: builds the vendored Oxylus engine and the game together.
--
--   . ./env.sh
--   xmake f --toolchain=clang --runtimes=c++_static -m release --llvmpipe=y --editor=n
--   xmake build OxCity
--   xmake run OxCity
--
-- The engine's root script is included as-is so its options, packages and rules apply unchanged.
includes("engine/xmake.lua")
includes("game/xmake.lua")
