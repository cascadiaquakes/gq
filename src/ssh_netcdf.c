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
#include "ssh_common.h"
#include "ssh_netcdf.h"

#include <ctype.h>
#include <errno.h>

struct SSH_NC_SOURCE {
	char *path;
	bool has_sentinel;
	double sentinel;
	struct GQ_TRANSFORM transform;
};

static char *ssh_nc_text_attribute(int ncid, int varid, const char *name)
{
	size_t length;
	char *value;
	if (nc_inq_attlen(ncid, varid, name, &length) != NC_NOERR) return NULL;
	value = calloc(length + 1, 1);
	if (!value) return NULL;
	if (nc_get_att_text(ncid, varid, name, value) != NC_NOERR) {
		free(value);
		return NULL;
	}
	return value;
}

static bool ssh_nc_numeric(nc_type type)
{
	return type == NC_BYTE || type == NC_UBYTE || type == NC_SHORT ||
	       type == NC_USHORT || type == NC_INT || type == NC_UINT ||
	       type == NC_INT64 || type == NC_UINT64 || type == NC_FLOAT ||
	       type == NC_DOUBLE;
}

static char *ssh_nc_modifier(char *text)
{
	char *p;
	for (p = text; *p; p++) {
		if (*p != '+') continue;
		if (p > text && (p[-1] == 'e' || p[-1] == 'E')) continue;
		if (strchr("nxyzXYZvV", p[1])) return p;
	}
	return p;
}

static int ssh_nc_parse_source(struct GMTAPI_CTRL *API, const char *text,
                               unsigned int dim,
                               struct SSH_NC_SOURCE *source)
{
	char *copy = NULL, *modifier;
	char message[GMT_LEN256];
	memset(source, 0, sizeof(*source));
	gq_transform_init(&source->transform);
	copy = strdup(text);
	if (!copy) return GMT_MEMORY_ERROR;
	modifier = ssh_nc_modifier(copy);
	if (*modifier) *modifier++ = '\0';
	if (modifier &&
	    gq_transform_parse(modifier,
	                       GQ_TRANSFORM_X_MASK |
	                       (dim >= 2 ? GQ_TRANSFORM_Y_MASK : 0U) |
	                       (dim >= 3 ? GQ_TRANSFORM_Z_MASK : 0U),
	                       true, &source->transform,
	                       &source->has_sentinel, &source->sentinel,
	                       message, sizeof(message)))
		goto bad;
	if (!copy[0]) goto bad;
	source->path = strdup(copy);
	free(copy);
	return source->path ? GMT_NOERROR : GMT_MEMORY_ERROR;
bad:
	GMT_Report(API, GMT_MSG_ERROR,
	           "Invalid model source %s; use +n, coordinate, value, and "
	           "target-unit modifiers\n", text);
	free(copy);
	free(source->path);
	gq_transform_free(&source->transform);
	return GMT_PARSE_ERROR;
}

static int ssh_nc_axis_name(const char *text)
{
	char lower[NC_MAX_NAME + 1];
	size_t k, length;
	if (!text || (length = strlen(text)) > NC_MAX_NAME) return -1;
	for (k = 0; k <= length; k++)
		lower[k] = (char)tolower((unsigned char)text[k]);
	if (!strcmp(lower, "x") || !strcmp(lower, "lon") ||
	    !strcmp(lower, "longitude") || !strcmp(lower, "easting") ||
	    !strcmp(lower, "time") || !strcmp(lower, "distance"))
		return 0;
	if (!strcmp(lower, "y") || !strcmp(lower, "lat") ||
	    !strcmp(lower, "latitude") || !strcmp(lower, "northing"))
		return 1;
	if (!strcmp(lower, "z") || !strcmp(lower, "depth") ||
	    !strcmp(lower, "elevation") || !strcmp(lower, "altitude") ||
	    !strcmp(lower, "level"))
		return 2;
	return -1;
}

static int ssh_nc_coordinate_axis(int ncid, int varid, const char *name)
{
	char *attribute;
	int axis = -1;
	attribute = ssh_nc_text_attribute(ncid, varid, "axis");
	if (attribute) {
		if ((attribute[0] == 'X' || attribute[0] == 'x' ||
		     attribute[0] == 'T' || attribute[0] == 't') && !attribute[1])
			axis = 0;
		else if ((attribute[0] == 'Y' || attribute[0] == 'y') && !attribute[1])
			axis = 1;
		else if ((attribute[0] == 'Z' || attribute[0] == 'z') && !attribute[1])
			axis = 2;
		free(attribute);
		if (axis >= 0) return axis;
	}
	attribute = ssh_nc_text_attribute(ncid, varid, "standard_name");
	if (attribute) {
		if (strstr(attribute, "longitude") ||
		    strstr(attribute, "projection_x_coordinate") ||
		    strstr(attribute, "time"))
			axis = 0;
		else if (strstr(attribute, "latitude") ||
		         strstr(attribute, "projection_y_coordinate"))
			axis = 1;
		else if (strstr(attribute, "depth") || strstr(attribute, "height") ||
		         strstr(attribute, "altitude"))
			axis = 2;
		free(attribute);
		if (axis >= 0) return axis;
	}
	return ssh_nc_axis_name(name);
}

