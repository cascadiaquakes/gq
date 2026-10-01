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

int main(int argc, char **argv)
{
	int ncid = -1, varid, ndims;
	size_t index[3] = {0, 0, 0};
	double actual, expected, tolerance;

	if (argc != 6 && argc != 8) {
		fprintf(stderr,
		        "usage: %s file variable i [j k] expected tolerance\n", argv[0]);
		return EXIT_FAILURE;
	}
	if (nc_open(argv[1], NC_NOWRITE, &ncid) != NC_NOERR ||
	    nc_inq_varid(ncid, argv[2], &varid) != NC_NOERR ||
	    nc_inq_varndims(ncid, varid, &ndims) != NC_NOERR ||
	    ndims != (argc == 6 ? 1 : 3)) {
		if (ncid >= 0) nc_close(ncid);
		return EXIT_FAILURE;
	}
	index[0] = (size_t)strtoull(argv[3], NULL, 10);
	if (ndims == 1) {
		expected = strtod(argv[4], NULL);
		tolerance = strtod(argv[5], NULL);
	}
	else {
		index[1] = (size_t)strtoull(argv[4], NULL, 10);
		index[2] = (size_t)strtoull(argv[5], NULL, 10);
		expected = strtod(argv[6], NULL);
		tolerance = strtod(argv[7], NULL);
	}
	if (nc_get_var1_double(ncid, varid, index, &actual) != NC_NOERR ||
	    (isnan(expected) ? !isnan(actual)
	                     : isnan(actual) || fabs(actual - expected) > tolerance)) {
		fprintf(stderr, "%s value is %.12g, expected %.12g\n",
		        argv[2], actual, expected);
		nc_close(ncid);
		return EXIT_FAILURE;
	}
	nc_close(ncid);
	return EXIT_SUCCESS;
}
