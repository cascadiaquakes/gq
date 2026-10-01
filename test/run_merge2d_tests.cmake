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
		RESULT_VARIABLE status
		OUTPUT_VARIABLE output
		ERROR_VARIABLE error
	)
	if(NOT status EQUAL 0)
		message(FATAL_ERROR "merge2d failed (${status}): ${ARGN}\n${output}\n${error}")
	endif()
endfunction()

run_checked("${NCGEN_EXECUTABLE}" -o "${GQ_TEST_DIR}/primary_multi.nc"
	"${GQ_SOURCE_DIR}/test/data/primary_multi.cdl")
run_checked("${NCGEN_EXECUTABLE}" -o "${GQ_TEST_DIR}/secondary_multi.nc"
	"${GQ_SOURCE_DIR}/test/data/secondary_multi.cdl")
run_checked("${NCGEN_EXECUTABLE}" -o "${GQ_TEST_DIR}/slice_inputs.nc"
	"${GQ_SOURCE_DIR}/test/data/primary_multi_3d.cdl")
run_checked("${NCGEN_EXECUTABLE}" -o "${GQ_TEST_DIR}/gaps2d.nc"
	"${GQ_SOURCE_DIR}/test/data/gaps2d.cdl")

# Fill enclosed holes field by field, but preserve gaps connected to an edge.
file(WRITE "${GQ_TEST_DIR}/gaps2d.merge2d"
	"${GQ_TEST_DIR}/gaps2d.nc - - - -\n")
foreach(gap_method IN ITEMS n l a3/4 s0 m0)
	string(REPLACE "/" "_" gap_name "${gap_method}")
	run_merge("${GQ_TEST_DIR}/gaps2d.merge2d" -R0/6/0/6 -I1
		"-H${gap_method}" "-G${GQ_TEST_DIR}/gaps_${gap_name}.nc" -Vq)
	run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/gaps_${gap_name}.nc"
		z 4 4 10 0.05)
	run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/gaps_${gap_name}.nc"
		z 3 0 nan 0)
endforeach()

# +m limits filling by each hole's x and y spans in grid nodes.
run_merge("${GQ_TEST_DIR}/gaps2d.merge2d" -R0/6/0/6 -I1 -H+m1
	"-G${GQ_TEST_DIR}/gaps_limited.nc" -Vq)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/gaps_limited.nc"
	z 2 2 10 1e-4)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/gaps_limited.nc"
	z 4 4 nan 0)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/gaps_limited.nc"
	z 3 0 nan 0)

# A 3-D NetCDF variable can supply a selected 2-D layer directly.
run_merge("${GQ_TEST_DIR}/slice_inputs.nc?vp[1]"
	"${GQ_TEST_DIR}/slice_inputs.nc?vp[0]" -R0/2/0/2 -I1 -Fvp
	"-G${GQ_TEST_DIR}/slice_index.nc" -Vq)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/slice_index.nc"
	vp 1 1 112 1e-6)
run_merge("${GQ_TEST_DIR}/slice_inputs.nc?vp(-1900)"
	"${GQ_TEST_DIR}/slice_inputs.nc?vp(0)" -R0/2/0/2 -I1
	"-G${GQ_TEST_DIR}/slice_level.nc" -Vq)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/slice_level.nc"
	vp 1 1 122 1e-6)
run_merge("${GQ_TEST_DIR}/slice_inputs.nc?vp[1]"
	"${GQ_TEST_DIR}/slice_inputs.nc?vp[0]" -R0/2/0/2 -I0.5 -Fvp -nl
	"-G${GQ_TEST_DIR}/slice_resampled.nc" -Vq)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/slice_resampled.nc"
	vp 2 2 112 1e-6)
run_merge("${GQ_TEST_DIR}/slice_inputs.nc?vp[1],den[1]"
	"${GQ_TEST_DIR}/slice_inputs.nc?vp[0],den[0]" -R0/2/0/2 -I1
	-Ffirst,second "-G${GQ_TEST_DIR}/slice_multi.nc" -Vq)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/slice_multi.nc"
	first 1 1 112 1e-6)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/slice_multi.nc"
	second 1 1 224 1e-6)
