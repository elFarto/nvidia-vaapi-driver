project('nvidia-vaapi-driver', 'c', version : '0.1', default_options : ['prefix=/usr', 'c_std=c11', 'warning_level=0'])

cc = meson.get_compiler('c')

m_dep = cc.find_library('m')
egl_dep = cc.find_library('EGL')
cuda_lib = cc.find_library('cuda')

if cuda_lib.found() and cc.has_function('cuInit', dependencies: cuda_lib)
    fs = import('fs')
    if fs.is_dir('/opt/cuda/include')
        cuda_dep = declare_dependency(dependencies: cuda_lib, include_directories: [ '/opt/cuda/include' ])
    else
        cuda_dep = declare_dependency(dependencies: cuda_lib, include_directories: [ '/usr/include/cuda' ])
    endif
endif

cuvid_lib = cc.find_library('nvcuvid')

if cuvid_lib.found() and cc.has_function('cuvidCtxLock', dependencies: cuvid_lib)
    cuvid_dep = declare_dependency(dependencies: cuvid_lib, include_directories: [])
endif

gst_codecs_deps = dependency('gstreamer-codecparsers-1.0')

shared_library('nvidia_drv_video', name_prefix: '', sources: [
           'src/mpeg4.c',
           'src/vp9.c',
           'src/vp8.c',
           'src/mpeg2.c',
           'src/h264.c',
           'src/export-buf.c',
           'src/hevc.c',
           'src/av1.c',
           'src/jpeg.c',
           'src/vc1.c',
           'src/vabackend.c'],
           dependencies: [ cuda_dep, cuvid_dep, gst_codecs_deps, egl_dep, m_dep ],
           install: true,
           install_dir: get_option('libdir') / 'dri',
           gnu_symbol_visibility: 'hidden',
           )
