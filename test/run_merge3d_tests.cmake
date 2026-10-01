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

file(REMOVE_RECURSE "${GQ_TEST_DIR}")
file(MAKE_DIRECTORY "${GQ_TEST_DIR}")

function(run_checked)
	execute_process(COMMAND ${ARGN} RESULT_VARIABLE status)
	if(status)
		message(FATAL_ERROR "Command failed (${status}): ${ARGN}")
	endif()
endfunction()

execute_process(
	COMMAND "${NCGEN_EXECUTABLE}" -o "${GQ_TEST_DIR}/primary.nc"
	        "${GQ_SOURCE_DIR}/test/data/primary_multi_3d.cdl"
	RESULT_VARIABLE status
)
if(status)
	message(FATAL_ERROR "ncgen failed for primary cube")
endif()
execute_process(
	COMMAND "${NCGEN_EXECUTABLE}" -o "${GQ_TEST_DIR}/secondary.nc"
	        "${GQ_SOURCE_DIR}/test/data/secondary_multi_3d.cdl"
	RESULT_VARIABLE status
)
if(status)
	message(FATAL_ERROR "ncgen failed for secondary cube")
endif()
execute_process(
	COMMAND "${NCGEN_EXECUTABLE}" -o "${GQ_TEST_DIR}/primary_small.nc"
	        "${GQ_SOURCE_DIR}/test/data/primary_small_3d.cdl"
	RESULT_VARIABLE status
)
if(status)
	message(FATAL_ERROR "ncgen failed for the small primary cube")
endif()
execute_process(
	COMMAND "${NCGEN_EXECUTABLE}" -o "${GQ_TEST_DIR}/aggregate_inputs.nc"
	        "${GQ_SOURCE_DIR}/test/data/aggregate_3d.cdl"
	RESULT_VARIABLE status
)
if(status)
	message(FATAL_ERROR "ncgen failed for the aggregate cube")
endif()

# Per-source axis/value transforms establish common internal units. Output
# transforms are positional by -F and do not reorder fields or weights.
execute_process(
	COMMAND "${CMAKE_COMMAND}" -E env
	        "GQ_PLUGIN=${GQ_PLUGIN}"
	        "${GQ_RUNNER}"
	        "${GQ_TEST_DIR}/primary.nc?vp,den+x2+Xm+y3+Ym+z-0.004+Zkm+v2,3+Vin_vp,in_den"
	        -R0/4/0/6 -I2/3 -T0/8/4 -Fvp,rho
	        "-Z+x-0.5+Xout_x+y-2+Yout_y+z-0.25+Zout_z+v10,100+Vout_vp,out_rho"
	        "-G${GQ_TEST_DIR}/scaled.nc" -nl
	RESULT_VARIABLE status
)
if(status)
	message(FATAL_ERROR "merge3d scaled-coordinate run failed")
endif()
foreach(check
		"vp;0;0;0;2000;1e-5"
		"vp;0;0;2;2040;1e-5"
		"vp;0;2;0;2040;1e-5"
		"vp;2;0;0;2400;1e-5"
		"vp;2;2;2;2480;1e-5"
		"rho;0;0;0;60000;1e-4")
	list(GET check 0 variable)
	list(GET check 1 layer)
	list(GET check 2 row)
	list(GET check 3 column)
	list(GET check 4 expected)
	list(GET check 5 tolerance)
	run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/scaled.nc"
		"${variable}" "${layer}" "${row}" "${column}" "${expected}" "${tolerance}")
endforeach()
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/scaled.nc" x 0 0 1e-9)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/scaled.nc" x 2 -2 1e-9)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/scaled.nc" y 0 0 1e-9)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/scaled.nc" y 2 -12 1e-9)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/scaled.nc" z 0 0 1e-9)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/scaled.nc" z 2 -2 1e-9)
run_checked("${GQ_CHECK_ATTRIBUTE}" "${GQ_TEST_DIR}/scaled.nc" x units out_x)
run_checked("${GQ_CHECK_ATTRIBUTE}" "${GQ_TEST_DIR}/scaled.nc" y units out_y)
run_checked("${GQ_CHECK_ATTRIBUTE}" "${GQ_TEST_DIR}/scaled.nc" z units out_z)
run_checked("${GQ_CHECK_ATTRIBUTE}" "${GQ_TEST_DIR}/scaled.nc" vp units out_vp)
run_checked("${GQ_CHECK_ATTRIBUTE}" "${GQ_TEST_DIR}/scaled.nc" rho units out_rho)