file(WRITE "${GQ_TEST_DIR}/slice_pair.merge2d"
	"${GQ_TEST_DIR}/slice_inputs.nc?vp[1] ${GQ_TEST_DIR}/slice_inputs.nc?vp(0) - boxcar/boxcar 0\n")
run_merge("${GQ_TEST_DIR}/slice_pair.merge2d" -R0/2/0/2 -I1 -Fvp
	"-G${GQ_TEST_DIR}/slice_pair.nc" -Vq)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/slice_pair.nc"
	vp 1 1 112 1e-6)

# Coordinate scaling precedes coordinate-value layer selection. Index selection
# remains positional, and output transforms are applied only after merging.
# Negative output scales leave the field in its original row and column order.
file(WRITE "${GQ_TEST_DIR}/slice_scaled.merge2d"
	"${GQ_TEST_DIR}/slice_inputs.nc?vp(1)+z-0.001+Zkm+x2+Xm+y2+Ym+v2+Vinput "
	"${GQ_TEST_DIR}/slice_inputs.nc?vp[0]+z100+x2+Xm+y2+Ym+v1+Vinput "
	"- boxcar/boxcar 0\n")
run_merge("${GQ_TEST_DIR}/slice_scaled.merge2d" -R0/4/0/4 -I2 -Fvp
	"-Z+x-0.5+Xkm+y-0.5+Ykm+v10+Vfinal"
	"-G${GQ_TEST_DIR}/slice_scaled.nc" -Vq)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/slice_scaled.nc"
	vp 1 1 2240 1e-5)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/slice_scaled.nc"
	vp 0 0 2200 1e-5)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/slice_scaled.nc"
	vp 2 2 2280 1e-5)
run_checked("${GQ_CHECK_COORD}" "${GQ_TEST_DIR}/slice_scaled.nc" x 0 0 1e-9)
run_checked("${GQ_CHECK_COORD}" "${GQ_TEST_DIR}/slice_scaled.nc" x 2 -2 1e-9)
run_checked("${GQ_CHECK_COORD}" "${GQ_TEST_DIR}/slice_scaled.nc" y 0 0 1e-9)
run_checked("${GQ_CHECK_COORD}" "${GQ_TEST_DIR}/slice_scaled.nc" y 2 -2 1e-9)
run_checked("${GQ_CHECK_ATTRIBUTE}" "${GQ_TEST_DIR}/slice_scaled.nc" x units km)
run_checked("${GQ_CHECK_ATTRIBUTE}" "${GQ_TEST_DIR}/slice_scaled.nc" y units km)
run_checked("${GQ_CHECK_ATTRIBUTE}" "${GQ_TEST_DIR}/slice_scaled.nc" vp units final)

# Retained input grid axes must remain increasing; negative input x/y scales
# are rejected instead of reordering grid data. Negative output -Z scales are
# covered above and preserve final row and column order.
execute_process(
	COMMAND "${CMAKE_COMMAND}" -E env "GQ_PLUGIN=${GQ_PLUGIN}"
		"${GQ_RUNNER}"
		"${GQ_TEST_DIR}/primary_multi.nc?vp+x-1"
		"${GQ_TEST_DIR}/primary_multi.nc?vp+x-1"
		-R-4/0/0/4 -I1 "-G${GQ_TEST_DIR}/invalid_x_scale.nc"
	RESULT_VARIABLE status
	ERROR_VARIABLE error)
if(status EQUAL 0 OR NOT error MATCHES "input \\+x scaling does not reorder grid data")
	message(FATAL_ERROR
		"merge2d did not reject a decreasing transformed x axis with scaling guidance:\n${error}")
endif()

# A single-variable grid can use transforms without naming its data variable.
run_merge("${GQ_TEST_DIR}/gaps2d.nc?+x2+Xm+y2+Ym+v3+Vscaled"
	"${GQ_TEST_DIR}/gaps2d.nc?+x2+Xm+y2+Ym+v3+Vscaled"
	-R0/12/0/12 -I2 "-G${GQ_TEST_DIR}/single_scaled.nc" -Vq)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/single_scaled.nc"
	z 1 1 30 1e-5)