static bool ssh_nc_same_dimensions(const int *a, const int *b,
                                   unsigned int dim)
{
	unsigned int i, j;
	for (i = 0; i < dim; i++) {
		bool found = false;
		for (j = 0; j < dim; j++) if (a[i] == b[j]) found = true;
		if (!found) return false;
	}
	return true;
}

void ssh_nc_free(struct SSH_NC *cube)
{
	unsigned int axis;
	size_t field;
	if (!cube) return;
	if (cube->ncid >= 0) nc_close(cube->ncid);
	free(cube->path);
	for (field = 0; field < cube->n_fields; field++)
		free(cube->field[field].name);
	free(cube->field);
	for (axis = 0; axis < SSH_MAX_DIM; axis++) {
		free(cube->coordinate_name[axis]);
		free(cube->coordinate[axis]);
	}
	gq_transform_free(&cube->transform);
	memset(cube, 0, sizeof(*cube));
	cube->ncid = -1;
}

int ssh_nc_open(struct GMTAPI_CTRL *API, const char *source_text,
                unsigned int dim, char **fields, size_t n_fields,
                struct SSH_NC *cube)
{
	struct SSH_NC_SOURCE source;
	int reference_dims[SSH_MAX_DIM], status = GMT_DATA_READ_ERROR;
	size_t field;
	unsigned int axis;

