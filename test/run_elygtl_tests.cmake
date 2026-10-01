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

foreach(fixture model vs30 mask transition scaled_model scaled_vs30
		gaps gaps_vs30 evidence_model evidence_vs30)
	execute_process(
		COMMAND "${NCGEN_EXECUTABLE}" -o "${GQ_TEST_DIR}/${fixture}.nc"
		        "${GQ_SOURCE_DIR}/test/data/elygtl_${fixture}.cdl"
		RESULT_VARIABLE status
	)
	if(status)
		message(FATAL_ERROR "ncgen failed for elygtl ${fixture} fixture")
	endif()
endforeach()

function(run_elygtl output)
	execute_process(
		COMMAND "${CMAKE_COMMAND}" -E env
		        "GQ_PLUGIN=${GQ_PLUGIN}"
		        "${GQ_RUNNER}"
		        "${GQ_TEST_DIR}/model.nc"
		        "${GQ_TEST_DIR}/vs30.nc?vs30"
		        "-G${GQ_TEST_DIR}/${output}.nc"
		        "-K${GQ_TEST_DIR}/mask.nc?mask"
		        -Dn -Vq ${ARGN}
		RESULT_VARIABLE status
	)
	if(status)
		message(FATAL_ERROR "elygtl ${output} run failed")
	endif()
endfunction()

function(check3 file variable layer row column expected)
	execute_process(
		COMMAND "${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/${file}.nc"
		        "${variable}" "${layer}" "${row}" "${column}"
		        "${expected}" 1e-2
		RESULT_VARIABLE status
	)
	if(status)
		message(FATAL_ERROR
		        "${file}?${variable}[${layer},${row},${column}] != ${expected}")
	endif()
endfunction()

# Input scaling never reorders data. A scale that leaves an axis decreasing is
# rejected and the user must choose a transform that makes it increasing.
execute_process(
	COMMAND "${CMAKE_COMMAND}" -E env
	        "GQ_PLUGIN=${GQ_PLUGIN}"
	        "${GQ_RUNNER}"
	        "${GQ_TEST_DIR}/model.nc+x-1+Xreversed"
	        "${GQ_TEST_DIR}/vs30.nc?vs30+x-1"
	        "-K${GQ_TEST_DIR}/mask.nc?mask+x-1"
	        "-G${GQ_TEST_DIR}/reverse_x.nc"
	        -Fvs=vs -Dn -Vq
	RESULT_VARIABLE status)
if(NOT status)
	message(FATAL_ERROR "elygtl accepted a decreasing transformed x axis")
endif()

# Vs-only operation plus generated Vp and density.
run_elygtl(vs_only -Fvs=vs -Cvp=vp,rho=rho)
check3(vs_only vs 0 0 0 200)
check3(vs_only vs 1 0 0 865.1948)
check3(vs_only vs 2 0 0 1000)
check3(vs_only vp 0 0 0 832.005)
check3(vs_only vp 1 0 0 2464.68)
check3(vs_only vp 2 0 0 2458.2)
check3(vs_only rho 0 0 0 1091.95)
check3(vs_only rho 1 0 0 2082.08)

# Explicitly wet columns retain existing fields and have no generated fields.
check3(vs_only vs 0 0 1 600)
check3(vs_only vp 0 0 1 nan)

# Missing Vs30 leaves the existing column unchanged.
check3(vs_only vs 0 1 1 600)

# Unselected three-dimensional variables are copied without modification.
check3(vs_only quality 1 1 0 23)

# A spatial transition grid changes the GTL base while retaining the endpoint.
run_elygtl(variable_transition -Fvs=vs
	"-E${GQ_TEST_DIR}/transition.nc?depth")
check3(variable_transition vs 0 1 0 200)
check3(variable_transition vs 1 1 0 846.463)

# Vp-only operation can create Vs from the direct Brocher relation.
run_elygtl(vp_only -Fvp=vp0 -Cvs=vs_new)
check3(vp_only vp0 0 0 0 832.005)
check3(vp_only vp0 2 0 0 2000)
check3(vp_only vs_new 0 0 0 200)
check3(vp_only vs_new 2 0 0 608.6)
check3(vp_only vs_new 0 0 1 nan)

