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

file(MAKE_DIRECTORY "${GQ_TEST_DIR}")

function(run_checked)
	execute_process(
		COMMAND ${ARGN}
		RESULT_VARIABLE status
		OUTPUT_VARIABLE output
		ERROR_VARIABLE error
	)
	if(NOT status EQUAL 0)
		message(FATAL_ERROR "command failed (${status}): ${ARGN}\n${output}\n${error}")
	endif()
endfunction()

function(run_merge)
	execute_process(
		COMMAND "${CMAKE_COMMAND}" -E env
			"GQ_PLUGIN=${GQ_PLUGIN}"
			"${GQ_RUNNER}" ${ARGN}
		WORKING_DIRECTORY "${GQ_SOURCE_DIR}"
		RESULT_VARIABLE status
		OUTPUT_VARIABLE output
		ERROR_VARIABLE error
	)
	if(NOT status EQUAL 0)
		message(FATAL_ERROR "merge1d failed (${status}): ${ARGN}\n${output}\n${error}")
	endif()
endfunction()

function(run_merge_fails)
	execute_process(
		COMMAND "${CMAKE_COMMAND}" -E env
			"GQ_PLUGIN=${GQ_PLUGIN}"
			"${GQ_RUNNER}" ${ARGN}
		WORKING_DIRECTORY "${GQ_SOURCE_DIR}"
		RESULT_VARIABLE status
		OUTPUT_QUIET
		ERROR_QUIET
	)
	if(status EQUAL 0)
		message(FATAL_ERROR "merge1d unexpectedly accepted: ${ARGN}")
	endif()
endfunction()

# GMT interpolation modes and the agreed linear default.
foreach(mode a c e l n s0.5)
	run_merge("${GQ_SOURCE_DIR}/test/data/merge1d_irregular.txt"
		-T0/4/1 "-S${mode}" "-G${GQ_TEST_DIR}/interpolation_${mode}.txt" -Vq)
endforeach()
run_checked("${GQ_CHECK_TABLE}" "${GQ_TEST_DIR}/interpolation_l.txt"
	1 1 2 1e-10)
run_merge("${GQ_SOURCE_DIR}/test/data/merge1d_nan_gap.txt"
	-T0/4/0.5 -Sl "-G${GQ_TEST_DIR}/nan_gap.txt" -Vq)
run_checked("${GQ_CHECK_TABLE}" "${GQ_TEST_DIR}/nan_gap.txt"
	1 1 nan 0)
run_merge("${GQ_SOURCE_DIR}/test/data/merge1d_nan_gap.txt"
	-T0/4/0.5 -Sl+g "-G${GQ_TEST_DIR}/nan_gap_bridged.txt" -Vq)
run_checked("${GQ_CHECK_TABLE}" "${GQ_TEST_DIR}/nan_gap_bridged.txt"
	2 1 1 1e-10)
run_merge("${GQ_SOURCE_DIR}/test/data/merge1d_nan_gap.txt"
	-T0/4/0.5 -Sl+g1.5 "-G${GQ_TEST_DIR}/nan_gap_limited.txt" -Vq)
run_checked("${GQ_CHECK_TABLE}" "${GQ_TEST_DIR}/nan_gap_limited.txt"
	2 1 nan 0)
run_merge("${GQ_SOURCE_DIR}/test/data/merge1d_nan_gap.txt"
	-T0/4/0.5 -Sl+g2 "-G${GQ_TEST_DIR}/nan_gap_at_limit.txt" -Vq)
run_checked("${GQ_CHECK_TABLE}" "${GQ_TEST_DIR}/nan_gap_at_limit.txt"
	2 1 1 1e-10)
run_merge("${GQ_SOURCE_DIR}/test/data/merge1d_edge_gap.txt"
	-T0/4/0.5 -Sl+g "-G${GQ_TEST_DIR}/edge_gap.txt" -Vq)
run_checked("${GQ_CHECK_TABLE}" "${GQ_TEST_DIR}/edge_gap.txt"
	0 1 nan 0)
run_checked("${GQ_CHECK_TABLE}" "${GQ_TEST_DIR}/edge_gap.txt"
	8 1 nan 0)
