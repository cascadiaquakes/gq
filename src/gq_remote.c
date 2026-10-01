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

#include "gmt_dev.h"
#include "gq_remote.h"

#include <stdlib.h>
#include <string.h>

int gq_resolve_remote_path(void *API, unsigned int family,
                           const char *source, char **resolved)
{
	char *path;

	if (!source || !resolved) return GMT_ARG_IS_NULL;
	*resolved = NULL;
	path = strdup(source);
	if (!path) return GMT_MEMORY_ERROR;
	if (source[0] == '@' &&
	    GMT_Get_FilePath(API, family, GMT_IN, GMT_FILE_REMOTE, &path) != GMT_NOERROR) {
		free(path);
		return GMT_DATA_READ_ERROR;
	}
	*resolved = path;
	return GMT_NOERROR;
}

int gq_resolve_remote_source(void *API, unsigned int family,
                             const char *source, const char *modifier_codes,
                             char **resolved)
{
	const char *suffix = NULL, *p;
	char *base = NULL, *path = NULL;
	size_t path_length, total;
	int status;

	if (!source || !resolved) return GMT_ARG_IS_NULL;
	*resolved = NULL;
	if (source[0] != '@') {
		*resolved = strdup(source);
		return *resolved ? GMT_NOERROR : GMT_MEMORY_ERROR;
	}
	suffix = strchr(source, '?');
	if (modifier_codes)
		for (p = source; (p = strchr(p, '+')) != NULL; p++)
			if (p[1] && strchr(modifier_codes, p[1]) &&
			    (!suffix || p < suffix)) {
				suffix = p;
				break;
			}
	path_length = suffix ? (size_t)(suffix - source) : strlen(source);
	base = calloc(path_length + 1, 1);
	if (!base) return GMT_MEMORY_ERROR;
	memcpy(base, source, path_length);
	status = gq_resolve_remote_path(API, family, base, &path);
	free(base);
	if (status != GMT_NOERROR) return status;
	if (!suffix) {
		*resolved = path;
		return GMT_NOERROR;
	}
	total = strlen(path) + strlen(suffix) + 1;
	*resolved = calloc(total, 1);
	if (!*resolved) {
		free(path);
		return GMT_MEMORY_ERROR;
	}
	strcpy(*resolved, path);
	strcat(*resolved, suffix);
	free(path);
	return GMT_NOERROR;
}
