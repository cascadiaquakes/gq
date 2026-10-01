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

find_package (Python3 COMPONENTS Interpreter QUIET)
find_program (SPHINX_BUILD_EXECUTABLE sphinx-build)

if (NOT Python3_Interpreter_FOUND OR NOT SPHINX_BUILD_EXECUTABLE)
	message (STATUS "GQ documentation targets disabled (Python 3 or sphinx-build not found)")
	return ()
endif ()

find_package (GMT REQUIRED)
add_executable (gq_usage_runner gq_usage_runner.c)
target_include_directories (gq_usage_runner PRIVATE ${GMT_INCLUDE_DIR})
target_link_libraries (gq_usage_runner PRIVATE ${GMT_LIBRARIES} ${CMAKE_DL_LIBS})

set (GQ_DOC_BUILD_DIR "${CMAKE_BINARY_DIR}/doc")
set (GQ_DOC_SOURCE_DIR "${GQ_DOC_BUILD_DIR}/source")
set (GQ_DOC_USAGE_DIR "${GQ_DOC_BUILD_DIR}/usage")
set (GQ_DOC_MODULES merge1d merge2d merge3d topobath elygtl ssh1d ssh2d ssh3d)

set (GQ_DOC_USAGE_FILES)
foreach (module IN LISTS GQ_DOC_MODULES)
	set (usage_file "${GQ_DOC_USAGE_DIR}/${module}.txt")
	add_custom_command (
		OUTPUT "${usage_file}"
		COMMAND "${CMAKE_COMMAND}"
			-DGQ_USAGE_RUNNER=$<TARGET_FILE:gq_usage_runner>
			-DGQ_PLUGIN=$<TARGET_FILE:gqlib>
			-DGQ_MODULE=${module}
			-DOUTPUT_FILE=${usage_file}
			-P "${CMAKE_CURRENT_SOURCE_DIR}/GenerateUsage.cmake"
		DEPENDS gqlib gq_usage_runner
			"${CMAKE_CURRENT_SOURCE_DIR}/GenerateUsage.cmake"
		VERBATIM)
	list (APPEND GQ_DOC_USAGE_FILES "${usage_file}")
endforeach ()
add_custom_target (docs_usage DEPENDS ${GQ_DOC_USAGE_FILES})

file (GLOB_RECURSE GQ_DOC_EXAMPLE_INPUTS CONFIGURE_DEPENDS
	"${CMAKE_SOURCE_DIR}/doc/examples/*/ex*/README.md"
	"${CMAKE_SOURCE_DIR}/doc/examples/*/ex*/*.sh"
	"${CMAKE_SOURCE_DIR}/doc/examples/*/ex*/*.png"
	"${CMAKE_SOURCE_DIR}/doc/examples/*/ex*/*.pdf")
file (GLOB_RECURSE GQ_DOC_STATIC_INPUTS CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/source/*")

set (GQ_DOC_EXAMPLES_STAMP "${GQ_DOC_SOURCE_DIR}/.examples.stamp")
add_custom_command (
	OUTPUT "${GQ_DOC_EXAMPLES_STAMP}"
	COMMAND "${CMAKE_COMMAND}" -E remove_directory "${GQ_DOC_SOURCE_DIR}"
	COMMAND "${CMAKE_COMMAND}" -E copy_directory
		"${CMAKE_CURRENT_SOURCE_DIR}/source" "${GQ_DOC_SOURCE_DIR}"
	COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate_examples.py"
		--examples "${CMAKE_SOURCE_DIR}/doc/examples"
		--output "${GQ_DOC_SOURCE_DIR}/examples"
	COMMAND "${CMAKE_COMMAND}" -E touch "${GQ_DOC_EXAMPLES_STAMP}"
	DEPENDS ${GQ_DOC_EXAMPLE_INPUTS} ${GQ_DOC_STATIC_INPUTS}
		"${CMAKE_CURRENT_SOURCE_DIR}/generate_examples.py"
	VERBATIM)
add_custom_target (docs_examples DEPENDS "${GQ_DOC_EXAMPLES_STAMP}")

set (GQ_SPHINX_ENV
	"GQ_DOC_USAGE_DIR=${GQ_DOC_USAGE_DIR}"
	"GQ_DOC_VERSION=${PROJECT_VERSION}")

add_custom_target (docs_html
	COMMAND "${CMAKE_COMMAND}" -E env ${GQ_SPHINX_ENV}
		"${SPHINX_BUILD_EXECUTABLE}" -W --keep-going -b html
		"${GQ_DOC_SOURCE_DIR}" "${GQ_DOC_BUILD_DIR}/html"
	DEPENDS docs_usage docs_examples
	COMMENT "Building GQ HTML documentation"
	VERBATIM)

add_custom_target (docs_man
	COMMAND "${CMAKE_COMMAND}" -E env ${GQ_SPHINX_ENV}
		"${SPHINX_BUILD_EXECUTABLE}" -W --keep-going -b man
		"${GQ_DOC_SOURCE_DIR}" "${GQ_DOC_BUILD_DIR}/man"
	DEPENDS docs_usage docs_examples
	COMMENT "Building GQ manual pages"
	VERBATIM)

add_custom_target (docs_latex
	COMMAND "${CMAKE_COMMAND}" -E env ${GQ_SPHINX_ENV}
		"${SPHINX_BUILD_EXECUTABLE}" -W --keep-going -b latex
		"${GQ_DOC_SOURCE_DIR}" "${GQ_DOC_BUILD_DIR}/latex"
	DEPENDS docs_usage docs_examples
	COMMENT "Building GQ LaTeX documentation"
	VERBATIM)

add_custom_target (docs DEPENDS docs_html docs_man)

find_program (LATEXMK_EXECUTABLE latexmk HINTS /Library/TeX/texbin)
if (LATEXMK_EXECUTABLE)
	add_custom_target (docs_pdf
		COMMAND "${LATEXMK_EXECUTABLE}" -pdf -interaction=nonstopmode gq.tex
		WORKING_DIRECTORY "${GQ_DOC_BUILD_DIR}/latex"
		DEPENDS docs_latex
		COMMENT "Building GQ PDF documentation"
		VERBATIM)
else ()
	message (STATUS "latexmk not found: docs_latex is available, but docs_pdf is disabled")
endif ()

install (DIRECTORY "${GQ_DOC_BUILD_DIR}/html/"
	DESTINATION "${GQ_INSTALL_DOCDIR}/html"
	OPTIONAL
	PATTERN ".doctrees" EXCLUDE)

install (DIRECTORY "${GQ_DOC_BUILD_DIR}/man/"
	DESTINATION "${CMAKE_INSTALL_MANDIR}/man1"
	OPTIONAL
	FILES_MATCHING PATTERN "*.1")

install (FILES "${GQ_DOC_BUILD_DIR}/latex/gq.pdf"
	DESTINATION "${GQ_INSTALL_DOCDIR}"
	OPTIONAL)
