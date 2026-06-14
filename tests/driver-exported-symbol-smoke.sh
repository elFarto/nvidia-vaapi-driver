#!/bin/sh
set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <driver-so-path>" >&2
    exit 2
fi

driver_so="$1"

if [ ! -f "$driver_so" ]; then
    echo "missing driver artifact: $driver_so" >&2
    exit 1
fi

if command -v readelf >/dev/null 2>&1; then
    if ! readelf -Ws "$driver_so" | grep -q "__vaDriverInit_1_0"; then
        echo "required exported symbol missing: __vaDriverInit_1_0" >&2
        exit 1
    fi
elif command -v nm >/dev/null 2>&1; then
    if ! nm -D "$driver_so" | grep -q "__vaDriverInit_1_0"; then
        echo "required exported symbol missing: __vaDriverInit_1_0" >&2
        exit 1
    fi
else
    echo "readelf/nm not found; cannot inspect exported symbols" >&2
    exit 1
fi

echo "required symbol exported: __vaDriverInit_1_0"
