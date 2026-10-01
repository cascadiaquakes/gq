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

foreach(fixture model1d model2d model3d descending3d gaps2d gaps3d)
	execute_process(
		COMMAND "${NCGEN_EXECUTABLE}" -o "${GQ_TEST_DIR}/${fixture}.nc"
		        "${GQ_SOURCE_DIR}/test/data/ssh_${fixture}.cdl"
		RESULT_VARIABLE status
	)
	if(status)
		message(FATAL_ERROR "ncgen failed for SSH ${fixture}")
	endif()
endforeach()

function(run_ssh module)
	execute_process(
		COMMAND "${CMAKE_COMMAND}" -E env
		        "GQ_PLUGIN=${GQ_PLUGIN}"
		        "${GQ_RUNNER}" "${module}" ${ARGN}
		RESULT_VARIABLE status
	)
	if(status)
		message(FATAL_ERROR "${module} run failed: ${ARGN}")
	endif()
endfunction()

function(check_stats file variable mean std tolerance)
	execute_process(
		COMMAND "${GQ_CHECK_SSH}" "${GQ_TEST_DIR}/${file}.nc"
		        "${variable}" "${mean}" "${std}" "${tolerance}"
		RESULT_VARIABLE status
	)
	if(status)
		message(FATAL_ERROR
		        "${file}?${variable} statistics do not match ${mean}/${std}")
	endif()
endfunction()

# 1-D generation, reproducibility, tapering, and text output/application.
run_ssh(ssh1d
	"-G${GQ_TEST_DIR}/one.nc" -T0/100/1 -D0.05 -C10 -U0.3 -Q42 -Vq)
check_stats(one heterogeneity 0 0.05 1e-5)
run_ssh(ssh1d
	"-G${GQ_TEST_DIR}/one_repeat.nc" -T0/100/1 -D0.05 -C10 -U0.3 -Q42 -Vq)
execute_process(
	COMMAND "${CMAKE_COMMAND}" -E compare_files
	        "${GQ_TEST_DIR}/one.nc" "${GQ_TEST_DIR}/one_repeat.nc"
	RESULT_VARIABLE status
)
if(status)
	message(FATAL_ERROR "ssh1d is not reproducible for a fixed seed")
endif()

# Omitting -U must use the documented shared default Hurst exponent of 0.15.
run_ssh(ssh1d
	"-G${GQ_TEST_DIR}/default_hurst.nc" -T0/100/1 -D0.05 -C10 -Q42 -Vq)
run_ssh(ssh1d
	"-G${GQ_TEST_DIR}/explicit_hurst.nc" -T0/100/1 -D0.05 -C10 -U0.15 -Q42 -Vq)
execute_process(
	COMMAND "${CMAKE_COMMAND}" -E compare_files
	        "${GQ_TEST_DIR}/default_hurst.nc" "${GQ_TEST_DIR}/explicit_hurst.nc"
	RESULT_VARIABLE status
)
if(status)
	message(FATAL_ERROR "Default SSH Hurst exponent is not 0.15")
endif()

run_ssh(ssh1d
	"-G${GQ_TEST_DIR}/one_taper.nc" -T0/100/1 -D0.05 -C10 -Q42
	"-Wcosine+r0.2/0.2+w" -Vq)
foreach(check "0;0.001510978" "50;1" "100;0.001510978")
	list(GET check 0 index)
	list(GET check 1 expected)
	execute_process(
		COMMAND "${GQ_CHECK_3D}" "${GQ_TEST_DIR}/one_taper.nc"
		        weight "${index}" "${expected}" 1e-6
		RESULT_VARIABLE status
	)
	if(status)
		message(FATAL_ERROR "ssh1d taper weight check failed: ${check}")
	endif()
endforeach()

run_ssh(ssh1d
	"-G${GQ_TEST_DIR}/one.txt" -T0/10/1 -D0.05 -C2 -Q42 -Vq)
file(STRINGS "${GQ_TEST_DIR}/one.txt" one_lines)
list(LENGTH one_lines one_count)
if(NOT one_count EQUAL 11)
	message(FATAL_ERROR "ssh1d text output has ${one_count} rows")
endif()
run_ssh(ssh1d
	"${GQ_SOURCE_DIR}/test/data/ssh_model1d.txt" -A -Fa,b
	"-G${GQ_TEST_DIR}/one_applied.txt" -D0.05 -C2 -Q42 -Vq)

