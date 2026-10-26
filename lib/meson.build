# SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only
# Meson build file for libpsu — for hashboard firmware integration.

project(
  'libpsu',
  'c',
  version: '0.1.0',
  license: 'Apache-2.0 OR GPL-2.0-only',
  default_options: ['c_std=c11', 'warning_level=2'],
)

libpsu_inc = include_directories('include')

libpsu = static_library(
  'psu',
  'src/pmbus_conversion.c',
  include_directories: libpsu_inc,
)

libpsu_dep = declare_dependency(
  include_directories: libpsu_inc,
  link_with: libpsu,
)

subdir('tests')