run_merge("${GQ_SOURCE_DIR}/test/data/merge1d_nan_gap.txt"
	-T0/4/0.5 -Ss0.5+g "-G${GQ_TEST_DIR}/smooth_gap.txt" -Vq)
run_merge_fails("${GQ_SOURCE_DIR}/test/data/merge1d_nan_gap.txt"
	-T0/4/0.5 -Sl+g0 "-G${GQ_TEST_DIR}/invalid_gap.txt" -Vq)
run_merge("${GQ_SOURCE_DIR}/test/data/merge1d_sentinel_gap.txt"
	-T0/4/0.5 -Sl -di-99999 "-G${GQ_TEST_DIR}/sentinel_gap.txt" -Vq)
run_checked("${GQ_CHECK_TABLE}" "${GQ_TEST_DIR}/sentinel_gap.txt"
	3 1 nan 0)
run_checked("${GQ_CHECK_TABLE}" "${GQ_TEST_DIR}/sentinel_gap.txt"
	4 1 nan 0)

# Primary/secondary blending, two text fields, and appended merging weight.
run_merge("${GQ_SOURCE_DIR}/test/data/merge1d_basic.merge"
	-T0/4/1 -W "-G${GQ_TEST_DIR}/basic.txt" -Vq)
run_checked("${GQ_CHECK_TABLE}" "${GQ_TEST_DIR}/basic.txt"
	0 1 3.45491502813 1e-10)
run_checked("${GQ_CHECK_TABLE}" "${GQ_TEST_DIR}/basic.txt"
	0 3 0.345491502813 1e-10)
run_checked("${GQ_CHECK_TABLE}" "${GQ_TEST_DIR}/basic.txt"
	2 1 10 1e-10)
run_checked("${GQ_CHECK_TABLE}" "${GQ_TEST_DIR}/basic.txt"
	2 2 100 1e-10)
run_checked("${GQ_CHECK_TABLE}" "${GQ_TEST_DIR}/basic.txt"
	2 3 1 1e-10)

# A paired secondary is restricted to the primary domain.
run_merge("${GQ_SOURCE_DIR}/test/data/merge1d_extended_secondary.merge"
	-T-1/5/1 -W "-G${GQ_TEST_DIR}/extended_secondary.txt")
run_checked("${GQ_CHECK_TABLE}" "${GQ_TEST_DIR}/extended_secondary.txt"
	0 1 nan 0)
run_checked("${GQ_CHECK_TABLE}" "${GQ_TEST_DIR}/extended_secondary.txt"
	6 1 nan 0)
run_merge("${GQ_SOURCE_DIR}/test/data/merge1d_extended_secondary.merge"
	-T-1/5/1 -A -W "-G${GQ_TEST_DIR}/extended_secondary_aggregate.txt")
run_checked("${GQ_CHECK_TABLE}" "${GQ_TEST_DIR}/extended_secondary_aggregate.txt"
	0 1 nan 0)
run_checked("${GQ_CHECK_TABLE}" "${GQ_TEST_DIR}/extended_secondary_aggregate.txt"
	6 1 nan 0)
run_merge("${GQ_SOURCE_DIR}/test/data/merge1d_extended_secondary_fallback.merge"
	-T-1/5/1 -W "-G${GQ_TEST_DIR}/extended_secondary_fallback.txt")
run_checked("${GQ_CHECK_TABLE}" "${GQ_TEST_DIR}/extended_secondary_fallback.txt"
	0 1 0 1e-10)
run_checked("${GQ_CHECK_TABLE}" "${GQ_TEST_DIR}/extended_secondary_fallback.txt"
	6 1 0 1e-10)
run_merge("${GQ_SOURCE_DIR}/test/data/merge1d_extended_secondary_fallback.merge"
	-T-1/5/1 -A -W "-G${GQ_TEST_DIR}/extended_secondary_fallback_aggregate.txt")
run_checked("${GQ_CHECK_TABLE}" "${GQ_TEST_DIR}/extended_secondary_fallback_aggregate.txt"
	0 1 0 1e-10)
