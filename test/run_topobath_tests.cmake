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

foreach(fixture flat surface existing unresolved gaps horizontal_gaps ocean mask missing_surface decimal roundoff_surface below_surface)
	execute_process(
		COMMAND "${NCGEN_EXECUTABLE}" -o "${GQ_TEST_DIR}/${fixture}.nc"
		        "${GQ_SOURCE_DIR}/test/data/topobath_${fixture}.cdl"
		RESULT_VARIABLE status
	)
	if(status)
		message(FATAL_ERROR "ncgen failed for topobath ${fixture} fixture")
	endif()
endforeach()

function(run_topobath output method model)
	execute_process(
		COMMAND "${CMAKE_COMMAND}" -E env
		        "GQ_PLUGIN=${GQ_PLUGIN}"
		        "${GQ_RUNNER}" "${model}" ${ARGN}
		        "-G${GQ_TEST_DIR}/${output}.nc" "${method}" -Dn -Vq
		RESULT_VARIABLE status
	)
	if(status)
		message(FATAL_ERROR "topobath ${output} run failed")
	endif()
endfunction()

function(check3 file variable layer row column expected)
	execute_process(
		COMMAND "${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/${file}.nc"
		        "${variable}" "${layer}" "${row}" "${column}"
		        "${expected}" 1e-5
		RESULT_VARIABLE status
	)
	if(status)
		message(FATAL_ERROR
		        "${file}?${variable}[${layer},${row},${column}] != ${expected}")
	endif()
endfunction()

# Internal gaps are preserved unless -S+g requests vertical bridging.
run_topobath(gap_default -Or "${GQ_TEST_DIR}/gaps.nc?vp"
	-T0/3/1 -Wvp/1.5)
check3(gap_default vp 2 0 0 nan)

run_topobath(gap_bridge -Or "${GQ_TEST_DIR}/gaps.nc?vp"
	-T0/3/1 -Wvp/1.5 -Sl+g)
check3(gap_bridge vp 2 0 0 8)

run_topobath(gap_limited -Or "${GQ_TEST_DIR}/gaps.nc?vp"
	-T0/3/1 -Wvp/1.5 -Sl+g1.5)
check3(gap_limited vp 2 0 0 nan)

# Horizontal holes are preserved by default and filled only when -H requests it.
run_topobath(horizontal_gap_default -Or
	"${GQ_TEST_DIR}/horizontal_gaps.nc?vp" -Cl -T0/2/1)
check3(horizontal_gap_default vp 1 1 1 nan)

run_topobath(horizontal_gap_filled -Or
	"${GQ_TEST_DIR}/horizontal_gaps.nc?vp" -Cl -T0/2/1 -Hl)
check3(horizontal_gap_filled vp 1 1 1 8)

run_topobath(horizontal_gap_limited -Or
	"${GQ_TEST_DIR}/horizontal_gaps.nc?vp" -Cl -T0/2/1 -Hl+m1)
check3(horizontal_gap_limited vp 1 1 1 nan)

run_topobath(horizontal_gap_resampled -Or
	"${GQ_TEST_DIR}/horizontal_gaps.nc?vp" -Cl -T0/2/1 -Hl
	-R0/4/0/4 -I0.5 -nl)
check3(horizontal_gap_resampled vp 1 2 2 8)

# Negative coordinate scales reverse both selected fields and retained
# horizontal ancillary variables into increasing output order.
run_topobath(reverse_x -Or "${GQ_TEST_DIR}/flat.nc?vp+x-1+Xreversed"
	-T0/3/1 -Wvp/1.5)
execute_process(
	COMMAND "${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/reverse_x.nc"
	        x 0 -1 1e-9
	RESULT_VARIABLE status)
if(status)
	message(FATAL_ERROR "topobath negative x scaling did not reverse the axis")
