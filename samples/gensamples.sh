#!/bin/sh

ffmpeg -f lavfi -i smptebars=duration=10:size=640x360:rate=30 smptebars_h264.mp4
ffmpeg -f lavfi -i smptebars=duration=10:size=640x360:rate=30 -c:v libx265 -crf 26 -preset fast smptebars_hevc_8bit.mp4
ffmpeg -f lavfi -i smptebars=duration=10:size=640x360:rate=30 -c:v libx265 -crf 26 -preset fast -pix_fmt yuv420p10le smptebars_hevc_10bit.mp4
ffmpeg -f lavfi -i smptebars=duration=10:size=640x360:rate=30 -c:v libx265 -crf 26 -preset fast -pix_fmt yuv420p12le smptebars_hevc_12bit.mp4
ffmpeg -f lavfi -i smptebars=duration=10:size=640x360:rate=30 -c:v libx265 -crf 26 -preset fast -pix_fmt yuv422 smptebars_hevc_422_8bit.mp4
ffmpeg -f lavfi -i smptebars=duration=10:size=640x360:rate=30 -c:v libx265 -crf 26 -preset fast -pix_fmt yuv422p10le smptebars_hevc_422_10bit.mp4
ffmpeg -f lavfi -i smptebars=duration=10:size=640x360:rate=30 -c:v libx265 -crf 26 -preset fast -pix_fmt yuv422p12le smptebars_hevc_422_12bit.mp4
ffmpeg -f lavfi -i smptebars=duration=10:size=640x360:rate=30 -c:v mpeg4 smptebars_mpeg4.mp4
ffmpeg -f lavfi -i smptebars=duration=10:size=640x360:rate=30 -c:v vp9 smptebars_vp9.mp4
#this one didn't work on my ffmpeg, complained about a mismatched ABI
#ffmpeg -f lavfi -i smptebars=duration=10:size=640x360:rate=30 -c:v av1 smptebars_av1.mp4
ffmpeg -f lavfi -i smptebars=duration=10:size=640x360:rate=30 -c:v libsvtav1 smptebars_av1.mp4

#TODO need mpeg2, vc1, vp8, perhaps h264 with different levels
