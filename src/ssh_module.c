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
#include "ssh_module.h"
#include "ssh_netcdf.h"

#include <ctype.h>
#include <netcdf.h>

struct SSH_TEXT {
	size_t n;
	size_t n_fields;
	double *coordinate;
	double **value;
};

struct SSH_TEXT_SOURCE {
	char *path;
	bool has_sentinel;
	double sentinel;
	struct GQ_TRANSFORM transform;
};

void ssh_ctrl_init(struct SSH_CTRL *Ctrl)
{
	memset(Ctrl, 0, sizeof(*Ctrl));
	Ctrl->model = SSH_MODEL_VON_KARMAN;
	Ctrl->random.seed = 1;
	Ctrl->random.padding = 1.0;
	Ctrl->H.method = 'l';
	Ctrl->H.sectors = 4;
	Ctrl->S.mode = GMT_SPLINE_LINEAR;
	Ctrl->S.max_gap = INFINITY;
	gq_transform_init(&Ctrl->Z.transform);
	ssh_taper_defaults(&Ctrl->taper);
}

void ssh_ctrl_free(struct SSH_CTRL *Ctrl)
{
	size_t k;
	if (!Ctrl) return;
	for (k = 0; k < Ctrl->In.n; k++) free(Ctrl->In.file[k]);
	free(Ctrl->In.file);
	ssh_free_field_list(Ctrl->F.name, Ctrl->F.n);
	free(Ctrl->G.file);
	ssh_free_stat_config(&Ctrl->statistic);
	ssh_taper_free(&Ctrl->taper);
	gq_transform_free(&Ctrl->Z.transform);
	memset(Ctrl, 0, sizeof(*Ctrl));
}

static int ssh_parse_interpolation(struct GMTAPI_CTRL *API, const char *text,
                                   struct SSH_INTERPOLATION *S)
{
	char copy[GMT_LEN128], *modifier = NULL, *end = NULL;
	if (!text || !text[0] || strlen(text) >= sizeof(copy)) goto bad;
	strcpy(copy, text);
	modifier = strchr(copy, '+');
	if (modifier) {
		*modifier++ = '\0';
		if (modifier[0] != 'g' || strchr(modifier + 1, '+')) goto bad;
		S->bridge = true;
		S->max_gap = INFINITY;
		if (modifier[1]) {
			errno = 0;
			S->max_gap = strtod(modifier + 1, &end);
			if (errno || end == modifier + 1 || *end ||
			    !isfinite(S->max_gap) || S->max_gap <= 0.0)
				goto bad;
		}
	}
	switch (copy[0]) {
		case 'a': S->mode = GMT_SPLINE_AKIMA; break;
		case 'c': S->mode = GMT_SPLINE_CUBIC; break;
		case 'e': S->mode = GMT_SPLINE_STEP; break;
		case 'l': S->mode = GMT_SPLINE_LINEAR; break;
		case 'n': S->mode = GMT_SPLINE_NN; break;
		case 's':
			if (!copy[1]) goto bad;
			errno = 0;
			S->fit = strtod(copy + 1, &end);
			if (errno || end == copy + 1 || *end ||
			    !isfinite(S->fit) || S->fit < 0.0)
				goto bad;
			S->mode = gmt_M_is_zero(S->fit)
			        ? GMT_SPLINE_CUBIC : GMT_SPLINE_SMOOTH;
			break;
		default: goto bad;
	}
	if (copy[0] != 's' && copy[1]) goto bad;
	S->active = true;
	return GMT_NOERROR;
bad:
	GMT_Report(API, GMT_MSG_ERROR,
	           "Option -S must be a, c, e, l, n, or s<p>, optionally followed "
	           "by +g[<maxgap>]\n");
	return GMT_PARSE_ERROR;
}

static int ssh_parse_gap_option(struct GMTAPI_CTRL *API, const char *text,
                                struct SSH_GAP *H)
{
	const char *p = text, *modifier;
	char value[GMT_LEN64] = {""}, *end = NULL;
	size_t length;
	H->method = 'l';
	H->argument = 0.0;
	H->sectors = 4;
	H->limited = false;
	H->max_gap = 0;
	if (p && *p && *p != '+') H->method = *p++;
	if (!strchr("nlasm", H->method)) goto bad;
	modifier = p ? strchr(p, '+') : NULL;
	length = modifier ? (size_t)(modifier - p) : (p ? strlen(p) : 0);
	if (length >= sizeof(value)) goto bad;
	if (length) {
		memcpy(value, p, length);
		value[length] = '\0';
		if (H->method == 'a') {
			char *slash = strchr(value, '/');
			if (slash) {
				unsigned long sectors;
				*slash++ = '\0';
				sectors = strtoul(slash, &end, 10);
				if (end == slash || *end || sectors < 1 || sectors > UINT_MAX)
					goto bad;
				H->sectors = (unsigned int)sectors;
			}
		}
		errno = 0;
		H->argument = strtod(value, &end);
		if (errno || end == value || *end || !isfinite(H->argument)) goto bad;
	}
	if (H->method == 'a' && H->argument <= 0.0) H->argument = 3.0;
	if (H->method == 'n' && H->argument < 0.0) goto bad;
	if ((H->method == 's' || H->method == 'm') &&
	    (H->argument < 0.0 || H->argument > 1.0)) goto bad;
	if (H->method == 'l' && length) goto bad;
	for (p = modifier; p && *p;) {
		unsigned long gap;
		if (p[0] != '+' || p[1] != 'm') goto bad;
		p += 2;
		errno = 0;
		gap = strtoul(p, &end, 10);
		if (errno || end == p || gap < 1 || gap > UINT_MAX) goto bad;
		H->limited = true;
		H->max_gap = (unsigned int)gap;
		p = end;
	}
	H->active = true;
	return GMT_NOERROR;
bad:
	GMT_Report(API, GMT_MSG_ERROR,
	           "Option -H must be n[<radius>], l, a[<radius>[/<sectors>]], "
	           "s[<tension>], or m[<tension>], optionally with +m<maxgap>\n");
	return GMT_PARSE_ERROR;
}

static int ssh_parse_output_transform(struct GMTAPI_CTRL *API,
	                                  const char *text, unsigned int dim,
	                                  struct SSH_CTRL *Ctrl)
{
	char message[GMT_LEN256] = {""};
	unsigned int mask = GQ_TRANSFORM_X_MASK |
	                    (dim >= 2 ? GQ_TRANSFORM_Y_MASK : 0U) |
	                    (dim >= 3 ? GQ_TRANSFORM_Z_MASK : 0U);
	if (!text || text[0] != '+') {
		GMT_Report(API, GMT_MSG_ERROR,
		           "Option -Z requires at least one explicit +x, +X, +y, +Y, "
		           "+z, +Z, +v, or +V modifier valid for this module\n");
		return GMT_PARSE_ERROR;
	}
	if (gq_transform_parse(text, mask, false, &Ctrl->Z.transform,
	                       NULL, NULL, message, sizeof(message))) {
		GMT_Report(API, GMT_MSG_ERROR, "Option -Z: %s\n", message);
		return GMT_PARSE_ERROR;
	}
	Ctrl->Z.active = true;
	return GMT_NOERROR;
}

static int ssh_parse_increment(struct GMTAPI_CTRL *API, const char *text,
                               unsigned int dim, struct SSH_CTRL *Ctrl)
{
	char copy[GMT_LEN128], *slash;
	if (!text || strlen(text) >= sizeof(copy)) goto bad;
	strcpy(copy, text);
	slash = strchr(copy, '/');
	if (slash) *slash++ = '\0';
	if (ssh_parse_number(copy, &Ctrl->I.value[0]) ||
	    Ctrl->I.value[0] <= 0.0)
		goto bad;
	if (slash) {
		if (dim < 2 || strchr(slash, '/') ||
		    ssh_parse_number(slash, &Ctrl->I.value[1]) ||
		    Ctrl->I.value[1] <= 0.0)
			goto bad;
	}
	else
		Ctrl->I.value[1] = Ctrl->I.value[0];
	return GMT_NOERROR;
bad:
	GMT_Report(API, GMT_MSG_ERROR,
	           "Option -I must be positive dx[/dy]\n");
	return GMT_PARSE_ERROR;
}

static int ssh_parse_range(struct GMTAPI_CTRL *API, const char *text,
                           struct SSH_RANGE *range)
{
	char copy[GMT_LEN256], *token = NULL, *save = NULL;
	double value[3], intervals, adjusted, tolerance;
	size_t n = 0;
	if (!text || strlen(text) >= sizeof(copy)) goto bad;
	strcpy(copy, text);
	for (token = strtok_r(copy, "/", &save); token && n < 3;
	     token = strtok_r(NULL, "/", &save)) {
		if (ssh_parse_number(token, &value[n])) goto bad;
		n++;
	}
	if (token || n != 3 || value[0] >= value[1] || value[2] <= 0.0)
		goto bad;
	intervals = (value[1] - value[0]) / value[2];
	if (intervals > (double)(SIZE_MAX - 1)) return GMT_DIM_TOO_LARGE;
	range->n = (size_t)floor(intervals + 0.5) + 1;
	adjusted = value[0] + (double)(range->n - 1) * value[2];
	tolerance = 32.0 * DBL_EPSILON *
	            MAX(1.0, MAX(fabs(value[1]), fabs(adjusted)));
	if (fabs(adjusted - value[1]) > tolerance)
		GMT_Report(API, GMT_MSG_WARNING,
		           "Adjusting range maximum from %.12g to %.12g\n",
		           value[1], adjusted);
	range->min = value[0];
	range->max = adjusted;
	range->increment = value[2];
	return GMT_NOERROR;
bad:
	GMT_Report(API, GMT_MSG_ERROR,
	           "Option -T must be min/max/inc with min < max and inc > 0\n");
	return GMT_PARSE_ERROR;
}