endif()
execute_process(
	COMMAND "${GQ_CHECK_GRID}" "${GQ_TEST_DIR}/reverse_x.nc"
	        quality 0 0 2 1e-6
	RESULT_VARIABLE status)
if(status)
	message(FATAL_ERROR "topobath did not reverse a retained x-dependent variable")
endif()

set(flat "${GQ_TEST_DIR}/flat.nc?vp,vs")
set(surface "${GQ_TEST_DIR}/surface.nc?elevation+s1")
set(mask "-K${GQ_TEST_DIR}/mask.nc?mask")
set(water -Wvp/1.5+t0.01 -Wvs/0+t0.01)

# A wet prior with no matching water signature falls back to the common
# shallowest finite boundary instead of discarding the entire column.
run_topobath(ocean_fallback -Or "${flat}" -T0/3/1 ${water}
	"-K${GQ_TEST_DIR}/ocean.nc?mask"
	"-Q${GQ_TEST_DIR}/ocean_surface.nc+c${GQ_TEST_DIR}/ocean_class.nc")
check3(ocean_fallback vp 0 0 0 10)
check3(ocean_fallback vs 0 1 1 20)
execute_process(
	COMMAND "${GQ_CHECK_GRID}" "${GQ_TEST_DIR}/ocean_surface.nc"
	        elevation 0 0 0 1e-5
	RESULT_VARIABLE status)
if(status)
	message(FATAL_ERROR "topobath ocean fallback surface is not zero")
endif()
execute_process(
	COMMAND "${GQ_CHECK_GRID}" "${GQ_TEST_DIR}/ocean_class.nc"
	        classification 1 1 2 1e-5
	RESULT_VARIABLE status)
if(status)
	message(FATAL_ERROR "topobath ocean fallback was not classified as wet")
endif()

run_topobath(pull -Oa "${flat}" "${surface}" -Mp
	-T-2/3/1 ${mask} ${water}
	"-Q${GQ_TEST_DIR}/flat_surface.nc+c${GQ_TEST_DIR}/flat_class.nc")
check3(pull vp 0 0 0 10)
check3(pull vp 1 0 0 9)
check3(pull vp 2 0 0 8)
check3(pull vp 2 0 1 1.5)
check3(pull vp 3 0 1 10)
check3(pull vs 2 0 1 0)
check3(pull vp 4 0 0 nan)

execute_process(
	COMMAND "${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/pull.nc"
	        ignored 0 0 0 0 1
	RESULT_VARIABLE status
)
if(NOT status)
	message(FATAL_ERROR "topobath retained an unselected 3-D variable")
endif()
execute_process(
	COMMAND "${GQ_CHECK_GRID}" "${GQ_TEST_DIR}/pull.nc"
	        quality 1 1 4 1e-5
	RESULT_VARIABLE status
)
if(status)
	message(FATAL_ERROR "topobath did not preserve a compatible 2-D ancillary variable")
endif()

run_topobath(extend -Oa "${flat}" "${surface}" -Me -T-2/3/1 ${mask} ${water})
check3(extend vp 0 0 0 10)
check3(extend vp 1 0 0 10)
check3(extend vp 2 0 0 10)
check3(extend vp 2 0 1 1.5)
check3(extend vp 3 0 1 10)

run_topobath(linear -Oa "${flat}" "${surface}" -Ml -T-2/3/1 ${mask} ${water}
	-Lvp/8 -Lvs/18)
check3(linear vp 0 0 0 8)
check3(linear vp 1 0 0 9)
check3(linear vs 0 0 0 18)
check3(linear vs 1 0 0 19)
check3(linear vp 1 1 0 8)

set(existing "${GQ_TEST_DIR}/existing.nc?vp,vs")
run_topobath(flatten -Or "${existing}" -T0/3/1 ${water}
	"-Q${GQ_TEST_DIR}/inferred.nc+c${GQ_TEST_DIR}/classification.nc")