# Vertical/axis interpolation keeps internal gaps unless +g requests bridging.
file(WRITE "${GQ_TEST_DIR}/one_gap.txt"
     "0 10\n1 10\n2 NaN\n3 10\n4 10\n")
run_ssh(ssh1d
	"${GQ_TEST_DIR}/one_gap.txt" -A -Fv
	"-G${GQ_TEST_DIR}/one_gap_kept.txt" -D1e-12 -C1 -Q7
	-T0/4/1 -Sl -Vq)
execute_process(
	COMMAND "${GQ_CHECK_TABLE}" "${GQ_TEST_DIR}/one_gap_kept.txt"
	        2 1 nan 0
	RESULT_VARIABLE status)
if(status)
	message(FATAL_ERROR "ssh1d interpolated across a gap without -S+g")
endif()
run_ssh(ssh1d
	"${GQ_TEST_DIR}/one_gap.txt" -A -Fv
	"-G${GQ_TEST_DIR}/one_gap_filled.txt" -D1e-12 -C1 -Q7
	-T0/4/1 "-Sl+g" -Vq)
execute_process(
	COMMAND "${GQ_CHECK_TABLE}" "${GQ_TEST_DIR}/one_gap_filled.txt"
	        2 1 10 1e-6
	RESULT_VARIABLE status)
if(status)
	message(FATAL_ERROR "ssh1d -S+g did not bridge an internal gap")
endif()

run_ssh(ssh1d
	"${GQ_TEST_DIR}/model1d.nc+x2+Xxunit+v2,3+Vaunit,bunit"
	-A -Fa,b "-G${GQ_TEST_DIR}/one_scaled.nc"
	-D0.1 -C2 -Q42 -Vq)
check_stats(one_scaled a 20 2 1e-4)
check_stats(one_scaled b 60 6 1e-4)
execute_process(
	COMMAND "${GQ_CHECK_3D}" "${GQ_TEST_DIR}/one_scaled.nc"
	        x 4 8 1e-9
	RESULT_VARIABLE status)
if(status)
	message(FATAL_ERROR "ssh1d input-axis transform failed")
endif()
foreach(check "x;units;xunit" "a;units;aunit" "b;units;bunit")
	list(GET check 0 variable)
	list(GET check 1 attribute)
	list(GET check 2 expected)
	execute_process(
		COMMAND "${GQ_CHECK_ATTRIBUTE}" "${GQ_TEST_DIR}/one_scaled.nc"
		        "${variable}" "${attribute}" "${expected}"
		RESULT_VARIABLE status)
	if(status)
		message(FATAL_ERROR "ssh1d transformed units failed: ${check}")
	endif()
endforeach()

# 2-D statistics, shared and independent fields, polygon taper, and application.
run_ssh(ssh2d
	"-G${GQ_TEST_DIR}/two.nc" -R0/20/0/10 -I1
	-D0.08 -C4/2 -U0.4 -Q42 -Vq)
check_stats(two heterogeneity 0 0.08 1e-5)

run_ssh(ssh2d
	"-G${GQ_TEST_DIR}/two_fields.nc" -R0/20/0/10 -I1 -Fvp,vs
	-D0.05 -Dvs/0.1 -C4/2 -U0.4 -Q42 -Vq)
check_stats(two_fields vp_heterogeneity 0 0.05 1e-5)
check_stats(two_fields vs_heterogeneity 0 0.1 1e-5)
execute_process(
	COMMAND "${GQ_CHECK_PAIR}" "${GQ_TEST_DIR}/two_fields.nc"
	        vp_heterogeneity vs_heterogeneity ratio 2 1e-6
	RESULT_VARIABLE status
)
if(status)
	message(FATAL_ERROR "shared SSH realization did not scale coherently")
endif()

run_ssh(ssh2d
	"-G${GQ_TEST_DIR}/two_independent.nc" -R0/20/0/10 -I1 -Fvp,vs
	-D0.05 -C4/2 -U0.4 -Q42+i -Vq)
execute_process(
	COMMAND "${GQ_CHECK_PAIR}" "${GQ_TEST_DIR}/two_independent.nc"
	        vp_heterogeneity vs_heterogeneity different 0 1e-6
	RESULT_VARIABLE status
)
if(status)
	message(FATAL_ERROR "independent SSH fields are identical")
endif()

run_ssh(ssh2d
	"-G${GQ_TEST_DIR}/two_polygon.nc" -R0/4/0/4 -I1
	-D0.1 -C1 -Q3
	"-P${GQ_SOURCE_DIR}/test/data/merge3d_square.txt"
	"-Wcosine+r0.2+w" -Vq)