static int ssh_append_input(struct SSH_CTRL *Ctrl, const char *text)
{
	char **next = realloc(Ctrl->In.file,
	                      (Ctrl->In.n + 1) * sizeof(*next));
	if (!next) return GMT_MEMORY_ERROR;
	Ctrl->In.file = next;
	Ctrl->In.file[Ctrl->In.n] = strdup(text);
	if (!Ctrl->In.file[Ctrl->In.n]) return GMT_MEMORY_ERROR;
	Ctrl->In.n++;
	return GMT_NOERROR;
}

int ssh_parse_options(struct GMT_CTRL *GMT, struct SSH_CTRL *Ctrl,
                      struct GMT_OPTION *options, unsigned int dim)
{
	struct GMT_OPTION *opt;
	struct GMTAPI_CTRL *API = GMT->parent;
	unsigned int errors = 0;
	bool field_option = false;

	for (opt = options; opt; opt = opt->next) {
		switch (opt->option) {
			case '<':
				errors += ssh_append_input(Ctrl, opt->arg);
				break;
			case 'A':
				errors += gmt_M_repeated_module_option(API, Ctrl->A.active);
				if (opt->arg[0]) errors++;
				break;
			case 'C':
				errors += ssh_parse_stat_option(API, opt->arg,
				                                SSH_STAT_CORRELATION,
				                                dim, &Ctrl->statistic);
				break;
			case 'E':
				errors += ssh_parse_monotone(API, opt->arg, &Ctrl->taper);
				break;
			case 'F':
				errors += gmt_M_repeated_module_option(API, field_option);
				errors += ssh_parse_field_list(API, opt->arg,
				                               &Ctrl->F.name, &Ctrl->F.n);
				Ctrl->F.active = true;
				break;
			case 'G':
				errors += gmt_M_repeated_module_option(API, Ctrl->G.active);
				if (!opt->arg[0]) errors++;
				else Ctrl->G.file = strdup(opt->arg);
				break;
			case 'D':
				errors += ssh_parse_stat_option(API, opt->arg,
				                                SSH_STAT_SIGMA,
				                                dim, &Ctrl->statistic);
				break;
			case 'H':
				errors += gmt_M_repeated_module_option(API, Ctrl->H.active);
				errors += ssh_parse_gap_option(API, opt->arg, &Ctrl->H);
				break;
			case 'I':
				errors += gmt_M_repeated_module_option(API, Ctrl->I.active);
				errors += ssh_parse_increment(API, opt->arg, dim, Ctrl);
				break;
			case 'L':
				errors += gmt_M_repeated_module_option(
				    API, Ctrl->taper.have_interval);
				errors += ssh_parse_interval(API, opt->arg,
				                             Ctrl->taper.interval);
				Ctrl->taper.active = true;
				break;
			case 'M':
				errors += gmt_M_repeated_module_option(API, Ctrl->M.active);
				errors += ssh_parse_model(API, opt->arg, &Ctrl->model);
				break;
			case 'P':
				if (Ctrl->taper.polygon) {
					errors++;
					break;
				}
				Ctrl->taper.polygon = strdup(opt->arg);
				if (!Ctrl->taper.polygon) return GMT_MEMORY_ERROR;
				Ctrl->taper.active = true;
				break;
			case 'Q':
				errors += gmt_M_repeated_module_option(API, Ctrl->Q.active);
				errors += ssh_parse_random(API, opt->arg, &Ctrl->random);
				break;
			case 'S':
				errors += gmt_M_repeated_module_option(API, Ctrl->S.active);
				errors += ssh_parse_interpolation(API, opt->arg, &Ctrl->S);
				break;
			case 'T':
				errors += gmt_M_repeated_module_option(API, Ctrl->T.active);
				errors += ssh_parse_range(API, opt->arg, &Ctrl->T);
				break;
			case 'W':
				if (Ctrl->taper.active &&
				    !Ctrl->taper.have_interval && !Ctrl->taper.polygon)
					errors++;
				errors += ssh_parse_taper(API, opt->arg, dim, &Ctrl->taper);
				break;
			case 'U':
				errors += ssh_parse_stat_option(API, opt->arg,
				                                SSH_STAT_HURST,
				                                dim, &Ctrl->statistic);
				break;
			case 'Z':
				errors += gmt_M_repeated_module_option(API, Ctrl->Z.active);
				errors += ssh_parse_output_transform(API, opt->arg, dim, Ctrl);
				break;
			default:
				errors += gmt_default_option_error(GMT, opt);
				break;
		}
	}
	errors += gmt_M_check_condition(GMT, Ctrl->In.n > 1,
	                                "At most one input model may be supplied\n");
	errors += gmt_M_check_condition(GMT, !Ctrl->G.active,
	                                "Option -G is required\n");
	errors += gmt_M_check_condition(GMT, Ctrl->A.active && Ctrl->In.n != 1,
	                                "Application mode -A requires one input model\n");
	errors += gmt_M_check_condition(GMT, !Ctrl->A.active && Ctrl->In.n != 0,
	                                "An input model requires application mode -A\n");
	errors += gmt_M_check_condition(GMT, Ctrl->A.active && !Ctrl->F.active,
	                                "Application mode requires -F<fields>\n");
	errors += gmt_M_check_condition(GMT, dim == 1 && !Ctrl->A.active &&
	                                !Ctrl->T.active,
	                                "Output-only ssh1d requires -T\n");
	errors += gmt_M_check_condition(GMT, dim > 1 && !Ctrl->A.active &&
	                                (!GMT->common.R.active[RSET] ||
	                                 !Ctrl->I.active),
	                                "Output-only ssh2d/ssh3d requires -R and -I\n");
	errors += gmt_M_check_condition(GMT, dim == 3 && !Ctrl->A.active &&
	                                !Ctrl->T.active,
	                                "Output-only ssh3d requires -T\n");
	errors += gmt_M_check_condition(GMT, dim == 1 && Ctrl->I.active,
	                                "ssh1d uses -T rather than -I\n");
	errors += gmt_M_check_condition(GMT, dim == 2 && Ctrl->T.active,
	                                "ssh2d does not use -T\n");
	errors += gmt_M_check_condition(GMT, dim == 1 && Ctrl->H.active,
	                                "ssh1d does not use horizontal gap filling -H\n");
	errors += gmt_M_check_condition(GMT, dim == 2 && Ctrl->S.active,
	                                "ssh2d does not use vertical interpolation -S\n");
	errors += gmt_M_check_condition(GMT, !Ctrl->A.active &&
	                                (Ctrl->H.active || Ctrl->S.active),
	                                "Options -H and -S require application mode -A\n");
	errors += gmt_M_check_condition(GMT, Ctrl->A.active && Ctrl->S.active &&
	                                !Ctrl->T.active && !Ctrl->S.bridge,
	                                "Option -S requires -T or the +g modifier in "
	                                "application mode\n");
	errors += gmt_M_check_condition(GMT, dim == 1 && Ctrl->taper.polygon,
	                                "ssh1d does not accept polygon supports\n");
	errors += gmt_M_check_condition(GMT, dim == 2 && Ctrl->taper.have_interval,
	                                "ssh2d does not accept -L\n");
	errors += gmt_M_check_condition(GMT, dim < 2 && Ctrl->taper.monotone,
	                                "Polygon conversion is only valid in 2-D and 3-D\n");
	if (errors)
		GMT_Report(API, GMT_MSG_ERROR,
		           "Invalid SSH options; use the module -? option for usage\n");
	return errors ? GMT_PARSE_ERROR : GMT_NOERROR;
}

static bool ssh_has_field(char **fields, size_t n, const char *name)
{
	size_t k;
	for (k = 0; k < n; k++) if (!strcmp(fields[k], name)) return true;
	return false;
}

static int ssh_validate_overrides(struct GMTAPI_CTRL *API,
                                  const struct SSH_CTRL *Ctrl,
                                  char **fields, size_t n_fields)
{
	size_t k;
	for (k = 0; k < Ctrl->statistic.n_overrides; k++)
		if (!ssh_has_field(fields, n_fields,
		                   Ctrl->statistic.override[k].field)) {
			GMT_Report(API, GMT_MSG_ERROR,
			           "Statistics were supplied for unselected field %s\n",
			           Ctrl->statistic.override[k].field);
			return GMT_PARSE_ERROR;
		}
	return GMT_NOERROR;
}