check3(flatten vp 0 0 0 10)
check3(flatten vp 0 0 1 10)
check3(flatten vp 0 1 1 10)

run_topobath(keep -Or+t "${existing}" -T0/3/1 ${water})
check3(keep vp 0 0 0 10)
check3(keep vp 0 0 1 1.5)
check3(keep vp 1 0 1 10)
check3(keep vs 0 1 1 0)

# Bathymetry-only removal leaves land relief unchanged while shifting the
# solid seafloor to sea level and discarding the water column.
run_topobath(remove_bathymetry -Or+b "${existing}" -T-2/3/1 ${water})
check3(remove_bathymetry vp 1 0 0 10)
check3(remove_bathymetry vp 2 0 1 10)

run_topobath(replace -Ox "${existing}" "${surface}" -Mp -T-2/3/1 ${water}
	"-E${GQ_TEST_DIR}/inferred.nc?elevation")
check3(replace vp 0 0 0 10)
check3(replace vp 1 0 0 9)
check3(replace vp 2 0 1 1.5)
check3(replace vp 3 0 1 10)
check3(replace vp 1 1 0 10)

foreach(check
		"elevation;0;0;-1"
		"elevation;0;1;1"
		"elevation;1;0;0"
		"elevation;1;1;2")
	list(GET check 0 variable)
	list(GET check 1 row)
	list(GET check 2 column)
	list(GET check 3 expected)
	execute_process(
		COMMAND "${GQ_CHECK_GRID}" "${GQ_TEST_DIR}/inferred.nc"
		        "${variable}" "${row}" "${column}" "${expected}" 1e-5
		RESULT_VARIABLE status
	)
	if(status)
		message(FATAL_ERROR "inferred surface check failed: ${check}")
	endif()
endforeach()

foreach(check
		"classification;0;0;1"
		"classification;0;1;2"
		"classification;1;0;1"
		"classification;1;1;2")
	list(GET check 0 variable)
	list(GET check 1 row)
	list(GET check 2 column)
	list(GET check 3 expected)
	execute_process(
		COMMAND "${GQ_CHECK_GRID}" "${GQ_TEST_DIR}/classification.nc"
		        "${variable}" "${row}" "${column}" "${expected}" 1e-5
		RESULT_VARIABLE status
	)
	if(status)
		message(FATAL_ERROR "inferred classification check failed: ${check}")
	endif()
endforeach()

run_topobath(auto_axis -Or "${existing}" ${water})
execute_process(
	COMMAND "${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/auto_axis.nc"
	        z 0 0 1e-9
	RESULT_VARIABLE status
)
if(status)
	message(FATAL_ERROR "topobath automatic z top is incorrect")
endif()
execute_process(
	COMMAND "${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/auto_axis.nc"
	        z 4 4 1e-9
	RESULT_VARIABLE status
)
if(status)
	message(FATAL_ERROR "topobath automatic z bottom is incorrect")
endif()

run_topobath(default_fields -Or "${GQ_TEST_DIR}/flat.nc"
	-T0/3/1 -A0/0/1)
check3(default_fields ignored 0 1 1 1)

# Columns with no model values remain outside coverage instead of preventing
# valid neighboring columns from being transformed.
run_topobath(unresolved -Or "${GQ_TEST_DIR}/unresolved.nc?vp"
	-T0/1/1
	"-Q${GQ_TEST_DIR}/unresolved_surface.nc+c${GQ_TEST_DIR}/unresolved_class.nc")
check3(unresolved vp 0 0 0 7)
check3(unresolved vp 0 0 1 nan)
execute_process(
	COMMAND "${GQ_CHECK_GRID}" "${GQ_TEST_DIR}/unresolved_surface.nc"
	        elevation 0 1 nan 1e-6
	RESULT_VARIABLE status)
if(status)
	message(FATAL_ERROR "unresolved old surface was not written as NaN")
