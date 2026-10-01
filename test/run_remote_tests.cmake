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
set(GMT_USERDIR "${GQ_TEST_DIR}/gmt")
set(GMT_CACHE "${GMT_USERDIR}/cache")
file(MAKE_DIRECTORY "${GMT_CACHE}")
file(WRITE "${GQ_TEST_DIR}/gmt.conf"
	"GMT_DATA_UPDATE_INTERVAL = off\n")

function(run_checked)
	execute_process(
		COMMAND ${ARGN}
		RESULT_VARIABLE status
		OUTPUT_VARIABLE output
		ERROR_VARIABLE error
	)
	if(status)
		message(FATAL_ERROR "command failed (${status}): ${ARGN}\n${output}\n${error}")
	endif()
endfunction()

function(run_remote runner)
	message(STATUS "Testing cached remote input: ${ARGN}")
	execute_process(
		COMMAND "${CMAKE_COMMAND}" -E env
		        "GQ_PLUGIN=${GQ_PLUGIN}"
		        "GMT_USERDIR=${GMT_USERDIR}"
		        "${runner}" ${ARGN}
		WORKING_DIRECTORY "${GQ_TEST_DIR}"
		RESULT_VARIABLE status
		OUTPUT_VARIABLE output
		ERROR_VARIABLE error
	)
	if(status)
		message(FATAL_ERROR
		        "remote module run failed (${status}): ${ARGN}\n${output}\n${error}")
	endif()
endfunction()

function(make_netcdf name cdl)
	run_checked("${NCGEN_EXECUTABLE}" -o "${GMT_CACHE}/${name}"
	            "${GQ_SOURCE_DIR}/test/data/${cdl}")
endfunction()

# Common cache fixtures.
configure_file("${GQ_SOURCE_DIR}/test/data/merge1d_primary1.txt"
	"${GMT_CACHE}/remote_merge1d_primary.txt" COPYONLY)
configure_file("${GQ_SOURCE_DIR}/test/data/merge3d_square.txt"
	"${GMT_CACHE}/remote_square.txt" COPYONLY)
configure_file("${GQ_SOURCE_DIR}/test/data/ssh_model1d.txt"
	"${GMT_CACHE}/remote_ssh1d.txt" COPYONLY)
make_netcdf(remote_model2d.nc primary_multi.cdl)
make_netcdf(remote_primary3d.nc primary_multi_3d.cdl)
make_netcdf(remote_secondary3d.nc secondary_multi_3d.cdl)
make_netcdf(remote_topobath_model.nc topobath_flat.cdl)
make_netcdf(remote_topobath_surface.nc topobath_surface.cdl)
make_netcdf(remote_elygtl_model.nc elygtl_model.cdl)
make_netcdf(remote_elygtl_vs30.nc elygtl_vs30.cdl)
make_netcdf(remote_elygtl_mask.nc elygtl_mask.cdl)
make_netcdf(remote_ssh2d.nc ssh_model2d.cdl)
make_netcdf(remote_ssh3d.nc ssh_model3d.cdl)

# merge1d: remote mergefile containing a remote text source.
file(WRITE "${GMT_CACHE}/remote_merge1d.merge"
	"@remote_merge1d_primary.txt - 0/6 boxcar 0\n")
run_remote("${GQ_MERGE1D_RUNNER}" @remote_merge1d.merge
	-T0/6/1 "-G${GQ_TEST_DIR}/merge1d.txt" -Vq)
run_checked("${GQ_CHECK_TABLE}" "${GQ_TEST_DIR}/merge1d.txt"
	5 1 10 1e-10)

# merge2d: remote multiparameter model, mergefile, and polygon.
file(WRITE "${GMT_CACHE}/remote_merge2d.merge"
	"@remote_model2d.nc?vp,vs,den+n-99999 - "
	"@remote_square.txt boxcar/boxcar 0\n")
run_remote("${GQ_MERGE2D_RUNNER}" @remote_merge2d.merge
	-R0/4/0/4 -I1 -Fvp,vs,rho -W
	"-G${GQ_TEST_DIR}/merge2d.nc" -Vq)
run_checked("${GQ_CHECK_2D}" "${GQ_TEST_DIR}/merge2d.nc"
	vs 1 1 20 1e-6)

# merge3d: remote mergefile, selected cubes, modifiers, and polygon.
file(WRITE "${GMT_CACHE}/remote_merge3d.merge"
	"@remote_primary3d.nc?vp,den+z-1/1000 "
	"@remote_secondary3d.nc?p,rho "
	"@remote_square.txt 0/2 cosine/cosine/cosine 0.25/0.25/0.25\n")
run_remote("${GQ_MERGE3D_RUNNER}" @remote_merge3d.merge
	-R0/2/0/2 -I1 -T0/2/1 -Fvp,rho -W
	"-G${GQ_TEST_DIR}/merge3d.nc" -nl -Vq)
run_checked("${GQ_CHECK_3D}" "${GQ_TEST_DIR}/merge3d.nc"
	vp 1 1 1 112 1e-5)

# topobath: remote 3-D model and remote topography grid.
run_remote("${GQ_TOPOBATH_RUNNER}"
	"@remote_topobath_model.nc?vp,vs"
	"@remote_topobath_surface.nc?elevation+s1"
	"-G${GQ_TEST_DIR}/topobath.nc" -Oa -Mp -T-2/3/1
	-Wvp/1.5+t0.01 -Wvs/0+t0.01 -Dn -Vq)
run_checked("${GQ_CHECK_3D}" "${GQ_TEST_DIR}/topobath.nc"
	vp 0 0 0 10 1e-5)

# elygtl: remote model, Vs30 grid, and classification mask.
run_remote("${GQ_ELYGTL_RUNNER}"
	@remote_elygtl_model.nc "@remote_elygtl_vs30.nc?vs30"
	"-K@remote_elygtl_mask.nc?mask"
	"-G${GQ_TEST_DIR}/elygtl.nc" -Fvs=vs -Dn -Vq)
run_checked("${GQ_CHECK_3D}" "${GQ_TEST_DIR}/elygtl.nc"
	vs 0 0 0 200 1e-2)

# SSH application modes: remote text, 2-D NetCDF, and 3-D NetCDF models.
run_remote("${GQ_SSH_RUNNER}" ssh1d @remote_ssh1d.txt -A -Fa,b
	"-G${GQ_TEST_DIR}/ssh1d.txt" -D0.05 -C2 -Q42 -Vq)
file(STRINGS "${GQ_TEST_DIR}/ssh1d.txt" ssh1d_lines)
list(LENGTH ssh1d_lines ssh1d_count)
if(NOT ssh1d_count EQUAL 11)
	message(FATAL_ERROR "remote ssh1d output has ${ssh1d_count} rows")
endif()

run_remote("${GQ_SSH_RUNNER}" ssh2d @remote_ssh2d.nc -A -Fvp
	"-G${GQ_TEST_DIR}/ssh2d.nc" -D0.1 -C2/1 -Q42 -Vq)
run_checked("${GQ_CHECK_SSH}" "${GQ_TEST_DIR}/ssh2d.nc"
	vp 1000 100 1e-3)

run_remote("${GQ_SSH_RUNNER}" ssh3d @remote_ssh3d.nc -A -Fvp
	"-G${GQ_TEST_DIR}/ssh3d.nc" -D0.1 -C2/1/1 -Q42 -Vq)
run_checked("${GQ_CHECK_SSH}" "${GQ_TEST_DIR}/ssh3d.nc"
	vp 2000 200 1e-2)