	memset(cube, 0, sizeof(*cube));
	gq_transform_init(&cube->transform);
	cube->ncid = -1;
	cube->dim = dim;
	if (!n_fields || ssh_nc_parse_source(API, source_text, dim, &source))
		return GMT_PARSE_ERROR;
	status = gq_resolve_remote_path(
	    API, dim == 1 ? GMT_IS_DATASET : GMT_IS_GRID,
	    source.path, &cube->path);
	if (status != GMT_NOERROR) goto cleanup;
	status = GMT_DATA_READ_ERROR;
	cube->has_sentinel = source.has_sentinel;
	cube->sentinel = source.sentinel;
	cube->transform = source.transform;
	gq_transform_init(&source.transform);
	if (nc_open(cube->path, NC_NOWRITE, &cube->ncid) != NC_NOERR)
		goto cleanup;
	cube->n_fields = n_fields;
	{
		char message[GMT_LEN256];
		if (gq_transform_validate_values(&cube->transform, n_fields,
		                                  message, sizeof(message))) {
			GMT_Report(API, GMT_MSG_ERROR, "%s: %s\n", source_text, message);
			goto cleanup;
		}
	}
	cube->field = calloc(n_fields, sizeof(*cube->field));
	if (!cube->field) goto cleanup;
	for (field = 0; field < n_fields; field++) {
		struct SSH_NC_FIELD *item = &cube->field[field];
		nc_type type;
		int ndims;
		unsigned int position;
		if (nc_inq_varid(cube->ncid, fields[field], &item->varid) != NC_NOERR ||
		    nc_inq_var(cube->ncid, item->varid, NULL, &type, &ndims,
		               item->dimids, NULL) != NC_NOERR ||
		    !ssh_nc_numeric(type) || ndims != (int)dim) {
			GMT_Report(API, GMT_MSG_ERROR,
			           "%s is not a numeric %u-D variable in %s\n",
			           fields[field], dim, cube->path);
			goto cleanup;
		}
		if (field == 0)
			memcpy(reference_dims, item->dimids, dim * sizeof(int));
		else if (!ssh_nc_same_dimensions(reference_dims, item->dimids, dim)) {
			GMT_Report(API, GMT_MSG_ERROR,
			           "Selected variables do not share coordinate dimensions\n");
			goto cleanup;
		}
		item->name = strdup(fields[field]);
		if (!item->name) goto cleanup;
		for (position = 0; position < dim; position++)
			item->axis_position[position] = -1;
	}
	if (dim == 1) {
		char name[NC_MAX_NAME + 1];
		if (nc_inq_dimname(cube->ncid, reference_dims[0], name) != NC_NOERR ||
		    nc_inq_varid(cube->ncid, name, &cube->coordinate_varid[0]) != NC_NOERR)
			goto cleanup;
		cube->axis_dimid[0] = reference_dims[0];
		cube->coordinate_name[0] = strdup(name);
	}
	else {
		for (axis = 0; axis < dim; axis++) {
			char name[NC_MAX_NAME + 1];
			int coordinate_varid, identified;
			if (nc_inq_dimname(cube->ncid, reference_dims[axis], name) != NC_NOERR ||
			    nc_inq_varid(cube->ncid, name, &coordinate_varid) != NC_NOERR)
				goto cleanup;
			identified = ssh_nc_coordinate_axis(cube->ncid, coordinate_varid,
			                                    name);
			if (identified < 0 || identified >= (int)dim ||
			    cube->coordinate_name[identified]) {
				GMT_Report(API, GMT_MSG_ERROR,
				           "Cannot uniquely identify coordinate axes in %s\n",
				           cube->path);
				goto cleanup;
			}
			cube->axis_dimid[identified] = reference_dims[axis];
			cube->coordinate_varid[identified] = coordinate_varid;
			cube->coordinate_name[identified] = strdup(name);
		}
	}
	for (field = 0; field < n_fields; field++) {
		unsigned int position;
		for (axis = 0; axis < dim; axis++)
			for (position = 0; position < dim; position++)
				if (cube->field[field].dimids[position] ==
				    cube->axis_dimid[axis])
					cube->field[field].axis_position[axis] = (int)position;
		for (axis = 0; axis < dim; axis++)
			if (cube->field[field].axis_position[axis] < 0) goto cleanup;
	}
	for (axis = 0; axis < dim; axis++) {
		int coordinate_ndims, coordinate_dimid;
		nc_type type;
		size_t k;
		if (!cube->coordinate_name[axis] ||
		    nc_inq_dimlen(cube->ncid, cube->axis_dimid[axis],
		                  &cube->n[axis]) != NC_NOERR ||
		    nc_inq_vartype(cube->ncid, cube->coordinate_varid[axis],
		                   &type) != NC_NOERR ||
		    !ssh_nc_numeric(type) ||
		    nc_inq_varndims(cube->ncid, cube->coordinate_varid[axis],
		                    &coordinate_ndims) != NC_NOERR ||
		    coordinate_ndims != 1 ||
		    nc_inq_vardimid(cube->ncid, cube->coordinate_varid[axis],
		                    &coordinate_dimid) != NC_NOERR ||
		    coordinate_dimid != cube->axis_dimid[axis])
			goto cleanup;
		cube->coordinate[axis] = calloc(cube->n[axis], sizeof(double));
		if (!cube->coordinate[axis] ||
		    nc_get_var_double(cube->ncid, cube->coordinate_varid[axis],
		                      cube->coordinate[axis]) != NC_NOERR)
			goto cleanup;
		{
			double scale = 1.0, offset = 0.0;
			nc_get_att_double(cube->ncid, cube->coordinate_varid[axis],
			                  "scale_factor", &scale);
			nc_get_att_double(cube->ncid, cube->coordinate_varid[axis],
			                  "add_offset", &offset);
			for (k = 0; k < cube->n[axis]; k++)
				cube->coordinate[axis][k] =
				    (cube->coordinate[axis][k] * scale + offset) *
				    cube->transform.axis_scale[axis];
		}
		if (cube->n[axis] < 2) goto cleanup;
		cube->increment[axis] =
		    (cube->coordinate[axis][cube->n[axis] - 1] -
		     cube->coordinate[axis][0]) / (double)(cube->n[axis] - 1);
		if (!isfinite(cube->increment[axis]) || cube->increment[axis] <= 0.0)
			goto cleanup;
		for (k = 1; k < cube->n[axis]; k++) {
			double step = cube->coordinate[axis][k] -
			              cube->coordinate[axis][k - 1];
			double tolerance = 256.0 * DBL_EPSILON *
			    MAX(1.0, MAX(fabs(step), fabs(cube->increment[axis])));
			if (!isfinite(step) || step <= 0.0 ||
			    fabs(step - cube->increment[axis]) > tolerance) {
				GMT_Report(API, GMT_MSG_ERROR,
				           "SSH coordinates must be finite, regular, and increasing after "
				           "input scaling. Scaling never reorders coordinates or data; use "
				           "a negative input axis scale to convert a descending convention.\n");
				goto cleanup;
			}
		}
	}
	for (axis = dim; axis < SSH_MAX_DIM; axis++) {
		cube->n[axis] = 1;
		cube->increment[axis] = 1.0;
	}
	status = GMT_NOERROR;
cleanup:
	free(source.path);
	gq_transform_free(&source.transform);
	if (status != GMT_NOERROR) {
		GMT_Report(API, GMT_MSG_ERROR,
		           "Unable to read %u-D model metadata from %s\n",
		           dim, source_text);
		ssh_nc_free(cube);
	}
	return status;
}

