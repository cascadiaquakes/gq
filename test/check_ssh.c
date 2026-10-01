/*--------------------------------------------------------------------
 *
 * Copyright (c) 2024-2026 by the CRESCENT cyberinfrastructure team (https://cascadiaquakes.org/)
 * See LICENSE for copying and redistribution conditions.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; version 3 or any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * Contact info: abioyeajala@gmail.com (Rasheed Ajala)
 *--------------------------------------------------------------------*/

#include <netcdf.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
	int ncid = -1, varid, ndims, dimids[NC_MAX_VAR_DIMS], k;
	size_t count = 1, length, finite = 0;
	double *value = NULL, mean = 0.0, sumsq = 0.0;
	double expected_mean, expected_std, tolerance;

	if (argc != 6) {
		fprintf(stderr, "usage: %s file variable mean std tolerance\n", argv[0]);
		return EXIT_FAILURE;
	}
	expected_mean = strtod(argv[3], NULL);
	expected_std = strtod(argv[4], NULL);
	tolerance = strtod(argv[5], NULL);
	if (nc_open(argv[1], NC_NOWRITE, &ncid) != NC_NOERR ||
	    nc_inq_varid(ncid, argv[2], &varid) != NC_NOERR ||
	    nc_inq_varndims(ncid, varid, &ndims) != NC_NOERR ||
	    nc_inq_vardimid(ncid, varid, dimids) != NC_NOERR)
		goto fail;
	for (k = 0; k < ndims; k++) {
		if (nc_inq_dimlen(ncid, dimids[k], &length) != NC_NOERR) goto fail;
		count *= length;
	}
	value = calloc(count, sizeof(*value));
	if (!value || nc_get_var_double(ncid, varid, value) != NC_NOERR) goto fail;
	for (length = 0; length < count; length++)
		if (isfinite(value[length])) {
			mean += value[length];
			finite++;
		}
	if (finite < 2) goto fail;
	mean /= (double)finite;
	for (length = 0; length < count; length++)
		if (isfinite(value[length]))
			sumsq += (value[length] - mean) * (value[length] - mean);
	sumsq = sqrt(sumsq / (double)(finite - 1));
	if (fabs(mean - expected_mean) > tolerance ||
	    fabs(sumsq - expected_std) > tolerance) {
		fprintf(stderr, "%s mean/std %.12g/%.12g, expected %.12g/%.12g\n",
		        argv[2], mean, sumsq, expected_mean, expected_std);
		goto fail;
	}
	free(value);
	nc_close(ncid);
	return EXIT_SUCCESS;
fail:
	free(value);
	if (ncid >= 0) nc_close(ncid);
	return EXIT_FAILURE;
}
