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

#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <limits.h>

struct SSH_RNG {
	uint64_t state;
	bool spare;
	double gaussian;
};

static char *ssh_trim(char *text)
{
	char *end;
	while (isspace((unsigned char)*text)) text++;
	if (!*text) return text;
	end = text + strlen(text) - 1;
	while (end >= text && isspace((unsigned char)*end)) *end-- = '\0';
	return text;
}

int ssh_parse_number(const char *text, double *value)
{
	char copy[GMT_LEN128], *slash, *end = NULL;
	double numerator, denominator = 1.0;

	if (!text || !text[0] || strlen(text) >= sizeof(copy))
		return GMT_PARSE_ERROR;
	strcpy(copy, text);
	slash = strchr(copy, '/');
	if (slash) {
		if (strchr(slash + 1, '/')) return GMT_PARSE_ERROR;
		*slash++ = '\0';
	}
	errno = 0;
	numerator = strtod(copy, &end);
	if (errno || end == copy || *end || !isfinite(numerator))
		return GMT_PARSE_ERROR;
	if (slash) {
		errno = 0;
		denominator = strtod(slash, &end);
		if (errno || end == slash || *end || !isfinite(denominator) ||
		    denominator == 0.0)
			return GMT_PARSE_ERROR;
	}
	*value = numerator / denominator;
	return isfinite(*value) ? GMT_NOERROR : GMT_PARSE_ERROR;
}

int ssh_parse_field_list(struct GMTAPI_CTRL *API, const char *text,
                         char ***fields, size_t *count)
{
	char *copy = NULL, *token = NULL, *save = NULL;
	size_t n = 0;
	char **list = NULL;

	if (!text || !text[0]) goto bad;
	copy = strdup(text);
	if (!copy) return GMT_MEMORY_ERROR;
	for (token = strtok_r(copy, ",", &save); token;
	     token = strtok_r(NULL, ",", &save)) {
		char **next;
		size_t k;
		token = ssh_trim(token);
		if (!token[0]) goto bad;
		for (k = 0; k < n; k++)
			if (!strcmp(list[k], token)) goto bad;
		next = realloc(list, (n + 1) * sizeof(*next));
		if (!next) {
			ssh_free_field_list(list, n);
			free(copy);
			return GMT_MEMORY_ERROR;
		}
		list = next;
		list[n] = strdup(token);
		if (!list[n]) {
			ssh_free_field_list(list, n);
			free(copy);
			return GMT_MEMORY_ERROR;
		}
		n++;
	}
	free(copy);
	*fields = list;
	*count = n;
	return GMT_NOERROR;
bad:
	GMT_Report(API, GMT_MSG_ERROR,
	           "Field list must contain unique comma-separated names: %s\n",
	           text ? text : "");
	ssh_free_field_list(list, n);
	free(copy);
	return GMT_PARSE_ERROR;
}

void ssh_free_field_list(char **fields, size_t count)
{
	size_t k;
	if (!fields) return;
	for (k = 0; k < count; k++) free(fields[k]);
	free(fields);
}

static int ssh_parse_values(const char *text, double value[SSH_MAX_DIM],
                            unsigned int dim)
{
	char copy[GMT_LEN256], *token = NULL, *save = NULL;
	unsigned int n = 0, k;

	if (!text || !text[0] || strlen(text) >= sizeof(copy))
		return GMT_PARSE_ERROR;
	strcpy(copy, text);
	for (token = strtok_r(copy, "/", &save); token && n < dim;
	     token = strtok_r(NULL, "/", &save)) {
		if (ssh_parse_number(token, &value[n])) return GMT_PARSE_ERROR;
		n++;
	}
	if (token || (n != 1 && n != dim)) return GMT_PARSE_ERROR;
	if (n == 1)
		for (k = 1; k < dim; k++) value[k] = value[0];
	return GMT_NOERROR;
}

static struct SSH_OVERRIDE *ssh_override(struct SSH_STAT_CONFIG *config,
                                         const char *field)
{
	size_t k;
	struct SSH_OVERRIDE *next;
	for (k = 0; k < config->n_overrides; k++)
		if (!strcmp(config->override[k].field, field))
			return &config->override[k];
	next = realloc(config->override,
	               (config->n_overrides + 1) * sizeof(*next));
	if (!next) return NULL;
	config->override = next;
	memset(&config->override[config->n_overrides], 0,
	       sizeof(config->override[config->n_overrides]));
	config->override[config->n_overrides].field = strdup(field);
	if (!config->override[config->n_overrides].field) return NULL;
	return &config->override[config->n_overrides++];
}

int ssh_parse_stat_option(struct GMTAPI_CTRL *API, const char *text,
                          enum SSH_STAT_KIND kind, unsigned int dim,
                          struct SSH_STAT_CONFIG *config)
{
	char copy[GMT_LEN256], *slash;
	const char *values = text;
	char *field = NULL;
	struct SSH_STAT *target = &config->global;
	double parsed[SSH_MAX_DIM] = {0.0, 0.0, 0.0};