endif()
execute_process(
	COMMAND "${GQ_CHECK_GRID}" "${GQ_TEST_DIR}/unresolved_class.nc"
	        classification 0 1 0 1e-6
	RESULT_VARIABLE status)
if(status)
	message(FATAL_ERROR "unresolved model column classification is not zero")
endif()
run_topobath(unresolved_known -Or "${GQ_TEST_DIR}/unresolved.nc?vp"
	-Cl -T0/1/1 "-E${GQ_TEST_DIR}/unresolved_surface.nc?elevation")
check3(unresolved_known vp 0 0 0 7)
check3(unresolved_known vp 0 0 1 nan)
run_topobath(unresolved_pull -Oa "${GQ_TEST_DIR}/unresolved.nc?vp"
	"${GQ_TEST_DIR}/surface.nc?elevation" -Mp -T-1/1/1 ${mask} -Wvp/1.5)
check3(unresolved_pull vp 1 0 1 nan)

# Scale all output axes and selected fields without reordering the cube.
run_topobath(output_transform -Or "${GQ_TEST_DIR}/flat.nc?vp,vs"
	-T0/1/1
	"-Z+x-2+Xout_x+y3+Yout_y+z-1000+Zout_z+v10,100+Vout_vp,out_vs"
	"-Q${GQ_TEST_DIR}/output_surface.nc+c${GQ_TEST_DIR}/output_class.nc")
foreach(check "x;1;-2" "y;1;3" "z;0;0" "z;1;-1000")
	list(GET check 0 variable)
	list(GET check 1 index)
	list(GET check 2 expected)
	execute_process(
		COMMAND "${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/output_transform.nc"
		        "${variable}" "${index}" "${expected}" 1e-9
		RESULT_VARIABLE status)
	if(status)
		message(FATAL_ERROR "topobath output-axis transform failed: ${check}")
	endif()
endforeach()
check3(output_transform vp 0 0 0 100)
check3(output_transform vp 1 0 0 90)
check3(output_transform vs 0 0 0 2000)
check3(output_transform vs 1 0 0 1800)
foreach(check "x;units;out_x" "y;units;out_y" "z;units;out_z"
	              "z;positive;up" "vp;units;out_vp" "vs;units;out_vs")
	list(GET check 0 variable)
	list(GET check 1 attribute)
	list(GET check 2 expected)
	execute_process(
		COMMAND "${GQ_CHECK_ATTRIBUTE}" "${GQ_TEST_DIR}/output_transform.nc"
		        "${variable}" "${attribute}" "${expected}"
		RESULT_VARIABLE status)
	if(status)
		message(FATAL_ERROR "topobath output metadata failed: ${check}")
	endif()
endforeach()
execute_process(
	COMMAND "${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/output_surface.nc"
	        x 1 -2 1e-9
	RESULT_VARIABLE status)
if(status)
	message(FATAL_ERROR "topobath -Q surface did not use output x scaling")
endif()
execute_process(
	COMMAND "${GQ_CHECK_ATTRIBUTE}" "${GQ_TEST_DIR}/output_surface.nc"
	        elevation units out_z
	RESULT_VARIABLE status)
if(status)
	message(FATAL_ERROR "topobath -Q surface did not use output z units")
endif()

execute_process(
	COMMAND "${CMAKE_COMMAND}" -E env "GQ_PLUGIN=${GQ_PLUGIN}"
	        "${GQ_RUNNER}" "${GQ_TEST_DIR}/flat.nc?vp,vs"
	        -Or -T0/1/1 "-Z+v1,2,3"
	        "-G${GQ_TEST_DIR}/bad_output_fields.nc" -Dn -Vq
	RESULT_VARIABLE status)
if(NOT status)
	message(FATAL_ERROR "topobath accepted the wrong number of -Z field scales")
endif()