run_checked("${GQ_CHECK_TABLE}" "${GQ_TEST_DIR}/extended_secondary_fallback_aggregate.txt"
	6 1 0 1e-10)
run_checked("${GQ_CHECK_TABLE}" "${GQ_TEST_DIR}/extended_secondary_fallback_aggregate.txt"
	0 3 0 1e-10)

# Direct lists default to first-value clobbering; -Co selects the last.
run_merge("${GQ_SOURCE_DIR}/test/data/merge1d_primary1.txt"
	"${GQ_SOURCE_DIR}/test/data/merge1d_primary2.txt"
	-T0/10/1 "-G${GQ_TEST_DIR}/clobber_first.txt" -Vq)
run_checked("${GQ_CHECK_TABLE}" "${GQ_TEST_DIR}/clobber_first.txt"
	5 1 10 1e-10)
run_merge("${GQ_SOURCE_DIR}/test/data/merge1d_primary1.txt"
	"${GQ_SOURCE_DIR}/test/data/merge1d_primary2.txt"
	-T0/10/1 -Co "-G${GQ_TEST_DIR}/clobber_last.txt" -Vq)
run_checked("${GQ_CHECK_TABLE}" "${GQ_TEST_DIR}/clobber_last.txt"
	5 1 20 1e-10)

# Normalized overlaps and secondary-pair validation.
run_merge("${GQ_SOURCE_DIR}/test/data/merge1d_overlap.merge"
	-T0/10/1 -A -W "-G${GQ_TEST_DIR}/overlap.txt" -Vq)
run_checked("${GQ_CHECK_TABLE}" "${GQ_TEST_DIR}/overlap.txt"
	5 1 15 1e-10)
run_checked("${GQ_CHECK_TABLE}" "${GQ_TEST_DIR}/overlap.txt"
	5 2 1 1e-10)
run_merge("${GQ_SOURCE_DIR}/test/data/merge1d_overlap_fallback.merge"
	-T0/10/1 -A -W "-G${GQ_TEST_DIR}/overlap_fallback.txt" -Vq)
run_checked("${GQ_CHECK_TABLE}" "${GQ_TEST_DIR}/overlap_fallback.txt"
	5 1 15 1e-10)
run_checked("${GQ_CHECK_TABLE}" "${GQ_TEST_DIR}/overlap_fallback.txt"
	5 2 1 1e-10)
run_merge("${GQ_SOURCE_DIR}/test/data/merge1d_overlap_three.merge"
	-T0/10/1 -A -W "-G${GQ_TEST_DIR}/overlap_three.txt" -Vq)
run_checked("${GQ_CHECK_TABLE}" "${GQ_TEST_DIR}/overlap_three.txt"
	5 1 23.3333333333 1e-9)
run_checked("${GQ_CHECK_TABLE}" "${GQ_TEST_DIR}/overlap_three.txt"
	5 2 1 1e-10)
file(WRITE "${GQ_TEST_DIR}/overlap_hierarchy.merge"
	"${GQ_SOURCE_DIR}/test/data/merge1d_primary1.txt ${GQ_SOURCE_DIR}/test/data/merge1d_background.txt 0/6 boxcar 0\n"
	"${GQ_SOURCE_DIR}/test/data/merge1d_primary2.txt ${GQ_SOURCE_DIR}/test/data/merge1d_background.txt 4/10 boxcar 0\n"
	"${GQ_SOURCE_DIR}/test/data/merge1d_background.txt ${GQ_SOURCE_DIR}/test/data/merge1d_other_background.txt 0/10 boxcar 0\n"
	"${GQ_SOURCE_DIR}/test/data/merge1d_other_background.txt - - - -\n")
run_merge("${GQ_TEST_DIR}/overlap_hierarchy.merge"
	-T0/10/1 -A -W "-G${GQ_TEST_DIR}/overlap_hierarchy.txt" -Vq)
run_checked("${GQ_CHECK_TABLE}" "${GQ_TEST_DIR}/overlap_hierarchy.txt"
	5 1 15 1e-10)
run_checked("${GQ_CHECK_TABLE}" "${GQ_TEST_DIR}/overlap_hierarchy.txt"
	5 2 1 1e-10)