	if (!text || !text[0] || strlen(text) >= sizeof(copy)) goto bad;
	strcpy(copy, text);
	slash = strchr(copy, '/');
	if (slash) {
		double first;
		*slash = '\0';
		if (ssh_parse_number(copy, &first)) {
			struct SSH_OVERRIDE *item;
			field = copy;
			values = slash + 1;
			if (!field[0] || !values[0]) goto bad;
			item = ssh_override(config, field);
			if (!item) return GMT_MEMORY_ERROR;
			target = &item->value;
		}
	}
	if (kind == SSH_STAT_CORRELATION) {
		if (ssh_parse_values(values, parsed, dim)) goto bad;
		for (unsigned int k = 0; k < dim; k++)
			if (parsed[k] <= 0.0) goto bad;
		memcpy(target->correlation, parsed, dim * sizeof(double));
		target->have_correlation = true;
	}
	else {
		if (ssh_parse_number(values, &parsed[0])) goto bad;
		if ((kind == SSH_STAT_SIGMA && parsed[0] <= 0.0) ||
		    (kind == SSH_STAT_HURST &&
		     (parsed[0] <= 0.0 || parsed[0] >= 1.0)))
			goto bad;
		if (kind == SSH_STAT_SIGMA) {
			target->sigma = parsed[0];
			target->have_sigma = true;
		}
		else {
			target->hurst = parsed[0];
			target->have_hurst = true;
		}
	}
	return GMT_NOERROR;
bad:
	GMT_Report(API, GMT_MSG_ERROR,
	           "Invalid -%c value %s; use <value> or <field>/<value>\n",
	           kind == SSH_STAT_SIGMA ? 'D' :
	           (kind == SSH_STAT_CORRELATION ? 'C' : 'U'),
	           text ? text : "");
	return GMT_PARSE_ERROR;
}

int ssh_resolve_stat(struct GMTAPI_CTRL *API,
                     const struct SSH_STAT_CONFIG *config,
                     const char *field, unsigned int dim,
                     enum SSH_MODEL model, struct SSH_STAT *result)
{
	size_t k;
	*result = config->global;
	for (k = 0; k < config->n_overrides; k++) {
		const struct SSH_STAT *value;
		if (strcmp(config->override[k].field, field)) continue;
		value = &config->override[k].value;
		if (value->have_sigma) {
			result->sigma = value->sigma;
			result->have_sigma = true;
		}
		if (value->have_correlation) {
			memcpy(result->correlation, value->correlation,
			       dim * sizeof(double));
			result->have_correlation = true;
		}
		if (value->have_hurst) {
			result->hurst = value->hurst;
			result->have_hurst = true;
		}
		break;
	}
	if (!result->have_sigma) {
		GMT_Report(API, GMT_MSG_ERROR,
		           "No fractional standard deviation was supplied for %s\n",
		           field);
		return GMT_PARSE_ERROR;
	}
	if (model != SSH_MODEL_WHITE && !result->have_correlation) {
		GMT_Report(API, GMT_MSG_ERROR,
		           "No correlation length was supplied for %s\n", field);
		return GMT_PARSE_ERROR;
	}
	if (model == SSH_MODEL_VON_KARMAN && !result->have_hurst)
		result->hurst = 0.15, result->have_hurst = true;
	return GMT_NOERROR;
}

void ssh_free_stat_config(struct SSH_STAT_CONFIG *config)
{
	size_t k;
	for (k = 0; k < config->n_overrides; k++)
		free(config->override[k].field);
	free(config->override);
	memset(config, 0, sizeof(*config));
}

int ssh_parse_model(struct GMTAPI_CTRL *API, const char *text,
                    enum SSH_MODEL *model)
{
	if (!text || !text[0] || text[1]) goto bad;
	switch (text[0]) {
		case 'v': *model = SSH_MODEL_VON_KARMAN; break;
		case 'g': *model = SSH_MODEL_GAUSSIAN; break;
		case 'e': *model = SSH_MODEL_EXPONENTIAL; break;
		case 'w': *model = SSH_MODEL_WHITE; break;
		default: goto bad;
	}
	return GMT_NOERROR;
bad:
	GMT_Report(API, GMT_MSG_ERROR,
	           "Option -M must be v, g, e, or w\n");
	return GMT_PARSE_ERROR;
}

const char *ssh_model_name(enum SSH_MODEL model)
{
	switch (model) {
		case SSH_MODEL_VON_KARMAN: return "von Karman";
		case SSH_MODEL_GAUSSIAN: return "Gaussian";
		case SSH_MODEL_EXPONENTIAL: return "exponential";
		case SSH_MODEL_WHITE: return "white";
	}
	return "unknown";
}

