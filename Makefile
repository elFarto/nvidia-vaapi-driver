SOURCES := $(wildcard *.c)
OBJECTS := $(patsubst %.c, %.o, $(SOURCES))

DRIVER_NAME=nvidia_drv_video.so

SDK := $(shell realpath ~/Downloads/Video_Codec_SDK_11.0.10)
PKGS := gstreamer-codecparsers-1.0 cuda

CFLAGS := $(shell pkg-config --cflags ${PKGS}) -I$(SDK)/Interface -I/usr/include/drm
LDFLAGS := $(shell pkg-config --libs ${PKGS}) -lnvcuvid -lEGL

%.o: %.c
	gcc -ggdb -c ${CFLAGS} -fPIC  -o $@ $<

all: $(OBJECTS)
	gcc -ggdb -shared -fPIC -Wl,-soname,lib${DRIVER_NAME}.1 -o ${DRIVER_NAME} $^ ${LDFLAGS}

clean:
	rm -f $(OBJECTS) ${DRIVER_NAME}
