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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
	int ncid = -1, varid;
	size_t length;
	char *value = NULL;
	int status = EXIT_FAILURE;

	if (argc != 5) {
		fprintf(stderr, "usage: %s file variable attribute expected\n", argv[0]);
		return EXIT_FAILURE;
	}
	if (nc_open(argv[1], NC_NOWRITE, &ncid) != NC_NOERR ||
	    nc_inq_varid(ncid, argv[2], &varid) != NC_NOERR ||
	    nc_inq_attlen(ncid, varid, argv[3], &length) != NC_NOERR)
		goto cleanup;
	value = calloc(length + 1, 1);
	if (!value || nc_get_att_text(ncid, varid, argv[3], value) != NC_NOERR)
		goto cleanup;
	if (strcmp(value, argv[4])) {
		fprintf(stderr, "%s:%s is '%s', expected '%s'\n",
		        argv[2], argv[3], value, argv[4]);
		goto cleanup;
	}
	status = EXIT_SUCCESS;

cleanup:
	free(value);
	if (ncid >= 0) nc_close(ncid);
	return status;
}