static int ssh_output_geometry(struct GMT_CTRL *GMT,
                               const struct SSH_CTRL *Ctrl, unsigned int dim,
                               size_t n[SSH_MAX_DIM],
                               double origin[SSH_MAX_DIM],
                               double increment[SSH_MAX_DIM],
                               double *coordinate[SSH_MAX_DIM])
{
	unsigned int axis;
	if (dim == 1) {
		n[0] = Ctrl->T.n;
		origin[0] = Ctrl->T.min;
		increment[0] = Ctrl->T.increment;
	}
	else {
		double width[2];
		origin[0] = GMT->common.R.wesn[XLO];
		origin[1] = GMT->common.R.wesn[YLO];
		increment[0] = Ctrl->I.value[0];
		increment[1] = Ctrl->I.value[1];
		width[0] = GMT->common.R.wesn[XHI] - origin[0];
		width[1] = GMT->common.R.wesn[YHI] - origin[1];
		n[0] = (size_t)floor(width[0] / increment[0] + 0.5) + 1;
		n[1] = (size_t)floor(width[1] / increment[1] + 0.5) + 1;
		if (dim == 3) {
			n[2] = Ctrl->T.n;
			origin[2] = Ctrl->T.min;
			increment[2] = Ctrl->T.increment;
		}
	}
	for (axis = dim; axis < SSH_MAX_DIM; axis++) {
		n[axis] = 1;
		origin[axis] = 0.0;
		increment[axis] = 1.0;
	}
	for (axis = 0; axis < dim; axis++) {
		size_t k;
		if (n[axis] < 2) return GMT_DIM_TOO_SMALL;
		coordinate[axis] = calloc(n[axis], sizeof(double));
		if (!coordinate[axis]) return GMT_MEMORY_ERROR;
		for (k = 0; k < n[axis]; k++)
			coordinate[axis][k] = origin[axis] + (double)k * increment[axis];
	}
	return GMT_NOERROR;
}

static int ssh_field_setup(struct GMTAPI_CTRL *API,
                           const struct SSH_CTRL *Ctrl, unsigned int dim,
                           char ***keys, char ***output_names, size_t *n_fields,
                           struct SSH_STAT **stat,
                           double max_correlation[SSH_MAX_DIM])
{
	size_t field;
	unsigned int axis;
	if (Ctrl->F.active) {
		*n_fields = Ctrl->F.n;
		*keys = calloc(*n_fields, sizeof(**keys));
		*output_names = calloc(*n_fields, sizeof(**output_names));
		if (!*keys || !*output_names) return GMT_MEMORY_ERROR;
		for (field = 0; field < *n_fields; field++) {
			size_t length = strlen(Ctrl->F.name[field]) +
			                strlen("_heterogeneity") + 1;
			(*keys)[field] = strdup(Ctrl->F.name[field]);
			(*output_names)[field] = calloc(length, 1);
			if (!(*keys)[field] || !(*output_names)[field])
				return GMT_MEMORY_ERROR;
			snprintf((*output_names)[field], length, "%s_heterogeneity",
			         Ctrl->F.name[field]);
		}
	}
	else {
		*n_fields = 1;
		*keys = calloc(1, sizeof(**keys));
		*output_names = calloc(1, sizeof(**output_names));
		if (!*keys || !*output_names) return GMT_MEMORY_ERROR;
		(*keys)[0] = strdup("heterogeneity");
		(*output_names)[0] = strdup("heterogeneity");
		if (!(*keys)[0] || !(*output_names)[0]) return GMT_MEMORY_ERROR;
	}
	if (ssh_validate_overrides(API, Ctrl, *keys, *n_fields))
		return GMT_PARSE_ERROR;
	*stat = calloc(*n_fields, sizeof(**stat));
	if (!*stat) return GMT_MEMORY_ERROR;
	for (field = 0; field < *n_fields; field++) {
		if (ssh_resolve_stat(API, &Ctrl->statistic, (*keys)[field], dim,
		                     Ctrl->model, &(*stat)[field]))
			return GMT_PARSE_ERROR;
		if (Ctrl->model != SSH_MODEL_WHITE)
			for (axis = 0; axis < dim; axis++)
				max_correlation[axis] =
				    MAX(max_correlation[axis],
				        (*stat)[field].correlation[axis]);
	}
	return GMT_NOERROR;
}

static void ssh_field_setup_free(char **keys, char **output_names,
                                 size_t n_fields, struct SSH_STAT *stat)
{
	if (keys) ssh_free_field_list(keys, n_fields);
	if (output_names) ssh_free_field_list(output_names, n_fields);
	free(stat);
}

static int ssh_generate_fields(struct GMT_CTRL *GMT,
                               const struct SSH_CTRL *Ctrl, unsigned int dim,
                               const size_t n[SSH_MAX_DIM],
                               const double origin[SSH_MAX_DIM],
                               const double increment[SSH_MAX_DIM],
                               char **keys, size_t n_fields,
                               const struct SSH_STAT *stat,
                               const double max_correlation[SSH_MAX_DIM],
                               double ***result, double **weight)
{
	struct SSH_SUPPORT support;
	size_t field, total = n[0] * n[1] * n[2];
	int status;
	memset(&support, 0, sizeof(support));
	*result = calloc(n_fields, sizeof(**result));
	if (!*result) return GMT_MEMORY_ERROR;
	if (Ctrl->taper.write_weight) {
		*weight = calloc(total, sizeof(**weight));
		if (!*weight) return GMT_MEMORY_ERROR;
	}
	if (Ctrl->taper.active) {
		status = ssh_prepare_support(GMT, dim, n, origin, increment,
		                             &Ctrl->taper, &support);
		if (status != GMT_NOERROR) return status;
	}
	for (field = 0; field < n_fields; field++) {
		uint64_t seed = ssh_field_seed(Ctrl->random.seed, keys[field],
		                               Ctrl->random.independent);
		(*result)[field] = calloc(total, sizeof(***result));
		if (!(*result)[field]) {
			status = GMT_MEMORY_ERROR;
			goto cleanup;
		}
		GMT_Report(GMT->parent, GMT_MSG_INFORMATION,
		           "Generating %s field for %s (sigma %.6g, seed %llu)\n",
		           ssh_model_name(Ctrl->model), keys[field],
		           stat[field].sigma, (unsigned long long)seed);
		status = ssh_generate(GMT->parent, dim, n, increment, max_correlation,
		                      &stat[field], Ctrl->model, &Ctrl->random,
		                      seed, (*result)[field]);
		if (status != GMT_NOERROR) goto cleanup;
		status = ssh_apply_taper(GMT->parent, dim, n,
		                         Ctrl->taper.active ? &support : NULL,
		                         (*result)[field],
		                         field == 0 ? *weight : NULL);
		if (status != GMT_NOERROR) goto cleanup;
	}
	status = GMT_NOERROR;
cleanup:
	ssh_support_free(&support);
	return status;
}

static void ssh_free_values(double **values, size_t n)
{
	size_t k;
	if (!values) return;
	for (k = 0; k < n; k++) free(values[k]);
	free(values);
}

static bool ssh_regular_coordinate(const double *coordinate, size_t n,
                                   double *increment)
{
	size_t k;
	double tolerance;
	if (n < 2) return false;
	*increment = (coordinate[n - 1] - coordinate[0]) / (double)(n - 1);
	tolerance = 256.0 * DBL_EPSILON *
	            MAX(1.0, MAX(fabs(coordinate[0]), fabs(coordinate[n - 1])));
	for (k = 1; k < n; k++)
		if (fabs((coordinate[k] - coordinate[k - 1]) - *increment) > tolerance)
			return false;
	return *increment > 0.0;
}

static int ssh_grid_size(struct GMTAPI_CTRL *API, const char *axis,
                         double lo, double hi, double inc, size_t *n,
                         double *adjusted_hi)
{
	double intervals = (hi - lo) / inc;
	double adjusted, tolerance;
	if (lo >= hi || inc <= 0.0 || intervals > (double)(SIZE_MAX - 1))
		return GMT_DIM_TOO_LARGE;
	*n = (size_t)floor(intervals + 0.5) + 1;
	if (*n < 2) return GMT_DIM_TOO_SMALL;
	adjusted = lo + (double)(*n - 1) * inc;
	tolerance = 32.0 * DBL_EPSILON *
	            MAX(1.0, MAX(fabs(hi), fabs(adjusted)));
	if (fabs(adjusted - hi) > tolerance)
		GMT_Report(API, GMT_MSG_WARNING,
		           "Adjusting %s maximum from %.12g to %.12g to fit increment\n",
		           axis, hi, adjusted);
	*adjusted_hi = adjusted;
	return GMT_NOERROR;
}

