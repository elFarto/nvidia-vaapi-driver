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

if [ ! -s "$driver_so" ]; then
    echo "driver artifact is empty: $driver_so" >&2
    exit 1
fi

echo "driver artifact exists: $driver_so"