static size_t ssh_nc_field_index(const struct SSH_NC *cube, size_t field,
                                 size_t i, size_t j, size_t k)
{
	const struct SSH_NC_FIELD *item = &cube->field[field];
	size_t logical[SSH_MAX_DIM] = {i, j, k};
	size_t index[SSH_MAX_DIM] = {0, 0, 0};
	size_t length[SSH_MAX_DIM] = {1, 1, 1};
	size_t stride = 1, offset = 0;
	int position;
	unsigned int axis;
	for (axis = 0; axis < cube->dim; axis++)
		index[item->axis_position[axis]] = logical[axis];
	for (position = 0; position < (int)cube->dim; position++)
		for (axis = 0; axis < cube->dim; axis++)
			if (item->axis_position[axis] == position)
				length[position] = cube->n[axis];
	for (position = (int)cube->dim - 1; position >= 0; position--) {
		offset += index[position] * stride;
		stride *= length[position];
	}
	return offset;
}

static bool ssh_nc_missing(struct GMTAPI_CTRL *API,
                           const struct SSH_NC *cube, int varid, double value)
{
	double missing;
	if (isnan(value)) return true;
	if (cube->has_sentinel && value == cube->sentinel) return true;
	if (nc_get_att_double(cube->ncid, varid, "_FillValue", &missing) == NC_NOERR &&
	    (isnan(missing) ? isnan(value) : value == missing))
		return true;
	if (nc_get_att_double(cube->ncid, varid, "missing_value", &missing) == NC_NOERR &&
	    (isnan(missing) ? isnan(value) : value == missing))
		return true;
	return API->GMT->common.d.active[GMT_IN] &&
	       value == API->GMT->common.d.nan_proxy[GMT_IN];
}

int ssh_nc_read_field(struct GMTAPI_CTRL *API, const struct SSH_NC *cube,
                      size_t field, double **values)
{
	size_t total = cube->n[0] * cube->n[1] * cube->n[2];
	double *raw = calloc(total, sizeof(*raw));
	double scale = 1.0, offset = 0.0;
	double user_scale = gq_transform_value_scale(&cube->transform, field);
	size_t i, j, k, logical = 0;
	int varid = cube->field[field].varid;
	if (!raw) return GMT_MEMORY_ERROR;
	*values = calloc(total, sizeof(**values));
	if (!*values) {
		free(raw);
		return GMT_MEMORY_ERROR;
	}
	if (nc_get_var_double(cube->ncid, varid, raw) != NC_NOERR) {
		free(raw);
		free(*values);
		*values = NULL;
		return GMT_DATA_READ_ERROR;
	}
	nc_get_att_double(cube->ncid, varid, "scale_factor", &scale);
	nc_get_att_double(cube->ncid, varid, "add_offset", &offset);
	for (k = 0; k < cube->n[2]; k++)
		for (j = 0; j < cube->n[1]; j++)
			for (i = 0; i < cube->n[0]; i++, logical++) {
				double value = raw[ssh_nc_field_index(cube, field, i, j, k)];
				(*values)[logical] = ssh_nc_missing(API, cube, varid, value)
				                   ? NAN : (value * scale + offset) * user_scale;
			}
	free(raw);
	return GMT_NOERROR;
}

static bool ssh_nc_skip_attribute(const char *name)
{
	return !strcmp(name, "_FillValue") || !strcmp(name, "missing_value") ||
	       !strcmp(name, "scale_factor") || !strcmp(name, "add_offset") ||
	       !strcmp(name, "actual_range");
}

static int ssh_nc_copy_attributes(int input, int input_var,
                                  int output, int output_var, bool unpacked)
{
	int natts, k;
	if (nc_inq_varnatts(input, input_var, &natts) != NC_NOERR)
		return NC_EINVAL;
	for (k = 0; k < natts; k++) {
		char name[NC_MAX_NAME + 1];
		if (nc_inq_attname(input, input_var, k, name) != NC_NOERR)
			return NC_EINVAL;
		if (unpacked && ssh_nc_skip_attribute(name)) continue;
		if (nc_copy_att(input, input_var, name, output, output_var) != NC_NOERR)
			return NC_EINVAL;
	}
	return NC_NOERR;
}

static int ssh_nc_selected(const struct SSH_NC *cube, int varid)
{
	size_t field;
	for (field = 0; field < cube->n_fields; field++)
		if (cube->field[field].varid == varid) return (int)field;
	return -1;
}