# Input scaling never reorders a cube. A decreasing source z axis must be
# scaled to increase before interpolation instead of being reversed silently.
execute_process(
	COMMAND "${CMAKE_COMMAND}" -E env "GQ_PLUGIN=${GQ_PLUGIN}"
		"${GQ_RUNNER}" "${GQ_TEST_DIR}/primary.nc?vp"
		-R0/2/0/2 -I1 -T-2000/0/1000
		"-G${GQ_TEST_DIR}/invalid_z_scale.nc" -nl
	RESULT_VARIABLE status
	ERROR_VARIABLE error)
if(status EQUAL 0 OR NOT error MATCHES "Input \\+z scaling does not reorder cube data")
	message(FATAL_ERROR
		"merge3d did not reject a decreasing transformed z axis with scaling guidance:\n${error}")
endif()

# GMT -di sentinels become NaN before 3-D interpolation and blending.
file(WRITE "${GQ_TEST_DIR}/sentinel_pair.merge"
	"${GQ_TEST_DIR}/aggregate_inputs.nc?v_missing "
	"${GQ_TEST_DIR}/aggregate_inputs.nc?s0 - - boxcar 0\n")
execute_process(
	COMMAND "${CMAKE_COMMAND}" -E env
	        "GQ_PLUGIN=${GQ_PLUGIN}"
	        "${GQ_RUNNER}" "${GQ_TEST_DIR}/sentinel_pair.merge"
	        -R0/2/0/2 -I0.5 -T0/2/0.5 -Fvp -di-99999 -nl -Sl
	        "-G${GQ_TEST_DIR}/sentinel_pair.nc"
	RESULT_VARIABLE status
)
if(status)
	message(FATAL_ERROR "merge3d sentinel pair run failed")
endif()
execute_process(
	COMMAND "${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/sentinel_pair.nc"
	        vp 2 2 2 nan 0
	RESULT_VARIABLE status
)
if(status)
	message(FATAL_ERROR "merge3d did not preserve a paired primary sentinel by default")
endif()
foreach(pair_case IN ITEMS fill interpolated aggregate aggregate_fill)
	if(pair_case STREQUAL "fill")
		set(pair_options -P -Sl)
		set(pair_increment 0.5)
		set(pair_index 2)
		set(pair_expected 0)
	elseif(pair_case STREQUAL "interpolated")
		set(pair_options -P -Sl+g)
		set(pair_increment 0.5)
		set(pair_index 2)
		set(pair_expected 10)
	elseif(pair_case STREQUAL "aggregate")
		set(pair_options -A -Sl)
		set(pair_increment 1)
		set(pair_index 1)
		set(pair_expected nan)
	else()
		set(pair_options -A -P -Sl)
		set(pair_increment 1)
		set(pair_index 1)
		set(pair_expected 0)
	endif()
	execute_process(
		COMMAND "${CMAKE_COMMAND}" -E env
		        "GQ_PLUGIN=${GQ_PLUGIN}"
		        "${GQ_RUNNER}" "${GQ_TEST_DIR}/sentinel_pair.merge"
		        -R0/2/0/2 "-I${pair_increment}" "-T0/2/${pair_increment}"
		        -Fvp -di-99999 -nl ${pair_options}
		        "-G${GQ_TEST_DIR}/sentinel_pair_${pair_case}.nc"
		RESULT_VARIABLE status
	)
	if(status)
		message(FATAL_ERROR "merge3d ${pair_case} paired-sentinel run failed")
	endif()
	execute_process(
		COMMAND "${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/sentinel_pair_${pair_case}.nc"
		        vp ${pair_index} ${pair_index} ${pair_index} ${pair_expected} 1e-6
		RESULT_VARIABLE status
	)
	if(status)
		message(FATAL_ERROR "merge3d ${pair_case} paired-sentinel check failed")
	endif()
endforeach()
file(WRITE "${GQ_TEST_DIR}/sentinel_only.merge"
	"${GQ_TEST_DIR}/aggregate_inputs.nc?v_missing - - - - -\n")