run_merge_fails("${GQ_SOURCE_DIR}/test/data/merge1d_overlap_mismatch.merge"
	-T0/10/1 -A "-G${GQ_TEST_DIR}/overlap_bad.txt" -Vq)
run_merge("${GQ_SOURCE_DIR}/test/data/merge1d_nonoverlap.merge"
	-T0/10/1 -A "-G${GQ_TEST_DIR}/nonoverlap.txt" -Vq)
run_checked("${GQ_CHECK_TABLE}" "${GQ_TEST_DIR}/nonoverlap.txt"
	2 1 10 1e-10)
run_checked("${GQ_CHECK_TABLE}" "${GQ_TEST_DIR}/nonoverlap.txt"
	8 1 30 1e-10)

# Ordered supports must expose every hierarchy level even when grids share a domain.
foreach(value 10 20 0 100)
	file(WRITE "${GQ_TEST_DIR}/weight_${value}.txt"
		"0 ${value}\n1 ${value}\n2 ${value}\n3 ${value}\n4 ${value}\n5 ${value}\n")
endforeach()
file(WRITE "${GQ_TEST_DIR}/weight_order.merge"
	"${GQ_TEST_DIR}/weight_10.txt ${GQ_TEST_DIR}/weight_0.txt 0/2 cosine 0.25\n"
	"${GQ_TEST_DIR}/weight_20.txt ${GQ_TEST_DIR}/weight_0.txt 2/4 cosine 0.25\n"
	"${GQ_TEST_DIR}/weight_0.txt ${GQ_TEST_DIR}/weight_100.txt 0/5 boxcar 0\n"
	"${GQ_TEST_DIR}/weight_100.txt - - - -\n")
foreach(mode regular aggregate)
	set(aggregate_option)
	if(mode STREQUAL "aggregate")
		set(aggregate_option -A)
	endif()
	foreach(weight_option -W -W+o)
		run_merge("${GQ_TEST_DIR}/weight_order.merge" -T0/5/1
			${aggregate_option} ${weight_option} "-G${GQ_TEST_DIR}/weight_order.txt")
		set(weight_column 2)
		if(weight_option STREQUAL "-W+o")
			set(weight_column 1)
		endif()
		set(overlap_weight 0.75)
		if(mode STREQUAL "aggregate")
			set(overlap_weight 1)
		endif()
		run_checked("${GQ_CHECK_TABLE}" "${GQ_TEST_DIR}/weight_order.txt" 2 ${weight_column} ${overlap_weight} 1e-6)
		run_checked("${GQ_CHECK_TABLE}" "${GQ_TEST_DIR}/weight_order.txt" 4 ${weight_column} 0.75 1e-6)
		run_checked("${GQ_CHECK_TABLE}" "${GQ_TEST_DIR}/weight_order.txt" 5 ${weight_column} 1 1e-6)
	endforeach()
endforeach()

# Multiparameter NetCDF mapping and an undeclared numeric missing sentinel.
run_checked("${NCGEN_EXECUTABLE}" -o "${GQ_TEST_DIR}/primary_multi.nc"
	"${GQ_SOURCE_DIR}/test/data/primary_multi_1d.cdl")
run_checked("${NCGEN_EXECUTABLE}" -o "${GQ_TEST_DIR}/secondary_multi.nc"
	"${GQ_SOURCE_DIR}/test/data/secondary_multi_1d.cdl")
file(WRITE "${GQ_TEST_DIR}/multi.merge"
	"${GQ_TEST_DIR}/primary_multi.nc?vp,vs+n-99999 "
	"${GQ_TEST_DIR}/secondary_multi.nc?p,s 0/4 boxcar 0\n")
run_merge("${GQ_TEST_DIR}/multi.merge" -T0/4/1 -Fvp,vs -W
	"-G${GQ_TEST_DIR}/multi.nc" -Vq)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/multi.nc"
	vp 2 nan 0 vp)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/multi.nc"
	vs 2 20 1e-10 vs)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/multi.nc"
	weight 2 1 1e-10 "merging weight")

