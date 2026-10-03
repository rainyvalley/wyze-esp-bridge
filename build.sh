#!/bin/sh
# Builds every board variant into out/. Run inside espressif/idf:release-v5.5:
#   docker run --rm -v "$PWD":/project -w /project espressif/idf:release-v5.5 ./build.sh
set -e
mkdir -p out
build() {  # name target extra-defaults
    defaults="sdkconfig.defaults${3:+;$3}"
    idf.py -B "build-$1" -D SDKCONFIG="build-$1/sdkconfig" -D SDKCONFIG_DEFAULTS="$defaults" set-target "$2" build
    (cd "build-$1" && esptool.py --chip "$2" merge_bin -o "../out/wyze-esp-bridge-$1-merged.bin" @flash_args)
    cp "build-$1/wyze-esp-bridge.bin" "out/wyze-esp-bridge-$1-ota.bin"
}
build p4 esp32p4
build p4-rev1 esp32p4 sdkconfig.defaults.p4-rev1
build s3-eth esp32s3