# Model, grid, velocity, and density scaling is inverted on output.
execute_process(
	COMMAND "${CMAKE_COMMAND}" -E env
	        "GQ_PLUGIN=${GQ_PLUGIN}"
	        "${GQ_RUNNER}"
	        "${GQ_TEST_DIR}/scaled_model.nc+z1000"
	        "${GQ_TEST_DIR}/scaled_vs30.nc?vs30+s1000"
	        "-G${GQ_TEST_DIR}/scaled.nc"
	        -Fvs=vs -Cvp=vp,rho=rho -U1000/1000 -Dn -Vq
	RESULT_VARIABLE status
)
if(status)
	message(FATAL_ERROR "elygtl scaled run failed")
endif()
check3(scaled vs 0 0 0 0.2)
check3(scaled vp 0 0 0 0.832006)
check3(scaled rho 0 0 0 1.09195)

# Source transforms precede the Ely SI-unit conversion; ancillary grids use
# the same transformed horizontal coordinates and +s remains a +v alias.
execute_process(
	COMMAND "${CMAKE_COMMAND}" -E env
	        "GQ_PLUGIN=${GQ_PLUGIN}"
	        "${GQ_RUNNER}"
	        "${GQ_TEST_DIR}/model.nc+x2+Xxunit+y3+Yyunit+z2+Zzunit+v2+Vscaled"
	        "${GQ_TEST_DIR}/vs30.nc?vs30+x2+y3+s2"
	        "-K${GQ_TEST_DIR}/mask.nc?mask+x2+y3"
	        "-G${GQ_TEST_DIR}/input_transform.nc"
	        -Fvs=vs -Dn -Vq
	RESULT_VARIABLE status)
if(status)
	message(FATAL_ERROR "elygtl transformed-input run failed")
endif()
check3(input_transform vs 0 0 0 400)
check3(input_transform vs 2 0 0 2000)
foreach(check "x;1;2" "y;1;3" "z;2;700")
	list(GET check 0 variable)
	list(GET check 1 index)
	list(GET check 2 expected)
	execute_process(
		COMMAND "${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/input_transform.nc"
		        "${variable}" "${index}" "${expected}" 1e-9
		RESULT_VARIABLE status)
	if(status)
		message(FATAL_ERROR "elygtl input-axis transform failed: ${check}")
	endif()
endforeach()
foreach(check "x;units;xunit" "y;units;yunit" "z;units;zunit"
	              "vs;units;scaled")
	list(GET check 0 variable)
	list(GET check 1 attribute)
	list(GET check 2 expected)
	execute_process(
		COMMAND "${GQ_CHECK_ATTRIBUTE}" "${GQ_TEST_DIR}/input_transform.nc"
		        "${variable}" "${attribute}" "${expected}"
		RESULT_VARIABLE status)
	if(status)
		message(FATAL_ERROR "elygtl transformed units failed: ${check}")
	endif()
endforeach()

# Input coordinate transforms precede -R/-I. The mapped field and ancillary
# grids are sampled onto the requested lattice, while an unselected 3-D field
# tied to the old horizontal dimensions is omitted.
execute_process(
	COMMAND "${CMAKE_COMMAND}" -E env
	        "GQ_PLUGIN=${GQ_PLUGIN}"
	        "${GQ_RUNNER}"
	        "${GQ_TEST_DIR}/model.nc+x2+Xscaled_x+y3+Yscaled_y"
	        "${GQ_TEST_DIR}/vs30.nc?vs30+x2+y3"
	        "-K${GQ_TEST_DIR}/mask.nc?mask+x2+y3"
	        "-G${GQ_TEST_DIR}/resampled.nc"
	        -Fvs=vs -R0/1/0/3 -I0.5/1.5 -Dn -Vq
	RESULT_VARIABLE status)
if(status)
	message(FATAL_ERROR "elygtl -R/-I run failed")
endif()
foreach(check "x;2;1" "y;1;1.5")
	list(GET check 0 variable)
	list(GET check 1 index)
	list(GET check 2 expected)
	execute_process(
		COMMAND "${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/resampled.nc"
		        "${variable}" "${index}" "${expected}" 1e-9
		RESULT_VARIABLE status)
	if(status)
		message(FATAL_ERROR "elygtl resampled coordinate failed: ${check}")
	endif()