# Paired-secondary filling is opt-in and follows internal-gap interpolation.
run_merge("${GQ_TEST_DIR}/multi.merge" -T0/4/1 -Fvp,vs -P
	"-G${GQ_TEST_DIR}/multi_pair_fill.nc" -Vq)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/multi_pair_fill.nc"
	vp 2 2 1e-10 vp)
run_merge("${GQ_TEST_DIR}/multi.merge" -T0/4/1 -Fvp,vs -P -Sl+g
	"-G${GQ_TEST_DIR}/multi_interpolated.nc" -Vq)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/multi_interpolated.nc"
	vp 2 10 1e-10 vp)
run_merge("${GQ_TEST_DIR}/multi.merge" -T0/4/1 -Fvp,vs -A
	"-G${GQ_TEST_DIR}/multi_aggregate.nc" -Vq)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/multi_aggregate.nc"
	vp 2 nan 0 vp)
run_merge("${GQ_TEST_DIR}/multi.merge" -T0/4/1 -Fvp,vs -A -P
	"-G${GQ_TEST_DIR}/multi_aggregate_pair_fill.nc" -Vq)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/multi_aggregate_pair_fill.nc"
	vp 2 2 1e-10 vp)

# Geometry-only NetCDF output does not require -F.
run_merge("${GQ_TEST_DIR}/multi.merge" -T0/4/1 -W+o
	"-G${GQ_TEST_DIR}/weight_only.nc" -Vq)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/weight_only.nc"
	weight 2 1 1e-10 "merging weight")

# Per-source transforms precede interpolation; output transforms occur at write time.
run_merge("${GQ_TEST_DIR}/primary_multi.nc?vp,vs+n-99999+x2+Xm+v3,4+Vm/s,m/s"
	-T0/8/2 -Fvp,vs
	"-Z+x-0.5+Xkm+v10,100+Vfinal_vp,final_vs"
	"-G${GQ_TEST_DIR}/scaled.nc" -Vq)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/scaled.nc"
	x 0 0 1e-10)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/scaled.nc"
	x 4 -4 1e-10)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/scaled.nc"
	vp 0 300 1e-10 vp)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/scaled.nc"
	vp 2 nan 0 vp)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/scaled.nc"
	vs 0 8000 1e-10 vs)
run_checked("${GQ_CHECK_ATTRIBUTE}" "${GQ_TEST_DIR}/scaled.nc" x units km)
run_checked("${GQ_CHECK_ATTRIBUTE}" "${GQ_TEST_DIR}/scaled.nc" vp units final_vp)
run_checked("${GQ_CHECK_ATTRIBUTE}" "${GQ_TEST_DIR}/scaled.nc" vs units final_vs)

# A negative output-axis scale writes decreasing coordinates without changing
# the row order of fields.
file(WRITE "${GQ_TEST_DIR}/output_order_input.txt" "0 10\n1 20\n2 30\n")
run_merge("${GQ_TEST_DIR}/output_order_input.txt" -T0/2/1 "-Z+x-1"
	"-G${GQ_TEST_DIR}/output_order.txt" -Vq)
run_checked("${GQ_CHECK_TABLE}" "${GQ_TEST_DIR}/output_order.txt" 0 0 0 1e-10)
run_checked("${GQ_CHECK_TABLE}" "${GQ_TEST_DIR}/output_order.txt" 0 1 10 1e-10)
run_checked("${GQ_CHECK_TABLE}" "${GQ_TEST_DIR}/output_order.txt" 2 0 -2 1e-10)
run_checked("${GQ_CHECK_TABLE}" "${GQ_TEST_DIR}/output_order.txt" 2 1 30 1e-10)

# Input scaling preserves sample order. It must make a decreasing source axis
# increase before interpolation; output -Z may then restore that convention.
run_checked("${NCGEN_EXECUTABLE}" -o "${GQ_TEST_DIR}/decreasing_axis.nc"
	"${GQ_SOURCE_DIR}/test/data/merge1d_decreasing_axis.cdl")
