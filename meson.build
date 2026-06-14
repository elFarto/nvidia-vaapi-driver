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
    dependency('ffnvcodec', version: '>= 13.0.19.0'),
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
    'src/hevc.c',
    'src/jpeg.c',
    'src/mpeg2.c',
    'src/mpeg4.c',
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

nvidia_drv_video = shared_library(
    'nvidia_drv_video',
    name_prefix: '',
    sources: sources,
    dependencies: deps,
    include_directories: nvidia_incdir,
    install: true,
    install_dir: nvidia_install_dir,
    gnu_symbol_visibility: 'hidden',
)

meson.add_devenv(environment({
    'NVD_LOG': '1',
    'LIBVA_DRIVER_NAME': 'nvidia',
    'LIBVA_DRIVERS_PATH': meson.project_build_root(),
}))

sh_prog = find_program('sh')
nvidia_drv_video_path = join_paths(meson.project_build_root(), 'nvidia_drv_video.so')

test(
    'driver-artifact-smoke',
    sh_prog,
    args: [
        files('tests/driver-artifact-smoke.sh'),
        nvidia_drv_video_path,
    ],
)

test(
    'driver-exported-symbol-smoke',
    sh_prog,
    args: [
        files('tests/driver-exported-symbol-smoke.sh'),
        nvidia_drv_video_path,
    ],
)

if get_option('runtime_tests')
    test(
        'driver-runtime-vaapi-smoke',
        sh_prog,
        args: [files('tests/driver-runtime-vaapi-smoke.sh')],
        env: environment({
            'LIBVA_DRIVER_NAME': 'nvidia',
            'LIBVA_DRIVERS_PATH': meson.project_build_root(),
        }),
        suite: ['runtime'],
        timeout: 300,
    )
endif