file(WRITE "${GQ_TEST_DIR}/multi.merge2d"
	"${GQ_TEST_DIR}/primary_multi.nc?vp,vs,den+n-99999 "
	"${GQ_TEST_DIR}/secondary_multi.nc?p,s,d\n")
run_merge("${GQ_TEST_DIR}/multi.merge2d" -R0/4/0/4 -I1 -Fvp,vs,rho -W
	"-G${GQ_TEST_DIR}/multi.nc" -Vq)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/multi.nc" vp 2 2 nan 0 vp)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/multi.nc" vs 2 2 20 1e-6 vs)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/multi.nc" rho 2 2 30 1e-6 rho)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/multi.nc" weight 2 2 1 1e-6 "merging weight")

file(WRITE "${GQ_TEST_DIR}/multi_scaled.merge2d"
	"${GQ_TEST_DIR}/primary_multi.nc?vp,vs,den+n-99999+v2,3,4+Vpin,sin,din "
	"${GQ_TEST_DIR}/secondary_multi.nc?p,s,d+v1,1,1+Vpin,sin,din\n")
run_merge("${GQ_TEST_DIR}/multi_scaled.merge2d" -R0/4/0/4 -I1 -Fvp,vs,rho
	"-Z+v10,100,1000+Vfinal_vp,final_vs,final_rho"
	"-G${GQ_TEST_DIR}/multi_scaled.nc" -Vq)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/multi_scaled.nc" vp 1 1 200 1e-5)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/multi_scaled.nc" vs 1 1 6000 1e-4)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/multi_scaled.nc" rho 1 1 120000 1e-3)
run_checked("${GQ_CHECK_ATTRIBUTE}" "${GQ_TEST_DIR}/multi_scaled.nc" vp units final_vp)
run_checked("${GQ_CHECK_ATTRIBUTE}" "${GQ_TEST_DIR}/multi_scaled.nc" vs units final_vs)
run_checked("${GQ_CHECK_ATTRIBUTE}" "${GQ_TEST_DIR}/multi_scaled.nc" rho units final_rho)

# Paired-secondary filling is opt-in, including for aggregate merging.
run_merge("${GQ_TEST_DIR}/multi.merge2d" -R0/4/0/4 -I1 -Fvp,vs,rho -P
	"-G${GQ_TEST_DIR}/multi_pair_fill.nc" -Vq)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/multi_pair_fill.nc" vp 2 2 2 1e-6 vp)
run_merge("${GQ_TEST_DIR}/multi.merge2d" -R0/4/0/4 -I1 -Fvp,vs,rho -A
	"-G${GQ_TEST_DIR}/multi_aggregate.nc" -Vq)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/multi_aggregate.nc" vp 2 2 nan 0 vp)
run_merge("${GQ_TEST_DIR}/multi.merge2d" -R0/4/0/4 -I1 -Fvp,vs,rho -A -P
	"-G${GQ_TEST_DIR}/multi_aggregate_pair_fill.nc" -Vq)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/multi_aggregate_pair_fill.nc" vp 2 2 2 1e-6 vp)

# GMT -di sentinels become NaN before blending and internal resampling.
file(WRITE "${GQ_TEST_DIR}/sentinel_pair.merge2d"
	"${GQ_TEST_DIR}/primary_multi.nc?vp ${GQ_TEST_DIR}/secondary_multi.nc?p\n")
run_merge("${GQ_TEST_DIR}/sentinel_pair.merge2d" -R0/4/0/4 -I1 -Fvp
	-di-99999 "-G${GQ_TEST_DIR}/sentinel_pair.nc" -Vq)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/sentinel_pair.nc"
	vp 2 2 nan 0)
run_merge("${GQ_TEST_DIR}/sentinel_pair.merge2d" -R0/4/0/4 -I1 -Fvp
	-di-99999 -P "-G${GQ_TEST_DIR}/sentinel_pair_fill.nc" -Vq)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/sentinel_pair_fill.nc"
	vp 2 2 2 1e-6)
run_merge("${GQ_TEST_DIR}/sentinel_pair.merge2d" -R0/4/0/4 -I1 -Fvp
	-di-99999 -Hl -P "-G${GQ_TEST_DIR}/sentinel_interpolated.nc" -Vq)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/sentinel_interpolated.nc"
	vp 2 2 10 1e-6)