execute_process(
	COMMAND "${CMAKE_COMMAND}" -E env "GQ_PLUGIN=${GQ_PLUGIN}"
		"${GQ_RUNNER}" "${GQ_TEST_DIR}/decreasing_axis.nc?vp"
		-T-16000/4000/10000 "-G${GQ_TEST_DIR}/invalid_axis.nc"
	RESULT_VARIABLE status
	ERROR_VARIABLE error)
if(status EQUAL 0 OR NOT error MATCHES "Input \\+x scaling does not reorder samples")
	message(FATAL_ERROR
		"merge1d did not reject a decreasing transformed axis with scaling guidance:\n${error}")
endif()
run_merge("${GQ_TEST_DIR}/decreasing_axis.nc?vp+x-1/1000+Xkm"
	-T-4/16/10 -Fvp "-Z+x-1000+Xm"
	"-G${GQ_TEST_DIR}/restored_axis.nc" -Vq)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/restored_axis.nc"
	depth 0 4000 1e-10)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/restored_axis.nc"
	depth 2 -16000 1e-10)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/restored_axis.nc"
	vp 0 4 1e-10 vp)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/restored_axis.nc"
	vp 2 16 1e-10 vp)

# Text may be packaged as named NetCDF variables.
run_merge("${GQ_SOURCE_DIR}/test/data/merge1d_basic.merge"
	-T0/4/1 -Ffirst,second -W "-G${GQ_TEST_DIR}/text_to_netcdf.nc" -Vq)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/text_to_netcdf.nc"
	first 2 10 1e-10 first)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/text_to_netcdf.nc"
	second 2 100 1e-10 second)

# Standard input may contain either a direct data table or a mergefile.
execute_process(
	COMMAND "${CMAKE_COMMAND}" -E env "GQ_PLUGIN=${GQ_PLUGIN}"
		"${GQ_RUNNER}" -T0/4/1 -Sl "-G${GQ_TEST_DIR}/stdin_table.txt" -Vq
	INPUT_FILE "${GQ_SOURCE_DIR}/test/data/merge1d_irregular.txt"
	WORKING_DIRECTORY "${GQ_SOURCE_DIR}"
	RESULT_VARIABLE stdin_table_status
)
if(NOT stdin_table_status EQUAL 0)
	message(FATAL_ERROR "merge1d failed to read a direct table from standard input")
endif()
run_checked("${GQ_CHECK_TABLE}" "${GQ_TEST_DIR}/stdin_table.txt"
	1 1 2 1e-10)

execute_process(
	COMMAND "${CMAKE_COMMAND}" -E env "GQ_PLUGIN=${GQ_PLUGIN}"
		"${GQ_RUNNER}" -T0/4/1 -W "-G${GQ_TEST_DIR}/stdin_merge.txt" -Vq
	INPUT_FILE "${GQ_SOURCE_DIR}/test/data/merge1d_basic.merge"
	WORKING_DIRECTORY "${GQ_SOURCE_DIR}"
	RESULT_VARIABLE stdin_merge_status
)
if(NOT stdin_merge_status EQUAL 0)
	message(FATAL_ERROR "merge1d failed to read a mergefile from standard input")
endif()
run_checked("${GQ_CHECK_TABLE}" "${GQ_TEST_DIR}/stdin_merge.txt"
	2 1 10 1e-10)

# Invalid coordinate ordering, mixed formats, and -A without supports fail.
run_merge_fails("${GQ_SOURCE_DIR}/test/data/merge1d_bad_coordinates.txt"
	-T0/2/1 "-G${GQ_TEST_DIR}/bad_coordinates.txt" -Vq)
run_merge_fails("${GQ_SOURCE_DIR}/test/data/merge1d_primary1.txt"
	"${GQ_TEST_DIR}/primary_multi.nc?vp"
	-T0/4/1 "-G${GQ_TEST_DIR}/mixed.txt" -Vq)
run_merge_fails("${GQ_SOURCE_DIR}/test/data/merge1d_primary1.txt"
	"${GQ_SOURCE_DIR}/test/data/merge1d_primary2.txt"
	-T0/10/1 -A "-G${GQ_TEST_DIR}/direct_aggregate.txt" -Vq)
