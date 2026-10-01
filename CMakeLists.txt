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
# To modify the cmake process: Edit your cmake/ConfigUser.cmake file
#
# To build out-of-source do (example):
#
#	mkdir build
#	cd build
#	cmake ..
#
# CMAKE_BUILD_TYPE can be: empty, Debug, Release, RelWithDebInfo or MinSizeRel
#
# Copy cmake/ConfigUserTemplate.cmake to cmake/ConfigUser.cmake and edit the
# copy to configure local paths and optional build components.
#

cmake_minimum_required (VERSION 3.15)

#------------------------------------------------------------------------
# Make sure the user doesn't play dirty with symlinks
get_filename_component (srcdir "${CMAKE_SOURCE_DIR}" REALPATH)
get_filename_component (bindir "${CMAKE_BINARY_DIR}" REALPATH)

# Disallow in-source builds
if (srcdir STREQUAL bindir)
	message(FATAL_ERROR "In-source builds are not allowed. "
	"Please create a directory and run cmake from there, passing the path "
	"to this source directory as the last argument. This process created "
	"the file `CMakeCache.txt' and the directory `CMakeFiles' in ${srcdir}. "
	"Please remove them.")
endif ()

# Load project metadata and defaults, followed by optional source- and
# build-tree overrides.
include ("${CMAKE_SOURCE_DIR}/cmake/ConfigDefault.cmake")
if (EXISTS "${CMAKE_SOURCE_DIR}/cmake/ConfigUser.cmake")
	include ("${CMAKE_SOURCE_DIR}/cmake/ConfigUser.cmake")
endif ()
if (EXISTS "${CMAKE_BINARY_DIR}/cmake/ConfigUser.cmake")
	include ("${CMAKE_BINARY_DIR}/cmake/ConfigUser.cmake")
endif ()
set (GQ_USER_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

project (${GQ_PACKAGE_NAME}
	VERSION ${GQ_PACKAGE_VERSION}
	DESCRIPTION "${GQ_PACKAGE_DESCRIPTION_SUMMARY}"
	LANGUAGES C)
set (CMAKE_INSTALL_PREFIX "${GQ_USER_INSTALL_PREFIX}" CACHE PATH
	"Install path prefix" FORCE)

# Use NEW behavior with newer CMake releases
foreach (p
		CMP0025 # CMake 3.0: Compiler id for Apple Clang is now AppleClang
		CMP0026 # CMake 3.0: Disallow use of the LOCATION target property
		CMP0058 # CMake 3.3: Ninja requires custom command byproducts to be explicit
		CMP0074	# CMake 3.12: find_package uses PackageName_ROOT variables
		CMP0115 # CMake 3.20: Source file extensions must be explicit.
		)
	if (POLICY ${p})
		cmake_policy (SET ${p} NEW)
	endif()
endforeach()

# Where to find our CMake modules (this variable is visible in subdirectories).
set (CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake/modules/"
	CACHE INTERNAL "Location of our custom CMake modules." FORCE)

include (GNUInstallDirs)

file (MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/src")
configure_file (
	src/gq_version.h.in
	"${CMAKE_CURRENT_BINARY_DIR}/src/gq_version.h"
	@ONLY)

set (GQ_BUILD_TESTS "${GQ_BUILD_TESTS}" CACHE BOOL
	"Build the GQ integration tests" FORCE)
set (GQ_BUILD_EXAMPLES "${GQ_BUILD_EXAMPLES}" CACHE BOOL
	"Build helper programs used by the GQ examples" FORCE)
set (GQ_BUILD_DOCS "${GQ_BUILD_DOCS}" CACHE BOOL
	"Build the GQ documentation targets" FORCE)
set (GQ_INSTALL_DOCDIR "doc" CACHE PATH
	"GQ documentation and examples install directory")
set (CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}" CACHE PATH
	"Install path prefix" FORCE)
if (IS_ABSOLUTE "${GQ_INSTALL_DOCDIR}")
	set (GQ_INSTALL_FULL_DOCDIR "${GQ_INSTALL_DOCDIR}")
else ()
	set (GQ_INSTALL_FULL_DOCDIR
		"${CMAKE_INSTALL_PREFIX}/${GQ_INSTALL_DOCDIR}")
endif ()
if (NOT CMAKE_CONFIGURATION_TYPES)
	set (CMAKE_BUILD_TYPE "${CMAKE_BUILD_TYPE}" CACHE STRING
		"Build type" FORCE)
endif ()

if (GQ_BUILD_TESTS)
	enable_testing ()
endif ()

# Add subdirectories
add_subdirectory (src)
if (GQ_BUILD_DOCS)
	add_subdirectory (doc/rst)
endif ()
set (GMT_CUSTOM_LIB_NAME "gq.so")

configure_file (
	"${CMAKE_SOURCE_DIR}/cmake/Uninstall.cmake.in"
	"${CMAKE_BINARY_DIR}/cmake_uninstall.cmake"
	@ONLY)
add_custom_target (uninstall
	COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_BINARY_DIR}/cmake_uninstall.cmake"
	COMMENT "Uninstalling GQ"
	VERBATIM)