endforeach()
execute_process(
	COMMAND "${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/resampled.nc"
	        vs 0 0 0 200 1e-2
	RESULT_VARIABLE status)
if(status)
	message(FATAL_ERROR "elygtl resampled mapped field is incorrect")
endif()
execute_process(
	COMMAND "${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/resampled.nc"
	        quality 0 0 0 0 1e-6
	RESULT_VARIABLE status)
if(NOT status)
	message(FATAL_ERROR "elygtl retained an unselected field on the old lattice")
endif()

# -T changes the positive-down z lattice and -S selects vertical interpolation.
execute_process(
	COMMAND "${CMAKE_COMMAND}" -E env
	        "GQ_PLUGIN=${GQ_PLUGIN}"
	        "${GQ_RUNNER}"
	        "${GQ_TEST_DIR}/model.nc"
	        "${GQ_TEST_DIR}/vs30.nc?vs30"
	        "-G${GQ_TEST_DIR}/vertical.nc"
	        -Fvs=vs -Ml -Dn -E1 -T0/350/87.5 -Sl -Vq
	RESULT_VARIABLE status)
if(status)
	message(FATAL_ERROR "elygtl vertical resampling run failed")
endif()
execute_process(
	COMMAND "${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/vertical.nc"
	        z 1 87.5 1e-9
	RESULT_VARIABLE status)
if(status)
	message(FATAL_ERROR "elygtl vertical coordinate is incorrect")
endif()
check3(vertical vs 1 0 0 700)

# -H fills only enclosed horizontal holes and -S+g bridges internal z gaps.
execute_process(
	COMMAND "${CMAKE_COMMAND}" -E env
	        "GQ_PLUGIN=${GQ_PLUGIN}"
	        "${GQ_RUNNER}"
	        "${GQ_TEST_DIR}/gaps.nc"
	        "${GQ_TEST_DIR}/gaps_vs30.nc?vs30"
	        "-G${GQ_TEST_DIR}/gaps_filled.nc"
	        -Fvs=vs -Ml -Dn -E1 -Hl -Sl+g -Vq
	RESULT_VARIABLE status)
if(status)
	message(FATAL_ERROR "elygtl horizontal/vertical gap-filling run failed")
endif()
check3(gaps_filled vs 2 1 1 1000)
check3(gaps_filled vs 1 0 0 800)

# -Ml and -Mw classify the complete domain without a shoreline or mask grid.
execute_process(
	COMMAND "${CMAKE_COMMAND}" -E env
	        "GQ_PLUGIN=${GQ_PLUGIN}"
	        "${GQ_RUNNER}"
	        "${GQ_TEST_DIR}/model.nc"
	        "${GQ_TEST_DIR}/vs30.nc?vs30"
	        "-G${GQ_TEST_DIR}/all_land.nc"
	        -Fvs=vs -Ml -Dn -Vq
	RESULT_VARIABLE status)
if(status)
	message(FATAL_ERROR "elygtl all-land classification run failed")
endif()
check3(all_land vs 0 0 1 200)
execute_process(
	COMMAND "${CMAKE_COMMAND}" -E env
	        "GQ_PLUGIN=${GQ_PLUGIN}"
	        "${GQ_RUNNER}"
	        "${GQ_TEST_DIR}/model.nc"
	        "${GQ_TEST_DIR}/vs30.nc?vs30"
	        "-G${GQ_TEST_DIR}/all_wet.nc"
	        -Fvs=vs -Mw -Dn -Vq
	RESULT_VARIABLE status)
if(status)
	message(FATAL_ERROR "elygtl all-wet classification run failed")
endif()
check3(all_wet vs 0 0 0 600)

# -M+e selects Vs30 or a mapped cube property as wet/land evidence. Model Vs
# uses zero as its default water value; Vp and density use explicit signatures.
function(run_evidence output mode)
	execute_process(
		COMMAND "${CMAKE_COMMAND}" -E env
		        "GQ_PLUGIN=${GQ_PLUGIN}"
		        "${GQ_RUNNER}"
		        "${GQ_TEST_DIR}/evidence_model.nc"
		        "${GQ_TEST_DIR}/evidence_vs30.nc?vs30"
		        "-G${GQ_TEST_DIR}/${output}.nc"
		        -Fvp=vp,vs=vs,rho=rho -Dn -E100 "${mode}" -Vq
		RESULT_VARIABLE status)
	if(status)
		message(FATAL_ERROR "elygtl ${output} evidence run failed")
	endif()