static int ssh_horizontal_bcr(struct GMT_CTRL *GMT,
                              const double *source_x, size_t source_nx,
                              const double *source_y, size_t source_ny,
                              const double *native,
                              const double *target_x, size_t target_nx,
                              const double *target_y, size_t target_ny,
                              double *output)
{
	struct GMT_GRID *Grid = NULL;
	double wesn[4], inc[2];
	size_t row, col, source_row;
	int status = GMT_MEMORY_ERROR;
	if (!ssh_regular_coordinate(source_x, source_nx, &inc[0]) ||
	    !ssh_regular_coordinate(source_y, source_ny, &inc[1])) {
		GMT_Report(GMT->parent, GMT_MSG_ERROR,
		           "Options -R and -I require regular increasing x and y coordinates\n");
		return GMT_RUNTIME_ERROR;
	}
	wesn[XLO] = source_x[0];
	wesn[XHI] = source_x[source_nx - 1];
	wesn[YLO] = source_y[0];
	wesn[YHI] = source_y[source_ny - 1];
	Grid = GMT_Create_Data(GMT->parent, GMT_IS_GRID, GMT_IS_SURFACE,
	                       GMT_CONTAINER_AND_DATA, NULL, wesn, inc,
	                       GMT_GRID_NODE_REG, GMT_NOTSET, NULL);
	if (!Grid) return GMT_MEMORY_ERROR;
	for (row = 0; row < source_ny; row++) {
		source_row = source_ny - 1 - row;
		for (col = 0; col < source_nx; col++)
			Grid->data[gmt_M_ijp(Grid->header, row, col)] =
			    (gmt_grdfloat)native[source_row * source_nx + col];
	}
	if (gmt_grd_BC_set(GMT, Grid, GMT_IN) != GMT_NOERROR) goto cleanup;
	for (row = 0; row < target_ny; row++)
		for (col = 0; col < target_nx; col++)
			output[row * target_nx + col] =
			    gmt_bcr_get_z(GMT, Grid, target_x[col], target_y[row]);
	status = GMT_NOERROR;
cleanup:
	if (GMT_Destroy_Data(GMT->parent, &Grid) != GMT_NOERROR &&
	    status == GMT_NOERROR)
		status = GMT_RUNTIME_ERROR;
	return status;
}

static int ssh_resample_horizontal(struct GMT_CTRL *GMT,
                                   const struct SSH_CTRL *Ctrl,
                                   struct SSH_NC *cube, double ***values)
{
	bool use_region = GMT->common.R.active[RSET];
	bool use_increment = Ctrl->I.active;
	double wesn[4], inc[2], source_inc[2], adjusted, tolerance_x, tolerance_y;
	double *x = NULL, *y = NULL, *native = NULL, *sampled = NULL;
	double **resampled = NULL;
	size_t source_nx = cube->n[0], source_ny = cube->n[1];
	size_t source_plane, target_plane, nx, ny, ix, iy, iz, k, field;
	int status = GMT_MEMORY_ERROR;
	if (cube->dim < 2 || (!use_region && !use_increment)) return GMT_NOERROR;
	if (!ssh_regular_coordinate(cube->coordinate[0], source_nx, &source_inc[0]) ||
	    !ssh_regular_coordinate(cube->coordinate[1], source_ny, &source_inc[1])) {
		GMT_Report(GMT->parent, GMT_MSG_ERROR,
		           "Options -R and -I require regular increasing x and y coordinates\n");
		return GMT_RUNTIME_ERROR;
	}
	wesn[XLO] = use_region ? GMT->common.R.wesn[XLO] : cube->coordinate[0][0];
	wesn[XHI] = use_region ? GMT->common.R.wesn[XHI]
	                       : cube->coordinate[0][source_nx - 1];
	wesn[YLO] = use_region ? GMT->common.R.wesn[YLO] : cube->coordinate[1][0];
	wesn[YHI] = use_region ? GMT->common.R.wesn[YHI]
	                       : cube->coordinate[1][source_ny - 1];
	inc[0] = use_increment ? Ctrl->I.value[0] : source_inc[0];
	inc[1] = use_increment ? Ctrl->I.value[1] : source_inc[1];
	tolerance_x = 64.0 * DBL_EPSILON * MAX(1.0,
	              MAX(fabs(cube->coordinate[0][0]),
	                  fabs(cube->coordinate[0][source_nx - 1])));
	tolerance_y = 64.0 * DBL_EPSILON * MAX(1.0,
	              MAX(fabs(cube->coordinate[1][0]),
	                  fabs(cube->coordinate[1][source_ny - 1])));
	if (wesn[XLO] < cube->coordinate[0][0] - tolerance_x ||
	    wesn[XHI] > cube->coordinate[0][source_nx - 1] + tolerance_x ||
	    wesn[YLO] < cube->coordinate[1][0] - tolerance_y ||
	    wesn[YHI] > cube->coordinate[1][source_ny - 1] + tolerance_y) {
		GMT_Report(GMT->parent, GMT_MSG_ERROR,
		           "Option -R must remain within the transformed input model domain\n");
		return GMT_RUNTIME_ERROR;
	}
	if (ssh_grid_size(GMT->parent, "x", wesn[XLO], wesn[XHI], inc[0],
	                  &nx, &adjusted))
		return GMT_RUNTIME_ERROR;
	wesn[XHI] = adjusted;
	if (ssh_grid_size(GMT->parent, "y", wesn[YLO], wesn[YHI], inc[1],
	                  &ny, &adjusted))
		return GMT_RUNTIME_ERROR;
	wesn[YHI] = adjusted;
	if (wesn[XHI] > cube->coordinate[0][source_nx - 1] + tolerance_x ||
	    wesn[YHI] > cube->coordinate[1][source_ny - 1] + tolerance_y) {
		GMT_Report(GMT->parent, GMT_MSG_ERROR,
		           "The adjusted -R/-I lattice extends beyond the input model domain\n");
		return GMT_RUNTIME_ERROR;
	}
	x = calloc(nx, sizeof(*x));
	y = calloc(ny, sizeof(*y));
	resampled = calloc(cube->n_fields, sizeof(*resampled));
	if (!x || !y || !resampled) goto cleanup;
	for (k = 0; k < nx; k++) x[k] = wesn[XLO] + (double)k * inc[0];
	for (k = 0; k < ny; k++) y[k] = wesn[YLO] + (double)k * inc[1];
	if (nx == source_nx && ny == source_ny &&
	    !memcmp(x, cube->coordinate[0], nx * sizeof(*x)) &&
	    !memcmp(y, cube->coordinate[1], ny * sizeof(*y))) {
		status = GMT_NOERROR;
		goto cleanup;
	}
	source_plane = source_nx * source_ny;
	target_plane = nx * ny;
	native = calloc(source_plane, sizeof(*native));
	sampled = calloc(target_plane, sizeof(*sampled));
	if (!native || !sampled) goto cleanup;
	for (field = 0; field < cube->n_fields; field++) {
		resampled[field] = calloc(cube->n[2] * target_plane,
		                           sizeof(*resampled[field]));
		if (!resampled[field]) goto cleanup;
		for (iz = 0; iz < cube->n[2]; iz++) {
			for (iy = 0; iy < source_ny; iy++)
				for (ix = 0; ix < source_nx; ix++)
					native[iy * source_nx + ix] =
					    (*values)[field][iz * source_plane + iy * source_nx + ix];
			status = ssh_horizontal_bcr(GMT, cube->coordinate[0], source_nx,
			                            cube->coordinate[1], source_ny, native,
			                            x, nx, y, ny, sampled);
			if (status != GMT_NOERROR) goto cleanup;
			memcpy(&resampled[field][iz * target_plane], sampled,
			       target_plane * sizeof(*sampled));
		}
	}
	for (field = 0; field < cube->n_fields; field++) {
		free((*values)[field]);
		(*values)[field] = resampled[field];
		resampled[field] = NULL;
	}
	free(cube->coordinate[0]);
	free(cube->coordinate[1]);
	cube->coordinate[0] = x;
	cube->coordinate[1] = y;
	x = y = NULL;
	cube->n[0] = nx;
	cube->n[1] = ny;
	cube->increment[0] = inc[0];
	cube->increment[1] = inc[1];
	cube->lattice_changed[0] = true;
	cube->lattice_changed[1] = true;
	GMT_Report(GMT->parent, GMT_MSG_INFORMATION,
	           "Resampled model horizontally to %zu by %zu nodes\n", nx, ny);
	status = GMT_NOERROR;
cleanup:
	if (resampled) {
		for (field = 0; field < cube->n_fields; field++) free(resampled[field]);
	}
	free(resampled);
	free(x);
	free(y);
	free(native);
	free(sampled);
	return status;
}

static int ssh_interpolate_run(struct GMT_CTRL *GMT,
                               const double *x, const double *value, size_t n,
                               const double *target, size_t n_target,
                               double *output, double fit, unsigned int mode)
{
	size_t first = 0, count;
	while (first < n_target && target[first] < x[0]) first++;
	count = first;
	while (count < n_target && target[count] <= x[n - 1]) count++;
	count -= first;
	if (n == 1) {
		if (count == 1 && fabs(target[first] - x[0]) <=
		    32.0 * DBL_EPSILON * MAX(1.0, fabs(x[0])))
			output[first] = value[0];
	}
	else if (count) {
		int status = gmt_intpol(GMT, (double *)x, (double *)value, NULL,
		                        n, count, (double *)&target[first],
		                        &output[first], fit, (int)mode);
		if (status != GMT_NOERROR) return status;
	}
	return GMT_NOERROR;
}

