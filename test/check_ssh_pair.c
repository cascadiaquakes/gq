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
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double *read_variable(int ncid, const char *name, size_t *count)
{
	int varid, ndims, dimids[NC_MAX_VAR_DIMS], k;
	size_t length;
	double *value;
	*count = 1;
	if (nc_inq_varid(ncid, name, &varid) != NC_NOERR ||
	    nc_inq_varndims(ncid, varid, &ndims) != NC_NOERR ||
	    nc_inq_vardimid(ncid, varid, dimids) != NC_NOERR)
		return NULL;
	for (k = 0; k < ndims; k++) {
		if (nc_inq_dimlen(ncid, dimids[k], &length) != NC_NOERR) return NULL;
		*count *= length;
	}
	value = calloc(*count, sizeof(*value));
	if (!value || nc_get_var_double(ncid, varid, value) != NC_NOERR) {
		free(value);
		return NULL;
	}
	return value;
}

int main(int argc, char **argv)
{
	int ncid = -1;
	size_t count_a, count_b, k;
	double *a = NULL, *b = NULL, ratio, tolerance;
	bool different = false;

	if (argc != 7) {
		fprintf(stderr,
		        "usage: %s file var_a var_b equal|different|ratio value tolerance\n",
		        argv[0]);
		return EXIT_FAILURE;
	}
	ratio = strtod(argv[5], NULL);
	tolerance = strtod(argv[6], NULL);
	if (nc_open(argv[1], NC_NOWRITE, &ncid) != NC_NOERR ||
	    !(a = read_variable(ncid, argv[2], &count_a)) ||
	    !(b = read_variable(ncid, argv[3], &count_b)) ||
	    count_a != count_b)
		goto fail;
	for (k = 0; k < count_a; k++) {
		if (!isfinite(a[k]) || !isfinite(b[k])) continue;
		if (!strcmp(argv[4], "equal") && fabs(a[k] - b[k]) > tolerance)
			goto fail;
		if (!strcmp(argv[4], "ratio") &&
		    fabs(b[k] - ratio * a[k]) > tolerance)
			goto fail;
		if (!strcmp(argv[4], "different") &&
		    fabs(a[k] - b[k]) > tolerance)
			different = true;
	}
	if (!strcmp(argv[4], "different") && !different) goto fail;
	if (strcmp(argv[4], "equal") && strcmp(argv[4], "different") &&
	    strcmp(argv[4], "ratio"))
		goto fail;
	free(a);
	free(b);
	nc_close(ncid);
	return EXIT_SUCCESS;
fail:
	free(a);
	free(b);
	if (ncid >= 0) nc_close(ncid);
	return EXIT_FAILURE;
}