execute_process(
	COMMAND "${CMAKE_COMMAND}" -E env
	        "GQ_PLUGIN=${GQ_PLUGIN}"
	        "${GQ_RUNNER}" "${GQ_TEST_DIR}/sentinel_only.merge"
	        -R0/2/0/2 -I1 -T0/2/1 -Fvp -di-99999 -nl
	        "-G${GQ_TEST_DIR}/sentinel_only.nc"
	RESULT_VARIABLE status
)
if(status)
	message(FATAL_ERROR "merge3d sentinel-only run failed")
endif()
execute_process(
	COMMAND "${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/sentinel_only.nc"
	        vp 1 1 1 nan 0
	RESULT_VARIABLE status
)
if(status)
	message(FATAL_ERROR "merge3d did not write a -di sentinel as NaN")
endif()
foreach(gap_case IN ITEMS unlimited limited accepted)
	if(gap_case STREQUAL "unlimited")
		set(gap_option "-Sl+g")
		set(gap_expected 10)
	elseif(gap_case STREQUAL "limited")
		set(gap_option "-Sl+g1.5")
		set(gap_expected nan)
	else()
		set(gap_option "-Sl+g2")
		set(gap_expected 10)
	endif()
	execute_process(
		COMMAND "${CMAKE_COMMAND}" -E env
		        "GQ_PLUGIN=${GQ_PLUGIN}"
		        "${GQ_RUNNER}" "${GQ_TEST_DIR}/sentinel_only.merge"
		        -R0/2/0/2 -I1 -T0/2/1 -Fvp -di-99999 -nl
		        "${gap_option}" "-G${GQ_TEST_DIR}/gap_${gap_case}.nc"
		RESULT_VARIABLE status
	)
	if(status)
		message(FATAL_ERROR "merge3d ${gap_case} vertical-gap run failed")
	endif()
	execute_process(
		COMMAND "${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/gap_${gap_case}.nc"
		        vp 1 1 1 ${gap_expected} 1e-6
		RESULT_VARIABLE status
	)
	if(status)
		message(FATAL_ERROR "merge3d ${gap_case} vertical-gap check failed")
	endif()
endforeach()

# Horizontal gap filling is independent of vertical +g bridging. The center
# sentinel is an internal x-y hole in the middle native layer.
foreach(horizontal_case IN ITEMS preserved filled limited)
	if(horizontal_case STREQUAL "preserved")
		set(horizontal_option)
		set(horizontal_expected nan)
	elseif(horizontal_case STREQUAL "filled")
		set(horizontal_option -Hl)
		set(horizontal_expected 10)
	else()
		set(horizontal_option "-Hl+m1")
		set(horizontal_expected 10)
	endif()
	execute_process(
		COMMAND "${CMAKE_COMMAND}" -E env
		        "GQ_PLUGIN=${GQ_PLUGIN}"
		        "${GQ_RUNNER}" "${GQ_TEST_DIR}/sentinel_only.merge"
		        -R0/2/0/2 -I1 -T0/2/1 -Fvp -di-99999 -nl -Sl
		        ${horizontal_option}
		        "-G${GQ_TEST_DIR}/horizontal_gap_${horizontal_case}.nc"
		RESULT_VARIABLE status
	)
	if(status)
		message(FATAL_ERROR
			"merge3d ${horizontal_case} horizontal-gap run failed")
	endif()
	execute_process(
		COMMAND "${GQ_CHECK_NETCDF}"
		        "${GQ_TEST_DIR}/horizontal_gap_${horizontal_case}.nc"
		        vp 1 1 1 ${horizontal_expected} 1e-6
		RESULT_VARIABLE status
	)
	if(status)
		message(FATAL_ERROR
			"merge3d ${horizontal_case} horizontal-gap check failed")
	endif()
endforeach()

# A paired secondary is restricted to the primary cube domain.
file(WRITE "${GQ_TEST_DIR}/domain_pair.merge"
	"${GQ_TEST_DIR}/primary_small.nc?vp "
	"${GQ_TEST_DIR}/secondary.nc?p - - boxcar 0\n")
