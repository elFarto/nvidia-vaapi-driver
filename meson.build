project(
    'nvidia-vaapi-driver',
    'c',
    version: '0.1',
    default_options: [
        'prefix=/usr',
        'c_std=c11',
        'warning_level=2',
    ],
    license: 'MIT',
    meson_version: '>= 0.58.0',
)

cc = meson.get_compiler('c')

deps = [
    cc.find_library('m'),
    cc.find_library('dl', required : false),
    dependency('egl'),
    dependency('ffnvcodec', version: '>= 11.1.5.1'),
    dependency('libdrm', version: '>=2.4.60').partial_dependency(compile_args: true),
    dependency('threads'),
]
libva_deps = dependency('libva', version: '>= 1.8.0').partial_dependency(compile_args: true)
deps += [libva_deps]

gst_codecs_deps = dependency('gstreamer-codecparsers-1.0', required: false)

if cc.get_argument_syntax() == 'gcc'
    add_project_arguments(
        cc.get_supported_arguments([
            '-Wno-missing-field-initializers',
            '-Wno-unused-parameter',
            '-Werror=format',
            '-Werror=format-security',
            '-Werror=incompatible-pointer-types',
            '-Werror=init-self',
            '-Werror=int-conversion',
            '-Werror=missing-declarations',
            '-Werror=missing-prototypes',
            '-Werror=pointer-arith',
            '-Werror=undef',
            '-Werror=vla',
            '-Wsuggest-attribute=format',
            '-Wwrite-strings',
        ]),
        language: ['c'],
    )
endif

sources = [
    'src/av1.c',
    'src/backend-common.c',
    'src/export-buf.c',
    'src/direct/direct-export-buf.c',
    'src/direct/nv-driver.c',
    'src/h264.c',
    'src/h264_encode.c',
    'src/hevc.c',
    'src/hevc_encode.c',
    'src/jpeg.c',
    'src/mpeg2.c',
    'src/mpeg4.c',
    'src/nvenc.c',
    'src/nvenc-ipc-client.c',
    'src/vabackend.c',
    'src/vc1.c',
    'src/vp8.c',
    'src/list.c',
]

if gst_codecs_deps.found()
    sources += ['src/vp9.c']
    deps += [gst_codecs_deps]
endif

nvidia_incdir = include_directories('nvidia-include')
nvidia_install_dir = libva_deps.get_variable(pkgconfig: 'driverdir')

shared_library(
    'nvidia_drv_video',
    name_prefix: '',
    sources: sources,
    dependencies: deps,
    include_directories: nvidia_incdir,
    install: true,
    install_dir: nvidia_install_dir,
    gnu_symbol_visibility: 'hidden',
)

# Build the 64-bit NVENC helper daemon (only for native builds, not cross-compiled i386)
if host_machine.cpu_family() == 'x86_64' or host_machine.cpu_family() == 'aarch64'
    helper_deps = [
        cc.find_library('dl', required : false),
        dependency('ffnvcodec', version: '>= 11.1.5.1'),
        dependency('threads'),
    ]
    executable(
        'nvenc-helper',
        'src/nvenc-helper.c',
        'src/nvenc-ipc-client.c',   # for nvenc_ipc_get_socket_path
        dependencies: helper_deps,
        install: true,
        install_dir: get_option('libexecdir'),
    )
endif

# Tests (native builds only, not cross-compiled)
if not meson.is_cross_build()
    libva_test_deps = [
        dependency('libva'),
        dependency('libva-drm'),
        cc.find_library('m', required : false),
    ]

    test_encode = executable('test_encode', 'tests/test_encode.c',
        dependencies : libva_test_deps, install : false)
    test('encode', test_encode, timeout : 60)

    test_encode_config = executable('test_encode_config', 'tests/test_encode_config.c',
        dependencies : libva_test_deps, install : false)
    test('encode_config', test_encode_config, timeout : 60)

    gst_launch = find_program('gst-launch-1.0', required : false)
    if gst_launch.found()
        test('gstreamer', find_program('tests/test_gstreamer.sh'),
             timeout : 120,
             env : ['GST_VAAPI_ALL_DRIVERS=1', 'LIBVA_DRIVER_NAME=nvidia'])
    endif
endif

meson.add_devenv(environment({
    'NVD_LOG': '1',
    'LIBVA_DRIVER_NAME': 'nvidia',
    'LIBVA_DRIVERS_PATH': meson.project_build_root(),
}))
