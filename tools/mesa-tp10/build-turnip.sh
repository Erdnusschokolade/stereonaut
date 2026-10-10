#!/usr/bin/env bash
# Cross-builds only Turnip (Mesa's Adreno Vulkan driver) for the Steam Frame
# from third_party/mesa, against a sysroot copied from the Frame:
#   rsync -a frame:/usr/include SYSROOT/usr/
#   rsync -a --include='*.so' --include='*.so.*' --include='*.o' \
#     --include='*.a' --include='pkgconfig/***' --include='gcc/***' \
#     --exclude='*' frame:/usr/lib/ SYSROOT/usr/lib/
# The result is loaded only by our own processes via VK_ICD_FILENAMES; the
# system Mesa stays untouched.
set -euo pipefail

repo=$(cd -- "$(dirname -- "$0")/../.." && pwd)
cache=${XDG_CACHE_HOME:-$HOME/.cache}/stereonaut
sysroot=${SYSROOT:-$cache/sysroot-frame}
build=${BUILD_DIR:-$cache/mesa-build-aarch64}
venv=${VENV:-$cache/venv}   # python with mako, pyyaml, packaging
source_dir=$repo/third_party/mesa
target=aarch64-unknown-linux-gnu

[[ -d $sysroot/usr/include ]] || { echo "sysroot missing: $sysroot" >&2; exit 1; }
[[ -d $source_dir ]] || { echo "mesa sources missing: $source_dir" >&2; exit 1; }

mkdir -p "$build"
cross=$build/cross-aarch64.ini
cat >"$cross" <<CROSS
[binaries]
c = 'clang'
cpp = 'clang++'
ar = 'llvm-ar'
strip = 'llvm-strip'
pkg-config = 'pkg-config'

[built-in options]
c_args = ['--target=$target', '--sysroot=$sysroot']
cpp_args = ['--target=$target', '--sysroot=$sysroot']
c_link_args = ['--target=$target', '--sysroot=$sysroot', '-fuse-ld=lld']
cpp_link_args = ['--target=$target', '--sysroot=$sysroot', '-fuse-ld=lld']

[properties]
sys_root = '$sysroot'
pkg_config_libdir = ['$sysroot/usr/lib/pkgconfig', '$sysroot/usr/share/pkgconfig']

[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
CROSS

export PATH="$venv/bin:$PATH"
if [[ ! -f $build/build.ninja ]]; then
    meson setup "$build" "$source_dir" --cross-file "$cross" \
        -Dbuildtype=release -Db_ndebug=true \
        -Dvulkan-drivers=freedreno -Dfreedreno-kmds=msm \
        -Dgallium-drivers= -Dplatforms= -Dopengl=false \
        -Dglx=disabled -Degl=disabled -Dgles1=disabled -Dgles2=disabled \
        -Dllvm=disabled -Dvalgrind=disabled -Dlibunwind=disabled \
        -Dvulkan-layers= -Dtools= -Dbuild-tests=false
fi
ninja -C "$build" src/freedreno/vulkan/libvulkan_freedreno.so
echo "built: $build/src/freedreno/vulkan/libvulkan_freedreno.so"