static int ssh_interpolate_finite(struct GMT_CTRL *GMT,
                                  const double *x, const double *value, size_t n,
                                  const double *target, size_t n_target,
                                  double *output, double fit, unsigned int mode,
                                  bool bridge, double max_gap,
                                  double *bridge_x, double *bridge_value)
{
	size_t k;
	int status;
	for (k = 0; k < n_target; k++) output[k] = NAN;
	if (!bridge) {
		size_t start = 0;
		while (start < n) {
			size_t stop;
			while (start < n && !isfinite(value[start])) start++;
			if (start == n) break;
			stop = start + 1;
			while (stop < n && isfinite(value[stop])) stop++;
			status = ssh_interpolate_run(GMT, &x[start], &value[start],
			                             stop - start, target, n_target, output,
			                             fit, mode);
			if (status != GMT_NOERROR) return status;
			start = stop;
		}
		return GMT_NOERROR;
	}
	if (!bridge_x || !bridge_value) return GMT_MEMORY_ERROR;
	{
		size_t run_count = 0, previous = 0;
		for (k = 0; k < n; k++) {
			if (!isfinite(value[k])) continue;
			if (run_count && k > previous + 1 && x[k] - x[previous] > max_gap) {
				status = ssh_interpolate_run(GMT, bridge_x, bridge_value,
				                             run_count, target, n_target, output,
				                             fit, mode);
				if (status != GMT_NOERROR) return status;
				run_count = 0;
			}
			bridge_x[run_count] = x[k];
			bridge_value[run_count++] = value[k];
			previous = k;
		}
		if (run_count)
			return ssh_interpolate_run(GMT, bridge_x, bridge_value, run_count,
			                           target, n_target, output, fit, mode);
	}
	return GMT_NOERROR;
}

static int ssh_resample_vertical(struct GMT_CTRL *GMT,
                                 const struct SSH_CTRL *Ctrl,
                                 struct SSH_NC *cube, double ***values)
{
	size_t axis = cube->dim == 1 ? 0 : 2;
	const double *source_z = cube->coordinate[axis];
	size_t source_nz = cube->n[axis], target_nz, plane, column, iz, field;
	double *target_z = NULL, *trace = NULL, *sampled = NULL;
	double *bridge_x = NULL, *bridge_value = NULL, **resampled = NULL;
	double tolerance;
	bool lattice_changed = false;
	int status = GMT_MEMORY_ERROR;
	if (!Ctrl->T.active && !Ctrl->S.bridge) return GMT_NOERROR;
	target_nz = Ctrl->T.active ? Ctrl->T.n : source_nz;
	plane = cube->dim == 1 ? 1 : cube->n[0] * cube->n[1];
	target_z = calloc(target_nz, sizeof(*target_z));
	trace = calloc(source_nz, sizeof(*trace));
	sampled = calloc(target_nz, sizeof(*sampled));
	resampled = calloc(cube->n_fields, sizeof(*resampled));
	if (Ctrl->S.bridge) {
		bridge_x = calloc(source_nz, sizeof(*bridge_x));
		bridge_value = calloc(source_nz, sizeof(*bridge_value));
	}
	if (!target_z || !trace || !sampled || !resampled ||
	    (Ctrl->S.bridge && (!bridge_x || !bridge_value)))
		goto cleanup;
	if (Ctrl->T.active) {
		for (iz = 0; iz < target_nz; iz++)
			target_z[iz] = Ctrl->T.min + (double)iz * Ctrl->T.increment;
		tolerance = 64.0 * DBL_EPSILON *
		            MAX(1.0, MAX(fabs(source_z[0]), fabs(source_z[source_nz - 1])));
		if (target_z[0] < source_z[0] - tolerance ||
		    target_z[target_nz - 1] > source_z[source_nz - 1] + tolerance) {
			GMT_Report(GMT->parent, GMT_MSG_ERROR,
			           "Option -T range must remain within %.12g/%.12g\n",
			           source_z[0], source_z[source_nz - 1]);
			status = GMT_RUNTIME_ERROR;
			goto cleanup;
		}
	}
	else memcpy(target_z, source_z, source_nz * sizeof(*target_z));
	if (target_nz != source_nz || memcmp(target_z, source_z,
	                                     source_nz * sizeof(*target_z)))
		lattice_changed = true;
	for (field = 0; field < cube->n_fields; field++) {
		resampled[field] = calloc(target_nz * plane, sizeof(*resampled[field]));
		if (!resampled[field]) goto cleanup;
		for (column = 0; column < plane; column++) {
			for (iz = 0; iz < source_nz; iz++)
				trace[iz] = (*values)[field][iz * plane + column];
			status = ssh_interpolate_finite(
			    GMT, source_z, trace, source_nz, target_z, target_nz, sampled,
			    Ctrl->S.fit, Ctrl->S.mode, Ctrl->S.bridge, Ctrl->S.max_gap,
			    bridge_x, bridge_value);
			if (status != GMT_NOERROR) goto cleanup;
			for (iz = 0; iz < target_nz; iz++)
				resampled[field][iz * plane + column] = sampled[iz];
		}
	}
	for (field = 0; field < cube->n_fields; field++) {
		free((*values)[field]);
		(*values)[field] = resampled[field];
		resampled[field] = NULL;
	}
	free(cube->coordinate[axis]);
	cube->coordinate[axis] = target_z;
	target_z = NULL;
	cube->n[axis] = target_nz;
	cube->increment[axis] = target_nz > 1
	                      ? (cube->coordinate[axis][target_nz - 1] -
	                         cube->coordinate[axis][0]) / (double)(target_nz - 1)
	                      : 1.0;
	cube->lattice_changed[axis] = lattice_changed;
	GMT_Report(GMT->parent, GMT_MSG_INFORMATION,
	           "%s model along %c on %zu nodes from %.12g to %.12g\n",
	           lattice_changed ? "Resampled" : "Processed",
	           cube->dim == 1 ? 'x' : 'z', target_nz,
	           cube->coordinate[axis][0], cube->coordinate[axis][target_nz - 1]);
	status = GMT_NOERROR;
cleanup:
	if (resampled) {
		for (field = 0; field < cube->n_fields; field++) free(resampled[field]);
	}
	free(resampled);
	free(target_z);
	free(trace);
	free(sampled);
	free(bridge_x);
	free(bridge_value);
	return status;
}

static bool ssh_is_geographic(const struct SSH_NC *cube)
{
	bool names = cube->coordinate_name[0] && cube->coordinate_name[1] &&
	             (!strcasecmp(cube->coordinate_name[0], "lon") ||
	              !strcasecmp(cube->coordinate_name[0], "longitude")) &&
	             (!strcasecmp(cube->coordinate_name[1], "lat") ||
	              !strcasecmp(cube->coordinate_name[1], "latitude"));
	bool units = cube->transform.axis_unit[0] && cube->transform.axis_unit[1] &&
	             strstr(cube->transform.axis_unit[0], "degree") &&
	             strstr(cube->transform.axis_unit[1], "degree");
	return names || units;
}

static const char *ssh_gap_method_name(char method)
{
	switch (method) {
		case 'n': return "nearest neighbor";
		case 'l': return "linear Delaunay";
		case 'a': return "local weighted average";
		case 's': return "spline";
		case 'm': return "minimum curvature";
		default: return "unknown";
	}
}

static int ssh_write_xyz(struct GMT_CTRL *GMT, struct GMT_GRID *Grid,
                         const char *path)
{
	FILE *fp = fopen(path, "w");
	size_t row, col;
	if (!fp) return GMT_DATA_WRITE_ERROR;
	for (row = 0; row < Grid->header->n_rows; row++) {
		double y = gmt_M_grd_row_to_y(GMT, row, Grid->header);
		for (col = 0; col < Grid->header->n_columns; col++) {
			gmt_grdfloat value = Grid->data[gmt_M_ijp(Grid->header, row, col)];
			double x;
			if (!isfinite(value)) continue;
			x = gmt_M_grd_col_to_x(GMT, col, Grid->header);
			if (fprintf(fp, "%.17g %.17g %.9g\n", x, y, (double)value) < 0) {
				fclose(fp);
				return GMT_DATA_WRITE_ERROR;
			}
		}
	}
	return fclose(fp) ? GMT_DATA_WRITE_ERROR : GMT_NOERROR;
}