# Scale all model axes and selected fields, and propagate target units.
run_topobath(input_transform -Or
	"${GQ_TEST_DIR}/flat.nc?vp,vs+x2+Xxunit+y3+Yyunit+z2+Zzunit+v10,100+Vvpunit,vsunit"
	-T0/6/2 -Wvp/15 -Wvs/0)
check3(input_transform vp 0 0 0 100)
check3(input_transform vs 0 0 0 2000)
foreach(check "x;1;2" "y;1;3" "z;0;0")
	list(GET check 0 variable)
	list(GET check 1 index)
	list(GET check 2 expected)
	execute_process(
		COMMAND "${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/input_transform.nc"
		        "${variable}" "${index}" "${expected}" 1e-9
		RESULT_VARIABLE status)
	if(status)
		message(FATAL_ERROR "topobath input-axis transform failed: ${check}")
	endif()
endforeach()

# Topobath never rearranges model layers to repair the working z convention.
# A scale that makes the transformed z axis decrease must be rejected.
execute_process(
	COMMAND "${CMAKE_COMMAND}" -E env "GQ_PLUGIN=${GQ_PLUGIN}"
	        "${GQ_RUNNER}" "${GQ_TEST_DIR}/flat.nc?vp+z-1"
	        -Or -T0/3/1 -Wvp/1.5
	        "-G${GQ_TEST_DIR}/decreasing_z.nc" -Dn -Vq
	RESULT_VARIABLE status)
if(NOT status)
	message(FATAL_ERROR "topobath accepted a decreasing transformed z axis")
endif()

execute_process(
	COMMAND "${CMAKE_COMMAND}" -E env "GQ_PLUGIN=${GQ_PLUGIN}"
	        "${GQ_RUNNER}" "${GQ_TEST_DIR}/flat.nc?vp"
	        -Or -T3/0/1 -Wvp/1.5
	        "-G${GQ_TEST_DIR}/decreasing_range.nc" -Dn -Vq
	RESULT_VARIABLE status)
if(NOT status)
	message(FATAL_ERROR "topobath accepted -T with zmin >= zmax")
endif()

# The model and surface use the required increasing, positive-down working
# convention. The surface-grid +z alias leaves its already standardized values.
run_topobath(positive_down -Oa
	"${GQ_TEST_DIR}/flat.nc?vp,vs+z1+Zkm"
	"${GQ_TEST_DIR}/surface.nc?elevation+z1+Zkm"
	-Mp -T-2/3/1 ${mask} ${water})
check3(positive_down vp 0 0 0 10)
check3(positive_down vp 1 0 0 9)
check3(positive_down vp 2 0 0 8)
foreach(check "z;0;-2" "z;5;3")
	list(GET check 0 variable)
	list(GET check 1 index)
	list(GET check 2 expected)
	execute_process(
		COMMAND "${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/positive_down.nc"
		        "${variable}" "${index}" "${expected}" 1e-9
		RESULT_VARIABLE status)
	if(status)
		message(FATAL_ERROR "positive-down vertical axis failed: ${check}")
	endif()
endforeach()
execute_process(
	COMMAND "${GQ_CHECK_ATTRIBUTE}" "${GQ_TEST_DIR}/positive_down.nc"
	        z positive down
	RESULT_VARIABLE status)
if(status)
	message(FATAL_ERROR "positive-down metadata was not retained")
endif()

# Surface sign never changes the wet/dry class. With an all-land
# classification, positive surface values describe dry land below sea level
# and no water values are needed.
run_topobath(dry_below_sea -Oa "${GQ_TEST_DIR}/flat.nc?vp"
	"${surface}" -Mp -Cl -T-2/3/1)
check3(dry_below_sea vp 3 0 1 10)
check3(dry_below_sea vp 2 0 1 nan)

# Air defaults to NaN per field and may be overridden independently.
run_topobath(custom_air -Oa "${flat}" "${surface}" -Mp ${mask}
	-T-3/3/1 ${water} -Fvp/-123)
