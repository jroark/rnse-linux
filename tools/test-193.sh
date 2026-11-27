#!/bin/sh
# Run from anywhere. RNSE_DISC must name a stock disc containing a 193 variant.
set -eu
: "${RNSE_DISC:?set RNSE_DISC to a stock firmware ISO}"
case $RNSE_DISC in /*) ;; *) RNSE_DISC=$PWD/$RNSE_DISC ;; esac
cd "$(dirname "$0")/.."
: "${RNSE_LINUX_193:=$PWD/out/193/VMLINUX.BIN}"
: "${RNSE_SDK:=$PWD/../rnse-sdk}"
if ! command -v "${CROSS:-sh4-linux-gnu-}gcc" >/dev/null; then
    PATH="$RNSE_SDK/tools/sh4-docker:$PATH"
fi
# A sibling emulator checkout can supply a local development pin. Cargo
# still verifies the revision in Cargo.lock; nothing is fetched or pushed
# to that checkout.
emu=${RNSE_EMU:-$PWD/../rnse}
pin=$(sed -n 's/.*rev = "\([0-9a-f]*\)".*/\1/p' Cargo.toml)
if [ -n "$pin" ] && git -C "$emu" cat-file -e "$pin^{commit}" 2>/dev/null; then
    emu=$(cd "$emu" && pwd)
    export CARGO_NET_GIT_FETCH_WITH_CLI=true
    export GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0="url.file://$emu.insteadOf"
    export GIT_CONFIG_VALUE_0=https://github.com/jroark/rnse
fi
export PATH RNSE_SDK RNSE_DISC RNSE_LINUX_193
RNSE_REQUIRE_193=1 cargo test --locked --release --test disc linux_boots_on_the_193 -- --exact --nocapture