run_merge("${GQ_TEST_DIR}/sentinel_pair.merge2d" -R0/4/0/4 -I0.5 -Fvp
	-di-99999 -nl "-G${GQ_TEST_DIR}/sentinel_resampled.nc" -Vq)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/sentinel_resampled.nc"
	vp 4 3 10 1e-6)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/sentinel_resampled.nc"
	vp 4 4 nan 0)
file(WRITE "${GQ_TEST_DIR}/sentinel_only.merge2d"
	"${GQ_TEST_DIR}/primary_multi.nc?vp - - - -\n")
run_merge("${GQ_TEST_DIR}/sentinel_only.merge2d" -R0/4/0/4 -I1 -Fvp
	-di-99999 "-G${GQ_TEST_DIR}/sentinel_only.nc" -Vq)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/sentinel_only.nc"
	vp 2 2 nan 0)

run_merge("${GQ_TEST_DIR}/multi.merge2d" -R0/4/0/4 -I1 -W+o
	"-G${GQ_TEST_DIR}/weight_only.nc" -Vq)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/weight_only.nc"
	weight 2 2 1 1e-6 "merging weight")

run_checked("${GMT_EXECUTABLE}" grdmath -R0/6/0/4 -I1 10 =
	"${GQ_TEST_DIR}/primary1.nc")
run_checked("${GMT_EXECUTABLE}" grdmath -R4/10/0/4 -I1 20 =
	"${GQ_TEST_DIR}/primary2.nc")
run_checked("${GMT_EXECUTABLE}" grdmath -R7/10/0/4 -I1 30 =
	"${GQ_TEST_DIR}/primary3.nc")
run_checked("${GMT_EXECUTABLE}" grdmath -R2/8/0/4 -I1 40 =
	"${GQ_TEST_DIR}/primary_overlap3.nc")
run_checked("${GMT_EXECUTABLE}" grdmath -R0/10/0/4 -I1 0 =
	"${GQ_TEST_DIR}/background.nc")
run_checked("${GMT_EXECUTABLE}" grdmath -R0/10/0/4 -I1 100 =
	"${GQ_TEST_DIR}/other_background.nc")

# Ordered support weights must not be hidden by wider primary grid extents.
foreach(value 10 20 0 100)
	run_checked("${GMT_EXECUTABLE}" grdmath -R0/6/0/6 -I1 ${value} =
		"${GQ_TEST_DIR}/weight_${value}.nc")
endforeach()
file(WRITE "${GQ_TEST_DIR}/weight_first.txt" "0 0\n2 0\n2 2\n0 2\n0 0\n")
file(WRITE "${GQ_TEST_DIR}/weight_second.txt" "1 1\n3 1\n3 3\n1 3\n1 1\n")
file(WRITE "${GQ_TEST_DIR}/weight_order.merge"
	"${GQ_TEST_DIR}/weight_10.nc ${GQ_TEST_DIR}/weight_0.nc ${GQ_TEST_DIR}/weight_first.txt cosine/cosine 0.25\n"
	"${GQ_TEST_DIR}/weight_20.nc ${GQ_TEST_DIR}/weight_0.nc ${GQ_TEST_DIR}/weight_second.txt cosine/cosine 0.25\n"
	"${GQ_TEST_DIR}/weight_0.nc ${GQ_TEST_DIR}/weight_100.nc - boxcar/boxcar 0\n"
	"${GQ_TEST_DIR}/weight_100.nc - - - -\n")
foreach(mode regular aggregate)
	set(aggregate_option)
	set(overlap_weight 0.119364378516)
	if(mode STREQUAL "aggregate")
		set(aggregate_option -A)
		set(overlap_weight 1)
	endif()
	foreach(weight_option -W -W+o)
		run_merge("${GQ_TEST_DIR}/weight_order.merge" -R0/6/0/6 -I1
			${aggregate_option} ${weight_option} "-G${GQ_TEST_DIR}/weight_order.nc")
		run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/weight_order.nc" weight 3 3 ${overlap_weight} 1e-6)
		run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/weight_order.nc" weight 4 4 0.119364378516 1e-6)
		run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/weight_order.nc" weight 6 0 1 1e-6)
	endforeach()
