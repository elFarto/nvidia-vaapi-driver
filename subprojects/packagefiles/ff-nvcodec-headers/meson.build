project('ff-nvcodec-headers', 'c', version: '13.0.19.0')

install_subdir('include/ffnvcodec', install_dir: get_option('includedir'))

incdir = include_directories('include')
ffnvcodec_dep = declare_dependency(
    include_directories: incdir,
    version: meson.project_version(),
)

pkg = import('pkgconfig')
pkg.generate(
    name: 'ffnvcodec',
    description: 'FFmpeg version of Nvidia Codec SDK headers',
    version: meson.project_version(),
)