execute_process(
	COMMAND "${GQ_CHECK_2D}" "${GQ_TEST_DIR}/two_polygon.nc"
	        weight 4 4 0 1e-6
	RESULT_VARIABLE status
)
if(status)
	message(FATAL_ERROR "ssh2d polygon did not mask outside nodes")
endif()

# Polygon vertices need not coincide exactly with output grid nodes.
file(WRITE "${GQ_TEST_DIR}/unaligned_polygon.txt"
     "0.2 0.2\n3.4 0.2\n3.4 3.4\n0.2 3.4\n")
run_ssh(ssh2d
	"-G${GQ_TEST_DIR}/two_unaligned_polygon.nc" -R0/4/0/4 -I1
	-D0.1 -C1 -Q3 "-P${GQ_TEST_DIR}/unaligned_polygon.txt"
	"-Wcosine+r0.2+w" -Vq)
execute_process(
	COMMAND "${GQ_CHECK_2D}" "${GQ_TEST_DIR}/two_unaligned_polygon.nc"
	        weight 4 4 0 1e-6
	RESULT_VARIABLE status
)
if(status)
	message(FATAL_ERROR "ssh2d unaligned polygon support failed")
endif()

run_ssh(ssh2d
	"${GQ_TEST_DIR}/model2d.nc" -A -Fvp,vs
	"-G${GQ_TEST_DIR}/two_applied.nc" -D0.1 -C4/2 -Q42 -Vq)
check_stats(two_applied vp 1000 100 1e-3)
check_stats(two_applied vs 500 50 1e-3)
execute_process(
	COMMAND "${GQ_CHECK_2D}" "${GQ_TEST_DIR}/two_applied.nc"
	        quality 2 3 7 1e-6
	RESULT_VARIABLE status
)
if(status)
	message(FATAL_ERROR "ssh2d did not preserve an unselected variable")
endif()

# Invalid field selection must fail cleanly rather than entering execution
# with partially initialized NetCDF metadata.
execute_process(
	COMMAND "${CMAKE_COMMAND}" -E env "GQ_PLUGIN=${GQ_PLUGIN}"
	        "${GQ_RUNNER}" ssh2d "${GQ_TEST_DIR}/model2d.nc"
	        -A -Fmissing "-G${GQ_TEST_DIR}/invalid_field.nc"
	        -D0.1 -C4/2 -Q42 -Vq
	RESULT_VARIABLE invalid_status
	ERROR_VARIABLE invalid_error
)
if(invalid_status EQUAL 0 OR
   invalid_error MATCHES "Caught signal|Segmentation fault")
	message(FATAL_ERROR "ssh2d invalid field selection did not fail cleanly")
endif()

# Scale input axes and fields before applying the shared perturbation.
run_ssh(ssh2d
	"${GQ_TEST_DIR}/model2d.nc+x2+Xxunit+y3+Yyunit+v2,3+Vvpunit,vsunit"
	-A -Fvp,vs "-G${GQ_TEST_DIR}/two_scaled.nc"
	-D0.1 -C4/2 -Q42 -Vq)
check_stats(two_scaled vp 2000 200 1e-3)
check_stats(two_scaled vs 1500 150 1e-3)
foreach(check "x;6;12" "y;4;12")
	list(GET check 0 variable)
	list(GET check 1 index)
	list(GET check 2 expected)
	execute_process(
		COMMAND "${GQ_CHECK_3D}" "${GQ_TEST_DIR}/two_scaled.nc"
		        "${variable}" "${index}" "${expected}" 1e-9
		RESULT_VARIABLE status)
	if(status)
		message(FATAL_ERROR "ssh2d input-axis transform failed: ${check}")
	endif()
endforeach()
foreach(check "x;units;xunit" "y;units;yunit"
	              "vp;units;vpunit" "vs;units;vsunit")
	list(GET check 0 variable)
	list(GET check 1 attribute)
	list(GET check 2 expected)
	execute_process(
		COMMAND "${GQ_CHECK_ATTRIBUTE}" "${GQ_TEST_DIR}/two_scaled.nc"
		        "${variable}" "${attribute}" "${expected}"
		RESULT_VARIABLE status)
	if(status)
		message(FATAL_ERROR "ssh2d transformed units failed: ${check}")
	endif()
endforeach()

