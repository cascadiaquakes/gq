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
	int ncid = -1, varid, ndims, dimids[NC_MAX_VAR_DIMS];
	size_t start[2], lengths[2];
	float actual, expected, tolerance;

	if (argc < 7) {
		fprintf(stderr, "usage: %s file variable row column expected tolerance [long_name]\n", argv[0]);
		return EXIT_FAILURE;
	}
	if (nc_open(argv[1], NC_NOWRITE, &ncid) != NC_NOERR ||
	    nc_inq_varid(ncid, argv[2], &varid) != NC_NOERR ||
	    nc_inq_varndims(ncid, varid, &ndims) != NC_NOERR ||
	    ndims != 2 ||
	    nc_inq_vardimid(ncid, varid, dimids) != NC_NOERR ||
	    nc_inq_dimlen(ncid, dimids[0], &lengths[0]) != NC_NOERR ||
	    nc_inq_dimlen(ncid, dimids[1], &lengths[1]) != NC_NOERR) {
		fprintf(stderr, "could not inspect %s variable %s\n", argv[1], argv[2]);
		if (ncid >= 0) nc_close(ncid);
		return EXIT_FAILURE;
	}
	start[0] = (size_t)strtoull(argv[3], NULL, 10);
	start[1] = (size_t)strtoull(argv[4], NULL, 10);
	if (start[0] >= lengths[0] || start[1] >= lengths[1] ||
	    nc_get_var1_float(ncid, varid, start, &actual) != NC_NOERR) {
		nc_close(ncid);
		return EXIT_FAILURE;
	}
	expected = strtof(argv[5], NULL);
	tolerance = strtof(argv[6], NULL);
	if ((isnan(expected) && !isnan(actual)) ||
	    (!isnan(expected) && (isnan(actual) || fabsf(actual - expected) > tolerance))) {
		fprintf(stderr, "%s[%zu,%zu] is %.9g, expected %.9g\n",
		        argv[2], start[0], start[1], actual, expected);
		nc_close(ncid);
		return EXIT_FAILURE;
	}
	if (argc > 7) {
		size_t length;
		char *value;
		if (nc_inq_attlen(ncid, varid, "long_name", &length) != NC_NOERR) {
			nc_close(ncid);
			return EXIT_FAILURE;
		}
		value = calloc(length + 1, 1);
		if (value == NULL || nc_get_att_text(ncid, varid, "long_name", value) != NC_NOERR ||
		    strcmp(value, argv[7])) {
			fprintf(stderr, "%s long_name does not match %s\n", argv[2], argv[7]);
			free(value);
			nc_close(ncid);
			return EXIT_FAILURE;
		}
		free(value);
	}
	nc_close(ncid);
	return EXIT_SUCCESS;
}