install (FILES LICENSE NOTICE
	DESTINATION "${GQ_INSTALL_DOCDIR}"
	COMPONENT Runtime)

install (FILES ChangeLog
	DESTINATION "${GQ_INSTALL_DOCDIR}"
	OPTIONAL
	COMPONENT Runtime)

install (DIRECTORY doc/examples
	DESTINATION "${GQ_INSTALL_DOCDIR}"
	USE_SOURCE_PERMISSIONS
	COMPONENT Runtime
	FILES_MATCHING
		PATTERN "*.c"
		PATTERN "*.cdl"
		PATTERN "*.md"
		PATTERN "*.png"
		PATTERN "*.sh"
		PATTERN "*.txt")

if (UNIX)
	configure_file (
		"${CMAKE_SOURCE_DIR}/share/tools/gq_uninstall.sh.in"
		"${CMAKE_BINARY_DIR}/gq_uninstall.sh"
		@ONLY)
	install (PROGRAMS "${CMAKE_BINARY_DIR}/gq_uninstall.sh"
		DESTINATION "${CMAKE_INSTALL_DATADIR}/tools"
		COMPONENT Runtime)
endif ()

if (GQ_BUILD_EXAMPLES)
	find_package (NETCDF REQUIRED)
	add_executable (gq_example_combine_cubes doc/examples/elygtl/combine_cubes.c)
	target_include_directories (gq_example_combine_cubes PRIVATE ${NETCDF_INCLUDE_DIR})
	target_link_libraries (gq_example_combine_cubes PRIVATE ${NETCDF_LIBRARY})
	set_target_properties (gq_example_combine_cubes PROPERTIES
		OUTPUT_NAME combine_cubes
		RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples")
endif ()

if (GQ_BUILD_TESTS)
	find_package (GMT REQUIRED)
	find_package (NETCDF REQUIRED)
	add_executable (merge1d_runner test/merge1d_runner.c)
	target_include_directories (merge1d_runner PRIVATE ${GMT_INCLUDE_DIR})
	target_link_libraries (merge1d_runner PRIVATE ${GMT_LIBRARIES} ${CMAKE_DL_LIBS})

	add_executable (merge2d_runner test/merge2d_runner.c)
	target_include_directories (merge2d_runner PRIVATE ${GMT_INCLUDE_DIR})
	target_link_libraries (merge2d_runner PRIVATE ${GMT_LIBRARIES} ${CMAKE_DL_LIBS})

	add_executable (merge3d_runner test/merge3d_runner.c)
	target_include_directories (merge3d_runner PRIVATE ${GMT_INCLUDE_DIR})
	target_link_libraries (merge3d_runner PRIVATE ${GMT_LIBRARIES} ${CMAKE_DL_LIBS})

	add_executable (topobath_runner test/topobath_runner.c)
	target_include_directories (topobath_runner PRIVATE ${GMT_INCLUDE_DIR})
	target_link_libraries (topobath_runner PRIVATE ${GMT_LIBRARIES} ${CMAKE_DL_LIBS})

	add_executable (elygtl_runner test/elygtl_runner.c)
	target_include_directories (elygtl_runner PRIVATE ${GMT_INCLUDE_DIR})
	target_link_libraries (elygtl_runner PRIVATE ${GMT_LIBRARIES} ${CMAKE_DL_LIBS})

	add_executable (ssh_runner test/ssh_runner.c)
	target_include_directories (ssh_runner PRIVATE ${GMT_INCLUDE_DIR})
	target_link_libraries (ssh_runner PRIVATE ${GMT_LIBRARIES} ${CMAKE_DL_LIBS})

	add_executable (check_table test/check_table.c)
	target_link_libraries (check_table PRIVATE m)

	add_executable (check_netcdf1d test/check_netcdf1d.c)
	target_include_directories (check_netcdf1d PRIVATE ${NETCDF_INCLUDE_DIR})
	target_link_libraries (check_netcdf1d PRIVATE ${NETCDF_LIBRARY})

	add_executable (check_netcdf test/check_netcdf.c)
	target_include_directories (check_netcdf PRIVATE ${NETCDF_INCLUDE_DIR})
	target_link_libraries (check_netcdf PRIVATE ${NETCDF_LIBRARY})

	add_executable (check_netcdf3d test/check_netcdf3d.c)
	target_include_directories (check_netcdf3d PRIVATE ${NETCDF_INCLUDE_DIR})
	target_link_libraries (check_netcdf3d PRIVATE ${NETCDF_LIBRARY})
	add_executable (check_netcdf_attribute test/check_netcdf_attribute.c)
	target_include_directories (check_netcdf_attribute PRIVATE ${NETCDF_INCLUDE_DIR})
	target_link_libraries (check_netcdf_attribute PRIVATE ${NETCDF_LIBRARY})
	add_executable (check_netcdf_variable test/check_netcdf_variable.c)
	target_include_directories (check_netcdf_variable PRIVATE ${NETCDF_INCLUDE_DIR})
	target_link_libraries (check_netcdf_variable PRIVATE ${NETCDF_LIBRARY})

	add_executable (check_ssh test/check_ssh.c)
	target_include_directories (check_ssh PRIVATE ${NETCDF_INCLUDE_DIR})
	target_link_libraries (check_ssh PRIVATE ${NETCDF_LIBRARY} m)

	add_executable (check_ssh_pair test/check_ssh_pair.c)
	target_include_directories (check_ssh_pair PRIVATE ${NETCDF_INCLUDE_DIR})
	target_link_libraries (check_ssh_pair PRIVATE ${NETCDF_LIBRARY} m)

	get_filename_component (GMT_BIN_DIR "${GMT_CONFIG}" DIRECTORY)
	find_program (GMT_EXECUTABLE gmt HINTS "${GMT_BIN_DIR}" REQUIRED)
	find_program (NCGEN_EXECUTABLE ncgen HINTS /usr/local/bin /opt/homebrew/bin REQUIRED)

	add_test (
		NAME merge1d.integration
		COMMAND "${CMAKE_COMMAND}"
			-DGQ_RUNNER=$<TARGET_FILE:merge1d_runner>
			-DGQ_PLUGIN=$<TARGET_FILE:gqlib>
			-DGQ_CHECK_TABLE=$<TARGET_FILE:check_table>
			-DGQ_CHECK_NETCDF=$<TARGET_FILE:check_netcdf1d>
			-DGQ_CHECK_ATTRIBUTE=$<TARGET_FILE:check_netcdf_attribute>
			-DNCGEN_EXECUTABLE=${NCGEN_EXECUTABLE}
			-DGQ_SOURCE_DIR=${CMAKE_SOURCE_DIR}
			-DGQ_TEST_DIR=${CMAKE_BINARY_DIR}/test-output-1d
			-P ${CMAKE_SOURCE_DIR}/test/run_merge1d_tests.cmake
	)

	add_test (
		NAME merge2d.integration
		COMMAND "${CMAKE_COMMAND}"
			-DGQ_RUNNER=$<TARGET_FILE:merge2d_runner>
			-DGQ_PLUGIN=$<TARGET_FILE:gqlib>
			-DGQ_CHECK_NETCDF=$<TARGET_FILE:check_netcdf>
			-DGQ_CHECK_COORD=$<TARGET_FILE:check_netcdf3d>
			-DGQ_CHECK_ATTRIBUTE=$<TARGET_FILE:check_netcdf_attribute>
			-DGMT_EXECUTABLE=${GMT_EXECUTABLE}
			-DNCGEN_EXECUTABLE=${NCGEN_EXECUTABLE}
			-DGQ_SOURCE_DIR=${CMAKE_SOURCE_DIR}
			-DGQ_TEST_DIR=${CMAKE_BINARY_DIR}/test-output
			-P ${CMAKE_SOURCE_DIR}/test/run_merge2d_tests.cmake
	)

	add_test (
		NAME merge3d.integration
		COMMAND "${CMAKE_COMMAND}"
			-DGQ_RUNNER=$<TARGET_FILE:merge3d_runner>
			-DGQ_PLUGIN=$<TARGET_FILE:gqlib>
			-DGQ_CHECK_NETCDF=$<TARGET_FILE:check_netcdf3d>
			-DGQ_CHECK_ATTRIBUTE=$<TARGET_FILE:check_netcdf_attribute>
			-DNCGEN_EXECUTABLE=${NCGEN_EXECUTABLE}
			-DGQ_SOURCE_DIR=${CMAKE_SOURCE_DIR}
			-DGQ_TEST_DIR=${CMAKE_BINARY_DIR}/test-output-3d
			-P ${CMAKE_SOURCE_DIR}/test/run_merge3d_tests.cmake
	)

	add_test (
		NAME topobath.integration
		COMMAND "${CMAKE_COMMAND}"
			-DGQ_RUNNER=$<TARGET_FILE:topobath_runner>
			-DGQ_PLUGIN=$<TARGET_FILE:gqlib>
			-DGQ_CHECK_NETCDF=$<TARGET_FILE:check_netcdf3d>
			-DGQ_CHECK_GRID=$<TARGET_FILE:check_netcdf>
			-DGQ_CHECK_ATTRIBUTE=$<TARGET_FILE:check_netcdf_attribute>
			-DNCGEN_EXECUTABLE=${NCGEN_EXECUTABLE}
			-DGQ_SOURCE_DIR=${CMAKE_SOURCE_DIR}
			-DGQ_TEST_DIR=${CMAKE_BINARY_DIR}/test-output-topobath
			-P ${CMAKE_SOURCE_DIR}/test/run_topobath_tests.cmake
	)

	add_test (
		NAME elygtl.integration
		COMMAND "${CMAKE_COMMAND}"
			-DGQ_RUNNER=$<TARGET_FILE:elygtl_runner>
			-DGQ_PLUGIN=$<TARGET_FILE:gqlib>
			-DGQ_CHECK_NETCDF=$<TARGET_FILE:check_netcdf3d>
			-DGQ_CHECK_ATTRIBUTE=$<TARGET_FILE:check_netcdf_attribute>
			-DNCGEN_EXECUTABLE=${NCGEN_EXECUTABLE}
			-DGQ_SOURCE_DIR=${CMAKE_SOURCE_DIR}
			-DGQ_TEST_DIR=${CMAKE_BINARY_DIR}/test-output-elygtl
			-P ${CMAKE_SOURCE_DIR}/test/run_elygtl_tests.cmake
	)

	add_test (
		NAME ssh.integration
		COMMAND "${CMAKE_COMMAND}"
			-DGQ_RUNNER=$<TARGET_FILE:ssh_runner>
			-DGQ_PLUGIN=$<TARGET_FILE:gqlib>
			-DGQ_CHECK_SSH=$<TARGET_FILE:check_ssh>
			-DGQ_CHECK_PAIR=$<TARGET_FILE:check_ssh_pair>
			-DGQ_CHECK_2D=$<TARGET_FILE:check_netcdf>
			-DGQ_CHECK_3D=$<TARGET_FILE:check_netcdf3d>
			-DGQ_CHECK_ATTRIBUTE=$<TARGET_FILE:check_netcdf_attribute>
			-DGQ_CHECK_VARIABLE=$<TARGET_FILE:check_netcdf_variable>
			-DGQ_CHECK_TABLE=$<TARGET_FILE:check_table>
			-DNCGEN_EXECUTABLE=${NCGEN_EXECUTABLE}
			-DGQ_SOURCE_DIR=${CMAKE_SOURCE_DIR}
			-DGQ_TEST_DIR=${CMAKE_BINARY_DIR}/test-output-ssh
			-P ${CMAKE_SOURCE_DIR}/test/run_ssh_tests.cmake
	)

	add_test (
		NAME remote-input.integration
		COMMAND "${CMAKE_COMMAND}"
			-DGQ_MERGE1D_RUNNER=$<TARGET_FILE:merge1d_runner>
			-DGQ_MERGE2D_RUNNER=$<TARGET_FILE:merge2d_runner>
			-DGQ_MERGE3D_RUNNER=$<TARGET_FILE:merge3d_runner>
			-DGQ_TOPOBATH_RUNNER=$<TARGET_FILE:topobath_runner>
			-DGQ_ELYGTL_RUNNER=$<TARGET_FILE:elygtl_runner>
			-DGQ_SSH_RUNNER=$<TARGET_FILE:ssh_runner>
			-DGQ_PLUGIN=$<TARGET_FILE:gqlib>
			-DGQ_CHECK_TABLE=$<TARGET_FILE:check_table>
			-DGQ_CHECK_2D=$<TARGET_FILE:check_netcdf>
			-DGQ_CHECK_3D=$<TARGET_FILE:check_netcdf3d>
			-DGQ_CHECK_SSH=$<TARGET_FILE:check_ssh>
			-DNCGEN_EXECUTABLE=${NCGEN_EXECUTABLE}
			-DGQ_SOURCE_DIR=${CMAKE_SOURCE_DIR}
			-DGQ_TEST_DIR=${CMAKE_BINARY_DIR}/test-output-remote
			-P ${CMAKE_SOURCE_DIR}/test/run_remote_tests.cmake
	)
	set_tests_properties(remote-input.integration PROPERTIES TIMEOUT 300)
endif ()

# Configuration done
message(
	"*  Locations:\n"              
	"*  GMT library found       : ${GMT_LIBRARY}\n"
	"*  GMT include dir found   : ${GMT_INCLUDE_DIR}\n"
	"*  GDAL include dir found  : ${GQ_GDAL_INCLUDE_DIR}\n"
	"*  BLEND library found     : ${GQ_BLEND_LIBRARY} (version ${GQ_BLEND_VERSION})\n"
	"*  BLEND include dir found : ${GQ_BLEND_INCLUDE_DIR}\n"
	"*  Project name            : ${CMAKE_PROJECT_NAME}\n"
	"*  Project version         : ${PROJECT_VERSION}\n"
	"*  Plug-in library         : ${GMT_CUSTOM_LIB_NAME}\n"
	"*  Installing plug-in in   : ${GQ_INSTALL_PLUGINDIR}\n"
	"*  GQ installation prefix  : ${CMAKE_INSTALL_PREFIX}\n"
	"*  Installing docs/examples: ${GQ_INSTALL_FULL_DOCDIR}\n")

# vim: textwidth=78 noexpandtab tabstop=2 softtabstop=2 shiftwidth=2