execute_process(
	COMMAND "${CMAKE_COMMAND}" -E env
	        "GQ_PLUGIN=${GQ_PLUGIN}"
	        "${GQ_RUNNER}" "${GQ_TEST_DIR}/domain_pair.merge"
	        -R0/2/0/2 -I1 -T0/2/1
	        "-G${GQ_TEST_DIR}/domain_pair.nc" -Fvp -nl
	RESULT_VARIABLE status
)
if(status)
	message(FATAL_ERROR "merge3d primary-domain run failed")
endif()
execute_process(
	COMMAND "${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/domain_pair.nc"
	        vp 0 0 0 nan 0
	RESULT_VARIABLE status
)
if(status)
	message(FATAL_ERROR "merge3d paired secondary escaped the primary domain")
endif()
execute_process(
	COMMAND "${CMAKE_COMMAND}" -E env
	        "GQ_PLUGIN=${GQ_PLUGIN}"
	        "${GQ_RUNNER}" "${GQ_TEST_DIR}/domain_pair.merge"
	        -R0/2/0/2 -I1 -T0/2/1 -A
	        "-G${GQ_TEST_DIR}/domain_pair_aggregate.nc" -Fvp -nl
	RESULT_VARIABLE status
)
if(status)
	message(FATAL_ERROR "merge3d aggregate primary-domain run failed")
endif()
execute_process(
	COMMAND "${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/domain_pair_aggregate.nc"
	        vp 0 0 0 nan 0
	RESULT_VARIABLE status
)
if(status)
	message(FATAL_ERROR "merge3d aggregate secondary escaped the primary domain")
endif()
file(WRITE "${GQ_TEST_DIR}/domain_fallback.merge"
	"${GQ_TEST_DIR}/primary_small.nc?vp "
	"${GQ_TEST_DIR}/secondary.nc?p - - boxcar 0\n"
	"${GQ_TEST_DIR}/secondary.nc?p - - - - -\n")
execute_process(
	COMMAND "${CMAKE_COMMAND}" -E env
	        "GQ_PLUGIN=${GQ_PLUGIN}"
	        "${GQ_RUNNER}" "${GQ_TEST_DIR}/domain_fallback.merge"
	        -R0/2/0/2 -I1 -T0/2/1
	        "-G${GQ_TEST_DIR}/domain_fallback.nc" -Fvp -nl
	RESULT_VARIABLE status
)
if(status)
	message(FATAL_ERROR "merge3d explicit fallback run failed")
endif()
execute_process(
	COMMAND "${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/domain_fallback.nc"
	        vp 0 0 0 0 1e-6
	RESULT_VARIABLE status
)
if(status)
	message(FATAL_ERROR "merge3d explicit fallback did not tile the secondary")
endif()
execute_process(
	COMMAND "${CMAKE_COMMAND}" -E env
	        "GQ_PLUGIN=${GQ_PLUGIN}"
	        "${GQ_RUNNER}" "${GQ_TEST_DIR}/domain_fallback.merge"
	        -R0/2/0/2 -I1 -T0/2/1 -A -W
	        "-G${GQ_TEST_DIR}/domain_fallback_aggregate.nc" -Fvp -nl
	RESULT_VARIABLE status
)
if(status)
	message(FATAL_ERROR "merge3d aggregate fallback run failed")
endif()
execute_process(
	COMMAND "${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/domain_fallback_aggregate.nc"
	        vp 0 0 0 0 1e-6
	RESULT_VARIABLE status
)
if(status)
	message(FATAL_ERROR "merge3d aggregate fallback did not tile the secondary")
endif()
execute_process(
	COMMAND "${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/domain_fallback_aggregate.nc"
	        weight 0 0 0 0 1e-6
	RESULT_VARIABLE status
)
if(status)
	message(FATAL_ERROR "merge3d standalone fallback entered aggregate weights")
endif()

file(WRITE "${GQ_TEST_DIR}/overlap_three.merge"
	"${GQ_TEST_DIR}/aggregate_inputs.nc?v10 ${GQ_TEST_DIR}/aggregate_inputs.nc?s0 - - boxcar 0\n"
	"${GQ_TEST_DIR}/aggregate_inputs.nc?v20 ${GQ_TEST_DIR}/aggregate_inputs.nc?s0 - - boxcar 0\n"
	"${GQ_TEST_DIR}/aggregate_inputs.nc?v40 ${GQ_TEST_DIR}/aggregate_inputs.nc?s0 - - boxcar 0\n"
	"${GQ_TEST_DIR}/aggregate_inputs.nc?s0 - - - - -\n")
