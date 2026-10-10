#!/bin/sh
# Builds board variants into out/. Run inside espressif/idf:release-v5.5:
#   docker run --rm -v "$PWD":/project -w /project espressif/idf:release-v5.5 ./build.sh
# With arguments, builds exactly one variant: ./build.sh p4 esp32p4 [extra-defaults]
# (same positional args as build() below). No arguments = the previous behavior:
# all three board variants. The variants run sequentially, not in parallel, because
# idf.py does not like two builds sharing a project directory.
set -e
mkdir -p out
build() {  # name target extra-defaults
    defaults="sdkconfig.defaults${3:+;$3}"
    # Site-specific values (gateway URI/token, WiFi creds) stay out of git:
    # sdkconfig.local.defaults is appended when present (see .gitignore).
    if [ -f sdkconfig.local.defaults ]; then
        defaults="$defaults;sdkconfig.local.defaults"
    fi
    idf.py -B "build-$1" -D SDKCONFIG="build-$1/sdkconfig" -D SDKCONFIG_DEFAULTS="$defaults" set-target "$2" build
    (cd "build-$1" && esptool.py --chip "$2" merge_bin -o "../out/wyze-esp-bridge-$1-merged.bin" @flash_args)
    cp "build-$1/wyze-esp-bridge.bin" "out/wyze-esp-bridge-$1-ota.bin"
}
case $# in
0) build p4 esp32p4
   build p4-rev1 esp32p4 sdkconfig.defaults.p4-rev1
   build s3-eth esp32s3
   ;;
1) echo "usage: build.sh [name target [extra-defaults]]" >&2
   exit 2
   ;;
*) build "$@"
   ;;
esac