int ssh_parse_random(struct GMTAPI_CTRL *API, const char *text,
                     struct SSH_RANDOM *random)
{
	char copy[GMT_LEN256], *modifier, *end = NULL;
	unsigned long long seed;

	if (!text || !text[0] || strlen(text) >= sizeof(copy)) goto bad;
	strcpy(copy, text);
	modifier = strchr(copy, '+');
	if (modifier) *modifier++ = '\0';
	errno = 0;
	seed = strtoull(copy, &end, 10);
	if (errno || end == copy || *end) goto bad;
	random->seed = (uint64_t)seed;
	while (modifier && *modifier) {
		char code = *modifier++;
		char *next = strchr(modifier, '+');
		if (next) *next = '\0';
		if (code == 'i' && !modifier[0])
			random->independent = true;
		else if (code == 'n' && !modifier[0])
			random->padding = 0.0;
		else if (code == 'p') {
			if (ssh_parse_number(modifier, &random->padding) ||
			    random->padding < 0.0)
				goto bad;
		}
		else
			goto bad;
		modifier = next ? next + 1 : NULL;
	}
	return GMT_NOERROR;
bad:
	GMT_Report(API, GMT_MSG_ERROR,
	           "Option -Q must be <seed>[+i][+n|+p<factor>]\n");
	return GMT_PARSE_ERROR;
}

