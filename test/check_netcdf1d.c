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

#include <math.h>
#include <netcdf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
	int ncid = -1, varid, ndims;
	size_t index;
	double expected, tolerance, value;

	if (argc < 6 || argc > 7) {
		fprintf(stderr,
		        "usage: %s file variable index expected tolerance [long_name]\n",
		        argv[0]);
		return EXIT_FAILURE;
	}
	index = (size_t)strtoull(argv[3], NULL, 10);
	expected = strtod(argv[4], NULL);
	tolerance = strtod(argv[5], NULL);
	if (nc_open(argv[1], NC_NOWRITE, &ncid) != NC_NOERR ||
	    nc_inq_varid(ncid, argv[2], &varid) != NC_NOERR ||
	    nc_inq_varndims(ncid, varid, &ndims) != NC_NOERR || ndims != 1 ||
	    nc_get_var1_double(ncid, varid, &index, &value) != NC_NOERR) {
		if (ncid >= 0) nc_close(ncid);
		return EXIT_FAILURE;
	}
	if (!((isnan(expected) && isnan(value)) ||
	      fabs(value - expected) <= tolerance)) {
		fprintf(stderr, "%s[%zu]: got %.12g, expected %.12g\n",
		        argv[2], index, value, expected);
		nc_close(ncid);
		return EXIT_FAILURE;
	}
	if (argc == 7) {
		size_t length;
		char *attribute;
		if (nc_inq_attlen(ncid, varid, "long_name", &length) != NC_NOERR) {
			nc_close(ncid);
			return EXIT_FAILURE;
		}
		attribute = calloc(length + 1, 1);
		if (attribute == NULL ||
		    nc_get_att_text(ncid, varid, "long_name", attribute) != NC_NOERR ||
		    strcmp(attribute, argv[6])) {
			free(attribute);
			nc_close(ncid);
			return EXIT_FAILURE;
		}
		free(attribute);
	}
	nc_close(ncid);
	return EXIT_SUCCESS;
}
