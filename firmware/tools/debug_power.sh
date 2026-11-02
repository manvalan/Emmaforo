#!/bin/bash
# Power debug firmware: separate build folder and config, never published.
#   tools/debug_power.sh build      compile (version 1.2-debug)
#   tools/debug_power.sh flash      compile and flash over USB
#   tools/debug_power.sh monitor    live log; the rows start with PWR,
#   tools/debug_power.sh read [out.csv]   copy the flash log from the board to CSV
#   tools/debug_power.sh clear      erase the flash log
set -euo pipefail
cd "$(dirname "$0")/.."

# The project builds with the IDF in $IDF_PATH and its own Python environment.
if [ -z "${IDF_PYTHON_ENV_PATH:-}" ]; then
    . "${IDF_PATH:-$HOME/esp/esp-idf}/export.sh" > /dev/null
fi

idf() {
    idf.py -B build-debug-power -D SDKCONFIG=build-debug-power/sdkconfig \
        -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.debug_power" "$@"
}

port() {
    local found
    found=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1 || true)
    if [ -z "$found" ]; then
        echo "No board on USB (/dev/cu.usbmodem*)." >&2
        exit 1
    fi
    echo "$found"
}

parttool() {
    python "$IDF_PATH/components/partition_table/parttool.py" --port "$(port)" "$@"
}

case "${1:-}" in
    build) idf build ;;
    flash) idf -p "$(port)" flash ;;
    monitor) idf -p "$(port)" monitor ;;
    read)
        out="${2:-power_log.csv}"
        parttool read_partition --partition-name powerlog --output build-debug-power/powerlog.bin
        python3 tools/power_log.py build-debug-power/powerlog.bin "$out"
        echo "Log in $out"
        ;;
    clear) parttool erase_partition --partition-name powerlog ;;
    *) sed -n 2,7p "$0"; exit 2 ;;
esac
