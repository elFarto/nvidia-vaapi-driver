project(
    'nvidia-vaapi-driver',
    'c',
    version: '0.1',
    default_options: [
        'prefix=/usr',
        'c_std=c11',
        'warning_level=0',
    ],
    license: 'MIT',
    meson_version: '>= 0.58.0',
)

cc = meson.get_compiler('c')

m_dep = cc.find_library('m')
egl_dep = cc.find_library('EGL')
gst_codecs_deps = dependency('gstreamer-codecparsers-1.0')

cuda_lib = cc.find_library('cuda')
if not cc.has_function('cuInit', dependencies: cuda_lib)
    error('cuInit() function not found')
endif

fs = import('fs')
if fs.is_dir('/opt/cuda/include')
    cuda_dep = declare_dependency(dependencies: cuda_lib, include_directories: [ '/opt/cuda/include' ])
else
    cuda_dep = declare_dependency(dependencies: cuda_lib, include_directories: [ '/usr/include/cuda' ])
endif

cuvid_lib = cc.find_library('nvcuvid')
cuvid_dep = declare_dependency(dependencies: cuvid_lib, include_directories: [])
if not cc.has_function('cuvidCtxLock', dependencies: cuvid_dep)
    error('cuvidCtxLock() function not found')
endif

shared_library(
    'nvidia_drv_video',
    name_prefix: '',
    sources: [
        'src/av1.c',
        'src/export-buf.c',
        'src/h264.c',
        'src/hevc.c',
        'src/jpeg.c',
        'src/mpeg2.c',
        'src/mpeg4.c',
        'src/vabackend.c',
        'src/vc1.c',
        'src/vp8.c',
        'src/vp9.c',
    ],
    dependencies: [
        cuda_dep,
        cuvid_dep,
        gst_codecs_deps,
        egl_dep,
        m_dep,
    ],
    install: true,
    install_dir: get_option('libdir') / 'dri',
    gnu_symbol_visibility: 'hidden',
)