# Fill enclosed horizontal holes, preserve boundary NaNs, and then resample.
run_ssh(ssh2d
	"${GQ_TEST_DIR}/gaps2d.nc" -A -Fvp
	"-G${GQ_TEST_DIR}/two_gaps.nc" -D1e-12 -C1 -Q7 -Hl -Vq)
foreach(check "2;2;100" "0;0;nan")
	list(GET check 0 row)
	list(GET check 1 column)
	list(GET check 2 expected)
	execute_process(
		COMMAND "${GQ_CHECK_2D}" "${GQ_TEST_DIR}/two_gaps.nc"
		        vp "${row}" "${column}" "${expected}" 1e-6
		RESULT_VARIABLE status)
	if(status)
		message(FATAL_ERROR "ssh2d internal-hole behavior failed: ${check}")
	endif()
endforeach()
run_ssh(ssh2d
	"${GQ_TEST_DIR}/gaps2d.nc" -A -Fvp -R0/4/0/4 -I0.5 -nl -Hl
	"-G${GQ_TEST_DIR}/two_resampled.nc" -D1e-12 -C1 -Q7 -Vq)
execute_process(
	COMMAND "${GQ_CHECK_3D}" "${GQ_TEST_DIR}/two_resampled.nc"
	        x 8 4 1e-12
	RESULT_VARIABLE status)
if(status)
	message(FATAL_ERROR "ssh2d -R/-I horizontal resampling failed")
endif()
foreach(check "quality;absent" "model_id;present")
	list(GET check 0 variable)
	list(GET check 1 expected)
	execute_process(
		COMMAND "${GQ_CHECK_VARIABLE}" "${GQ_TEST_DIR}/two_resampled.nc"
		        "${variable}" "${expected}"
		RESULT_VARIABLE status)
	if(status)
		message(FATAL_ERROR "ssh2d ancillary-variable retention failed: ${check}")
	endif()
endforeach()

# Exercise every initial spectral model and disabled padding.
foreach(model g e w)
	set(correlation -C4/2)
	if(model STREQUAL "w")
		set(correlation)
	endif()
	run_ssh(ssh2d
		"-G${GQ_TEST_DIR}/two_${model}.nc" -R0/20/0/10 -I1
		-D0.06 ${correlation} "-M${model}" -Q7+n -Vq)
	check_stats(two_${model} heterogeneity 0 0.06 1e-5)
endforeach()

# 3-D generation, extruded taper support, and model application.
run_ssh(ssh3d
	"-G${GQ_TEST_DIR}/three.nc" -R0/8/0/6 -I1 -T0/4/1
	-D0.1 -C3/2/1 -U0.2 -Q42 -Vq)
check_stats(three heterogeneity 0 0.1 1e-5)

run_ssh(ssh3d
	"-G${GQ_TEST_DIR}/three_taper.nc" -R0/4/0/4 -I1 -T0/4/1
	-D0.1 -C1
	"-P${GQ_SOURCE_DIR}/test/data/merge3d_square.txt" -L1/3
	"-Wcosine/cosine/cosine+r0.2+w" -Q3 -Vq)
execute_process(
	COMMAND "${GQ_CHECK_3D}" "${GQ_TEST_DIR}/three_taper.nc"
	        weight 0 0 0 0 1e-6
	RESULT_VARIABLE status
)
if(status)
	message(FATAL_ERROR "ssh3d vertical support did not mask outside nodes")
endif()

run_ssh(ssh3d
	"${GQ_TEST_DIR}/model3d.nc" -A -Fvp
	"-G${GQ_TEST_DIR}/three_applied.nc" -D0.1 -C3/2/1 -Q42 -Vq)
check_stats(three_applied vp 2000 200 1e-2)
execute_process(
	COMMAND "${GQ_CHECK_3D}" "${GQ_TEST_DIR}/three_applied.nc"
	        marker 1 2 3 9 1e-6
	RESULT_VARIABLE status
)
if(status)
	message(FATAL_ERROR "ssh3d did not preserve an unselected variable")
endif()

# Horizontal holes are filled layer by layer; vertical gaps are handled by -S.
run_ssh(ssh3d
	"${GQ_TEST_DIR}/gaps3d.nc" -A -Fhorizontal
	"-G${GQ_TEST_DIR}/three_horizontal_gaps.nc" -D1e-12 -C1 -Q7 -Hl -Vq)
execute_process(
	COMMAND "${GQ_CHECK_3D}" "${GQ_TEST_DIR}/three_horizontal_gaps.nc"
	        horizontal 1 1 1 100 1e-6
	RESULT_VARIABLE status)
