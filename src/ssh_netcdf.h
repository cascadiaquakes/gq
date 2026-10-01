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

/* NetCDF helpers shared by ssh1d, ssh2d, and ssh3d. */

#ifndef SSH_NETCDF_H
#define SSH_NETCDF_H

#include "gq_transform.h"

#include <netcdf.h>

struct SSH_NC_FIELD {
	int varid;
	int dimids[SSH_MAX_DIM];
	int axis_position[SSH_MAX_DIM];
	char *name;
};

struct SSH_NC {
	char *path;
	int ncid;
	unsigned int dim;
	bool has_sentinel;
	double sentinel;
	struct GQ_TRANSFORM transform;
	bool lattice_changed[SSH_MAX_DIM];
	size_t n[SSH_MAX_DIM];
	int axis_dimid[SSH_MAX_DIM];
	int coordinate_varid[SSH_MAX_DIM];
	char *coordinate_name[SSH_MAX_DIM];
	double *coordinate[SSH_MAX_DIM];
	double increment[SSH_MAX_DIM];
	size_t n_fields;
	struct SSH_NC_FIELD *field;
};

int ssh_nc_open(struct GMTAPI_CTRL *API, const char *source, unsigned int dim,
                char **fields, size_t n_fields, struct SSH_NC *cube);
void ssh_nc_free(struct SSH_NC *cube);
int ssh_nc_read_field(struct GMTAPI_CTRL *API, const struct SSH_NC *cube,
                      size_t field, double **values);
int ssh_nc_write_synthetic(struct GMTAPI_CTRL *API, const char *path,
                           unsigned int dim, const size_t n[SSH_MAX_DIM],
                           const double *coordinate[SSH_MAX_DIM],
                           char **fields, size_t n_fields,
                           double **values, const double *weight,
                           const struct GQ_TRANSFORM *output_transform);
int ssh_nc_write_application(struct GMTAPI_CTRL *API, const char *path,
                             const struct SSH_NC *cube, double **values,
                             const double *weight,
                             const struct GQ_TRANSFORM *output_transform);

#endif