static int ssh_nc_coordinate(const struct SSH_NC *cube, int varid)
{
	unsigned int axis;
	for (axis = 0; axis < cube->dim; axis++)
		if (cube->coordinate_varid[axis] == varid) return (int)axis;
	return -1;
}

static int ssh_nc_replace_units(int ncid, int varid, const char *units,
                                bool invalidate)
{
	if (units) {
		nc_del_att(ncid, varid, "units");
		return nc_put_att_text(ncid, varid, "units", strlen(units), units);
	}
	if (invalidate) nc_del_att(ncid, varid, "units");
	return NC_NOERR;
}

static int ssh_nc_update_positive(const struct SSH_NC *cube,
	                              const struct GQ_TRANSFORM *output,
	                              int ncid, int varid)
{
	char *positive;
	const char *replacement = NULL;
	double sign;
	if (cube->dim < 3) return NC_NOERR;
	sign = cube->transform.axis_scale[2] * output->axis_scale[2];
	if (sign >= 0.0) return NC_NOERR;
	positive = ssh_nc_text_attribute(cube->ncid, cube->coordinate_varid[2],
	                                 "positive");
	if (!positive) return NC_NOERR;
	if (!strcasecmp(positive, "up")) replacement = "down";
	else if (!strcasecmp(positive, "down")) replacement = "up";
	free(positive);
	if (!replacement) return NC_NOERR;
	nc_del_att(ncid, varid, "positive");
	return nc_put_att_text(ncid, varid, "positive", strlen(replacement),
	                       replacement);
}

static const char *ssh_nc_output_axis_units(
    const struct SSH_NC *cube, const struct GQ_TRANSFORM *output, size_t axis)
{
	if (output->axis_unit[axis]) return output->axis_unit[axis];
	if (output->axis_scale[axis] != 1.0) return NULL;
	return cube->transform.axis_unit[axis];
}

static const char *ssh_nc_output_field_units(
    const struct SSH_NC *cube, const struct GQ_TRANSFORM *output, size_t field)
{
	const char *units = gq_transform_value_unit(output, field);
	if (units) return units;
	if (gq_transform_value_scale(output, field) != 1.0) return NULL;
	return gq_transform_value_unit(&cube->transform, field);
}

static bool ssh_nc_incompatible_ancillary(const struct SSH_NC *cube,
	                                      const int *dimids, int ndims)
{
	int position;
	unsigned int axis;
	for (position = 0; position < ndims; position++)
		for (axis = 0; axis < cube->dim; axis++)
			if (dimids[position] == cube->axis_dimid[axis] &&
			    cube->lattice_changed[axis])
				return true;
	return false;
}

static int ssh_nc_copy_variable(const struct SSH_NC *cube, int input_varid,
                                int output_ncid, int output_varid,
                                nc_type type, size_t count)
{
	size_t type_size;
	void *data;
	int status;
	if (nc_inq_type(cube->ncid, type, NULL, &type_size) != NC_NOERR)
		return NC_EINVAL;
	data = calloc(count, type_size);
	if (!data) return NC_ENOMEM;
	status = nc_get_var(cube->ncid, input_varid, data);
	if (status == NC_NOERR)
		status = nc_put_var(output_ncid, output_varid, data);
	if (type == NC_STRING && status == NC_NOERR)
		nc_free_string(count, (char **)data);
	free(data);
	return status;
}

static int ssh_nc_write_logical(const struct SSH_NC *cube, size_t field,
                                int ncid, int varid, const double *logical,
                                double scale)
{
	size_t total = cube->n[0] * cube->n[1] * cube->n[2];
	float *raw = calloc(total, sizeof(*raw));
	size_t i, j, k, source = 0;
	int status;
	if (!raw) return GMT_MEMORY_ERROR;
	for (k = 0; k < cube->n[2]; k++)
		for (j = 0; j < cube->n[1]; j++)
			for (i = 0; i < cube->n[0]; i++, source++)
				raw[ssh_nc_field_index(cube, field, i, j, k)] =
				    isfinite(logical[source]) ? (float)(logical[source] * scale) : NAN;
	status = nc_put_var_float(ncid, varid, raw);
	free(raw);
	return status == NC_NOERR ? GMT_NOERROR : GMT_RUNTIME_ERROR;
}

