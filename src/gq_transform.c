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
#include "gq_transform.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char gq_axis_code[GQ_TRANSFORM_N_AXES] = {'x', 'y', 'z'};

void gq_transform_init(struct GQ_TRANSFORM *transform)
{
	size_t axis;
	memset(transform, 0, sizeof(*transform));
	for (axis = 0; axis < GQ_TRANSFORM_N_AXES; axis++)
		transform->axis_scale[axis] = 1.0;
}

void gq_transform_free(struct GQ_TRANSFORM *transform)
{
	size_t axis, field;
	if (transform == NULL) return;
	for (axis = 0; axis < GQ_TRANSFORM_N_AXES; axis++)
		free(transform->axis_unit[axis]);
	for (field = 0; field < transform->n_value_unit; field++)
		free(transform->value_unit[field]);
	free(transform->value_unit);
	free(transform->value_scale);
	gq_transform_init(transform);
}

bool gq_transform_active(const struct GQ_TRANSFORM *transform)
{
	size_t axis;
	if (transform == NULL) return false;
	if (transform->values_set || transform->value_units_set) return true;
	for (axis = 0; axis < GQ_TRANSFORM_N_AXES; axis++)
		if (transform->axis_set[axis] || transform->axis_unit[axis]) return true;
	return false;
}

int gq_transform_parse_number(const char *text, double *value)
{
	char copy[GMT_LEN128], *slash, *end = NULL;
	double numerator, denominator = 1.0;

	if (text == NULL || !text[0] || strlen(text) >= sizeof(copy)) return GMT_PARSE_ERROR;
	strcpy(copy, text);
	slash = strchr(copy, '/');
	if (slash) {
		if (strchr(slash + 1, '/')) return GMT_PARSE_ERROR;
		*slash++ = '\0';
		if (!copy[0] || !slash[0]) return GMT_PARSE_ERROR;
	}
	errno = 0;
	numerator = strtod(copy, &end);
	if (errno || end == copy || *end || !isfinite(numerator)) return GMT_PARSE_ERROR;
	if (slash) {
		errno = 0;
		denominator = strtod(slash, &end);
		if (errno || end == slash || *end || !isfinite(denominator) || denominator == 0.0)
			return GMT_PARSE_ERROR;
	}
	*value = numerator / denominator;
	return isfinite(*value) ? GMT_NOERROR : GMT_PARSE_ERROR;
}

static const char *gq_transform_modifier_end(const char *start)
{
	const char *p;
	for (p = start; *p; p++) {
		if (*p != '+') continue;
		if (p > start && (p[-1] == 'e' || p[-1] == 'E')) continue;
		return p;
	}
	return p;
}

static int gq_transform_scale_list(const char *text, double **values,
                                   size_t *count)
{
	char *copy = NULL, *save = NULL, *token;
	size_t capacity = 0;

	copy = strdup(text);
	if (copy == NULL) return GMT_MEMORY_ERROR;
	for (token = strtok_r(copy, ",", &save); token;
	     token = strtok_r(NULL, ",", &save)) {
		double value, *next;
		if (gq_transform_parse_number(token, &value) || value == 0.0) {
			free(copy);
			return GMT_PARSE_ERROR;
		}
		if (*count == capacity) {
			capacity = capacity ? 2 * capacity : 4;
			next = realloc(*values, capacity * sizeof(**values));
			if (next == NULL) {
				free(copy);
				return GMT_MEMORY_ERROR;
			}
			*values = next;
		}
		(*values)[(*count)++] = value;
	}
	free(copy);
	return *count ? GMT_NOERROR : GMT_PARSE_ERROR;
}

