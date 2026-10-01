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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
	FILE *fp;
	char line[8192];
	size_t wanted_row, wanted_col, row = 0;
	double expected, tolerance;

	if (argc != 6) {
		fprintf(stderr, "usage: %s file row column expected tolerance\n", argv[0]);
		return EXIT_FAILURE;
	}
	wanted_row = (size_t)strtoull(argv[2], NULL, 10);
	wanted_col = (size_t)strtoull(argv[3], NULL, 10);
	expected = strtod(argv[4], NULL);
	tolerance = strtod(argv[5], NULL);
	fp = fopen(argv[1], "r");
	if (fp == NULL) return EXIT_FAILURE;
	while (fgets(line, sizeof(line), fp)) {
		char *save = NULL, *token;
		size_t col = 0;
		if (line[0] == '#') continue;
		for (token = strtok_r(line, " \t\r\n", &save); token;
		     token = strtok_r(NULL, " \t\r\n", &save), col++) {
			if (row == wanted_row && col == wanted_col) {
				double value = strtod(token, NULL);
				fclose(fp);
				if ((isnan(expected) && isnan(value)) ||
				    fabs(value - expected) <= tolerance)
					return EXIT_SUCCESS;
				fprintf(stderr, "row %zu column %zu: got %.12g, expected %.12g\n",
				        row, col, value, expected);
				return EXIT_FAILURE;
			}
		}
		row++;
	}
	fclose(fp);
	return EXIT_FAILURE;
}