int ssh_nc_write_application(struct GMTAPI_CTRL *API, const char *path,
                             const struct SSH_NC *cube, double **values,
                             const double *weight,
                             const struct GQ_TRANSFORM *output_transform)
{
	int ndims, nvars, ngatts, nunlim = 0, *unlim = NULL;
	int *dimid = NULL, *output_varid = NULL, weight_varid = -1;
	int ncid = -1, varid, k, status = GMT_RUNTIME_ERROR;
	float fill = NAN;
	if (nc_inq(cube->ncid, &ndims, &nvars, &ngatts, NULL) != NC_NOERR)
		return GMT_DATA_READ_ERROR;
	dimid = calloc(ndims, sizeof(*dimid));
	output_varid = malloc(nvars * sizeof(*output_varid));
	if (!dimid || !output_varid) {
		status = GMT_MEMORY_ERROR;
		goto cleanup;
	}
	for (varid = 0; varid < nvars; varid++) output_varid[varid] = -1;
	nc_inq_unlimdims(cube->ncid, &nunlim, NULL);
	if (nunlim) {
		unlim = calloc(nunlim, sizeof(*unlim));
		if (!unlim || nc_inq_unlimdims(cube->ncid, &nunlim, unlim) != NC_NOERR)
			goto cleanup;
	}
	if (nc_create(path, NC_CLOBBER | NC_NETCDF4, &ncid) != NC_NOERR)
		goto cleanup;
	for (k = 0; k < ndims; k++) {
		char name[NC_MAX_NAME + 1];
		size_t length;
		bool unlimited = false;
		int u;
		unsigned int axis;
		if (nc_inq_dim(cube->ncid, k, name, &length) != NC_NOERR) goto cleanup;
		for (axis = 0; axis < cube->dim; axis++)
			if (k == cube->axis_dimid[axis]) length = cube->n[axis];
		for (u = 0; u < nunlim; u++) if (unlim[u] == k) unlimited = true;
		if (nc_def_dim(ncid, name, unlimited ? NC_UNLIMITED : length,
		               &dimid[k]) != NC_NOERR)
			goto cleanup;
	}
	for (varid = 0; varid < nvars; varid++) {
		char name[NC_MAX_NAME + 1];
		nc_type type;
		int vndims, input_dims[NC_MAX_VAR_DIMS], output_dims[NC_MAX_VAR_DIMS];
		int selected, coordinate, position;
		if (nc_inq_var(cube->ncid, varid, name, &type, &vndims,
		               input_dims, NULL) != NC_NOERR)
			goto cleanup;
		for (position = 0; position < vndims; position++)
			output_dims[position] = dimid[input_dims[position]];
		selected = ssh_nc_selected(cube, varid);
		coordinate = ssh_nc_coordinate(cube, varid);
		if (selected < 0 && coordinate < 0 &&
		    ssh_nc_incompatible_ancillary(cube, input_dims, vndims)) {
			GMT_Report(API, GMT_MSG_INFORMATION,
			           "Omitting coordinate-dependent variable %s because the "
			           "model lattice changed\n", name);
			continue;
		}
		if (nc_def_var(ncid, name,
		               selected >= 0 ? NC_FLOAT : (coordinate >= 0 ? NC_DOUBLE : type),
		               vndims, output_dims, &output_varid[varid]) != NC_NOERR)
			goto cleanup;
		if (selected >= 0 &&
		    nc_put_att_float(ncid, output_varid[varid], "_FillValue",
		                     NC_FLOAT, 1, &fill) != NC_NOERR)
			goto cleanup;
		if (ssh_nc_copy_attributes(cube->ncid, varid, ncid,
		                           output_varid[varid],
		                           selected >= 0 || coordinate >= 0) != NC_NOERR)
			goto cleanup;
		if (coordinate >= 0 &&
		    ssh_nc_replace_units(
		        ncid, output_varid[varid],
		        ssh_nc_output_axis_units(cube, output_transform,
		                                 (size_t)coordinate),
		        (cube->transform.axis_set[coordinate] &&
		         cube->transform.axis_scale[coordinate] != 1.0) ||
		        output_transform->axis_scale[coordinate] != 1.0) != NC_NOERR)
			goto cleanup;
		if (coordinate >= 0 && cube->lattice_changed[coordinate])
			nc_del_att(ncid, output_varid[varid], "bounds");
		if (coordinate == 2 &&
		    ssh_nc_update_positive(cube, output_transform, ncid,
		                           output_varid[varid]) != NC_NOERR)
			goto cleanup;
		if (selected >= 0 &&
		    ssh_nc_replace_units(
		        ncid, output_varid[varid],
		        ssh_nc_output_field_units(cube, output_transform,
		                                  (size_t)selected),
		        gq_transform_value_scale(&cube->transform,
		                                 (size_t)selected) != 1.0 ||
		        gq_transform_value_scale(output_transform,
		                                 (size_t)selected) != 1.0) != NC_NOERR)
			goto cleanup;
	}
	if (weight) {
		const struct SSH_NC_FIELD *reference = &cube->field[0];
		int dims[SSH_MAX_DIM], position, existing;
		if (nc_inq_varid(cube->ncid, "weight", &existing) == NC_NOERR) {
			GMT_Report(API, GMT_MSG_ERROR,
			           "Cannot add weight because the input already contains it\n");
			goto cleanup;
		}
		for (position = 0; position < (int)cube->dim; position++)
			dims[position] = dimid[reference->dimids[position]];
		if (nc_def_var(ncid, "weight", NC_FLOAT, cube->dim, dims,
		               &weight_varid) != NC_NOERR ||
		    nc_put_att_float(ncid, weight_varid, "_FillValue",
		                     NC_FLOAT, 1, &fill) != NC_NOERR)
			goto cleanup;
		nc_put_att_text(ncid, weight_varid, "long_name",
		                strlen("heterogeneity taper weight"),
		                "heterogeneity taper weight");
		nc_put_att_text(ncid, weight_varid, "units", 1, "1");
	}
	for (k = 0; k < ngatts; k++) {
		char name[NC_MAX_NAME + 1];
		if (nc_inq_attname(cube->ncid, NC_GLOBAL, k, name) == NC_NOERR &&
		    strcmp(name, "_NCProperties") && strcmp(name, "_SuperblockVersion"))
			nc_copy_att(cube->ncid, NC_GLOBAL, name, ncid, NC_GLOBAL);
	}
	nc_put_att_text(ncid, NC_GLOBAL, "source",
	                strlen("Created by GMT ssh"), "Created by GMT ssh");
	if (nc_enddef(ncid) != NC_NOERR) goto cleanup;
	for (varid = 0; varid < nvars; varid++) {
		nc_type type;
		int vndims, dims[NC_MAX_VAR_DIMS], position;
		size_t count = 1, length;
		if (output_varid[varid] < 0 || ssh_nc_selected(cube, varid) >= 0 ||
		    ssh_nc_coordinate(cube, varid) >= 0) continue;
		if (nc_inq_var(cube->ncid, varid, NULL, &type, &vndims,
		               dims, NULL) != NC_NOERR)
			goto cleanup;
		for (position = 0; position < vndims; position++) {
			if (nc_inq_dimlen(cube->ncid, dims[position], &length) != NC_NOERR)
				goto cleanup;
			count *= length;
		}
		if (ssh_nc_copy_variable(cube, varid, ncid, output_varid[varid],
		                         type, count) != NC_NOERR)
			goto cleanup;
	}
	for (k = 0; k < (int)cube->dim; k++) {
		double *coordinate = calloc(cube->n[k], sizeof(*coordinate));
		size_t node;
		if (!coordinate) {
			status = GMT_MEMORY_ERROR;
			goto cleanup;
		}
		for (node = 0; node < cube->n[k]; node++)
			coordinate[node] = cube->coordinate[k][node] *
			                   output_transform->axis_scale[k];
		if (nc_put_var_double(ncid, output_varid[cube->coordinate_varid[k]],
		                      coordinate) != NC_NOERR) {
			free(coordinate);
			goto cleanup;
		}
		free(coordinate);
	}
	for (k = 0; k < (int)cube->n_fields; k++)
		if (ssh_nc_write_logical(cube, (size_t)k, ncid,
		                         output_varid[cube->field[k].varid],
		                         values[k],
		                         gq_transform_value_scale(output_transform,
		                                                  (size_t)k)) != GMT_NOERROR)
			goto cleanup;
	if (weight &&
	    ssh_nc_write_logical(cube, 0, ncid, weight_varid, weight, 1.0) != GMT_NOERROR)
		goto cleanup;
	if (nc_close(ncid) != NC_NOERR) goto cleanup;
	ncid = -1;
	status = GMT_NOERROR;
cleanup:
	if (ncid >= 0) nc_close(ncid);
	free(dimid);
	free(output_varid);
	free(unlim);
	if (status != GMT_NOERROR)
		GMT_Report(API, GMT_MSG_ERROR,
		           "Unable to create NetCDF output %s\n", path);
	return status;
}