endforeach()

# Two partial tapers retain their sum below one.
file(WRITE "${GQ_TEST_DIR}/weight_second.txt" "0 0\n2 0\n2 2\n0 2\n0 0\n")
run_merge("${GQ_TEST_DIR}/weight_order.merge" -R0/6/0/6 -I1 -A -W+o
	"-G${GQ_TEST_DIR}/weight_partial.nc")
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/weight_partial.nc" weight 3 3 0.238728757032 1e-6)
file(WRITE "${GQ_TEST_DIR}/weight_second.txt" "1 1\n3 1\n3 3\n1 3\n1 1\n")

# A point outside the triangle must use the next support, even inside its box.
file(WRITE "${GQ_TEST_DIR}/weight_first.txt" "0 0\n2 0\n0 2\n0 0\n")
run_merge("${GQ_TEST_DIR}/weight_order.merge" -R0/6/0/6 -I1 -W+o
	"-G${GQ_TEST_DIR}/weight_triangle.nc")
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/weight_triangle.nc" weight 2 2 1 1e-6)

# A paired secondary is restricted to the primary grid domain.
run_checked("${GMT_EXECUTABLE}" grdmath -R1/3/1/3 -I1 10 =
	"${GQ_TEST_DIR}/domain_primary.nc")
run_checked("${GMT_EXECUTABLE}" grdmath -R0/4/0/4 -I1 0 =
	"${GQ_TEST_DIR}/domain_secondary.nc")
file(WRITE "${GQ_TEST_DIR}/domain_pair.merge2d"
	"${GQ_TEST_DIR}/domain_primary.nc ${GQ_TEST_DIR}/domain_secondary.nc\n")
run_merge("${GQ_TEST_DIR}/domain_pair.merge2d" -R0/4/0/4 -I1
	"-G${GQ_TEST_DIR}/domain_pair.nc" -Vq)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/domain_pair.nc"
	z 0 0 nan 0)
run_merge("${GQ_TEST_DIR}/domain_pair.merge2d" -R0/4/0/4 -I1 -A
	"-G${GQ_TEST_DIR}/domain_pair_aggregate.nc" -Vq)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/domain_pair_aggregate.nc"
	z 0 0 nan 0)
file(WRITE "${GQ_TEST_DIR}/domain_fallback.merge2d"
	"${GQ_TEST_DIR}/domain_primary.nc ${GQ_TEST_DIR}/domain_secondary.nc\n"
	"${GQ_TEST_DIR}/domain_secondary.nc - - - -\n")
run_merge("${GQ_TEST_DIR}/domain_fallback.merge2d" -R0/4/0/4 -I1
	"-G${GQ_TEST_DIR}/domain_fallback.nc" -Vq)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/domain_fallback.nc"
	z 0 0 0 1e-6)
run_merge("${GQ_TEST_DIR}/domain_fallback.merge2d" -R0/4/0/4 -I1 -A -W
	"-G${GQ_TEST_DIR}/domain_fallback_aggregate.nc" -Vq)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/domain_fallback_aggregate.nc"
	z 0 0 0 1e-6)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/domain_fallback_aggregate.nc"
	weight 0 0 0 1e-6)

file(WRITE "${GQ_TEST_DIR}/overlap.merge2d"
	"${GQ_TEST_DIR}/primary1.nc ${GQ_TEST_DIR}/background.nc\n"
	"${GQ_TEST_DIR}/primary2.nc ${GQ_TEST_DIR}/background.nc\n")
run_merge("${GQ_TEST_DIR}/overlap.merge2d" -R0/10/0/4 -I1 -A
	"-G${GQ_TEST_DIR}/overlap.nc" -Vq)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/overlap.nc" z 2 5 15 1e-5)
file(WRITE "${GQ_TEST_DIR}/overlap_three.merge2d"
	"${GQ_TEST_DIR}/primary1.nc ${GQ_TEST_DIR}/background.nc\n"
	"${GQ_TEST_DIR}/primary2.nc ${GQ_TEST_DIR}/background.nc\n"
	"${GQ_TEST_DIR}/primary_overlap3.nc ${GQ_TEST_DIR}/background.nc\n"
	"${GQ_TEST_DIR}/background.nc - - - -\n")
