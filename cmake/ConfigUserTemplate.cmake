#
# Copyright (c) 2024-2026 by the CRESCENT cyberinfrastructure team (https://cascadiaquakes.org/)
# See LICENSE for copying and redistribution conditions.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU Lesser General Public License as published by
# the Free Software Foundation; version 3 or any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Lesser General Public License for more details.
#
# Contact info: abioyeajala@gmail.com (Rasheed Ajala)
#-------------------------------------------------------------------------------
#
# Copy this file to cmake/ConfigUser.cmake and edit that file.
# Do not edit this template directly.
#
# Settings in cmake/ConfigUser.cmake override the defaults in
# cmake/ConfigDefault.cmake. ConfigUser.cmake should not be tracked by Git.

# 1. GQ installation prefix [CMake default, usually /usr/local]:
# This controls GQ-owned files such as the examples, documentation, and
# uninstall helper. The plug-in destination is configured separately below.
# set (CMAKE_INSTALL_PREFIX "/usr/local")

# 2. GMT plug-in installation directory [reported by GMT]:
# set (GQ_INSTALL_PLUGINDIR "/path/to/gmt/lib/gmt/plugins")
#
# An absolute path installs directly into that directory. Packaging systems
# may use a relative path together with CMAKE_INSTALL_PREFIX and DESTDIR.

# 3. Build type for single-configuration generators [Release]:
# set (CMAKE_BUILD_TYPE Release)

# 4. Dependency locations when they are not found automatically [auto]:
# set (GMT_ROOT "/path/to/gmt")
# set (NETCDF_ROOT "/path/to/netcdf")
# set (BLEND_ROOT "/path/to/blend")

# 5. Integration tests [OFF]:
# set (GQ_BUILD_TESTS ON)

# 6. Helper programs used by the examples [OFF]:
# The example scripts and figures remain available in doc/examples regardless
# of this setting. Enabling it compiles required C helper programs but does not
# run the examples or download their datasets.
# set (GQ_BUILD_EXAMPLES ON)

# 7. Sphinx documentation targets [OFF]:
# The docs target builds HTML and man pages. The large PDF remains opt-in via
# the separate docs_pdf target.
# set (GQ_BUILD_DOCS ON)

# 8. Documentation tools when they are not found automatically [auto]:
# set (Python3_EXECUTABLE "/path/to/python")
# set (SPHINX_BUILD_EXECUTABLE "/path/to/sphinx-build")
# set (LATEXMK_EXECUTABLE "/path/to/latexmk")
