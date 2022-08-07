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
dl_dep = cc.find_library('dl', required : false)
egl_dep = cc.find_library('EGL')
gst_codecs_deps = dependency('gstreamer-codecparsers-1.0')
ffnvcodec_deps = dependency('ffnvcodec', version: '>= 11.1.5.1')
libva_deps = dependency('libva', version: '>= 1.8.0')
thread_dep = dependency('threads')
x11_dep = cc.find_library('X11')
xcb_dep = dependency('xcb')

nvidia_incdir = include_directories('nvidia-include')

if get_option('build_shader')
    nvcc_dep = find_program('nvcc', required: true)
    cudaShaderPtx = custom_target('nvcc-ptx',
        output : 'YUVtoRGB.ptx',
        input : 'src/YUVtoRGB.cu',
        command : [ nvcc_dep, '-allow-unsupported-compiler', '--ptx', '-o', '@OUTPUT@', '@INPUT@' ],
    )
else
    #if we're not building the shader, just use the prebuilt version
    #we have to copy to the same place nvcc would output it's version to as ld uses the
    #input file name as part of the symbol name, so they need to match
    cudaShaderPtx = custom_target('nvcc-ptx',
        output : 'YUVtoRGB.ptx',
        input : 'src/YUVtoRGB.ptx',
        command : [ 'cp', '@INPUT@', '@OUTPUT@' ],
    )
endif

ld = find_program('ld')
cudaShader = custom_target('objectify-data',
    output : 'YUVtoRGB.ptx.o',
    input : cudaShaderPtx,
    command : [ ld, '--relocatable', '--format', 'binary', '--output', '@OUTPUT@', '@INPUT@' ],
)

shared_library(
    'nvidia_drv_video',
    name_prefix: '',
    sources: [
        'src/av1.c',
        'src/export-buf.c',
        'src/direct/direct-export-buf.c',
        'src/direct/nv-driver.c',
        'src/h264.c',
        'src/hevc.c',
        'src/jpeg.c',
        'src/mpeg2.c',
        'src/mpeg4.c',
        'src/vabackend.c',
        'src/vc1.c',
        'src/vp8.c',
        'src/vp9.c',
        'src/list.c',
        'src/vpp.c',
        'src/x11.c',
        cudaShader
    ],
    dependencies: [
        libva_deps,
        ffnvcodec_deps,
        gst_codecs_deps,
        egl_dep,
        m_dep,
        dl_dep,
        thread_dep,
        x11_dep,
        xcb_dep
    ],
    include_directories: nvidia_incdir,
    install: true,
    install_dir: get_option('libdir') / 'dri',
    gnu_symbol_visibility: 'hidden',
)

meson.add_devenv(environment({
    'NVD_LOG': '1',
    'LIBVA_DRIVER_NAME': 'nvidia',
    'LIBVA_DRIVERS_PATH': meson.project_build_root(),
}))
