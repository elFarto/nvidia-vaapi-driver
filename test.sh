#!/bin/sh

export MPV=mpv

export LD_LIBRARY_PATH=/opt/ffmpeg/lib/

#make sure we use our driver
export LIBVA_DRIVER_NAME=nvidia
export LIBVA_DRIVERS_PATH=$(dirname $(realpath $0))/build

#enable our logging
export NVD_LOG=1

#--hwdec=vaapi-copy is needed as NVDEC/Cuda has no support for DMA-BUF currently, so buffers need to be copied back to the CPU
#--hwdec-codecs=all FFMpeg will only use hardware decoding for specific codecs, this overrides that
#--vd-lavc-check-hw-profile=no FFMpeg will only use hardware decoding for specific profiles of some codec, this overrides that

#mpv -v --hwdec=vaapi-copy --hwdec-codecs=all --vd-lavc-check-hw-profile=no $@
#mpv -v -v --msg-level=all=debug --hwdec=vaapi --hwdec-codecs=all --vd-lavc-check-hw-profile=no $@
#apitrace trace -v -a egl 
$MPV -v --msg-level=all=debug --hwdec=vaapi --gpu-debug --hwdec-codecs=all --vd-lavc-check-hw-profile=no "$@"
