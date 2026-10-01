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

#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

typedef int (*gq_module_function)(void *, int, void *);

int main(int argc, char **argv)
{
	void *API;
	gq_module_function module;
	char symbol[128];
	int status;

#ifdef _WIN32
	HMODULE plugin;
#else
	void *plugin;
#endif

	if (argc != 3) {
		fprintf(stderr, "usage: %s <gq-plugin> <module>\n", argv[0]);
		return EXIT_FAILURE;
	}
	if (snprintf(symbol, sizeof(symbol), "GMT_%s", argv[2]) >=
	    (int)sizeof(symbol)) {
		fputs("GQ module name is too long\n", stderr);
		return EXIT_FAILURE;
	}

#ifdef _WIN32
	plugin = LoadLibraryA(argv[1]);
	if (plugin == NULL) {
		fprintf(stderr, "Unable to load GQ plug-in: %lu\n",
		        (unsigned long)GetLastError());
		return EXIT_FAILURE;
	}
	module = (gq_module_function)GetProcAddress(plugin, symbol);
#else
	plugin = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	if (plugin == NULL) {
		fprintf(stderr, "%s\n", dlerror());
		return EXIT_FAILURE;
	}
	module = (gq_module_function)dlsym(plugin, symbol);
#endif
	if (module == NULL) {
		fprintf(stderr, "Unable to load module symbol %s\n", symbol);
#ifdef _WIN32
		FreeLibrary(plugin);
#else
		dlclose(plugin);
#endif
		return EXIT_FAILURE;
	}

	API = GMT_Create_Session("gq-usage", 2U, 0U, NULL);
	if (API == NULL) {
#ifdef _WIN32
		FreeLibrary(plugin);
#else
		dlclose(plugin);
#endif
		return EXIT_FAILURE;
	}
	status = module(API, GMT_MODULE_CMD, "-?");
	GMT_Destroy_Session(API);
#ifdef _WIN32
	FreeLibrary(plugin);
#else
	dlclose(plugin);
#endif
	return status == GMT_NOERROR ? EXIT_SUCCESS : EXIT_FAILURE;
}