int ssh_nc_write_synthetic(struct GMTAPI_CTRL *API, const char *path,
                           unsigned int dim, const size_t n[SSH_MAX_DIM],
                           const double *coordinate[SSH_MAX_DIM],
                           char **fields, size_t n_fields,
                           double **values, const double *weight,
                           const struct GQ_TRANSFORM *output_transform)
{
	const char *axis_name[SSH_MAX_DIM] = {"x", "y", "z"};
	int ncid = -1, dimid[SSH_MAX_DIM], coordinate_varid[SSH_MAX_DIM];
	int *field_varid = NULL, weight_varid = -1, dimensions[SSH_MAX_DIM];
	unsigned int axis;
	size_t field, total = n[0] * n[1] * n[2];
	float fill = NAN;
	int status = GMT_RUNTIME_ERROR;
	field_varid = calloc(n_fields, sizeof(*field_varid));
	if (!field_varid) return GMT_MEMORY_ERROR;
	if (nc_create(path, NC_CLOBBER | NC_NETCDF4, &ncid) != NC_NOERR)
		goto cleanup;
	for (axis = 0; axis < dim; axis++) {
		const char *axis_letter[SSH_MAX_DIM] = {"X", "Y", "Z"};
		if (nc_def_dim(ncid, axis_name[axis], n[axis], &dimid[axis]) != NC_NOERR ||
		    nc_def_var(ncid, axis_name[axis], NC_DOUBLE, 1, &dimid[axis],
		               &coordinate_varid[axis]) != NC_NOERR)
			goto cleanup;
		nc_put_att_text(ncid, coordinate_varid[axis], "axis", 1,
		                axis_letter[axis]);
		if (output_transform->axis_unit[axis])
			nc_put_att_text(ncid, coordinate_varid[axis], "units",
			                strlen(output_transform->axis_unit[axis]),
			                output_transform->axis_unit[axis]);
	}
	for (axis = 0; axis < dim; axis++)
		dimensions[dim - 1 - axis] = dimid[axis];
	for (field = 0; field < n_fields; field++) {
		if (nc_def_var(ncid, fields[field], NC_FLOAT, dim, dimensions,
		               &field_varid[field]) != NC_NOERR ||
		    nc_put_att_float(ncid, field_varid[field], "_FillValue",
		                     NC_FLOAT, 1, &fill) != NC_NOERR)
			goto cleanup;
		nc_put_att_text(ncid, field_varid[field], "long_name",
		                strlen("fractional small-scale heterogeneity"),
		                "fractional small-scale heterogeneity");
		{
			const char *units = gq_transform_value_unit(output_transform, field);
			if (!units) units = "1";
			nc_put_att_text(ncid, field_varid[field], "units", strlen(units), units);
		}
	}
	if (weight) {
		if (nc_def_var(ncid, "weight", NC_FLOAT, dim, dimensions,
		               &weight_varid) != NC_NOERR ||
		    nc_put_att_float(ncid, weight_varid, "_FillValue",
		                     NC_FLOAT, 1, &fill) != NC_NOERR)
			goto cleanup;
		nc_put_att_text(ncid, weight_varid, "long_name",
		                strlen("heterogeneity taper weight"),
		                "heterogeneity taper weight");
		nc_put_att_text(ncid, weight_varid, "units", 1, "1");
	}
	nc_put_att_text(ncid, NC_GLOBAL, "Conventions", 6, "CF-1.8");
	nc_put_att_text(ncid, NC_GLOBAL, "source",
	                strlen("Created by GMT ssh"), "Created by GMT ssh");
	if (nc_enddef(ncid) != NC_NOERR) goto cleanup;
	for (axis = 0; axis < dim; axis++) {
		double *scaled = calloc(n[axis], sizeof(*scaled));
		size_t k;
		if (!scaled) {
			status = GMT_MEMORY_ERROR;
			goto cleanup;
		}
		for (k = 0; k < n[axis]; k++)
			scaled[k] = coordinate[axis][k] * output_transform->axis_scale[axis];
		status = nc_put_var_double(ncid, coordinate_varid[axis], scaled);
		free(scaled);
		if (status != NC_NOERR) goto cleanup;
	}
	for (field = 0; field < n_fields; field++) {
		float *data = calloc(total, sizeof(*data));
		size_t k;
		if (!data) {
			status = GMT_MEMORY_ERROR;
			goto cleanup;
		}
		for (k = 0; k < total; k++)
			data[k] = (float)(values[field][k] *
			                  gq_transform_value_scale(output_transform, field));
		status = nc_put_var_float(ncid, field_varid[field], data);
		free(data);
		if (status != NC_NOERR) goto cleanup;
	}
	if (weight) {
		float *data = calloc(total, sizeof(*data));
		size_t k;
		if (!data) {
			status = GMT_MEMORY_ERROR;
			goto cleanup;
		}
		for (k = 0; k < total; k++) data[k] = (float)weight[k];
		status = nc_put_var_float(ncid, weight_varid, data);
		free(data);
		if (status != NC_NOERR) goto cleanup;
	}
	if (nc_close(ncid) != NC_NOERR) goto cleanup;
	ncid = -1;
	status = GMT_NOERROR;
cleanup:
	if (ncid >= 0) nc_close(ncid);
	free(field_varid);
	if (status != GMT_NOERROR)
		GMT_Report(API, GMT_MSG_ERROR,
		           "Unable to create NetCDF output %s\n", path);
	return status;
}
