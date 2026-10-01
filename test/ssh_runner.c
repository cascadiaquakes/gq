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

typedef int (*ssh_function)(void *, int, void *);

int main(int argc, char **argv)
{
	const char *plugin_path = getenv("GQ_PLUGIN");
	void *plugin = NULL, *API = NULL;
	ssh_function function;
	char symbol[64], command[8192] = {0};
	size_t used = 0;
	int k, status;

	if (!plugin_path || argc < 3) return EXIT_FAILURE;
	plugin = dlopen(plugin_path, RTLD_NOW | RTLD_GLOBAL);
	if (!plugin) {
		fprintf(stderr, "%s\n", dlerror());
		return EXIT_FAILURE;
	}
	snprintf(symbol, sizeof(symbol), "GMT_%s", argv[1]);
	function = (ssh_function)dlsym(plugin, symbol);
	if (!function) {
		fprintf(stderr, "%s\n", dlerror());
		dlclose(plugin);
		return EXIT_FAILURE;
	}
	for (k = 2; k < argc; k++) {
		int written = snprintf(command + used, sizeof(command) - used,
		                       "%s%s", k == 2 ? "" : " ", argv[k]);
		if (written < 0 || (size_t)written >= sizeof(command) - used) {
			dlclose(plugin);
			return EXIT_FAILURE;
		}
		used += (size_t)written;
	}
	API = GMT_Create_Session("ssh-test", 2U, 0U, NULL);
	if (!API) {
		dlclose(plugin);
		return EXIT_FAILURE;
	}
	status = function(API, GMT_MODULE_CMD, command);
	if (GMT_Destroy_Session(API) != GMT_NOERROR) status = EXIT_FAILURE;
	dlclose(plugin);
	return status == GMT_NOERROR ? EXIT_SUCCESS : EXIT_FAILURE;
}