endfunction()

# Positive Vs30 treats the zero-Vs water column as land, while cube evidence
# recognizes both that water signature and a first finite solid layer below zero.
run_evidence(evidence_default -Mm)
check3(evidence_default vs 0 0 1 200)
run_evidence(evidence_vs -Mm+evs)
check3(evidence_vs vs 0 0 1 0)
check3(evidence_vs vs 1 1 1 900)

# Tolerances are expressed in transformed model units before -U conversion.
run_evidence(evidence_vp_exact -Mm+evp+w1500)
check3(evidence_vp_exact vs 0 0 1 200)
run_evidence(evidence_vp -Mm+evp+w1500+t20)
check3(evidence_vp vs 0 0 1 0)
run_evidence(evidence_rho -Mm+erho+w1000+t20)
check3(evidence_rho vs 0 0 1 0)

# Reject evidence configurations that cannot classify the mapped model.
foreach(case
		"missing_water;-Fvp=vp,vs=vs;-Mm+evp"
		"unmapped_property;-Fvs=vs;-Mm+evp+w1500"
		"all_land_modifiers;-Fvs=vs;-Ml+evs"
		"negative_tolerance;-Fvs=vs;-Mm+evs+t-1")
	list(GET case 0 name)
	list(GET case 1 fields)
	list(GET case 2 mode)
	execute_process(
		COMMAND "${CMAKE_COMMAND}" -E env
		        "GQ_PLUGIN=${GQ_PLUGIN}"
		        "${GQ_RUNNER}"
		        "${GQ_TEST_DIR}/evidence_model.nc"
		        "${GQ_TEST_DIR}/evidence_vs30.nc?vs30"
		        "-G${GQ_TEST_DIR}/${name}.nc"
		        "${fields}" -Dn -E100 "${mode}" -Vq
		RESULT_VARIABLE status)
	if(NOT status)
		message(FATAL_ERROR "elygtl accepted invalid evidence case ${name}")
	endif()
endforeach()

# -Z scales output coordinates and values in place without reordering layers.
execute_process(
	COMMAND "${CMAKE_COMMAND}" -E env
	        "GQ_PLUGIN=${GQ_PLUGIN}"
	        "${GQ_RUNNER}"
	        "${GQ_TEST_DIR}/model.nc"
	        "${GQ_TEST_DIR}/vs30.nc?vs30"
	        "-G${GQ_TEST_DIR}/output_transform.nc"
	        -Fvs=vs -Ml -Dn -E1
	        "-Z+x2+Xscaled_x+y3+Yscaled_y+z-0.001+Zkm+v0.001+Vkm/s"
	        -Vq
	RESULT_VARIABLE status)
if(status)
	message(FATAL_ERROR "elygtl output transform run failed")
endif()
foreach(check "x;1;2" "y;1;3" "z;2;-0.35")
	list(GET check 0 variable)
	list(GET check 1 index)
	list(GET check 2 expected)
	execute_process(
		COMMAND "${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/output_transform.nc"
		        "${variable}" "${index}" "${expected}" 1e-9
		RESULT_VARIABLE status)
	if(status)
		message(FATAL_ERROR "elygtl output-axis transform failed: ${check}")
	endif()
endforeach()
check3(output_transform vs 2 0 0 1)
foreach(check "z;units;km" "z;positive;up" "vs;units;km/s")
	list(GET check 0 variable)
	list(GET check 1 attribute)
	list(GET check 2 expected)
	execute_process(
		COMMAND "${GQ_CHECK_ATTRIBUTE}" "${GQ_TEST_DIR}/output_transform.nc"
		        "${variable}" "${attribute}" "${expected}"
		RESULT_VARIABLE status)
	if(status)
		message(FATAL_ERROR "elygtl output metadata failed: ${check}")
	endif()
endforeach()
