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

#ifndef GQ_TRANSFORM_H
#define GQ_TRANSFORM_H

#include <stdbool.h>
#include <stddef.h>

enum GQ_TRANSFORM_AXIS {
	GQ_TRANSFORM_X = 0,
	GQ_TRANSFORM_Y,
	GQ_TRANSFORM_Z,
	GQ_TRANSFORM_N_AXES
};

#define GQ_TRANSFORM_X_MASK (1U << GQ_TRANSFORM_X)
#define GQ_TRANSFORM_Y_MASK (1U << GQ_TRANSFORM_Y)
#define GQ_TRANSFORM_Z_MASK (1U << GQ_TRANSFORM_Z)

struct GQ_TRANSFORM {
	bool axis_set[GQ_TRANSFORM_N_AXES];
	double axis_scale[GQ_TRANSFORM_N_AXES];
	char *axis_unit[GQ_TRANSFORM_N_AXES];
	bool values_set;
	double *value_scale;
	size_t n_value_scale;
	bool value_units_set;
	char **value_unit;
	size_t n_value_unit;
};

void gq_transform_init(struct GQ_TRANSFORM *transform);
void gq_transform_free(struct GQ_TRANSFORM *transform);
bool gq_transform_active(const struct GQ_TRANSFORM *transform);
int gq_transform_parse(const char *modifiers, unsigned int axis_mask,
                       bool allow_missing, struct GQ_TRANSFORM *transform,
                       bool *has_missing, double *missing,
                       char *error, size_t error_size);
int gq_transform_validate_values(const struct GQ_TRANSFORM *transform,
                                 size_t n_fields, char *error,
                                 size_t error_size);
double gq_transform_value_scale(const struct GQ_TRANSFORM *transform,
                                size_t field);
const char *gq_transform_value_unit(const struct GQ_TRANSFORM *transform,
                                    size_t field);
int gq_transform_format(const struct GQ_TRANSFORM *transform,
                        bool has_missing, double missing,
                        size_t field, size_t n_fields,
                        bool include_values,
                        char *output, size_t output_size);
int gq_transform_parse_number(const char *text, double *value);

#endif
