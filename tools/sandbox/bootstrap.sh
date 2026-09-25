#!/usr/bin/env bash
# Builds the whole toolchain inside .sandbox/ without writing anywhere else.
#  - xmake from source (github release downloads are blocked by the egress proxy, git clones work)
#  - clang/lld/libc++ 23.1.2 from conda-forge via a sandboxed micromamba (apt.llvm.org is blocked)
#  - mesa lavapipe (software vulkan) from Ubuntu noble-updates .debs, extracted with dpkg -x
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SB="$ROOT/.sandbox"
mkdir -p "$SB"/{tmp,src,debs,sysroot,bin,conda,home,apt/lists/partial,apt/cache/archives/partial}
export TMPDIR="$SB/tmp"

if [ ! -x "$SB/xmake/bin/xmake" ]; then
  git clone -q --depth 1 --branch v3.0.9 --recurse-submodules --shallow-submodules \
    https://github.com/xmake-io/xmake.git "$SB/src/xmake"
  (cd "$SB/src/xmake" && ./configure --prefix="$SB/xmake" && make -j"$(nproc)" && make install PREFIX="$SB/xmake")
fi

if [ ! -x "$SB/llvm23/bin/clang++" ]; then
  mkdir -p "$SB/micromamba"
  curl -sS -o "$SB/conda/micromamba.tar.bz2" https://conda.anaconda.org/conda-forge/linux-64/micromamba-2.9.0-0.tar.bz2
  tar -xjf "$SB/conda/micromamba.tar.bz2" -C "$SB/micromamba"
  printf "channels: [conda-forge]\n" > "$SB/condarc"
  HOME="$SB/home" MAMBA_ROOT_PREFIX="$SB/mamba-root" CONDARC="$SB/condarc" \
    "$SB/micromamba/bin/micromamba" create -y -p "$SB/llvm23" -c conda-forge --override-channels \
    clangxx=23.1.2 clang=23.1.2 lld=23.1.2 libcxx-devel=23.1.2 libcxx=23.1.2 compiler-rt=23.1.2 llvm-tools=23.1.2
  # conda-forge's libc++.a does not have libc++abi merged in, so `-static-libstdc++` (what xmake's
  # c++_static runtime uses) fails to link. make libc++.a a linker script that pulls in both
  mv "$SB/llvm23/lib/libc++.a" "$SB/llvm23/lib/libc++_real.a"
  printf 'GROUP ( libc++_real.a libc++abi.a )\n' > "$SB/llvm23/lib/libc++.a"
fi
# conda's clang defaults to the x86_64-conda-linux-gnu triple and ships target-prefixed .cfg files that
# force conda's sysroot and gcc 16 install. we want the host's glibc/X11/wayland headers instead, so
# every cfg is rewritten to target the host triple, use the host gcc 13 install for crt/libgcc, link
# with lld and still find libc++ in the conda prefix
for cfg in x86_64-conda-linux-gnu x86_64-conda-linux-gnu-clang x86_64-conda-linux-gnu-clang++ \
           x86_64-conda-linux-gnu-clang-cpp clang clang++; do
  printf -- '--target=x86_64-linux-gnu\n--gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/13\n-fuse-ld=lld\n$-Wl,-L,<CFGDIR>/../lib\n' \
    > "$SB/llvm23/bin/$cfg.cfg"
done

# tools that xmake builds and runs during package install (lua, rcli) link libc++ dynamically, expose
# only the libc++ runtime to the loader instead of the whole conda lib dir
mkdir -p "$SB/runtime-libs"
for f in "$SB"/llvm23/lib/libc++.so.1* "$SB"/llvm23/lib/libc++abi.so.1* "$SB"/llvm23/lib/libunwind.so.1*; do
  [ -e "$f" ] && ln -sf "$f" "$SB/runtime-libs/"
done

if [ ! -f "$SB/sysroot/usr/lib/x86_64-linux-gnu/libvulkan_lvp.so" ]; then
  APT=(-o "Dir::State::Lists=$SB/apt/lists" -o "Dir::Cache=$SB/apt/cache" -o Debug::NoLocking=1 -o APT::Sandbox::User=root)
  apt-get "${APT[@]}" update || true
  # lavapipe, plus the X11 dev packages SDL3 wants (the image has the runtime libs but not the headers,
  # and xmake-repo would otherwise try to build them from x.org tarballs, see tools/xmake-repo)
  (cd "$SB/debs" && apt-get "${APT[@]}" download mesa-vulkan-drivers libllvm20 \
    libxi-dev libxi6 libxrandr-dev libxrandr2 libxcursor-dev libxcursor1 libxfixes-dev libxfixes3 libxss-dev libxss1)
  for d in "$SB"/debs/*.deb; do dpkg -x "$d" "$SB/sysroot"; done
fi

# vulkan validation layers + spirv tools, for `tools/run_headless.sh --validation`
if [ ! -f "$SB/sysroot-vvl/usr/lib/x86_64-linux-gnu/libVkLayer_khronos_validation.so" ]; then
  APT=(-o "Dir::State::Lists=$SB/apt/lists" -o "Dir::Cache=$SB/apt/cache" -o Debug::NoLocking=1 -o APT::Sandbox::User=root)
  mkdir -p "$SB/debs-vvl" "$SB/sysroot-vvl"
  (cd "$SB/debs-vvl" && apt-get "${APT[@]}" download vulkan-validationlayers spirv-tools)
  for d in "$SB"/debs-vvl/*.deb; do dpkg -x "$d" "$SB/sysroot-vvl"; done
fi

cat > "$SB/lvp_icd.json" <<JSON
{ "ICD": { "api_version": "1.4.318", "library_path": "$SB/sysroot/usr/lib/x86_64-linux-gnu/libvulkan_lvp.so" },
  "file_format_version": "1.0.1" }
JSON
# package overrides for the blocked-download cases (vulkan-headers git tags, sol2 lua download, X11 dev libs)
XMAKE_ROOT=y XMAKE_GLOBALDIR="$SB/xmake-global" "$SB/xmake/bin/xmake" repo -a -g oxcity-overrides "$ROOT/tools/xmake-repo" || true
echo "sandbox ready, run: . ./env.sh"
