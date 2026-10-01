#
# $Id: CMakeLists.txt 11838 2013-06-28 06:49:32Z pwessel $
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
# Sources for the GQ GMT plug-in.
set (GQ_SOURCES
	"${CMAKE_CURRENT_BINARY_DIR}/gq_version.h"
	gq_remote.h
	gq_remote.c
	gq_transform.h
	gq_transform.c
	ssh_common.h
	ssh_common.c
	ssh_netcdf.h
	ssh_netcdf.c
	ssh_module.h
	ssh_module.c
	ssh1d_inc.h
	ssh1d.c
	ssh2d_inc.h
	ssh2d.c
	ssh3d_inc.h
	ssh3d.c
	elygtl_inc.h
	elygtl.c
	merge1d_inc.h
	merge1d.c
	merge2d_inc.h
	merge2d.c
	merge3d_inc.h
	merge3d.c
	topobath_inc.h
	topobath.c)
#=========================================================================
# Most likely no changes below here

# On Windows do not prefix the DLLs with the 'lib' prefix
if (WIN32)
	set(LIB_PREFIX "")
else (WIN32)
	set(LIB_PREFIX "lib")
endif (WIN32)

# Find libraries
message (STATUS "Searching dependent libraries. This may take a few minutes...")
find_package (GMT REQUIRED)
find_package (GDAL REQUIRED)
include_directories (${GMT_INCLUDE_DIRS})

if (NOT GQ_INSTALL_PLUGINDIR)
	set (_gq_install_plugindir)
	if (GMT_CONFIG)
		execute_process (
			COMMAND "${GMT_CONFIG}" --plugindir
			RESULT_VARIABLE _gq_plugindir_status
			OUTPUT_VARIABLE _gq_install_plugindir
			OUTPUT_STRIP_TRAILING_WHITESPACE
			ERROR_QUIET)
	endif ()
	if (NOT _gq_install_plugindir)
		if (GMT_CONFIG)
			get_filename_component (_gq_gmt_bindir "${GMT_CONFIG}" DIRECTORY)
		endif ()
		find_program (_GQ_GMT_EXECUTABLE gmt HINTS "${_gq_gmt_bindir}")
		if (_GQ_GMT_EXECUTABLE)
			execute_process (
				COMMAND "${_GQ_GMT_EXECUTABLE}" --show-plugindir
				RESULT_VARIABLE _gq_plugindir_status
				OUTPUT_VARIABLE _gq_install_plugindir
				OUTPUT_STRIP_TRAILING_WHITESPACE
				ERROR_QUIET)
		endif ()
	endif ()
	if (NOT _gq_install_plugindir OR NOT _gq_plugindir_status EQUAL 0)
		message (FATAL_ERROR
			"Could not determine GMT's plug-in directory. Set "
			"GQ_INSTALL_PLUGINDIR in cmake/ConfigUser.cmake.")
	endif ()
	set (GQ_INSTALL_PLUGINDIR "${_gq_install_plugindir}")
endif ()
set (GQ_INSTALL_PLUGINDIR "${GQ_INSTALL_PLUGINDIR}" CACHE PATH
	"Directory in which to install the GQ plug-in" FORCE)

find_package (NETCDF REQUIRED)
include_directories (${NETCDF_INCLUDE_DIR})
if (BLEND_ROOT)
	set (blend_ROOT "${BLEND_ROOT}")
endif ()
find_package (blend 2.0 REQUIRED CONFIG)

set (_gq_blend_library "${BLEND_LIBRARIES}")
get_target_property (_gq_blend_configurations
	BLEND::blend IMPORTED_CONFIGURATIONS)
foreach (_gq_blend_configuration IN LISTS _gq_blend_configurations)
	string (TOUPPER "${_gq_blend_configuration}"
		_gq_blend_configuration_upper)
	get_target_property (_gq_blend_location BLEND::blend
		"IMPORTED_LOCATION_${_gq_blend_configuration_upper}")
	if (_gq_blend_location)
		set (_gq_blend_library "${_gq_blend_location}")
		break ()
	endif ()