static int ssh_fill_horizontal_layer(struct GMT_CTRL *GMT,
                                     const struct SSH_CTRL *Ctrl,
                                     const struct SSH_NC *cube, size_t iz,
                                     double *values, size_t *hole_count,
                                     size_t *node_count)
{
	struct GMT_GRID *Grid = NULL, *Candidate = NULL;
	uint8_t *visited = NULL, *eligible = NULL;
	uint64_t *queue = NULL;
	char input[PATH_MAX] = {""}, candidate[PATH_MAX] = {""};
	char xyz[PATH_MAX] = {""}, command[4 * PATH_MAX + GMT_LEN512] = {""};
	static char *V_level = GMT_VERBOSE_CODES;
	double wesn[4], inc[2];
	size_t nx = cube->n[0], ny = cube->n[1], nxy, iy, ix;
	size_t eligible_holes = 0, eligible_nodes = 0, filled_nodes = 0;
	int status = GMT_RUNTIME_ERROR;
	if (ny && nx > SIZE_MAX / ny) return GMT_MEMORY_ERROR;
	nxy = nx * ny;
	visited = calloc(nxy, sizeof(*visited));
	eligible = calloc(nxy, sizeof(*eligible));
	queue = calloc(nxy, sizeof(*queue));
	if (!visited || !eligible || !queue) {
		status = GMT_MEMORY_ERROR;
		goto cleanup;
	}
	for (iy = 0; iy < ny; iy++) for (ix = 0; ix < nx; ix++) {
		size_t node = iy * nx + ix, head = 0, tail = 0, q;
		size_t min_y = iy, max_y = iy, min_x = ix, max_x = ix;
		bool boundary = false, accepted;
		if (visited[node] || isfinite(values[iz * nxy + node])) continue;
		visited[node] = 1;
		queue[tail++] = node;
		while (head < tail) {
			size_t current = queue[head++], cy = current / nx, cx = current % nx;
			int dy, dx;
			if (cy == 0 || cy + 1 == ny || cx == 0 || cx + 1 == nx) boundary = true;
			if (cy < min_y) min_y = cy;
			if (cy > max_y) max_y = cy;
			if (cx < min_x) min_x = cx;
			if (cx > max_x) max_x = cx;
			for (dy = -1; dy <= 1; dy++) for (dx = -1; dx <= 1; dx++) {
				long next_y, next_x;
				size_t next;
				if (!dx && !dy) continue;
				next_y = (long)cy + dy;
				next_x = (long)cx + dx;
				if (next_y < 0 || next_x < 0 || next_y >= (long)ny ||
				    next_x >= (long)nx) continue;
				next = (size_t)next_y * nx + (size_t)next_x;
				if (visited[next] || isfinite(values[iz * nxy + next])) continue;
				visited[next] = 1;
				queue[tail++] = next;
			}
		}
		accepted = !boundary && (!Ctrl->H.limited ||
		           (max_x - min_x + 1 <= Ctrl->H.max_gap &&
		            max_y - min_y + 1 <= Ctrl->H.max_gap));
		if (!accepted) continue;
		eligible_holes++;
		eligible_nodes += tail;
		for (q = 0; q < tail; q++) eligible[queue[q]] = 1;
	}
	if (!eligible_nodes) {
		status = GMT_NOERROR;
		goto cleanup;
	}
	wesn[XLO] = cube->coordinate[0][0];
	wesn[XHI] = cube->coordinate[0][nx - 1];
	wesn[YLO] = cube->coordinate[1][0];
	wesn[YHI] = cube->coordinate[1][ny - 1];
	if (!ssh_regular_coordinate(cube->coordinate[0], nx, &inc[0]) ||
	    !ssh_regular_coordinate(cube->coordinate[1], ny, &inc[1])) {
		status = GMT_RUNTIME_ERROR;
		goto cleanup;
	}
	Grid = GMT_Create_Data(GMT->parent, GMT_IS_GRID, GMT_IS_SURFACE,
	                       GMT_CONTAINER_AND_DATA, NULL, wesn, inc,
	                       GMT_GRID_NODE_REG, GMT_NOTSET, NULL);
	if (!Grid) {
		status = GMT_MEMORY_ERROR;
		goto cleanup;
	}
	for (iy = 0; iy < ny; iy++) {
		size_t row = ny - 1 - iy;
		for (ix = 0; ix < nx; ix++)
			Grid->data[gmt_M_ijp(Grid->header, row, ix)] =
			    (gmt_grdfloat)values[iz * nxy + iy * nx + ix];
	}
	if (gmt_get_tempname(GMT->parent, "ssh_gap_input", ".nc", input) ||
	    gmt_get_tempname(GMT->parent, "ssh_gap_candidate", ".nc", candidate) ||
	    GMT_Write_Data(GMT->parent, GMT_IS_GRID, GMT_IS_FILE, GMT_IS_SURFACE,
	                   GMT_CONTAINER_AND_DATA, NULL, input, Grid) != GMT_NOERROR) {
		status = GMT_DATA_WRITE_ERROR;
		goto cleanup;
	}
	if (Ctrl->H.method == 'n') {
		if (Ctrl->H.argument > 0.0)
			snprintf(command, sizeof(command),
			         "%s -An%.12g -G%s -V%c --GMT_HISTORY=readonly", input,
			         Ctrl->H.argument, candidate,
			         V_level[GMT->current.setting.verbose]);
		else
			snprintf(command, sizeof(command),
			         "%s -An -G%s -V%c --GMT_HISTORY=readonly", input, candidate,
			         V_level[GMT->current.setting.verbose]);
		status = GMT_Call_Module(GMT->parent, "grdfill", GMT_MODULE_CMD, command);
	}
	else if (Ctrl->H.method == 's') {
		snprintf(command, sizeof(command),
		         "%s -As%.12g -G%s -V%c --GMT_HISTORY=readonly", input,
		         Ctrl->H.argument, candidate,
		         V_level[GMT->current.setting.verbose]);
		status = GMT_Call_Module(GMT->parent, "grdfill", GMT_MODULE_CMD, command);
	}
	else {
		const char *registration = Grid->header->registration ? "-rp" : "-rg";
		const char *geographic = ssh_is_geographic(cube) ? "-fg" : "";
		double radius = Ctrl->H.argument * MAX(inc[0], inc[1]);
		unsigned int minimum_sectors = MAX(1U, (Ctrl->H.sectors + 1U) / 2U);
		if (gmt_get_tempname(GMT->parent, "ssh_gap_points", ".txt", xyz) ||
		    ssh_write_xyz(GMT, Grid, xyz) != GMT_NOERROR) {
			status = GMT_DATA_WRITE_ERROR;
			goto cleanup;
		}
		if (Ctrl->H.method == 'l')
			snprintf(command, sizeof(command),
			         "%s -R%.17g/%.17g/%.17g/%.17g -I%.17g/%.17g %s %s "
			         "-Z -G%s -V%c --GMT_HISTORY=readonly", xyz, wesn[XLO],
			         wesn[XHI], wesn[YLO], wesn[YHI], inc[0], inc[1],
			         registration, geographic, candidate,
			         V_level[GMT->current.setting.verbose]);
		else if (Ctrl->H.method == 'a')
			snprintf(command, sizeof(command),
			         "%s -R%.17g/%.17g/%.17g/%.17g -I%.17g/%.17g "
			         "-S%.17g%s -N%u+m%u %s %s -G%s -V%c --GMT_HISTORY=readonly",
			         xyz, wesn[XLO], wesn[XHI], wesn[YLO], wesn[YHI], inc[0],
			         inc[1], radius, geographic[0] ? "d" : "", Ctrl->H.sectors,
			         minimum_sectors, registration, geographic, candidate,
			         V_level[GMT->current.setting.verbose]);
		else
			snprintf(command, sizeof(command),
			         "%s -R%.17g/%.17g/%.17g/%.17g -I%.17g/%.17g -T%.17g "
			         "%s %s %s -G%s -V%c --GMT_HISTORY=readonly", xyz, wesn[XLO],
			         wesn[XHI], wesn[YLO], wesn[YHI], inc[0], inc[1],
			         Ctrl->H.argument, registration, geographic,
			         geographic[0] ? "-Am" : "", candidate,
			         V_level[GMT->current.setting.verbose]);
		status = GMT_Call_Module(GMT->parent,
		                         Ctrl->H.method == 'l' ? "triangulate" :
		                         Ctrl->H.method == 'a' ? "nearneighbor" : "surface",
		                         GMT_MODULE_CMD, command);
	}
	if (status != GMT_NOERROR) goto cleanup;
	Candidate = GMT_Read_Data(GMT->parent, GMT_IS_GRID, GMT_IS_FILE,
	                          GMT_IS_SURFACE, GMT_CONTAINER_AND_DATA,
	                          NULL, candidate, NULL);
	if (!Candidate || Candidate->header->n_columns != nx ||
	    Candidate->header->n_rows != ny) {
		status = GMT_DATA_READ_ERROR;
		goto cleanup;
	}
	for (iy = 0; iy < ny; iy++) {
		size_t row = ny - 1 - iy;
		for (ix = 0; ix < nx; ix++) {
			size_t node = iy * nx + ix;
			gmt_grdfloat value;
			if (!eligible[node]) continue;
			value = Candidate->data[gmt_M_ijp(Candidate->header, row, ix)];
			if (!isfinite(value)) continue;
			values[iz * nxy + node] = value;
			filled_nodes++;
		}
	}
	*hole_count += eligible_holes;
	*node_count += filled_nodes;
	status = GMT_NOERROR;
cleanup:
	if (Candidate) GMT_Destroy_Data(GMT->parent, &Candidate);
	if (Grid) GMT_Destroy_Data(GMT->parent, &Grid);
	free(visited);
	free(eligible);
	free(queue);
	if (input[0]) gmt_remove_file(GMT, input);
	if (candidate[0]) gmt_remove_file(GMT, candidate);
	if (xyz[0]) gmt_remove_file(GMT, xyz);
	return status;
}