check3(custom_air vp 0 0 0 -123)
check3(custom_air vp 0 0 1 -123)
check3(custom_air vs 0 0 0 nan)

# A categorical wet mask remains authoritative when interpolated relief puts
# a nominal wet cell above sea level; that requested seafloor is clamped to
# sea level instead of changing the class.
run_topobath(wet_clamp -Oa+b "${GQ_TEST_DIR}/flat.nc?vp" "${surface}" -Mp
	"-K${GQ_TEST_DIR}/ocean.nc?mask"
	"-E${GQ_TEST_DIR}/flat_surface.nc?elevation"
	-T-2/3/1 -Wvp/1.5)
check3(wet_clamp vp 2 0 0 10)

# Sampling an old-surface grid may introduce machine-precision offsets from a
# model level. Such roundoff must not discard the valid surface sample and
# create a one-level gap when bathymetry is replaced.
run_topobath(surface_roundoff -Ox+b "${flat}" "${surface}" -Mp
	-T-2/3/0.1 ${mask} ${water}
	"-E${GQ_TEST_DIR}/roundoff_surface.nc?elevation")
check3(surface_roundoff vp 31 0 1 9.9)

# A scoped operation leaves unselected columns unchanged. In particular, a
# bathymetry-only replacement must not apply an inferred air mask to land.
run_topobath(scope_preserves_land -Ox+b "${flat}" "${surface}" -Mp
	-T0/3/1 ${mask} ${water}
	"-E${GQ_TEST_DIR}/below_surface.nc?elevation")
check3(scope_preserves_land vp 0 0 0 10)

# Once an explicit old surface and classification are available, operations
# that remove water or leave it untouched do not require -W.
run_topobath(remove_known -Or "${existing}" -T0/3/1 ${mask}
	"-E${GQ_TEST_DIR}/inferred.nc?elevation")
check3(remove_known vp 0 0 1 10)
run_topobath(retain_known_water -Or+t "${existing}" -T0/3/1 ${mask}
	"-E${GQ_TEST_DIR}/inferred.nc?elevation")
check3(retain_known_water vp 0 0 1 1.5)

# Creating bathymetry still requires a water value for every selected field.
execute_process(
	COMMAND "${CMAKE_COMMAND}" -E env "GQ_PLUGIN=${GQ_PLUGIN}"
	        "${GQ_RUNNER}" "${flat}" "${surface}"
	        -Oa+b -Mp ${mask}
	        "-E${GQ_TEST_DIR}/flat_surface.nc?elevation"
	        -T-2/3/1 "-G${GQ_TEST_DIR}/missing_water.nc" -Dn -Vq
	RESULT_VARIABLE status)
if(NOT status)
	message(FATAL_ERROR "topobath created bathymetry without -W values")
endif()

# A surface identified by one field is shared by all selected fields, but a
# missing value in another field at that exact boundary remains missing.
run_topobath(missing_surface -Or
	"${GQ_TEST_DIR}/missing_surface.nc?vp,vs" -Cl -T0/2/1)
check3(missing_surface vp 0 0 0 nan)
check3(missing_surface vs 0 0 0 20)

# Without -T, derive increasing positive-down bounds from the model.
run_topobath(inherit_down -Or
	"${GQ_TEST_DIR}/positive_down.nc?vp,vs" ${water})
execute_process(
	COMMAND "${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/inherit_down.nc"
	        z 0 0 1e-9
	RESULT_VARIABLE status)
if(status)
	message(FATAL_ERROR "automatic positive-down top is incorrect")
endif()
execute_process(
	COMMAND "${GQ_CHECK_ATTRIBUTE}" "${GQ_TEST_DIR}/inherit_down.nc"
	        z positive down
	RESULT_VARIABLE status)
if(status)
	message(FATAL_ERROR "automatic positive-down metadata is incorrect")
endif()