execute_process(
	COMMAND "${CMAKE_COMMAND}" -E env
	        "GQ_PLUGIN=${GQ_PLUGIN}"
	        "${GQ_RUNNER}" "${GQ_TEST_DIR}/overlap_three.merge"
	        -R0/2/0/2 -I1 -T0/2/1 -A -W
	        "-G${GQ_TEST_DIR}/overlap_three.nc" -Fvp -nl
	RESULT_VARIABLE status
)
if(status)
	message(FATAL_ERROR "merge3d three-primary aggregate run failed")
endif()
execute_process(
	COMMAND "${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/overlap_three.nc"
	        vp 1 1 1 23.3333333333 1e-5
	RESULT_VARIABLE status
)
if(status)
	message(FATAL_ERROR "merge3d three-primary aggregate value check failed")
endif()

file(WRITE "${GQ_TEST_DIR}/overlap_hierarchy.merge"
	"${GQ_TEST_DIR}/aggregate_inputs.nc?v10 ${GQ_TEST_DIR}/aggregate_inputs.nc?s0 - - boxcar 0\n"
	"${GQ_TEST_DIR}/aggregate_inputs.nc?v20 ${GQ_TEST_DIR}/aggregate_inputs.nc?s0 - - boxcar 0\n"
	"${GQ_TEST_DIR}/aggregate_inputs.nc?s0 ${GQ_TEST_DIR}/aggregate_inputs.nc?s100 - - boxcar 0\n"
	"${GQ_TEST_DIR}/aggregate_inputs.nc?s100 - - - - -\n")
execute_process(
	COMMAND "${CMAKE_COMMAND}" -E env
	        "GQ_PLUGIN=${GQ_PLUGIN}"
	        "${GQ_RUNNER}" "${GQ_TEST_DIR}/overlap_hierarchy.merge"
	        -R0/2/0/2 -I1 -T0/2/1 -A -W
	        "-G${GQ_TEST_DIR}/overlap_hierarchy.nc" -Fvp -nl
	RESULT_VARIABLE status
)
if(status)
	message(FATAL_ERROR "merge3d hierarchical aggregate run failed")
endif()
execute_process(
	COMMAND "${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/overlap_hierarchy.nc"
	        vp 1 1 1 15 1e-5
	RESULT_VARIABLE status
)
if(status)
	message(FATAL_ERROR "merge3d hierarchical aggregate value check failed")
endif()
execute_process(
	COMMAND "${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/overlap_hierarchy.nc"
	        weight 1 1 1 1 1e-6
	RESULT_VARIABLE status
)
if(status)
	message(FATAL_ERROR "merge3d hierarchical aggregate weight check failed")
endif()

file(WRITE "${GQ_TEST_DIR}/weight_first.txt" "0 0\n2 0\n2 2\n0 2\n0 0\n")
file(WRITE "${GQ_TEST_DIR}/weight_second.txt" "2 2\n4 2\n4 4\n2 4\n2 2\n")
set(weight_source "${GQ_TEST_DIR}/aggregate_inputs.nc")
set(weight_transform "+x2+y2+z2")
file(WRITE "${GQ_TEST_DIR}/weight_order.merge"
	"${weight_source}?v10${weight_transform} ${weight_source}?s0${weight_transform} ${GQ_TEST_DIR}/weight_first.txt 0/2 cosine 0.25\n"
	"${weight_source}?v20${weight_transform} ${weight_source}?s0${weight_transform} ${GQ_TEST_DIR}/weight_second.txt 2/4 cosine 0.25\n"
	"${weight_source}?s0${weight_transform} ${weight_source}?s100${weight_transform} - - boxcar 0\n"
	"${weight_source}?s100${weight_transform} - - - - -\n")