run_merge("${GQ_TEST_DIR}/overlap_three.merge2d" -R0/10/0/4 -I1 -A -W
	"-G${GQ_TEST_DIR}/overlap_three.nc" -Vq)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/overlap_three.nc"
	z 2 5 23.3333333333 1e-5)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/overlap_three.nc"
	weight 2 5 1 1e-6)
file(WRITE "${GQ_TEST_DIR}/overlap_hierarchy.merge2d"
	"${GQ_TEST_DIR}/primary1.nc ${GQ_TEST_DIR}/background.nc\n"
	"${GQ_TEST_DIR}/primary2.nc ${GQ_TEST_DIR}/background.nc\n"
	"${GQ_TEST_DIR}/background.nc ${GQ_TEST_DIR}/other_background.nc\n"
	"${GQ_TEST_DIR}/other_background.nc - - - -\n")
run_merge("${GQ_TEST_DIR}/overlap_hierarchy.merge2d" -R0/10/0/4 -I1 -A -W
	"-G${GQ_TEST_DIR}/overlap_hierarchy.nc" -Vq)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/overlap_hierarchy.nc"
	z 2 5 15 1e-5)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/overlap_hierarchy.nc"
	weight 2 5 1 1e-6)

file(WRITE "${GQ_TEST_DIR}/overlap_bad.merge2d"
	"${GQ_TEST_DIR}/primary1.nc ${GQ_TEST_DIR}/background.nc\n"
	"${GQ_TEST_DIR}/primary2.nc ${GQ_TEST_DIR}/other_background.nc\n")
execute_process(
	COMMAND "${CMAKE_COMMAND}" -E env "GQ_PLUGIN=${GQ_PLUGIN}"
		"${GQ_RUNNER}" "${GQ_TEST_DIR}/overlap_bad.merge2d"
		-R0/10/0/4 -I1 -A "-G${GQ_TEST_DIR}/overlap_bad.nc" -Vq
	RESULT_VARIABLE overlap_bad_status
)
if(overlap_bad_status EQUAL 0)
	message(FATAL_ERROR "overlapping primaries with different secondary grids were accepted")
endif()

file(WRITE "${GQ_TEST_DIR}/nonoverlap.merge2d"
	"${GQ_TEST_DIR}/primary1.nc ${GQ_TEST_DIR}/background.nc\n"
	"${GQ_TEST_DIR}/primary3.nc ${GQ_TEST_DIR}/other_background.nc\n")
run_merge("${GQ_TEST_DIR}/nonoverlap.merge2d" -R0/10/0/4 -I1 -A
	"-G${GQ_TEST_DIR}/nonoverlap.nc" -Vq)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/nonoverlap.nc" z 2 2 10 1e-5)
run_checked("${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/nonoverlap.nc" z 2 8 30 1e-5)

run_checked("${GMT_EXECUTABLE}" grdmath -R0/4/0/4 -I1 10 =
	"${GQ_TEST_DIR}/mono_primary.nc")
run_checked("${GMT_EXECUTABLE}" grdmath -R0/4/0/4 -I1 0 =
	"${GQ_TEST_DIR}/mono_secondary.nc")
file(COPY "${GQ_SOURCE_DIR}/test/data/nonmonotone.txt" DESTINATION "${GQ_TEST_DIR}")
file(WRITE "${GQ_TEST_DIR}/nonmonotone.merge2d"
	"${GQ_TEST_DIR}/mono_primary.nc ${GQ_TEST_DIR}/mono_secondary.nc "
	"${GQ_TEST_DIR}/nonmonotone.txt cosine/cosine 0.1/0.2/0.3/0.4\n")
run_merge("${GQ_TEST_DIR}/nonmonotone.merge2d" -R0/4/0/4 -I1 -ME+w
	"-G${GQ_TEST_DIR}/monotone.nc" -Vq)
if(NOT EXISTS "${GQ_TEST_DIR}/nonmonotone_monotone.txt")
	message(FATAL_ERROR "-ME+w did not write the converted polygon")
endif()