if(status)
	message(FATAL_ERROR "ssh3d -H did not fill an enclosed horizontal gap")
endif()
run_ssh(ssh3d
	"${GQ_TEST_DIR}/gaps3d.nc" -A -Fvertical -T0/2/0.5 "-Sl+g"
	"-G${GQ_TEST_DIR}/three_vertical_gaps.nc" -D1e-12 -C1 -Q7 -Vq)
execute_process(
	COMMAND "${GQ_CHECK_3D}" "${GQ_TEST_DIR}/three_vertical_gaps.nc"
	        vertical 2 1 1 200 1e-6
	RESULT_VARIABLE status)
if(status)
	message(FATAL_ERROR "ssh3d -S+g did not bridge and resample a vertical gap")
endif()
foreach(check "marker;absent" "model_id;present")
	list(GET check 0 variable)
	list(GET check 1 expected)
	execute_process(
		COMMAND "${GQ_CHECK_VARIABLE}" "${GQ_TEST_DIR}/three_vertical_gaps.nc"
		        "${variable}" "${expected}"
		RESULT_VARIABLE status)
	if(status)
		message(FATAL_ERROR "ssh3d ancillary-variable retention failed: ${check}")
	endif()
endforeach()

run_ssh(ssh3d
	"${GQ_TEST_DIR}/model3d.nc+x2+Xxunit+y3+Yyunit+z4+Zzunit+v0.5+Vvpunit"
	-A -Fvp "-G${GQ_TEST_DIR}/three_scaled.nc"
	-D0.1 -C3/2/4 -Q42 "-Z+z-1+Zzout+v2+Vvpout" -Vq)
check_stats(three_scaled vp 2000 200 1e-2)
foreach(check "x;4;8" "y;3;9" "z;2;-8")
	list(GET check 0 variable)
	list(GET check 1 index)
	list(GET check 2 expected)
	execute_process(
		COMMAND "${GQ_CHECK_3D}" "${GQ_TEST_DIR}/three_scaled.nc"
		        "${variable}" "${index}" "${expected}" 1e-9
		RESULT_VARIABLE status)
	if(status)
		message(FATAL_ERROR "ssh3d input-axis transform failed: ${check}")
	endif()
endforeach()
foreach(check "x;units;xunit" "y;units;yunit" "z;units;zout"
	              "z;positive;down" "vp;units;vpout")
	list(GET check 0 variable)
	list(GET check 1 attribute)
	list(GET check 2 expected)
	execute_process(
		COMMAND "${GQ_CHECK_ATTRIBUTE}" "${GQ_TEST_DIR}/three_scaled.nc"
		        "${variable}" "${attribute}" "${expected}"
		RESULT_VARIABLE status)
	if(status)
		message(FATAL_ERROR "ssh3d transformed units failed: ${check}")
	endif()
endforeach()

# A negative input scale converts a descending convention to the required
# increasing working axis. A negative output scale restores the convention;
# neither transform moves data between layers.
run_ssh(ssh3d
	"${GQ_TEST_DIR}/descending3d.nc+z-1+Zkm" -A -Fvp
	"-G${GQ_TEST_DIR}/three_descending.nc" -D1e-12 -C1 -Q7
	"-Z+z-1+Zm" -Vq)
execute_process(
	COMMAND "${GQ_CHECK_3D}" "${GQ_TEST_DIR}/three_descending.nc"
	        z 2 -2 1e-12
	RESULT_VARIABLE status)
if(status)
	message(FATAL_ERROR "ssh3d did not restore the descending output convention")
endif()
execute_process(
	COMMAND "${GQ_CHECK_3D}" "${GQ_TEST_DIR}/three_descending.nc"
	        vp 2 0 0 300 1e-6
	RESULT_VARIABLE status)
if(status)
	message(FATAL_ERROR "SSH axis scaling reordered three-dimensional data")
endif()

# Field-specific overrides must refer to selected fields.
execute_process(
	COMMAND "${CMAKE_COMMAND}" -E env
	        "GQ_PLUGIN=${GQ_PLUGIN}"
	        "${GQ_RUNNER}" ssh2d
	        "-G${GQ_TEST_DIR}/invalid.nc" -R0/10/0/10 -I1 -Fvp
	        -D0.1 -Dvs/0.2 -C2 -Vq
	RESULT_VARIABLE status
)
if(NOT status)
	message(FATAL_ERROR "ssh2d accepted statistics for an unselected field")
endif()