endforeach ()
set (GQ_BLEND_LIBRARY "${_gq_blend_library}" PARENT_SCOPE)
set (GQ_BLEND_INCLUDE_DIR "${BLEND_INCLUDE_DIRS}" PARENT_SCOPE)
set (GQ_BLEND_VERSION "${BLEND_VERSION}" PARENT_SCOPE)
set (GQ_GDAL_INCLUDE_DIR "${GDAL_INCLUDE_DIRS}" PARENT_SCOPE)

# Support glue code for the modules.
set (GQ_GLUE_SOURCE ${CMAKE_PROJECT_NAME}_glue.c)

# Plug-in target.
set (GQ_PLUGIN_TARGET gqlib)

# gqlib
set (CMAKE_SHARED_MODULE_SUFFIX .so)
add_library (gqlib MODULE
	${GQ_GLUE_SOURCE}
	${GQ_SOURCES})

target_include_directories (gqlib PRIVATE
	"${CMAKE_CURRENT_BINARY_DIR}"
	${GDAL_INCLUDE_DIRS})

target_link_libraries (gqlib
	${GMT_LIBRARIES}
	${NETCDF_LIBRARY}
	BLEND::blend)

if (UNIX)
	target_link_libraries (gqlib m)
endif (UNIX)

set_target_properties (gqlib
	PROPERTIES
	OUTPUT_NAME ${CMAKE_PROJECT_NAME}
	RUNTIME_OUTPUT_NAME ${CMAKE_PROJECT_NAME}
	LIBRARY_OUTPUT_DIRECTORY plugins
	RUNTIME_OUTPUT_DIRECTORY plugins
	INSTALL_RPATH_USE_LINK_PATH TRUE
	PREFIX ""
	LINK_FLAGS "${USER_CUSTOMLIB_LINK_FLAGS}"
	DEFINE_SYMBOL "LIBRARY_EXPORTS")

# If a renaming of the custom dll has been set in ConfigUser.cmake
if (WIN32 AND CUSTOM_CUSTOM_RENAME)
	set_target_properties (gqlib PROPERTIES RUNTIME_OUTPUT_NAME ${CUSTOM_CUSTOM_RENAME})
endif ()

# add the install targets
install (TARGETS ${GQ_PLUGIN_TARGET}
	ARCHIVE DESTINATION ${GQ_INSTALL_PLUGINDIR}
	COMPONENT Runtime
	LIBRARY DESTINATION ${GQ_INSTALL_PLUGINDIR}
	COMPONENT Runtime
	RUNTIME DESTINATION ${GQ_INSTALL_PLUGINDIR}
	COMPONENT Runtime)

if (WIN32 AND _debug_build)
	# install program database files
	set (_pdb_files)
	foreach (_target ${LIB_PREFIX}${CUSTOM_CUSTOM_RENAME})
		list (APPEND _pdb_files "${CMAKE_CURRENT_BINARY_DIR}/${_target}.pdb")
	endforeach ()
	install (FILES ${_pdb_files}
		DESTINATION ${GQ_INSTALL_PLUGINDIR}
		COMPONENT Runtime)
endif (WIN32 AND _debug_build)

# this prints out the final version of the compiler/linker flags
string (TOUPPER "${CMAKE_BUILD_TYPE}" CMAKE_BUILD_TYPE_UPPER)
string(REGEX REPLACE "[ ]+" " " _using_cflags ${CMAKE_C_FLAGS} " "
	${CMAKE_C_FLAGS_${CMAKE_BUILD_TYPE_UPPER}})
message (STATUS "Using CFLAGS = '${_using_cflags}'")

# vim: textwidth=78 noexpandtab tabstop=2 softtabstop=2 shiftwidth=2
