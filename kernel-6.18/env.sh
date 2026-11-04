# env.sh: where the 6.18 work lives, sourced by the scripts here. Everything big stays in WSL's own
# file system, not in the repo; set these from outside to put it elsewhere:
#   NB    the build area (default ~/xiaomi13-k618-build of the user the script runs as -- also under
#         `systemd-run --uid=`, whose HOME is still root's)
#   K618  the 6.18 tree: ws/ (Google's kernel workspace), out-gki/, soc/, src/fuxi/, boot-test/
K618_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
NB=${NB:-$(getent passwd "$(id -un)" | cut -d: -f6)/xiaomi13-k618-build}
K618=${K618:-$NB/k618}
W=$K618/ws
O=$K618/out-gki
S=$K618/soc
R=$W/prebuilts/rust-toolchain/linux-x86/1.91.1.p3
CLANG=$W/prebuilts/clang/host/linux-x86/clang-r584948c/bin
TOOLS=$W/prebuilts/kernel-build-tools/linux_musl-x86/bin