foreach(mode regular aggregate)
	set(aggregate_option)
	set(overlap_weight 0.421875)
	if(mode STREQUAL "aggregate")
		set(aggregate_option -A)
		set(overlap_weight 1)
	endif()
	foreach(weight_option -W -W+o)
		run_checked("${CMAKE_COMMAND}" -E env "GQ_PLUGIN=${GQ_PLUGIN}"
			"${GQ_RUNNER}" "${GQ_TEST_DIR}/weight_order.merge"
			-R0/4/0/4 -I1 -T0/4/1 -Fvp -nl ${aggregate_option} ${weight_option}
			"-G${GQ_TEST_DIR}/weight_order.nc")
		run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/weight_order.nc" weight 2 2 2 ${overlap_weight} 1e-6)
		run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/weight_order.nc" weight 4 4 4 0.421875 1e-6)
		run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/weight_order.nc" weight 4 4 0 1 1e-6)
		run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/weight_order.nc" weight 3 1 1 1 1e-6)
	endforeach()
endforeach()

file(WRITE "${GQ_TEST_DIR}/weight_first.txt" "0 0\n2 0\n0 2\n0 0\n")
run_checked("${CMAKE_COMMAND}" -E env "GQ_PLUGIN=${GQ_PLUGIN}"
	"${GQ_RUNNER}" "${GQ_TEST_DIR}/weight_order.merge"
	-R0/4/0/4 -I1 -T0/4/1 -Fvp -nl -W+o "-G${GQ_TEST_DIR}/weight_triangle.nc")
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/weight_triangle.nc" weight 2 2 2 1 1e-6)

file(WRITE "${GQ_TEST_DIR}/overlap_bad.merge"
	"${GQ_TEST_DIR}/aggregate_inputs.nc?v10 ${GQ_TEST_DIR}/aggregate_inputs.nc?s0 - - boxcar 0\n"
	"${GQ_TEST_DIR}/aggregate_inputs.nc?v20 ${GQ_TEST_DIR}/aggregate_inputs.nc?s100 - - boxcar 0\n")
execute_process(
	COMMAND "${CMAKE_COMMAND}" -E env
	        "GQ_PLUGIN=${GQ_PLUGIN}"
	        "${GQ_RUNNER}" "${GQ_TEST_DIR}/overlap_bad.merge"
	        -R0/2/0/2 -I1 -T0/2/1 -A
	        "-G${GQ_TEST_DIR}/overlap_bad.nc" -Fvp -nl
	RESULT_VARIABLE status
	OUTPUT_QUIET
	ERROR_QUIET
)
if(status EQUAL 0)
	message(FATAL_ERROR "merge3d accepted overlapping primaries with different secondary variables")
endif()

file(WRITE "${GQ_TEST_DIR}/merge.txt"
	"${GQ_TEST_DIR}/primary.nc?vp,den+z-1/1000 "
	"${GQ_TEST_DIR}/secondary.nc?p,rho "
	"${GQ_SOURCE_DIR}/test/data/merge3d_square.txt "
	"0/2 cosine/cosine/cosine 0.25/0.25/0.25\n")

execute_process(
	COMMAND "${CMAKE_COMMAND}" -E env
	        "GQ_PLUGIN=${GQ_PLUGIN}"
	        "${GQ_RUNNER}" "${GQ_TEST_DIR}/merge.txt"
	        -R0/2/0/2 -I1 -T0/2/1
	        "-G${GQ_TEST_DIR}/merged.nc" -Fvp,rho -W "-Z+z-1000+Zm" -nl
	RESULT_VARIABLE status
)
if(status)
	message(FATAL_ERROR "merge3d regular merge failed")
endif()

foreach(check
		"vp;1;1;1;112;1e-5"
		"vp;0;0;0;100;1e-5"
		"vp;2;2;2;52.3125;1e-5"
		"rho;1;1;1;224;1e-5"
		"weight;1;1;1;1;1e-5"
		"weight;0;0;0;1;1e-5"
		"weight;2;2;2;0.421875;1e-5")
	list(GET check 0 variable)
	list(GET check 1 layer)
	list(GET check 2 row)
	list(GET check 3 column)
	list(GET check 4 expected)
	list(GET check 5 tolerance)
	execute_process(
		COMMAND "${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/merged.nc"
		        "${variable}" "${layer}" "${row}" "${column}"
		        "${expected}" "${tolerance}"
		RESULT_VARIABLE status
	)
	if(status)
		message(FATAL_ERROR "merge3d value check failed: ${check}")
	endif()
endforeach()

execute_process(
	COMMAND "${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/merged.nc"
	        z 0 0 1e-9
	RESULT_VARIABLE status
)
if(status)
	message(FATAL_ERROR "merge3d output z scaling check failed")