static int gq_transform_unit_list(const char *text, char ***units,
                                  size_t *count)
{
	char *copy = NULL, *save = NULL, *token;
	size_t capacity = 0;

	copy = strdup(text);
	if (copy == NULL) return GMT_MEMORY_ERROR;
	for (token = strtok_r(copy, ",", &save); token;
	     token = strtok_r(NULL, ",", &save)) {
		char **next;
		if (!token[0]) {
			free(copy);
			return GMT_PARSE_ERROR;
		}
		if (*count == capacity) {
			capacity = capacity ? 2 * capacity : 4;
			next = realloc(*units, capacity * sizeof(**units));
			if (next == NULL) {
				free(copy);
				return GMT_MEMORY_ERROR;
			}
			*units = next;
		}
		(*units)[*count] = strdup(token);
		if ((*units)[*count] == NULL) {
			free(copy);
			return GMT_MEMORY_ERROR;
		}
		(*count)++;
	}
	free(copy);
	return *count ? GMT_NOERROR : GMT_PARSE_ERROR;
}

int gq_transform_parse(const char *modifiers, unsigned int axis_mask,
                       bool allow_missing, struct GQ_TRANSFORM *transform,
                       bool *has_missing, double *missing,
                       char *error, size_t error_size)
{
	const char *p = modifiers;
	int status = GMT_NOERROR;

	if (error && error_size) error[0] = '\0';
	while (p && *p) {
		const char *end;
		char code, *payload;
		size_t length, axis;
		if (*p == '+') p++;
		if (!*p) goto bad;
		code = *p++;
		end = gq_transform_modifier_end(p);
		length = (size_t)(end - p);
		if (length == 0) goto bad;
		payload = calloc(length + 1, 1);
		if (payload == NULL) return GMT_MEMORY_ERROR;
		memcpy(payload, p, length);

		if (code == 'n' && allow_missing) {
			if (!has_missing || !missing || *has_missing ||
			    gq_transform_parse_number(payload, missing)) status = GMT_PARSE_ERROR;
			else *has_missing = true;
		}
		else if (code == 'v') {
			if (transform->values_set) status = GMT_PARSE_ERROR;
			else {
				status = gq_transform_scale_list(payload, &transform->value_scale,
				                                 &transform->n_value_scale);
				if (status == GMT_NOERROR) transform->values_set = true;
			}
		}
		else if (code == 'V') {
			if (transform->value_units_set) status = GMT_PARSE_ERROR;
			else {
				status = gq_transform_unit_list(payload, &transform->value_unit,
				                                &transform->n_value_unit);
				if (status == GMT_NOERROR) transform->value_units_set = true;
			}
		}
		else {
			bool units = code >= 'X' && code <= 'Z';
			char lower = units ? (char)(code - 'A' + 'a') : code;
			for (axis = 0; axis < GQ_TRANSFORM_N_AXES; axis++)
				if (lower == gq_axis_code[axis]) break;
			if (axis == GQ_TRANSFORM_N_AXES || !(axis_mask & (1U << axis)))
				status = GMT_PARSE_ERROR;
			else if (units) {
				if (transform->axis_unit[axis]) status = GMT_PARSE_ERROR;
				else {
					transform->axis_unit[axis] = strdup(payload);
					if (transform->axis_unit[axis] == NULL) status = GMT_MEMORY_ERROR;
				}
			}
			else if (transform->axis_set[axis] ||
			         gq_transform_parse_number(payload, &transform->axis_scale[axis]) ||
			         transform->axis_scale[axis] == 0.0)
				status = GMT_PARSE_ERROR;
			else transform->axis_set[axis] = true;
		}
		free(payload);
		if (status != GMT_NOERROR) goto bad_status;
		p = *end ? end + 1 : NULL;
	}
	return GMT_NOERROR;

bad:
	status = GMT_PARSE_ERROR;
bad_status:
	if (error && error_size)
		snprintf(error, error_size,
		         "invalid or repeated transform modifier near +%c", p && p > modifiers ? p[-1] : '?');
	return status;
}

