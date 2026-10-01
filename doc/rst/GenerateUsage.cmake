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

if (NOT DEFINED GQ_USAGE_RUNNER OR NOT DEFINED GQ_PLUGIN OR
	NOT DEFINED GQ_MODULE OR NOT DEFINED OUTPUT_FILE)
	message (FATAL_ERROR "GenerateUsage.cmake requires GQ_USAGE_RUNNER, GQ_PLUGIN, GQ_MODULE, and OUTPUT_FILE")
endif ()

get_filename_component (output_dir "${OUTPUT_FILE}" DIRECTORY)
file (MAKE_DIRECTORY "${output_dir}")

execute_process (
	COMMAND "${GQ_USAGE_RUNNER}" "${GQ_PLUGIN}" "${GQ_MODULE}"
	RESULT_VARIABLE result
	OUTPUT_VARIABLE stdout
	ERROR_VARIABLE stderr)

set (usage "${stdout}${stderr}")
if (usage STREQUAL "" OR NOT usage MATCHES "usage: gmt ${GQ_MODULE}")
	message (FATAL_ERROR "Failed to obtain usage for ${GQ_MODULE} (status ${result}):\n${usage}")
endif ()

file (WRITE "${OUTPUT_FILE}" "${usage}")
