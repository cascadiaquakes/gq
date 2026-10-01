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

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
	int ncid = -1, varid, found;
	bool expected;
	if (argc != 4 || (strcmp(argv[3], "present") && strcmp(argv[3], "absent"))) {
		fprintf(stderr, "usage: %s file variable present|absent\n", argv[0]);
		return EXIT_FAILURE;
	}
	expected = !strcmp(argv[3], "present");
	if (nc_open(argv[1], NC_NOWRITE, &ncid) != NC_NOERR) return EXIT_FAILURE;
	found = nc_inq_varid(ncid, argv[2], &varid) == NC_NOERR;
	nc_close(ncid);
	if ((bool)found != expected) {
		fprintf(stderr, "%s is %s, expected %s\n", argv[2],
		        found ? "present" : "absent", argv[3]);
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
