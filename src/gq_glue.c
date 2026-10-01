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
/* gmt_gq_glue.c populates the external array of this shared lib with
 * module parameters such as name, group, purpose and keys strings.
 * This file also contains the following convenience functions to
 * display all module purposes, list their names, or return keys or group:
 *
 *   int gq_module_show_all    (void *API);
 *   int gq_module_list_all    (void *API);
 *   int gq_module_classic_all (void *API);
 *
 * These functions may be called by gmt --help and gmt --show-modules
 *
 * Developers of external APIs for accessing GMT modules will use this
 * function indirectly via GMT_Encode_Options to retrieve option keys
 * needed for module arg processing:
 *
 *   const char * gq_module_keys  (void *API, char *candidate);
 *   const char * gq_module_group (void *API, char *candidate);
 *
 * All functions are exported by the shared gq library so that gmt can call these
 * functions by name to learn about the contents of the library.
 */

#include "gmt_dev.h"
#include "gq_version.h"

/* Sorted array with information for all GMT gq modules */
static struct GMT_MODULEINFO modules[] = {
	{"elygtl", "elygtl", "gq", "Apply Ely geotechnical layering to three-dimensional multiparameter NetCDF cubes", "<G{,GG}"},
	{"merge1d", "merge1d", "gq", "Tile or smoothly merge one-dimensional tables and multiparameter NetCDF series", "<D{"},
	{"merge2d", "merge2d", "gq", "Tile or smoothly merge two-dimensional multiparameter NetCDF grids", "<G{+,GG}"},
	{"merge3d", "merge3d", "gq", "Tile or smoothly merge three-dimensional multiparameter NetCDF cubes", "<G{+,GG}"},
	{"ssh1d", "ssh1d", "gq", "Generate or apply one-dimensional small-scale heterogeneities in tables and multiparameter NetCDF series", "<D{"},
	{"ssh2d", "ssh2d", "gq", "Generate or apply two-dimensional small-scale heterogeneities in multiparameter NetCDF grids", "<G{,GG}"},
	{"ssh3d", "ssh3d", "gq", "Generate or apply three-dimensional small-scale heterogeneities in multiparameter NetCDF cubes", "<G{,GG}"},
	{"topobath", "topobath", "gq", "Add, remove, or replace topography and/or bathymetry in three-dimensional multiparameter NetCDF cubes", "<G{,GG}"},
	{NULL, NULL, NULL, NULL, NULL} /* last element == NULL detects end of array */
};

/* Return the GQ release used to build this plug-in. */
EXTERN_MSC const char *gq_module_version (void) {
	return (GQ_VERSION);
}

/* Pretty print all shared module names and their purposes for gmt --help */
EXTERN_MSC int gq_module_show_all (void *API) {
	return (GMT_Show_ModuleInfo (API, modules, "The CRESCENT cyberinfrastructure team supplements to the Generic Mapping Tools", GMT_MODULE_HELP));
}

/* Produce single list on stdout of all shared module names for gmt --show-modules */
EXTERN_MSC int gq_module_list_all (void *API) {
	return (GMT_Show_ModuleInfo (API, modules, NULL, GMT_MODULE_SHOW_MODERN));
}

/* Produce single list on stdout of all shared module names for gmt --show-classic [i.e., classic mode names] */
EXTERN_MSC int gq_module_classic_all (void *API) {
	return (GMT_Show_ModuleInfo (API, modules, NULL, GMT_MODULE_SHOW_CLASSIC));
}

/* Lookup module id by name, return option keys pointer (for external API developers) */
EXTERN_MSC const char *gq_module_keys (void *API, char *candidate) {
	return (GMT_Get_ModuleInfo (API, modules, candidate, GMT_MODULE_KEYS));
}

/* Lookup module id by name, return group char name (for external API developers) */
EXTERN_MSC const char *gq_module_group (void *API, char *candidate) {
	return (GMT_Get_ModuleInfo (API, modules, candidate, GMT_MODULE_GROUP));
}
