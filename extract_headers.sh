#!/bin/bash

if [ "x$1" = "x" ]; then
    echo "Usage: $(basename $0) <path of NVIDIA open-gpu-kernel-modules project>"
    exit 1
fi

OUTPUT_DIR=nvidia-include
DIR=$1
NEWLINE=$'\n'

DIRS=""
INC_DIRS=""
INC_FILES="#ifndef NVIDIA_H_${NEWLINE}#define NVIDIA_H_${NEWLINE}"

while IFS= read -r line; do
    #echo got line: $line
    if [[ $line =~ ^d\ .+ ]]; then
	DIRS="$DIRS$DIR/${line//d /}${NEWLINE}"
        INC_DIRS="$INC_DIRS -I$DIR/${line//d /}"
    elif [[ $line =~ ^f\ .+ ]]; then
        INC_FILES="${INC_FILES}#include <${line//f /}>${NEWLINE}"
    elif [[ $line =~ ^#.+ ]]; then
        INC_FILES="${INC_FILES}${line}${NEWLINE}"
    fi
done < headers.in

INC_FILES="${INC_FILES}#endif${NEWLINE}"

#echo got dirs: "$DIRS"
#echo got inc dirs: $INC_DIRS
#echo got inc files: "$INC_FILES"

mkdir -p ${OUTPUT_DIR}

echo "${INC_FILES}" > ${OUTPUT_DIR}/nvidia.h

INCLUDED_FILES=$(cpp $INC_DIRS -H ${OUTPUT_DIR}/nvidia.h 2>&1 1>/dev/null | grep -E '^\.' | grep -Eo "$DIR.*")

for f in $INCLUDED_FILES; do
    for d in $DIRS; do
        if [[ "$f" == "$d"* ]]; then
           dest_file=${OUTPUT_DIR}/${f//$d/}
           mkdir -p $(dirname ${dest_file})
           echo Copying ${dest_file}
           cp $f ${dest_file}
        fi
    done
done

#fixup nvidia-drm-ioctl.h as it includes a kernel header, and we'd like it to be able to choose
mv ${OUTPUT_DIR}/nvidia-drm-ioctl.h ${OUTPUT_DIR}/nvidia-drm-ioctl.h.bak
sed 's$#include <drm/drm.h>$#if defined __has_include \&\& __has_include(<libdrm/drm.h>)\n#  include <libdrm/drm.h>\n#else\n#  include <drm/drm.h>\n#endif$' < ${OUTPUT_DIR}/nvidia-drm-ioctl.h.bak > ${OUTPUT_DIR}/nvidia-drm-ioctl.h

