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

#include <gmt.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int (*merge2d_function)(void *, int, void *);

int main(int argc, char **argv)
{
	void *API;
	void *plugin;
	merge2d_function merge2d;
	char *command;
	const char *plugin_path;
	size_t length = 1;
	int i, status;

	plugin_path = getenv("GQ_PLUGIN");
	if (plugin_path == NULL) {
		fputs("GQ_PLUGIN must name the newly built gq plugin\n", stderr);
		return EXIT_FAILURE;
	}
	plugin = dlopen(plugin_path, RTLD_NOW | RTLD_LOCAL);
	if (plugin == NULL) {
		fprintf(stderr, "%s\n", dlerror());
		return EXIT_FAILURE;
	}
	merge2d = (merge2d_function)dlsym(plugin, "GMT_merge2d");
	if (merge2d == NULL) {
		fprintf(stderr, "%s\n", dlerror());
		dlclose(plugin);
		return EXIT_FAILURE;
	}

	for (i = 1; i < argc; i++) length += strlen(argv[i]) + 1;
	command = calloc(length, 1);
	if (command == NULL) return EXIT_FAILURE;
	for (i = 1; i < argc; i++) {
		if (i > 1) strcat(command, " ");
		strcat(command, argv[i]);
	}

	API = GMT_Create_Session("merge2d-test", 2U, 0U, NULL);
	if (API == NULL) {
		free(command);
		dlclose(plugin);
		return EXIT_FAILURE;
	}
	status = merge2d(API, GMT_MODULE_CMD, command);
	GMT_Destroy_Session(API);
	free(command);
	dlclose(plugin);
	return status == GMT_NOERROR ? EXIT_SUCCESS : EXIT_FAILURE;
}