int gq_transform_validate_values(const struct GQ_TRANSFORM *transform,
                                 size_t n_fields, char *error,
                                 size_t error_size)
{
	if (transform->values_set && transform->n_value_scale != 1 &&
	    transform->n_value_scale != n_fields) {
		if (error && error_size)
			snprintf(error, error_size,
			         "+v lists %zu scales but %zu fields are selected",
			         transform->n_value_scale, n_fields);
		return GMT_PARSE_ERROR;
	}
	if (transform->value_units_set && transform->n_value_unit != 1 &&
	    transform->n_value_unit != n_fields) {
		if (error && error_size)
			snprintf(error, error_size,
			         "+V lists %zu units but %zu fields are selected",
			         transform->n_value_unit, n_fields);
		return GMT_PARSE_ERROR;
	}
	return GMT_NOERROR;
}

double gq_transform_value_scale(const struct GQ_TRANSFORM *transform,
                                size_t field)
{
	if (!transform->values_set) return 1.0;
	return transform->value_scale[transform->n_value_scale == 1 ? 0 : field];
}

const char *gq_transform_value_unit(const struct GQ_TRANSFORM *transform,
	                                size_t field)
{
	if (!transform->value_units_set) return NULL;
	return transform->value_unit[transform->n_value_unit == 1 ? 0 : field];
}

static int gq_transform_append(char *output, size_t output_size,
                               const char *format, char code,
                               double value, const char *text)
{
	size_t used = strlen(output);
	int written = text
	            ? snprintf(output + used, output_size - used, format, code, text)
	            : snprintf(output + used, output_size - used, format, code, value);
	return written < 0 || (size_t)written >= output_size - used
	       ? GMT_DIM_TOO_SMALL : GMT_NOERROR;
}

int gq_transform_format(const struct GQ_TRANSFORM *transform,
                        bool has_missing, double missing,
                        size_t field, size_t n_fields,
                        bool include_values,
                        char *output, size_t output_size)
{
	size_t axis, k, first, last;
	char buffer[GMT_BUFSIZ] = {""};

	if (!output || output_size == 0) return GMT_ARG_IS_NULL;
	output[0] = '\0';
	if (has_missing && gq_transform_append(output, output_size, "+%c%.17g",
	                                       'n', missing, NULL))
		return GMT_DIM_TOO_SMALL;
	for (axis = 0; axis < GQ_TRANSFORM_N_AXES; axis++) {
		if (transform->axis_set[axis] &&
		    gq_transform_append(output, output_size, "+%c%.17g",
		                            gq_axis_code[axis], transform->axis_scale[axis], NULL))
			return GMT_DIM_TOO_SMALL;
		if (transform->axis_unit[axis] &&
		    gq_transform_append(output, output_size, "+%c%s",
		                            (char)(gq_axis_code[axis] - 'a' + 'A'), 0.0,
		                            transform->axis_unit[axis]))
			return GMT_DIM_TOO_SMALL;
	}
	if (include_values &&
	    gq_transform_validate_values(transform, n_fields, buffer, sizeof(buffer)))
		return GMT_PARSE_ERROR;
	first = field == SIZE_MAX ? 0 : field;
	last = field == SIZE_MAX ? n_fields : field + 1;
	if (include_values && transform->values_set) {
		if (gq_transform_append(output, output_size, "+%c%s", 'v', 0.0, ""))
			return GMT_DIM_TOO_SMALL;
		for (k = first; k < last; k++) {
			size_t used = strlen(output);
			int written = snprintf(output + used, output_size - used, "%s%.17g",
			                       k == first ? "" : ",",
			                       gq_transform_value_scale(transform, k));
			if (written < 0 || (size_t)written >= output_size - used)
				return GMT_DIM_TOO_SMALL;
		}
	}
	if (include_values && transform->value_units_set) {
		if (gq_transform_append(output, output_size, "+%c%s", 'V', 0.0, ""))
			return GMT_DIM_TOO_SMALL;
		for (k = first; k < last; k++) {
			const char *unit = gq_transform_value_unit(transform, k);
			size_t used = strlen(output);
			int written = snprintf(output + used, output_size - used, "%s%s",
			                       k == first ? "" : ",", unit);
			if (written < 0 || (size_t)written >= output_size - used)
				return GMT_DIM_TOO_SMALL;
		}
	}
	return GMT_NOERROR;
}