uint64_t ssh_field_seed(uint64_t seed, const char *field, bool independent)
{
	uint64_t hash = 1469598103934665603ULL;
	const unsigned char *p;
	if (!independent) return seed;
	for (p = (const unsigned char *)field; *p; p++) {
		hash ^= (uint64_t)*p;
		hash *= 1099511628211ULL;
	}
	return seed ^ (hash + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
}

void ssh_taper_defaults(struct SSH_TAPER *taper)
{
	unsigned int k;
	memset(taper, 0, sizeof(*taper));
	for (k = 0; k < SSH_MAX_DIM; k++) taper->function[k] = WFUNC_COSINE;
	for (k = 0; k < 2 * SSH_MAX_DIM; k++) taper->ratio[k] = 0.2;
}

void ssh_taper_free(struct SSH_TAPER *taper)
{
	free(taper->polygon);
	memset(taper, 0, sizeof(*taper));
}

static int ssh_parse_functions(const char *text, unsigned int dim,
                               blend_window_function function[SSH_MAX_DIM])
{
	char copy[GMT_LEN256], *token = NULL, *save = NULL;
	unsigned int n = 0, k;
	if (!text || !text[0] || strlen(text) >= sizeof(copy))
		return GMT_PARSE_ERROR;
	strcpy(copy, text);
	for (token = strtok_r(copy, "/", &save); token && n < dim;
	     token = strtok_r(NULL, "/", &save)) {
		if (blend_window_function_from_name(token, &function[n]) != SUCCESS)
			return GMT_PARSE_ERROR;
		n++;
	}
	if (token || (n != 1 && n != dim)) return GMT_PARSE_ERROR;
	if (n == 1)
		for (k = 1; k < dim; k++) function[k] = function[0];
	return GMT_NOERROR;
}

static int ssh_parse_ratios(const char *text, unsigned int dim,
                            double ratio[2 * SSH_MAX_DIM])
{
	char copy[GMT_LEN256], *token = NULL, *save = NULL;
	double value[2 * SSH_MAX_DIM];
	unsigned int n = 0, k;
	if (!text || !text[0] || strlen(text) >= sizeof(copy))
		return GMT_PARSE_ERROR;
	strcpy(copy, text);
	for (token = strtok_r(copy, "/", &save); token && n < 2 * dim;
	     token = strtok_r(NULL, "/", &save)) {
		if (ssh_parse_number(token, &value[n])) return GMT_PARSE_ERROR;
		n++;
	}
	if (token || (n != 1 && n != dim && n != 2 * dim))
		return GMT_PARSE_ERROR;
	if (n == 1)
		for (k = 0; k < 2 * dim; k++) ratio[k] = value[0];
	else if (n == dim)
		for (k = 0; k < dim; k++) ratio[2 * k] = ratio[2 * k + 1] = value[k];
	else
		for (k = 0; k < 2 * dim; k++) ratio[k] = value[k];
	for (k = 0; k < 2 * dim; k++)
		if (ratio[k] < 0.0 || ratio[k] >= 0.5)
			return GMT_PARSE_ERROR;
	return GMT_NOERROR;
}

int ssh_parse_taper(struct GMTAPI_CTRL *API, const char *text,
                    unsigned int dim, struct SSH_TAPER *taper)
{
	char copy[GMT_LEN512], *modifier, *function;
	if (!text || strlen(text) >= sizeof(copy)) goto bad;
	strcpy(copy, text);
	function = copy;
	modifier = strchr(copy, '+');
	if (modifier) *modifier++ = '\0';
	if (function[0] && ssh_parse_functions(function, dim, taper->function))
		goto bad;
	while (modifier && *modifier) {
		char code = *modifier++;
		char *next = strchr(modifier, '+');
		if (next) *next = '\0';
		if (code == 'r') {
			if (ssh_parse_ratios(modifier, dim, taper->ratio)) goto bad;
		}
		else if (code == 'w' && !modifier[0])
			taper->write_weight = true;
		else
			goto bad;
		modifier = next ? next + 1 : NULL;
	}
	taper->active = true;
	return GMT_NOERROR;
bad:
	GMT_Report(API, GMT_MSG_ERROR,
	           "Option -W must be [xwindow[/ywindow[/zwindow]]]"
	           "[+r<ratios>][+w]\n");
	return GMT_PARSE_ERROR;
}

int ssh_parse_interval(struct GMTAPI_CTRL *API, const char *text,
                       double interval[2])
{
	char copy[GMT_LEN128], *slash;
	if (!text || strlen(text) >= sizeof(copy)) goto bad;
	strcpy(copy, text);
	slash = strchr(copy, '/');
	if (!slash || strchr(slash + 1, '/')) goto bad;
	*slash++ = '\0';
	if (ssh_parse_number(copy, &interval[0]) ||
	    ssh_parse_number(slash, &interval[1]) ||
	    interval[0] >= interval[1])
		goto bad;
	return GMT_NOERROR;
bad:
	GMT_Report(API, GMT_MSG_ERROR,
	           "Support interval must be <lo>/<hi>\n");
	return GMT_PARSE_ERROR;
}

int ssh_parse_monotone(struct GMTAPI_CTRL *API, const char *text,
                       struct SSH_TAPER *taper)
{
	if (!text || (text[0] != 'E' && text[0] != 'B')) goto bad;
	taper->monotone = text[0];
	if (!text[1]) return GMT_NOERROR;
	if (!strcmp(text + 1, "+w")) {
		taper->write_polygon = true;
		return GMT_NOERROR;
	}
bad:
	GMT_Report(API, GMT_MSG_ERROR, "Option -E must be E or B[+w]\n");
	return GMT_PARSE_ERROR;
}

static int ssh_support_indices(double origin, double increment,
                               double lo, double hi, int *i0, int *i1)
{
	*i0 = (int)floor((lo - origin) / increment + GMT_CONV8_LIMIT);
	*i1 = (int)ceil((hi - origin) / increment - GMT_CONV8_LIMIT);
	return *i1 > *i0 ? GMT_NOERROR : GMT_DIM_TOO_SMALL;
}

static int ssh_polygon_to_real(const polygon *local,
                               const double origin[SSH_MAX_DIM],
                               const double increment[SSH_MAX_DIM],
                               int i0, int j0, polygon *real)
{
	size_t k;
	if (blend_polygon_alloc(real, local->n_vertices) != SUCCESS)
		return GMT_MEMORY_ERROR;
	for (k = 0; k < local->n_vertices; k++)
		if (blend_polygon_set_vertex(
		        real, k,
		        origin[0] + (double)(i0 + (int)lrint(local->vertices[k].x)) *
		                    increment[0],
		        origin[1] + (double)(j0 + (int)lrint(local->vertices[k].y)) *
		                    increment[1]) != SUCCESS) {
			blend_polygon_free(real);
			return GMT_RUNTIME_ERROR;
		}
	return GMT_NOERROR;
}

static int ssh_monotone_name(const char *path, char output[PATH_MAX])
{
	const char *slash = strrchr(path, '/');
	const char *dot = strrchr(path, '.');
	size_t stem;
	if (dot && slash && dot < slash) dot = NULL;
	stem = dot ? (size_t)(dot - path) : strlen(path);
	if (stem + strlen("_monotone") + (dot ? strlen(dot) : 0) + 1 > PATH_MAX)
		return GMT_RUNTIME_ERROR;
	memcpy(output, path, stem);
	output[stem] = '\0';
	strcat(output, "_monotone");
	if (dot) strcat(output, dot);
	return GMT_NOERROR;
}

int ssh_prepare_support(struct GMT_CTRL *GMT, unsigned int dim,
                        const size_t n[SSH_MAX_DIM],
                        const double origin[SSH_MAX_DIM],
                        const double increment[SSH_MAX_DIM],
                        const struct SSH_TAPER *taper,
                        struct SSH_SUPPORT *support)
{
	polygon input = {0}, local = {0}, monotone = {0}, converted_real = {0};
	permuted_vertex boundary = {0};
	char *polygon_path = NULL;
	double xmin, xmax, ymin, ymax, zlo, zhi;
	int is_strict = 0, converted = 0, status = GMT_RUNTIME_ERROR;
	size_t k;

	memset(support, 0, sizeof(*support));
	for (k = 0; k < SSH_MAX_DIM; k++) {
		support->lo[k] = 0;
		support->hi[k] = (int)n[k] - 1;
	}
	if (dim == 1) {
		if (taper->have_interval) {
			double domain_hi = origin[0] + (double)(n[0] - 1) * increment[0];
			if (taper->interval[0] < origin[0] ||
			    taper->interval[1] > domain_hi ||
			    ssh_support_indices(origin[0], increment[0],
			                        taper->interval[0], taper->interval[1],
			                        &support->lo[0], &support->hi[0])) {
				GMT_Report(GMT->parent, GMT_MSG_ERROR,
				           "The taper support interval must lie inside the domain\n");
				return GMT_PARSE_ERROR;
			}
		}
		support->blend.nx = support->hi[0] - support->lo[0] + 1;
	}
	else {
		if (taper->polygon) {
			status = gq_resolve_remote_path(GMT->parent, GMT_IS_DATASET,
			                                taper->polygon, &polygon_path);
			if (status != GMT_NOERROR ||
			    blend_polygon_read(polygon_path, &input) != SUCCESS ||
			    blend_polygon_validate(&input) != SUCCESS ||
			    blend_polygon_bounds(&input, &xmin, &xmax, &ymin, &ymax)
			    != SUCCESS) {
				GMT_Report(GMT->parent, GMT_MSG_ERROR,
				           "Unable to read polygon %s\n", taper->polygon);
				goto cleanup;
			}
		}
		else {
			xmin = origin[0];
			xmax = origin[0] + (double)(n[0] - 1) * increment[0];
			ymin = origin[1];
			ymax = origin[1] + (double)(n[1] - 1) * increment[1];
			if (blend_polygon_alloc(&input, 4) != SUCCESS ||
			    blend_polygon_set_vertex(&input, 0, xmin, ymin) != SUCCESS ||
			    blend_polygon_set_vertex(&input, 1, xmax, ymin) != SUCCESS ||
			    blend_polygon_set_vertex(&input, 2, xmax, ymax) != SUCCESS ||
			    blend_polygon_set_vertex(&input, 3, xmin, ymax) != SUCCESS)
				goto cleanup;
		}
		if (xmin < origin[0] ||
		    xmax > origin[0] + (double)(n[0] - 1) * increment[0] ||
		    ymin < origin[1] ||
		    ymax > origin[1] + (double)(n[1] - 1) * increment[1]) {
			GMT_Report(GMT->parent, GMT_MSG_ERROR,
			           "The polygon support must lie inside the output domain\n");
			status = GMT_PARSE_ERROR;
			goto cleanup;
		}
		if (ssh_support_indices(origin[0], increment[0], xmin, xmax,
		                        &support->lo[0], &support->hi[0]) ||
		    ssh_support_indices(origin[1], increment[1], ymin, ymax,
		                        &support->lo[1], &support->hi[1]))
			goto cleanup;
		support->lo[0] = MAX(0, support->lo[0]);
		support->lo[1] = MAX(0, support->lo[1]);
		support->hi[0] = MIN((int)n[0] - 1, support->hi[0]);
		support->hi[1] = MIN((int)n[1] - 1, support->hi[1]);
		support->blend.nx = support->hi[0] - support->lo[0] + 1;
		support->blend.ny = support->hi[1] - support->lo[1] + 1;
		xmin = origin[0] + (double)support->lo[0] * increment[0];
		xmax = origin[0] + (double)support->hi[0] * increment[0];
		ymin = origin[1] + (double)support->lo[1] * increment[1];
		ymax = origin[1] + (double)support->hi[1] * increment[1];
		if (blend_polygon_map_to_grid(&input, xmin, xmax, ymin, ymax,
		                              support->blend.nx, support->blend.ny,
		                              &local) != SUCCESS)
			goto cleanup;
		for (k = 0; k < local.n_vertices; k++) {
			local.vertices[k].x = floor(local.vertices[k].x + 0.5);
			local.vertices[k].y = floor(local.vertices[k].y + 0.5);
		}
		{
			int local_xmin = INT_MAX, local_xmax = INT_MIN;
			int local_ymin = INT_MAX, local_ymax = INT_MIN;
			for (k = 0; k < local.n_vertices; k++) {
				int x = (int)local.vertices[k].x;
				int y = (int)local.vertices[k].y;
				local_xmin = MIN(local_xmin, x);
				local_xmax = MAX(local_xmax, x);
				local_ymin = MIN(local_ymin, y);
				local_ymax = MAX(local_ymax, y);
			}
			if (local_xmax <= local_xmin || local_ymax <= local_ymin)
				goto cleanup;
			for (k = 0; k < local.n_vertices; k++) {
				local.vertices[k].x -= local_xmin;
				local.vertices[k].y -= local_ymin;
			}
			support->lo[0] += local_xmin;
			support->lo[1] += local_ymin;
			support->hi[0] = support->lo[0] + local_xmax - local_xmin;
			support->hi[1] = support->lo[1] + local_ymax - local_ymin;
			support->blend.nx = local_xmax - local_xmin + 1;
			support->blend.ny = local_ymax - local_ymin + 1;
		}
		if (blend_polygon_validate(&local) != SUCCESS ||
		    blend_polygon_is_xy_monotone_strict(&local, &is_strict) != SUCCESS)
			goto cleanup;
		if (is_strict) {
			if (blend_polygon_copy(&local, &monotone) != SUCCESS) goto cleanup;
		}
		else {
			if (!taper->monotone) {
				GMT_Report(GMT->parent, GMT_MSG_ERROR,
				           "Polygon is not strictly xy-monotone; use -ME or -MB\n");
				status = GMT_PARSE_ERROR;
				goto cleanup;
			}
			if ((taper->monotone == 'E' &&
			     blend_polygon_xy_monotone_envelope_strict(
			         &local, &monotone) != SUCCESS) ||
			    (taper->monotone == 'B' &&
			     blend_polygon_xy_monotone_best_piecewise_envelope_strict(
			         &local, &monotone, 0.0,
			         (double)(support->blend.nx - 1), 0.0,
			         (double)(support->blend.ny - 1),
			         support->blend.nx, support->blend.ny) != SUCCESS))
				goto cleanup;
			converted = 1;
		}
		if (ssh_polygon_to_real(&monotone, origin, increment,
		                        support->lo[0], support->lo[1],
		                        &support->real_polygon) ||
		    blend_window_set_polygon(&support->blend, &monotone) != SUCCESS ||
		    boundary_assembly(&support->blend, &boundary) != SUCCESS)
			goto cleanup;
		if (converted && taper->write_polygon && taper->polygon) {
			char output[PATH_MAX];
			if (ssh_monotone_name(taper->polygon, output) ||
			    ssh_polygon_to_real(&monotone, origin, increment,
			                        support->lo[0], support->lo[1],
			                        &converted_real) ||
			    blend_polygon_write(output, &converted_real) != SUCCESS)
				goto cleanup;
			GMT_Report(GMT->parent, GMT_MSG_INFORMATION,
			           "Wrote converted polygon %s\n", output);
		}
		if (dim == 3) {
			zlo = taper->have_interval ? taper->interval[0] : origin[2];
			zhi = taper->have_interval ? taper->interval[1]
			     : origin[2] + (double)(n[2] - 1) * increment[2];
			if (zlo < origin[2] ||
			    zhi > origin[2] + (double)(n[2] - 1) * increment[2] ||
			    ssh_support_indices(origin[2], increment[2], zlo, zhi,
			                        &support->lo[2], &support->hi[2])) {
				GMT_Report(GMT->parent, GMT_MSG_ERROR,
				           "The vertical taper support must lie inside the domain\n");
				status = GMT_PARSE_ERROR;
				goto cleanup;
			}
			support->lo[2] = MAX(0, support->lo[2]);
			support->hi[2] = MIN((int)n[2] - 1, support->hi[2]);
			support->blend.nz = support->hi[2] - support->lo[2] + 1;
		}
	}
	support->blend.ratio_x1 = taper->ratio[0];
	support->blend.ratio_x2 = taper->ratio[1];
	support->blend.ratio_y1 = taper->ratio[2];
	support->blend.ratio_y2 = taper->ratio[3];
	support->blend.ratio_z1 = taper->ratio[4];
	support->blend.ratio_z2 = taper->ratio[5];
	support->blend.x_function = taper->function[0];
	support->blend.y_function = taper->function[1];
	support->blend.z_function = taper->function[2];
	support->ready = true;
	status = GMT_NOERROR;
cleanup:
	free(polygon_path);
	blend_permuted_vertex_free(&boundary);
	blend_polygon_free(&converted_real);
	blend_polygon_free(&monotone);
	blend_polygon_free(&local);
	blend_polygon_free(&input);
	if (status != GMT_NOERROR) ssh_support_free(support);
	return status;
}

int ssh_support_weight(struct SSH_SUPPORT *support, unsigned int dim,
                       size_t i, size_t j, size_t k, double *weight)
{
	if (!support->ready ||
	    (int)i < support->lo[0] || (int)i > support->hi[0] ||
	    (dim > 1 && ((int)j < support->lo[1] || (int)j > support->hi[1])) ||
	    (dim > 2 && ((int)k < support->lo[2] || (int)k > support->hi[2]))) {
		*weight = 0.0;
		return GMT_NOERROR;
	}
	if ((dim == 1 &&
	     embedding_contribution1d((int)i - support->lo[0],
	                              &support->blend) != SUCCESS) ||
	    (dim == 2 &&
	     embedding_contribution2d((int)i - support->lo[0],
	                              (int)j - support->lo[1],
	                              &support->blend) != SUCCESS) ||
	    (dim == 3 &&
	     embedding_contribution3d((int)i - support->lo[0],
	                              (int)j - support->lo[1],
	                              (int)k - support->lo[2],
	                              &support->blend) != SUCCESS))
		return GMT_RUNTIME_ERROR;
	*weight = support->blend.contribution;
	return GMT_NOERROR;
}

void ssh_support_free(struct SSH_SUPPORT *support)
{
	if (!support) return;
	blend_window_boundary_clear(&support->blend);
	blend_polygon_free(&support->real_polygon);
	memset(support, 0, sizeof(*support));
}

static uint64_t ssh_rng_next(struct SSH_RNG *rng)
{
	uint64_t x = rng->state;
	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	rng->state = x;
	return x * 2685821657736338717ULL;
}

static double ssh_uniform(struct SSH_RNG *rng)
{
	return ((ssh_rng_next(rng) >> 11) + 0.5) *
	       (1.0 / 9007199254740992.0);
}

static double ssh_gaussian(struct SSH_RNG *rng)
{
	double radius, angle;
	if (rng->spare) {
		rng->spare = false;
		return rng->gaussian;
	}
	radius = sqrt(-2.0 * log(ssh_uniform(rng)));
	angle = 2.0 * M_PI * ssh_uniform(rng);
	rng->gaussian = radius * sin(angle);
	rng->spare = true;
	return radius * cos(angle);
}

static double ssh_frequency(size_t index, size_t n, double increment)
{
	ptrdiff_t signed_index =
	    index <= n / 2 ? (ptrdiff_t)index : (ptrdiff_t)index - (ptrdiff_t)n;
	return 2.0 * M_PI * (double)signed_index / ((double)n * increment);
}

static double ssh_amplitude(enum SSH_MODEL model, unsigned int dim,
                            const struct SSH_STAT *stat,
                            const double wave[SSH_MAX_DIM])
{
	double q2 = 0.0;
	unsigned int axis;
	if (model == SSH_MODEL_WHITE) return 1.0;
	for (axis = 0; axis < dim; axis++) {
		double q = stat->correlation[axis] * wave[axis];
		q2 += q * q;
	}
	switch (model) {
		case SSH_MODEL_VON_KARMAN:
			return pow(1.0 + q2, -0.5 * (stat->hurst + 0.5 * dim));
		case SSH_MODEL_GAUSSIAN:
			return exp(-q2 / 8.0);
		case SSH_MODEL_EXPONENTIAL:
			return pow(1.0 + q2, -0.25 * (dim + 1.0));
		case SSH_MODEL_WHITE:
			return 1.0;
	}
	return 0.0;
}

static int ssh_fft3d(void *API, gmt_grdfloat *data,
                     const size_t n[SSH_MAX_DIM], int direction)
{
	size_t z, y, x;
	gmt_grdfloat *line = NULL;
	if (n[0] > UINT_MAX || n[1] > UINT_MAX) return GMT_DIM_TOO_LARGE;
	for (z = 0; z < n[2]; z++)
		if (GMT_FFT_2D(API, data + 2 * z * n[0] * n[1],
		               (unsigned int)n[0], (unsigned int)n[1],
		               direction, GMT_FFT_COMPLEX))
			return GMT_RUNTIME_ERROR;
	line = calloc(2 * n[2], sizeof(*line));
	if (!line) return GMT_MEMORY_ERROR;
	for (y = 0; y < n[1]; y++)
		for (x = 0; x < n[0]; x++) {
			for (z = 0; z < n[2]; z++) {
				size_t index = 2 * (z * n[0] * n[1] + y * n[0] + x);
				line[2 * z] = data[index];
				line[2 * z + 1] = data[index + 1];
			}
			if (GMT_FFT_1D(API, line, n[2], direction, GMT_FFT_COMPLEX)) {
				free(line);
				return GMT_RUNTIME_ERROR;
			}
			for (z = 0; z < n[2]; z++) {
				size_t index = 2 * (z * n[0] * n[1] + y * n[0] + x);
				data[index] = line[2 * z];
				data[index + 1] = line[2 * z + 1];
			}
		}
	free(line);
	return GMT_NOERROR;
}

static int ssh_fft(void *API, unsigned int dim, gmt_grdfloat *data,
                   const size_t n[SSH_MAX_DIM], int direction)
{
	if (dim == 1)
		return GMT_FFT_1D(API, data, n[0], direction, GMT_FFT_COMPLEX)
		       ? GMT_RUNTIME_ERROR : GMT_NOERROR;
	if (dim == 2) {
		if (n[0] > UINT_MAX || n[1] > UINT_MAX) return GMT_DIM_TOO_LARGE;
		return GMT_FFT_2D(API, data, (unsigned int)n[0], (unsigned int)n[1],
		                  direction, GMT_FFT_COMPLEX)
		       ? GMT_RUNTIME_ERROR : GMT_NOERROR;
	}
	return ssh_fft3d(API, data, n, direction);
}

int ssh_generate(void *API, unsigned int dim,
                 const size_t n[SSH_MAX_DIM],
                 const double increment[SSH_MAX_DIM],
                 const double max_correlation[SSH_MAX_DIM],
                 const struct SSH_STAT *stat, enum SSH_MODEL model,
                 const struct SSH_RANDOM *random, uint64_t seed,
                 double *output)
{
	size_t padded[SSH_MAX_DIM] = {1, 1, 1};
	size_t pad[SSH_MAX_DIM] = {0, 0, 0};
	size_t total = 1, cropped = 1, i, j, k, index;
	gmt_grdfloat *data = NULL;
	struct SSH_RNG rng = {0};
	double mean = 0.0, sumsq = 0.0, sigma;
	int status;
	unsigned int axis;

	for (axis = 0; axis < dim; axis++) {
		if (random->padding > 0.0 && model != SSH_MODEL_WHITE)
			pad[axis] = (size_t)ceil(
			    random->padding * max_correlation[axis] / increment[axis]);
		if (n[axis] > SIZE_MAX - 2 * pad[axis]) return GMT_DIM_TOO_LARGE;
		padded[axis] = n[axis] + 2 * pad[axis];
		if (padded[axis] < 2 || total > SIZE_MAX / padded[axis])
			return GMT_DIM_TOO_LARGE;
		total *= padded[axis];
		cropped *= n[axis];
	}
	if (total > SIZE_MAX / (2 * sizeof(*data))) return GMT_DIM_TOO_LARGE;
	data = calloc(2 * total, sizeof(*data));
	if (!data) return GMT_MEMORY_ERROR;
	rng.state = seed ? seed : 0x9e3779b97f4a7c15ULL;
	for (index = 0; index < total; index++)
		data[2 * index] = (gmt_grdfloat)ssh_gaussian(&rng);
	status = ssh_fft(API, dim, data, padded, GMT_FFT_FWD);
	if (status != GMT_NOERROR) goto cleanup;
	for (k = 0; k < padded[2]; k++)
		for (j = 0; j < padded[1]; j++)
			for (i = 0; i < padded[0]; i++) {
				double wave[SSH_MAX_DIM] = {0.0, 0.0, 0.0};
				double amplitude;
				index = k * padded[0] * padded[1] + j * padded[0] + i;
				wave[0] = ssh_frequency(i, padded[0], increment[0]);
				if (dim > 1)
					wave[1] = ssh_frequency(j, padded[1], increment[1]);
				if (dim > 2)
					wave[2] = ssh_frequency(k, padded[2], increment[2]);
				amplitude = (i == 0 && j == 0 && k == 0)
				          ? 0.0 : ssh_amplitude(model, dim, stat, wave);
				data[2 * index] *= (gmt_grdfloat)amplitude;
				data[2 * index + 1] *= (gmt_grdfloat)amplitude;
			}
	status = ssh_fft(API, dim, data, padded, GMT_FFT_INV);
	if (status != GMT_NOERROR) goto cleanup;
	index = 0;
	for (k = 0; k < n[2]; k++)
		for (j = 0; j < n[1]; j++)
			for (i = 0; i < n[0]; i++) {
				size_t source =
				    (k + pad[2]) * padded[0] * padded[1] +
				    (j + pad[1]) * padded[0] + i + pad[0];
				output[index] = data[2 * source];
				mean += output[index++];
			}
	mean /= (double)cropped;
	for (index = 0; index < cropped; index++) {
		output[index] -= mean;
		sumsq += output[index] * output[index];
	}
	if (cropped < 2 || sumsq <= 0.0 || !isfinite(sumsq)) {
		status = GMT_RUNTIME_ERROR;
		goto cleanup;
	}
	sigma = sqrt(sumsq / (double)(cropped - 1));
	for (index = 0; index < cropped; index++)
		output[index] *= stat->sigma / sigma;
	status = GMT_NOERROR;
cleanup:
	free(data);
	return status;
}

int ssh_apply_taper(struct GMTAPI_CTRL *API, unsigned int dim,
                    const size_t n[SSH_MAX_DIM],
                    struct SSH_SUPPORT *support, double *field,
                    double *weight)
{
	size_t i, j, k, index = 0;
	for (k = 0; k < n[2]; k++)
		for (j = 0; j < n[1]; j++)
			for (i = 0; i < n[0]; i++, index++) {
				double value = 1.0;
				if (support && support->ready &&
				    ssh_support_weight(support, dim, i, j, k, &value)) {
					GMT_Report(API, GMT_MSG_ERROR,
					           "Unable to evaluate BLEND taper weight\n");
					return GMT_RUNTIME_ERROR;
				}
				field[index] *= value;
				if (weight) weight[index] = value;
			}
	return GMT_NOERROR;
}