static int ssh_fill_horizontal_gaps(struct GMT_CTRL *GMT,
                                    const struct SSH_CTRL *Ctrl,
                                    const struct SSH_NC *cube, double **values)
{
	size_t field;
	if (!Ctrl->H.active) return GMT_NOERROR;
	for (field = 0; field < cube->n_fields; field++) {
		size_t iz, holes = 0, nodes = 0;
		for (iz = 0; iz < cube->n[2]; iz++) {
			int status = ssh_fill_horizontal_layer(GMT, Ctrl, cube, iz,
			                                       values[field], &holes, &nodes);
			if (status != GMT_NOERROR) {
				GMT_Report(GMT->parent, GMT_MSG_ERROR,
				           "Unable to fill horizontal gaps in %s?%s at layer %zu "
				           "using %s interpolation\n", cube->path,
				           cube->field[field].name, iz,
				           ssh_gap_method_name(Ctrl->H.method));
				return status;
			}
		}
		GMT_Report(GMT->parent, GMT_MSG_INFORMATION,
		           "Filled %zu nodes in %zu internal horizontal gap%s in %s?%s "
		           "using %s interpolation\n", nodes, holes, holes == 1 ? "" : "s",
		           cube->path, cube->field[field].name,
		           ssh_gap_method_name(Ctrl->H.method));
	}
	return GMT_NOERROR;
}

static bool ssh_is_netcdf(struct GMTAPI_CTRL *API, const char *source)
{
	char path[PATH_MAX], *modifier, *resolved = NULL;
	int ncid;
	bool result = false;
	if (!source || strlen(source) >= sizeof(path)) return false;
	strcpy(path, source);
	modifier = path;
	while ((modifier = strchr(modifier, '+')) != NULL) {
		if ((modifier == path || (modifier[-1] != 'e' && modifier[-1] != 'E')) &&
		    strchr("nxyzXYZvV", modifier[1]))
			break;
		modifier++;
	}
	if (modifier) *modifier = '\0';
	if (gq_resolve_remote_path(API, GMT_IS_DATASET, path, &resolved))
		return false;
	if (nc_open(resolved, NC_NOWRITE, &ncid) == NC_NOERR) {
		nc_close(ncid);
		result = true;
	}
	free(resolved);
	return result;
}

static bool ssh_output_is_netcdf(const char *path)
{
	const char *dot;
	if (!path || !strcmp(path, "-")) return false;
	dot = strrchr(path, '.');
	return dot && (!strcasecmp(dot, ".nc") || !strcasecmp(dot, ".nc4") ||
	               !strcasecmp(dot, ".cdf"));
}

static void ssh_text_free(struct SSH_TEXT *table)
{
	size_t field;
	for (field = 0; field < table->n_fields; field++) free(table->value[field]);
	free(table->value);
	free(table->coordinate);
	memset(table, 0, sizeof(*table));
}

static int ssh_text_source_parse(struct GMTAPI_CTRL *API, const char *text,
                                 struct SSH_TEXT_SOURCE *source)
{
	char *copy = strdup(text), *modifier;
	char message[GMT_LEN256];
	if (!copy) return GMT_MEMORY_ERROR;
	memset(source, 0, sizeof(*source));
	gq_transform_init(&source->transform);
	modifier = copy;
	while ((modifier = strchr(modifier, '+')) != NULL) {
		if ((modifier == copy || (modifier[-1] != 'e' && modifier[-1] != 'E')) &&
		    strchr("nxXvV", modifier[1]))
			break;
		modifier++;
	}
	if (modifier) *modifier++ = '\0';
	if (modifier && gq_transform_parse(
	                    modifier, GQ_TRANSFORM_X_MASK, true,
	                    &source->transform, &source->has_sentinel,
	                    &source->sentinel, message, sizeof(message))) {
		GMT_Report(API, GMT_MSG_ERROR, "Invalid text source %s: %s\n", text, message);
		free(copy);
		gq_transform_free(&source->transform);
		return GMT_PARSE_ERROR;
	}
	source->path = strdup(copy);
	free(copy);
	return source->path ? GMT_NOERROR : GMT_MEMORY_ERROR;
}

static void ssh_text_source_free(struct SSH_TEXT_SOURCE *source)
{
	free(source->path);
	gq_transform_free(&source->transform);
	memset(source, 0, sizeof(*source));
}

static int ssh_text_read(struct GMTAPI_CTRL *API, const char *source_text,
                         struct SSH_TEXT *table,
                         struct GQ_TRANSFORM *input_transform)
{
	struct SSH_TEXT_SOURCE source;
	char *resolved = NULL;
	FILE *fp;
	char line[GMT_BUFSIZ];
	size_t capacity = 0, line_number = 0;
	int status = GMT_DATA_READ_ERROR;
	memset(&source, 0, sizeof(source));
	if (ssh_text_source_parse(API, source_text, &source))
		return GMT_PARSE_ERROR;
	if (strcmp(source.path, "-") &&
	    gq_resolve_remote_path(API, GMT_IS_DATASET, source.path, &resolved)) {
		ssh_text_source_free(&source);
		return GMT_DATA_READ_ERROR;
	}
	fp = !strcmp(source.path, "-") ? stdin : fopen(resolved, "r");
	if (!fp) {
		GMT_Report(API, GMT_MSG_ERROR, "Unable to open %s\n", source.path);
		free(resolved);
		ssh_text_source_free(&source);
		return GMT_DATA_READ_ERROR;
	}
	memset(table, 0, sizeof(*table));
	while (fgets(line, sizeof(line), fp)) {
		char *p = line, *end;
		double row[GMT_MAX_COLUMNS];
		size_t columns = 0, field;
		line_number++;
		while (isspace((unsigned char)*p)) p++;
		if (!*p || *p == '#') continue;
		while (*p && *p != '#') {
			if (columns >= GMT_MAX_COLUMNS) goto cleanup;
			errno = 0;
			row[columns] = strtod(p, &end);
			if (errno || end == p) goto cleanup;
			columns++;
			p = end;
			while (isspace((unsigned char)*p)) p++;
		}
		if (columns < 2) goto cleanup;
		if (!table->n_fields) {
			char message[GMT_LEN256];
			table->n_fields = columns - 1;
			if (gq_transform_validate_values(&source.transform, table->n_fields,
			                                  message, sizeof(message))) {
				GMT_Report(API, GMT_MSG_ERROR, "%s: %s\n", source_text, message);
				goto cleanup;
			}
			table->value = calloc(table->n_fields, sizeof(*table->value));
			if (!table->value) {
				status = GMT_MEMORY_ERROR;
				goto cleanup;
			}
		}
		if (columns != table->n_fields + 1) {
			GMT_Report(API, GMT_MSG_ERROR,
			           "%s:%zu has an inconsistent column count\n",
			           source.path, line_number);
			goto cleanup;
		}
		if (table->n == capacity) {
			size_t next_capacity = capacity ? capacity * 2 : 256;
			double *next_coordinate =
			    realloc(table->coordinate, next_capacity * sizeof(double));
			if (!next_coordinate) {
				status = GMT_MEMORY_ERROR;
				goto cleanup;
			}
			table->coordinate = next_coordinate;
			for (field = 0; field < table->n_fields; field++) {
				double *next = realloc(table->value[field],
				                       next_capacity * sizeof(double));
				if (!next) {
					status = GMT_MEMORY_ERROR;
					goto cleanup;
				}
				table->value[field] = next;
			}
			capacity = next_capacity;
		}
		table->coordinate[table->n] = row[0] * source.transform.axis_scale[0];
		for (field = 0; field < table->n_fields; field++)
			table->value[field][table->n] =
			    (isnan(row[field + 1]) ||
			     (source.has_sentinel && row[field + 1] == source.sentinel) ||
			     (API->GMT->common.d.active[GMT_IN] &&
			      row[field + 1] == API->GMT->common.d.nan_proxy[GMT_IN]))
			        ? NAN
			        : row[field + 1] *
			          gq_transform_value_scale(&source.transform, field);
		table->n++;
	}
	if (table->n < 2) goto cleanup;
	for (size_t k = 1; k < table->n; k++)
		if (!isfinite(table->coordinate[k]) ||
		    table->coordinate[k] <= table->coordinate[k - 1])
			goto cleanup;
	status = GMT_NOERROR;
cleanup:
	if (fp != stdin) fclose(fp);
	free(resolved);
	if (status == GMT_NOERROR) {
		*input_transform = source.transform;
		gq_transform_init(&source.transform);
	}
	ssh_text_source_free(&source);
	if (status != GMT_NOERROR) {
		if (status != GMT_MEMORY_ERROR)
			GMT_Report(API, GMT_MSG_ERROR,
			           "Unable to read a text table whose transformed coordinate "
			           "is regular and increasing: %s\n", source_text);
		ssh_text_free(table);
	}
	return status;
}

static int ssh_text_write(struct GMTAPI_CTRL *API, const char *path,
                          const struct SSH_TEXT *table, const double *weight,
                          const struct GQ_TRANSFORM *output_transform)
{
	FILE *fp = !strcmp(path, "-") ? stdout : fopen(path, "w");
	size_t row, field;
	if (!fp) return GMT_DATA_WRITE_ERROR;
	for (row = 0; row < table->n; row++) {
		fprintf(fp, "%.17g", table->coordinate[row] *
		        output_transform->axis_scale[0]);
		for (field = 0; field < table->n_fields; field++)
			fprintf(fp, "\t%.17g", table->value[field][row] *
			        gq_transform_value_scale(output_transform, field));
		if (weight) fprintf(fp, "\t%.17g", weight[row]);
		fputc('\n', fp);
	}
	if (fp != stdout && fclose(fp)) {
		GMT_Report(API, GMT_MSG_ERROR, "Unable to close %s\n", path);
		return GMT_DATA_WRITE_ERROR;
	}
	return GMT_NOERROR;
}