endif()
execute_process(
	COMMAND "${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/merged.nc"
	        z 2 -2000 1e-9
	RESULT_VARIABLE status
)
if(status)
	message(FATAL_ERROR "merge3d decreasing output z check failed")
endif()

execute_process(
	COMMAND "${CMAKE_COMMAND}" -E env
	        "GQ_PLUGIN=${GQ_PLUGIN}"
	        "${GQ_RUNNER}" "${GQ_TEST_DIR}/merge.txt"
	        -R0/2/0/2 -I1 -T0/2/1
	        "-G${GQ_TEST_DIR}/weights.nc" -Fvp,rho -W+o -nl
	RESULT_VARIABLE status
)
if(status)
	message(FATAL_ERROR "merge3d weight-only run failed")
endif()

file(WRITE "${GQ_TEST_DIR}/overlap.merge"
	"${GQ_TEST_DIR}/primary.nc?vp,den+z-1/1000 "
	"${GQ_TEST_DIR}/secondary.nc?p,rho "
	"${GQ_SOURCE_DIR}/test/data/merge3d_square.txt "
	"0/2 cosine 0.25/0.25/0.25\n"
	"${GQ_TEST_DIR}/primary.nc?vp,den+z-1/1000 "
	"${GQ_TEST_DIR}/secondary.nc?p,rho "
	"${GQ_SOURCE_DIR}/test/data/merge3d_square.txt "
	"0/2 cosine 0.25/0.25/0.25\n")
execute_process(
	COMMAND "${CMAKE_COMMAND}" -E env
	        "GQ_PLUGIN=${GQ_PLUGIN}"
	        "${GQ_RUNNER}" "${GQ_TEST_DIR}/overlap.merge"
	        -R0/2/0/2 -I1 -T0/2/1
	        "-G${GQ_TEST_DIR}/aggregate.nc" -Fvp,rho -A -W -nl
	RESULT_VARIABLE status
)
if(status)
	message(FATAL_ERROR "merge3d aggregate-overlap run failed")
endif()
execute_process(
	COMMAND "${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/aggregate.nc"
	        vp 1 1 1 112 1e-5
	RESULT_VARIABLE status
)
if(status)
	message(FATAL_ERROR "merge3d aggregate value check failed")
endif()

execute_process(
	COMMAND "${CMAKE_COMMAND}" -E env
	        "GQ_PLUGIN=${GQ_PLUGIN}"
	        "${GQ_RUNNER}" "${GQ_TEST_DIR}/primary.nc?vp,den+z-1/1000"
	        -R0/2/0/2 -I0.5 -T0/2/0.5
	        "-G${GQ_TEST_DIR}/interpolated.nc" -nc
	RESULT_VARIABLE status
)
if(status)
	message(FATAL_ERROR "merge3d direct cubic interpolation run failed")
endif()
execute_process(
	COMMAND "${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/interpolated.nc"
	        vp 1 1 1 106 1e-4
	RESULT_VARIABLE status
)
if(status)
	message(FATAL_ERROR "merge3d cubic interpolation value check failed")
endif()

file(COPY "${GQ_SOURCE_DIR}/test/data/merge3d_nonmonotone.txt"
     DESTINATION "${GQ_TEST_DIR}")
file(WRITE "${GQ_TEST_DIR}/monotone.merge"
	"${GQ_TEST_DIR}/primary.nc?vp,den+z-1/1000 "
	"${GQ_TEST_DIR}/secondary.nc?p,rho "
	"${GQ_TEST_DIR}/merge3d_nonmonotone.txt "
	"0/2 cosine 0.25/0.25/0.25\n")
execute_process(
	COMMAND "${CMAKE_COMMAND}" -E env
	        "GQ_PLUGIN=${GQ_PLUGIN}"
	        "${GQ_RUNNER}" "${GQ_TEST_DIR}/monotone.merge"
	        -R0/2/0/2 -I0.5 -T0/2/0.5
	        "-G${GQ_TEST_DIR}/monotone.nc" -Fvp,rho -ME+w -nl
	RESULT_VARIABLE status
)
if(status OR NOT EXISTS "${GQ_TEST_DIR}/merge3d_nonmonotone_monotone.txt")
	message(FATAL_ERROR "merge3d monotone conversion run failed")
endif()