# -R and -I define an in-domain target lattice. Horizontal model sampling
# follows -n, while inferred classifications use nearest source nodes.
run_topobath(horizontal -Or "${GQ_TEST_DIR}/flat.nc?vp"
	-R0/1/0/1 -I0.5 -nl -T0/3/1 -Wvp/1.5)
foreach(check "x;1;0.5" "y;1;0.5")
	list(GET check 0 variable)
	list(GET check 1 index)
	list(GET check 2 expected)
	execute_process(
		COMMAND "${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/horizontal.nc"
		        "${variable}" "${index}" "${expected}" 1e-9
		RESULT_VARIABLE status)
	if(status)
		message(FATAL_ERROR "horizontal target coordinate failed: ${check}")
	endif()
endforeach()
check3(horizontal vp 0 1 1 10)

# Each horizontal option may also be supplied independently.
run_topobath(increment_only -Or "${GQ_TEST_DIR}/flat.nc?vp"
	-I0.5 -nl -T0/3/1 -Wvp/1.5)
execute_process(
	COMMAND "${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/increment_only.nc"
	        x 2 1 1e-9
	RESULT_VARIABLE status)
if(status)
	message(FATAL_ERROR "-I did not inherit the model region")
endif()

run_topobath(region_only -Or "${GQ_TEST_DIR}/flat.nc?vp"
	-R0/1/0/1 -T0/3/1 -Wvp/1.5)
execute_process(
	COMMAND "${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/region_only.nc"
	        x 1 1 1e-9
	RESULT_VARIABLE status)
if(status)
	message(FATAL_ERROR "-R did not inherit the model increment")
endif()

# A target lattice that differs from the source only by floating-point
# roundoff is unchanged and retains compatible ancillary variables.
run_topobath(decimal_lattice -Or "${GQ_TEST_DIR}/decimal.nc?vp"
	-Cl -R0/0.6/0/0.6 -I0.2 -T0/1/1)
execute_process(
	COMMAND "${GQ_CHECK_GRID}" "${GQ_TEST_DIR}/decimal_lattice.nc"
	        quality 3 3 16 1e-6
	RESULT_VARIABLE status)
if(status)
	message(FATAL_ERROR
	        "topobath resampled a lattice that differed only by roundoff")
endif()

run_topobath(cropped -Or "${GQ_TEST_DIR}/flat.nc?vp"
	-R0/0.5/0/1 -I0.5 -nl -T0/3/1 -Wvp/1.5)
execute_process(
	COMMAND "${GQ_CHECK_NETCDF}" "${GQ_TEST_DIR}/cropped.nc"
	        x 1 0.5 1e-9
	RESULT_VARIABLE status)
if(status)
	message(FATAL_ERROR "topobath -R crop was not retained")
endif()

execute_process(
	COMMAND "${CMAKE_COMMAND}" -E env "GQ_PLUGIN=${GQ_PLUGIN}"
	        "${GQ_RUNNER}" "${GQ_TEST_DIR}/flat.nc?vp"
	        -Or -R-0.5/1/0/1 -I0.5 -T0/3/1 -Wvp/1.5
	        "-G${GQ_TEST_DIR}/outside.nc" -Dn -Vq
	RESULT_VARIABLE status)
if(NOT status)
	message(FATAL_ERROR "topobath accepted -R outside the model domain")
endif()
foreach(check "x;units;xunit" "y;units;yunit" "z;units;zunit"
	              "vp;units;vpunit" "vs;units;vsunit")
	list(GET check 0 variable)
	list(GET check 1 attribute)
	list(GET check 2 expected)
	execute_process(
		COMMAND "${GQ_CHECK_ATTRIBUTE}" "${GQ_TEST_DIR}/input_transform.nc"
		        "${variable}" "${attribute}" "${expected}"
		RESULT_VARIABLE status)
	if(status)
		message(FATAL_ERROR "topobath transformed units failed: ${check}")
	endif()
endforeach()