static int ssh_execute_text(struct GMT_CTRL *GMT, const struct SSH_CTRL *Ctrl)
{
	struct SSH_TEXT table;
	struct SSH_NC series;
	struct GQ_TRANSFORM input_transform;
	struct SSH_STAT *stat = NULL;
	char **keys = NULL, **names = NULL;
	double **heterogeneity = NULL, *weight = NULL;
	double max_correlation[SSH_MAX_DIM] = {0.0, 0.0, 0.0};
	double origin[SSH_MAX_DIM] = {0.0, 0.0, 0.0};
	double increment[SSH_MAX_DIM] = {1.0, 1.0, 1.0};
	size_t n[SSH_MAX_DIM] = {1, 1, 1}, field, row;
	int status;
	memset(&table, 0, sizeof(table));
	memset(&series, 0, sizeof(series));
	series.ncid = -1;
	gq_transform_init(&input_transform);
	status = ssh_text_read(GMT->parent, Ctrl->In.file[0], &table,
	                       &input_transform);
	if (status != GMT_NOERROR) goto cleanup;
	if (Ctrl->F.n != table.n_fields) {
		GMT_Report(GMT->parent, GMT_MSG_ERROR,
		           "Option -F must name all %zu text data columns\n",
		           table.n_fields);
		status = GMT_PARSE_ERROR;
		goto cleanup;
	}
	status = ssh_field_setup(GMT->parent, Ctrl, 1, &keys, &names,
	                         &field, &stat, max_correlation);
	if (status != GMT_NOERROR) goto cleanup;
	if (field != table.n_fields) {
		status = GMT_PARSE_ERROR;
		goto cleanup;
	}
	series.dim = 1;
	series.n_fields = table.n_fields;
	series.n[0] = table.n;
	series.n[1] = series.n[2] = 1;
	series.coordinate[0] = table.coordinate;
	series.increment[1] = series.increment[2] = 1.0;
	if (!ssh_regular_coordinate(series.coordinate[0], series.n[0],
	                            &series.increment[0])) {
		GMT_Report(GMT->parent, GMT_MSG_ERROR,
		           "ssh1d application requires a regular increasing transformed axis\n");
		status = GMT_PARSE_ERROR;
		goto cleanup;
	}
	status = ssh_resample_vertical(GMT, Ctrl, &series, &table.value);
	if (status != GMT_NOERROR) goto cleanup;
	table.coordinate = series.coordinate[0];
	table.n = series.n[0];
	n[0] = table.n;
	origin[0] = table.coordinate[0];
	increment[0] = series.increment[0];
	status = ssh_generate_fields(GMT, Ctrl, 1, n, origin, increment, keys,
	                             table.n_fields, stat, max_correlation,
	                             &heterogeneity, &weight);
	if (status != GMT_NOERROR) goto cleanup;
	for (field = 0; field < table.n_fields; field++)
		for (row = 0; row < table.n; row++)
			if (isfinite(table.value[field][row]))
				table.value[field][row] *= 1.0 + heterogeneity[field][row];
	status = ssh_text_write(GMT->parent, Ctrl->G.file, &table, weight,
	                        &Ctrl->Z.transform);
cleanup:
	series.coordinate[0] = NULL;
	ssh_text_free(&table);
	gq_transform_free(&input_transform);
	ssh_field_setup_free(keys, names, Ctrl->F.n, stat);
	ssh_free_values(heterogeneity, Ctrl->F.n);
	free(weight);
	return status;
}

int ssh_execute(struct GMT_CTRL *GMT, const struct SSH_CTRL *Ctrl,
                unsigned int dim)
{
	struct SSH_NC cube;
	struct SSH_STAT *stat = NULL;
	char **keys = NULL, **output_names = NULL;
	double **heterogeneity = NULL, **output = NULL, *weight = NULL;
	double *coordinate[SSH_MAX_DIM] = {NULL, NULL, NULL};
	double max_correlation[SSH_MAX_DIM] = {0.0, 0.0, 0.0};
	double origin[SSH_MAX_DIM] = {0.0, 0.0, 0.0};
	double increment[SSH_MAX_DIM] = {1.0, 1.0, 1.0};
	size_t n[SSH_MAX_DIM] = {1, 1, 1}, n_fields = 0, field, k, total;
	int status = GMT_RUNTIME_ERROR;
	memset(&cube, 0, sizeof(cube));
	cube.ncid = -1;
	if (dim == 1 && Ctrl->A.active &&
	    !ssh_is_netcdf(GMT->parent, Ctrl->In.file[0]))
		return ssh_execute_text(GMT, Ctrl);
	status = ssh_field_setup(GMT->parent, Ctrl, dim, &keys, &output_names,
	                         &n_fields, &stat, max_correlation);
	if (status != GMT_NOERROR) goto cleanup;
	{
		char message[GMT_LEN256];
		if (gq_transform_validate_values(&Ctrl->Z.transform, n_fields,
		                                  message, sizeof(message))) {
			GMT_Report(GMT->parent, GMT_MSG_ERROR, "Option -Z: %s\n", message);
			status = GMT_PARSE_ERROR;
			goto cleanup;
		}
	}
	if (Ctrl->A.active) {
		status = ssh_nc_open(GMT->parent, Ctrl->In.file[0], dim,
		                     Ctrl->F.name, Ctrl->F.n, &cube);
		if (status != GMT_NOERROR) goto cleanup;
		output = calloc(n_fields, sizeof(*output));
		if (!output) {
			status = GMT_MEMORY_ERROR;
			goto cleanup;
		}
		for (field = 0; field < n_fields; field++) {
			status = ssh_nc_read_field(GMT->parent, &cube, field, &output[field]);
			if (status != GMT_NOERROR) goto cleanup;
		}
		status = ssh_fill_horizontal_gaps(GMT, Ctrl, &cube, output);
		if (status != GMT_NOERROR) goto cleanup;
		status = ssh_resample_horizontal(GMT, Ctrl, &cube, &output);
		if (status != GMT_NOERROR) goto cleanup;
		status = ssh_resample_vertical(GMT, Ctrl, &cube, &output);
		if (status != GMT_NOERROR) goto cleanup;
		for (unsigned int axis = 0; axis < dim; axis++) {
			n[axis] = cube.n[axis];
			origin[axis] = cube.coordinate[axis][0];
			increment[axis] = cube.increment[axis];
		}
		for (unsigned int axis = dim; axis < SSH_MAX_DIM; axis++) n[axis] = 1;
	}
	else {
		status = ssh_output_geometry(GMT, Ctrl, dim, n, origin,
		                             increment, coordinate);
		if (status != GMT_NOERROR) goto cleanup;
	}
	total = n[0] * n[1] * n[2];
	status = ssh_generate_fields(GMT, Ctrl, dim, n, origin, increment, keys,
	                             n_fields, stat, max_correlation,
	                             &heterogeneity, &weight);
	if (status != GMT_NOERROR) goto cleanup;
	if (Ctrl->A.active) {
		for (field = 0; field < n_fields; field++) {
			size_t nonpositive = 0;
			for (k = 0; k < total; k++)
				if (isfinite(output[field][k])) {
					output[field][k] *= 1.0 + heterogeneity[field][k];
					if (output[field][k] <= 0.0) nonpositive++;
				}
			if (nonpositive)
				GMT_Report(GMT->parent, GMT_MSG_WARNING,
				           "%s contains %zu non-positive perturbed values\n",
				           keys[field], nonpositive);
		}
		status = ssh_nc_write_application(GMT->parent, Ctrl->G.file,
		                                  &cube, output, weight,
		                                  &Ctrl->Z.transform);
	}
	else
		if (dim == 1 && !ssh_output_is_netcdf(Ctrl->G.file)) {
			struct SSH_TEXT table;
			memset(&table, 0, sizeof(table));
			table.n = n[0];
			table.n_fields = n_fields;
			table.coordinate = coordinate[0];
			table.value = heterogeneity;
			status = ssh_text_write(GMT->parent, Ctrl->G.file, &table, weight,
			                        &Ctrl->Z.transform);
			table.coordinate = NULL;
			table.value = NULL;
		}
		else
			status = ssh_nc_write_synthetic(GMT->parent, Ctrl->G.file, dim, n,
			                                (const double **)coordinate,
			                                output_names, n_fields,
			                                heterogeneity, weight,
			                                &Ctrl->Z.transform);
cleanup:
	for (unsigned int axis = 0; axis < SSH_MAX_DIM; axis++)
		free(coordinate[axis]);
	ssh_nc_free(&cube);
	ssh_field_setup_free(keys, output_names, n_fields, stat);
	ssh_free_values(heterogeneity, n_fields);
	ssh_free_values(output, n_fields);
	free(weight);
	return status;
}
