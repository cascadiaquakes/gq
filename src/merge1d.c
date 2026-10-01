/*--------------------------------------------------------------------
 *
 *	Copyright (c) 2024-2026 by the CRESCENT cyberinfrastructure team (https://cascadiaquakes.org/)
 *	See LICENSE for copying and redistribution conditions.
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU Lesser General Public License as published by
 *	the Free Software Foundation; version 3 or any later version.
 *
 *	This program is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *	GNU Lesser General Public License for more details.
 *
 *	Contact info: abioyeajala@gmail.com (Rasheed Ajala)
 *--------------------------------------------------------------------*/
/*
 * merge1d combines 1-D data series on a regular output axis.
 * GMT supplies interpolation and table I/O, while BLEND supplies support
 * weights. Text tables and multiparameter NetCDF files are supported.
 */

#include "gmt_dev.h"
#include "gq_remote.h"
#include "gq_transform.h"
#include "merge1d_inc.h"

#include <blend/blend.h>
#include <netcdf.h>

#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <limits.h>

#define THIS_MODULE_CLASSIC_NAME "merge1d"
#define THIS_MODULE_MODERN_NAME "merge1d"
#define THIS_MODULE_LIB "gq"
#define THIS_MODULE_PURPOSE "Tile or smoothly merge one-dimensional tables and multiparameter NetCDF series"
#define THIS_MODULE_KEYS "<D{"
#define THIS_MODULE_NEEDS ""
#define THIS_MODULE_OPTIONS "Vdfh"

enum MERGE1D_FORMAT {
	MERGE1D_FORMAT_UNKNOWN = 0,
	MERGE1D_FORMAT_TEXT,
	MERGE1D_FORMAT_NETCDF
};

enum MERGE1D_CLOBBER {
	MERGE1D_UPPER = 0,
	MERGE1D_LOWER,
	MERGE1D_FIRST,
	MERGE1D_LAST
};

struct MERGE1D_CTRL {
	struct {
		char **file;
		size_t n;
	} In;
	struct {
		bool active;
	} A;
	struct {
		bool active;
		unsigned int mode;
		int sign;
	} C;
	struct {
		bool active;
		char *fields;
	} F;
	struct {
		bool active;
		char *file;
	} G;
	struct {
		bool active;
	} P;
	struct {
		bool active;
		bool bridge;
		unsigned int mode;
		double fit;
		double max_gap;
	} S;
	struct {
		bool active;
		double min;
		double max;
		double inc;
		size_t n;
	} T;
	struct {
		bool active;
		bool only;
	} W;
	struct {
		bool active;
		struct GQ_TRANSFORM transform;
	} Z;
};

struct MERGE1D_SERIES {
	char *source;
	char *path;
	enum MERGE1D_FORMAT format;
	bool explicit_selector;
	bool has_sentinel;
	double sentinel;
	size_t n;
	size_t n_fields;
	double *x;
	double **value;
	double **sampled;
	char **field_name;
	char **units;
	char *coordinate_name;
	char *coordinate_units;
	struct GQ_TRANSFORM transform;
};

struct MERGE1D_SPEC {
	char *primary_source;
	char *secondary_source;
	bool has_secondary;
	bool has_interval;
	double west;
	double east;
	double ratio1;
	double ratio2;
	blend_window_function function;
	int nx;
	struct MERGE1D_SERIES primary;
	struct MERGE1D_SERIES secondary;
};

struct MERGE1D_JOB {
	struct MERGE1D_SPEC *spec;
	size_t count;
	bool mergefile;
	enum MERGE1D_FORMAT format;
	size_t n_fields;
	char **output_fields;
	char *coordinate_name;
	char *coordinate_units;
};

static void merge1d_series_free(struct MERGE1D_SERIES *series)
{
	size_t k;

	if (series == NULL) return;
	free(series->source);
	free(series->path);
	free(series->x);
	for (k = 0; k < series->n_fields; k++) {
		free(series->value ? series->value[k] : NULL);
		free(series->sampled ? series->sampled[k] : NULL);
		free(series->field_name ? series->field_name[k] : NULL);
		free(series->units ? series->units[k] : NULL);
	}
	free(series->value);
	free(series->sampled);
	free(series->field_name);
	free(series->units);
	free(series->coordinate_name);
	free(series->coordinate_units);
	gq_transform_free(&series->transform);
	memset(series, 0, sizeof(*series));
}

static void merge1d_job_free(struct MERGE1D_JOB *job)
{
	size_t k;

	if (job == NULL) return;
	for (k = 0; k < job->count; k++) {
		free(job->spec[k].primary_source);
		free(job->spec[k].secondary_source);
		merge1d_series_free(&job->spec[k].primary);
		merge1d_series_free(&job->spec[k].secondary);
	}
	free(job->spec);
	for (k = 0; k < job->n_fields; k++) free(job->output_fields ? job->output_fields[k] : NULL);
	free(job->output_fields);
	free(job->coordinate_name);
	free(job->coordinate_units);
	memset(job, 0, sizeof(*job));
}

static void *New_Ctrl(struct GMT_CTRL *GMT)
{
	struct MERGE1D_CTRL *Ctrl = gmt_M_memory(GMT, NULL, 1, struct MERGE1D_CTRL);

	Ctrl->C.mode = MERGE1D_FIRST;
	Ctrl->S.mode = GMT_SPLINE_LINEAR;
	gq_transform_init(&Ctrl->Z.transform);
	return Ctrl;
}

static void Free_Ctrl(struct GMT_CTRL *GMT, struct MERGE1D_CTRL *Ctrl)
{
	size_t k;

	if (Ctrl == NULL) return;
	for (k = 0; k < Ctrl->In.n; k++) free(Ctrl->In.file[k]);
	free(Ctrl->In.file);
	free(Ctrl->F.fields);
	free(Ctrl->G.file);
	gq_transform_free(&Ctrl->Z.transform);
	gmt_M_free(GMT, Ctrl);
}

static int usage(struct GMTAPI_CTRL *API, int level)
{
	const char *name = gmt_show_name_and_purpose(API, THIS_MODULE_LIB,
	                                            THIS_MODULE_CLASSIC_NAME,
	                                            THIS_MODULE_PURPOSE);

	if (level == GMT_MODULE_PURPOSE) return GMT_NOERROR;
	GMT_Usage(API, 0,
	          "usage: %s [<mergefile> | <input1> <input2> ...] "
	          "-T<min>/<max>/<inc> [-A] [-Cf|l|o|u[+n|p]] "
	          "[-F<fields>] [-G<output>] [-P] [-Sa|c|e|l|n|s<p>] "
	          "[-W[+o]] [-Z+x<scale>[+X<unit>]+v<scales>[+V<units>]] "
	          "[%s] [%s] [%s] [%s]\n",
	          name, GMT_V_OPT, GMT_di_OPT, GMT_f_OPT, GMT_h_OPT);
	if (level == GMT_SYNOPSIS) return GMT_MODULE_SYNOPSIS;

	GMT_Message(API, GMT_TIME_NONE, "  REQUIRED ARGUMENTS:\n");
	GMT_Usage(API, 1, "\n<mergefile> | <input1> <input2> ...");
	GMT_Usage(API, -2,
	          "Supply one mergefile or list one or more data inputs directly. "
	          "Direct inputs use clobber/tiling mode in availability order. "
	          "A run may use text or NetCDF inputs, but not both.");
	GMT_Usage(API, 3,
	          "Each non-comment mergefile record contains up to five "
	          "whitespace-separated fields:");
	GMT_Usage(API, 3,
	          "primary: Required source providing the primary values for the record.");
	GMT_Usage(API, 3,
	          "secondary: Optional source paired with primary for merging. Use '-' "
	          "for an unpaired fallback tile.");
	GMT_Usage(API, 3,
	          "west/east: Support interval for a paired primary. It must lie within "
	          "the primary coordinate domain. The default is the entire primary domain.");
	GMT_Usage(API, 3,
	          "window: BLEND window function applied within the support. The default "
	          "is cosine.");
	GMT_Usage(API, 3,
	          "r1/r2: Dimensionless beginning and ending taper ratios. Each must be "
	          "in [0, 0.5). One ratio applies symmetrically. The default is 0.2/0.2.");
	GMT_Usage(API, 3,
	          "A taper ratio sets the fraction of the support length used by the "
	          "transition at that boundary. r1 applies at west (low coordinate) and "
	          "r2 at east (high coordinate). For example, on support 20/80, 0/0.2 "
	          "disables the west taper and uses approximately the final 12 coordinate "
	          "units for the east taper.");
	GMT_Usage(API, 3,
	          "Within a taper, the selected window controls how the primary merging "
	          "weight changes between its boundary value and 1. The paired secondary "
	          "receives the complementary weight. A larger ratio gives a broader "
	          "transition and a smaller full-primary interior. A ratio of 0 disables "
	          "the taper on that side. The boxcar window ignores taper ratios and has "
	          "unit weight throughout the support.");
	GMT_Usage(API, 3,
	          "Use '-' to skip an optional field when supplying a later field. "
	          "Trailing optional fields may be omitted. Blank lines and text after "
	          "'#' are ignored.");
	GMT_Usage(API, 3, "Example mergefile:");
	GMT_Usage(API, 3,
	          "  primary.nc secondary.nc 20/80 cosine 0.25/0.25");
	GMT_Usage(API, 3, "  secondary.nc - - - -");
	GMT_Usage(API, 3,
	          "Text tables use the first column as the axis and remaining columns "
	          "as fields.");
	GMT_Usage(API, 3,
	          "For NetCDF, append ?field1,field2,... to select one or more data "
	          "variables, which must share one coordinate. If ? is omitted, the "
	          "first eligible one-dimensional data variable is used.");
	GMT_Usage(API, 3,
	          "Append selector modifiers after the field list. +x scales the input "
	          "axis, +v supplies one broadcast scale or one scale per selected "
	          "field, +X and +V set their target-unit metadata, and +n declares an "
	          "additional missing-value sentinel. Use "
	          "file.nc?<field1,field2,...>+<modifiers> for named fields or "
	          "file.nc?+<modifiers> to transform the default field without naming "
	          "it. These input transforms are applied before interpolation and "
	          "merging.");
	GMT_Usage(API, 3,
	          "Input +x scaling does not reorder coordinates or samples. The "
	          "transformed input axis must be strictly increasing. Use a negative "
	          "+x scale to make a decreasing source axis increase, then use output "
	          "-Z+x<scale> to restore a decreasing output convention if desired.");
	GMT_Usage(API, 3,
	          "Example: model.nc?vp,vs+x0.001+Xkm+v0.001,0.001+Vkm/s,km/s "
	          "selects vp and vs, scales the input axis from m to km, and scales "
	          "both fields from m/s to km/s.");
	GMT_Usage(API, 1, "\n-T<min>/<max>/<inc>");
	GMT_Usage(API, -2,
	          "Set the regular output coordinate range and increment.");

	GMT_Message(API, GMT_TIME_NONE, "\n  OPTIONAL ARGUMENTS:\n");
	GMT_Usage(API, 1, "\n-A");
	GMT_Usage(API, -2,
	          "Normalize positive weights where primary supports overlap. "
	          "Overlapping supports must use the same secondary source. "
	          "A later record for that secondary starts a lower-priority layer. "
	          "Unpaired records remain fallback tiles.");
	GMT_Usage(API, 1, "\n-Cf|l|o|u[+n|p]");
	GMT_Usage(API, -2, "Select clobber mode instead of blending:");
	GMT_Usage(API, 3,
	          "f: Keep the first value. This is the default for direct file lists.");
	GMT_Usage(API, 3, "l: Keep the lowest value.");
	GMT_Usage(API, 3, "o: Keep the last value.");
	GMT_Usage(API, 3, "u: Keep the highest value.");
	GMT_Usage(API, 3, "+n: Only consider non-positive values for clobbering.");
	GMT_Usage(API, 3, "+p: Only consider non-negative values for clobbering.");
	GMT_Usage(API, 1, "\n-F<field1,field2,...>");
	GMT_Usage(API, -2,
	          "Set NetCDF output variable names. -F does not select source "
	          "variables: names selected with ? map positionally to -F. Thus "
	          "model1.nc?vp,vs,den and model2.nc?p,s,d may be merged with "
	          "-Fvp,vs,rho when the differently named variables are equivalent.");
	GMT_Usage(API, 1, "\n-G<output>");
	GMT_Usage(API, -2,
	          "Write to output. A .nc suffix selects NetCDF. Otherwise a text "
	          "table is written. Text output defaults to standard output.");
	GMT_Usage(API, 1, "\n-P");
	GMT_Usage(API, -2,
	          "Fill primary values that remain missing after interpolation from "
	          "their paired secondary values. By default, primary NaNs "
	          "are preserved.");
	GMT_Usage(API, 1, "\n-Sa|c|e|l|n|s<p>[+g[<maxgap>]]");
	GMT_Usage(API, -2,
	          "Choose GMT 1-D interpolation: Akima (a), cubic (c), step-up (e), "
	          "linear (l), nearest (n), or smoothing spline (s<p>) with "
	          "non-negative fit parameter p. Linear (l) is the default. "
	          "Append +g to bridge internal missing runs, optionally only when "
	          "the bracketing coordinate distance does not exceed maxgap.");
	GMT_Option(API, "V");
	GMT_Usage(API, 1, "\n-W[+o]");
	GMT_Usage(API, -2,
	          "Include the shared merging weight. For text it is appended as "
	          "the final column. Append +o for coordinate and weight only.");
	GMT_Usage(API, 3, "Weights follow paired supports in mergefile order. The first support "
	          "containing the coordinate supplies its primary weight; outside it, later supports "
	          "remain visible. Zero-valued support boundaries are retained. With -A, sum and cap "
	          "weights at 1 only where positive weights overlap with the same secondary. "
	          "Unpaired background weights are 0. These shared taper weights do not represent "
	          "final per-source fractions or field-specific missing-value replacements.");
	GMT_Usage(API, 1, "\n-Z[+x<scale>][+X<unit>][+v<scales>][+V<units>]");
	GMT_Usage(API, -2,
	          "Transform output coordinates and fields after merging. Unlike "
	          "modifiers appended to an input selector, -Z applies only to the "
	          "completed output. +x scales the output axis and +v supplies one "
	          "broadcast scale or one scale per output field, in -F order when "
	          "-F is used and selector order otherwise. +X and +V set target-unit "
	          "metadata. -Z scales output coordinates and fields in place and does "
	          "not reorder coordinates, fields, or weights. A negative axis scale "
	          "therefore produces a decreasing output axis.");
	GMT_Usage(API, 3,
	          "Example: with -Fvp,vs, -Z+x-0.001+Xkm+v0.001,0.001"
	          "+Vkm/s,km/s converts a positive-down output axis from m to a "
	          "negative-down axis in km and converts vp and vs from m/s to km/s. "
	          "The output samples remain in their original order.");
	GMT_Option(API, "di,f,h,.");
	return GMT_MODULE_USAGE;
}

static int merge1d_parse_range(struct GMTAPI_CTRL *API, const char *text,
                               struct MERGE1D_CTRL *Ctrl)
{
	char copy[GMT_LEN256], *token = NULL, *save = NULL, *end = NULL;
	double values[3];
	size_t n = 0;
	double intervals, adjusted, tolerance;

	if (text == NULL || strlen(text) >= sizeof(copy)) return GMT_PARSE_ERROR;
	strcpy(copy, text);
	for (token = strtok_r(copy, "/", &save); token && n < 3;
	     token = strtok_r(NULL, "/", &save)) {
		errno = 0;
		values[n] = strtod(token, &end);
		if (errno || end == token || *end || !isfinite(values[n])) {
			GMT_Report(API, GMT_MSG_ERROR, "Option -T: Invalid range %s\n", text);
			return GMT_PARSE_ERROR;
		}
		n++;
	}
	if (n != 3 || strtok_r(NULL, "/", &save) != NULL ||
	    values[0] >= values[1] || values[2] <= 0.0) {
		GMT_Report(API, GMT_MSG_ERROR,
		           "Option -T must be min/max/inc with min < max and inc > 0\n");
		return GMT_PARSE_ERROR;
	}
	intervals = (values[1] - values[0]) / values[2];
	if (intervals > (double)(SIZE_MAX - 1)) return GMT_DIM_TOO_LARGE;
	Ctrl->T.n = (size_t)floor(intervals + 0.5) + 1;
	if (Ctrl->T.n < 2) return GMT_DIM_TOO_SMALL;
	adjusted = values[0] + (double)(Ctrl->T.n - 1) * values[2];
	tolerance = 32.0 * DBL_EPSILON * MAX(1.0, MAX(fabs(values[1]), fabs(adjusted)));
	if (fabs(adjusted - values[1]) > tolerance)
		GMT_Report(API, GMT_MSG_WARNING,
		           "Option -T: Adjusting maximum from %.12g to %.12g to fit increment\n",
		           values[1], adjusted);
	Ctrl->T.min = values[0];
	Ctrl->T.max = adjusted;
	Ctrl->T.inc = values[2];
	return GMT_NOERROR;
}

static int merge1d_parse_interpolation(struct GMTAPI_CTRL *API, const char *text,
                                       struct MERGE1D_CTRL *Ctrl)
{
	char copy[GMT_LEN128], *modifier = NULL, *end = NULL;

	if (text == NULL || !text[0] || strlen(text) >= sizeof(copy)) {
		GMT_Report(API, GMT_MSG_ERROR, "Option -S requires an interpolation mode\n");
		return GMT_PARSE_ERROR;
	}
	strcpy(copy, text);
	modifier = strchr(copy, '+');
	if (modifier) {
		*modifier++ = '\0';
		if (modifier[0] != 'g' || strchr(modifier + 1, '+')) goto bad;
		Ctrl->S.bridge = true;
		Ctrl->S.max_gap = INFINITY;
		if (modifier[1]) {
			errno = 0;
			Ctrl->S.max_gap = strtod(modifier + 1, &end);
			if (errno || end == modifier + 1 || *end ||
			    !isfinite(Ctrl->S.max_gap) || Ctrl->S.max_gap <= 0.0)
				goto bad;
		}
	}
	if (!copy[0]) goto bad;
	switch (copy[0]) {
		case 'a': Ctrl->S.mode = GMT_SPLINE_AKIMA; break;
		case 'c': Ctrl->S.mode = GMT_SPLINE_CUBIC; break;
		case 'e': Ctrl->S.mode = GMT_SPLINE_STEP; break;
		case 'l': Ctrl->S.mode = GMT_SPLINE_LINEAR; break;
		case 'n': Ctrl->S.mode = GMT_SPLINE_NN; break;
		case 's':
			if (!copy[1]) {
				GMT_Report(API, GMT_MSG_ERROR,
				           "Option -Ss requires a smoothing fit parameter\n");
				return GMT_PARSE_ERROR;
			}
			errno = 0;
			Ctrl->S.fit = strtod(copy + 1, &end);
			if (errno || end == copy + 1 || *end || !isfinite(Ctrl->S.fit) ||
			    Ctrl->S.fit < 0.0) {
				GMT_Report(API, GMT_MSG_ERROR,
				           "Option -Ss requires a non-negative fit parameter\n");
				return GMT_PARSE_ERROR;
			}
			Ctrl->S.mode = gmt_M_is_zero(Ctrl->S.fit)
			             ? GMT_SPLINE_CUBIC : GMT_SPLINE_SMOOTH;
			break;
		default: goto bad;
	}
	if (copy[0] != 's' && copy[1]) goto bad;
	return GMT_NOERROR;

bad:
	GMT_Report(API, GMT_MSG_ERROR,
	           "Option -S must be a, c, e, l, n, or s<p>, optionally followed "
	           "by +g[<positive maxgap>]\n");
	return GMT_PARSE_ERROR;
}

static int parse(struct GMT_CTRL *GMT, struct MERGE1D_CTRL *Ctrl,
                 struct GMT_OPTION *options)
{
	struct GMT_OPTION *opt;
	struct GMTAPI_CTRL *API = GMT->parent;
	unsigned int n_errors = 0;

	for (opt = options; opt; opt = opt->next) {
		switch (opt->option) {
			case '<': {
				char **next;
				next = realloc(Ctrl->In.file, (Ctrl->In.n + 1) * sizeof(*next));
				if (next == NULL) return GMT_MEMORY_ERROR;
				Ctrl->In.file = next;
				Ctrl->In.file[Ctrl->In.n++] = strdup(opt->arg);
				break;
			}
			case 'A':
				n_errors += gmt_M_repeated_module_option(API, Ctrl->A.active);
				n_errors += gmt_get_no_argument(GMT, opt->arg, opt->option, 0);
				break;
			case 'C':
				n_errors += gmt_M_repeated_module_option(API, Ctrl->C.active);
				switch (opt->arg[0]) {
					case 'f': Ctrl->C.mode = MERGE1D_FIRST; break;
					case 'l': Ctrl->C.mode = MERGE1D_LOWER; break;
					case 'o': Ctrl->C.mode = MERGE1D_LAST; break;
					case 'u': Ctrl->C.mode = MERGE1D_UPPER; break;
					default:
						GMT_Report(API, GMT_MSG_ERROR,
						           "Option -C: Mode must be f, l, o, or u\n");
						n_errors++;
				}
				if (!opt->arg[1]) Ctrl->C.sign = 0;
				else if (!strcmp(opt->arg + 1, "+n")) Ctrl->C.sign = -1;
				else if (!strcmp(opt->arg + 1, "+p")) Ctrl->C.sign = 1;
				else {
					GMT_Report(API, GMT_MSG_ERROR,
					           "Option -C: Modifiers are +n or +p\n");
					n_errors++;
				}
				break;
			case 'F':
				n_errors += gmt_M_repeated_module_option(API, Ctrl->F.active);
				if (!opt->arg[0]) {
					GMT_Report(API, GMT_MSG_ERROR,
					           "Option -F requires comma-separated field names\n");
					n_errors++;
				}
				else Ctrl->F.fields = strdup(opt->arg);
				break;
			case 'G':
				n_errors += gmt_M_repeated_module_option(API, Ctrl->G.active);
				if (!opt->arg[0]) {
					GMT_Report(API, GMT_MSG_ERROR, "Option -G requires a filename\n");
					n_errors++;
				}
				else Ctrl->G.file = strdup(opt->arg);
				break;
			case 'P':
				n_errors += gmt_M_repeated_module_option(API, Ctrl->P.active);
				n_errors += gmt_get_no_argument(GMT, opt->arg, opt->option, 0);
				break;
			case 'S':
				n_errors += gmt_M_repeated_module_option(API, Ctrl->S.active);
				n_errors += merge1d_parse_interpolation(API, opt->arg, Ctrl);
				break;
			case 'T':
				n_errors += gmt_M_repeated_module_option(API, Ctrl->T.active);
				n_errors += merge1d_parse_range(API, opt->arg, Ctrl);
				break;
			case 'W':
				n_errors += gmt_M_repeated_module_option(API, Ctrl->W.active);
				if (opt->arg[0] && strcmp(opt->arg, "+o")) {
					GMT_Report(API, GMT_MSG_ERROR,
					           "Option -W only accepts the +o modifier\n");
					n_errors++;
				}
				Ctrl->W.only = strstr(opt->arg, "+o") != NULL;
				break;
			case 'Z': {
				char message[GMT_LEN256];
				n_errors += gmt_M_repeated_module_option(API, Ctrl->Z.active);
				if (!opt->arg[0] || opt->arg[0] != '+' ||
				    gq_transform_parse(opt->arg, GQ_TRANSFORM_X_MASK, false,
				                       &Ctrl->Z.transform, NULL, NULL,
				                       message, sizeof(message))) {
					GMT_Report(API, GMT_MSG_ERROR,
					           "Option -Z requires explicit +x/+X/+v/+V modifiers: %s\n",
					           message);
					n_errors++;
				}
				break;
			}
			default:
				n_errors += gmt_default_option_error(GMT, opt);
				break;
		}
	}
	n_errors += gmt_M_check_condition(GMT, !Ctrl->T.active,
	                                  "Option -T is required\n");
	n_errors += gmt_M_check_condition(GMT, Ctrl->A.active && Ctrl->C.active,
	                                  "Options -A and -C are mutually exclusive\n");
	return n_errors ? GMT_PARSE_ERROR : GMT_NOERROR;
}

static char *merge1d_trim(char *text)
{
	char *end;

	while (isspace((unsigned char)*text)) text++;
	if (!*text) return text;
	end = text + strlen(text) - 1;
	while (end >= text && isspace((unsigned char)*end)) *end-- = '\0';
	return text;
}

static int merge1d_parse_ratio(struct GMTAPI_CTRL *API, const char *text,
                               double *ratio1, double *ratio2)
{
	char copy[GMT_LEN128], *slash, *end = NULL;

	if (!text || strlen(text) >= sizeof(copy)) return GMT_PARSE_ERROR;
	strcpy(copy, text);
	slash = strchr(copy, '/');
	if (slash) *slash++ = '\0';
	errno = 0;
	*ratio1 = strtod(copy, &end);
	if (errno || end == copy || *end || !isfinite(*ratio1)) goto bad;
	if (slash) {
		errno = 0;
		*ratio2 = strtod(slash, &end);
		if (errno || end == slash || *end || !isfinite(*ratio2)) goto bad;
	}
	else *ratio2 = *ratio1;
	if (*ratio1 < 0.0 || *ratio1 >= 0.5 || *ratio2 < 0.0 || *ratio2 >= 0.5)
		goto bad;
	return GMT_NOERROR;
bad:
	GMT_Report(API, GMT_MSG_ERROR,
	           "Taper ratio must be r1[/r2] with each ratio in [0, 0.5): %s\n",
	           text ? text : "");
	return GMT_PARSE_ERROR;
}

static int merge1d_parse_interval(struct GMTAPI_CTRL *API, const char *text,
                                  double *west, double *east)
{
	char copy[GMT_LEN128], *slash, *end = NULL;

	if (!text || strlen(text) >= sizeof(copy)) return GMT_PARSE_ERROR;
	strcpy(copy, text);
	slash = strchr(copy, '/');
	if (!slash) goto bad;
	*slash++ = '\0';
	errno = 0;
	*west = strtod(copy, &end);
	if (errno || end == copy || *end || !isfinite(*west)) goto bad;
	errno = 0;
	*east = strtod(slash, &end);
	if (errno || end == slash || *end || !isfinite(*east) || *west >= *east) goto bad;
	return GMT_NOERROR;
bad:
	GMT_Report(API, GMT_MSG_ERROR,
	           "Support interval must be west/east with west < east: %s\n",
	           text ? text : "");
	return GMT_PARSE_ERROR;
}

static int merge1d_append_spec(struct MERGE1D_JOB *job,
                               const struct MERGE1D_SPEC *spec)
{
	struct MERGE1D_SPEC *next = realloc(job->spec,
	                                   (job->count + 1) * sizeof(*next));
	if (next == NULL) return GMT_MEMORY_ERROR;
	job->spec = next;
	job->spec[job->count++] = *spec;
	return GMT_NOERROR;
}

static int merge1d_read_mergefile(struct GMTAPI_CTRL *API, const char *path,
                                  struct MERGE1D_JOB *job)
{
	FILE *fp = fopen(path, "r");
	char line[GMT_BUFSIZ];
	size_t line_number = 0;

	if (fp == NULL) {
		GMT_Report(API, GMT_MSG_ERROR, "Unable to open mergefile %s: %s\n",
		           path, strerror(errno));
		return GMT_DATA_READ_ERROR;
	}
	while (fgets(line, sizeof(line), fp)) {
		char *tokens[6] = {NULL}, *save = NULL, *token, *comment, *text;
		size_t n = 0;
		struct MERGE1D_SPEC spec;

		line_number++;
		comment = strchr(line, '#');
		if (comment) *comment = '\0';
		text = merge1d_trim(line);
		if (!*text) continue;
		for (token = strtok_r(text, " \t\r\n", &save); token && n < 6;
		     token = strtok_r(NULL, " \t\r\n", &save))
			tokens[n++] = token;
		if (n == 0) continue;
		if (n > 5) {
			GMT_Report(API, GMT_MSG_ERROR,
			           "%s:%zu: Expected at most five fields\n", path, line_number);
			fclose(fp);
			return GMT_PARSE_ERROR;
		}
		memset(&spec, 0, sizeof(spec));
		spec.function = WFUNC_COSINE;
		spec.ratio1 = spec.ratio2 = 0.2;
		spec.primary_source = strdup(tokens[0]);
		if (spec.primary_source == NULL) {
			fclose(fp);
			return GMT_MEMORY_ERROR;
		}
		if (n > 1 && strcmp(tokens[1], "-")) {
			spec.secondary_source = strdup(tokens[1]);
			if (spec.secondary_source == NULL) {
				free(spec.primary_source);
				fclose(fp);
				return GMT_MEMORY_ERROR;
			}
			spec.has_secondary = true;
		}
		if (n > 2 && strcmp(tokens[2], "-")) {
			if (merge1d_parse_interval(API, tokens[2], &spec.west, &spec.east)) {
				free(spec.primary_source);
				free(spec.secondary_source);
				fclose(fp);
				return GMT_PARSE_ERROR;
			}
			spec.has_interval = true;
		}
		if (n > 3 && strcmp(tokens[3], "-") &&
		    blend_window_function_from_name(tokens[3], &spec.function) != SUCCESS) {
			GMT_Report(API, GMT_MSG_ERROR, "%s:%zu: Unknown window function %s\n",
			           path, line_number, tokens[3]);
			free(spec.primary_source);
			free(spec.secondary_source);
			fclose(fp);
			return GMT_PARSE_ERROR;
		}
		if (n > 4 && strcmp(tokens[4], "-") &&
		    merge1d_parse_ratio(API, tokens[4], &spec.ratio1, &spec.ratio2)) {
			free(spec.primary_source);
			free(spec.secondary_source);
			fclose(fp);
			return GMT_PARSE_ERROR;
		}
		if (merge1d_append_spec(job, &spec)) {
			free(spec.primary_source);
			free(spec.secondary_source);
			fclose(fp);
			return GMT_MEMORY_ERROR;
		}
	}
	if (ferror(fp)) {
		fclose(fp);
		return GMT_DATA_READ_ERROR;
	}
	fclose(fp);
	if (job->count == 0) {
		GMT_Report(API, GMT_MSG_ERROR, "Mergefile %s contains no records\n", path);
		return GMT_DATA_READ_ERROR;
	}
	job->mergefile = true;
	return GMT_NOERROR;
}

static int merge1d_source_parts(struct GMTAPI_CTRL *API, const char *source,
                                char **path, char ***names, size_t *count,
                                bool *explicit_selector, bool *has_sentinel,
                                double *sentinel,
                                struct GQ_TRANSFORM *transform)
{
	char *copy = NULL, *question, *mods = NULL, *save = NULL, *token;
	size_t capacity = 0;
	char message[GMT_LEN256];

	*path = NULL;
	*names = NULL;
	*count = 0;
	*explicit_selector = false;
	*has_sentinel = false;
	gq_transform_init(transform);
	copy = strdup(source);
	if (copy == NULL) return GMT_MEMORY_ERROR;
	question = strchr(copy, '?');
	if (question) {
		*question++ = '\0';
		*explicit_selector = *question != '\0' && *question != '+';
		mods = strchr(question, '+');
		if (mods) *mods++ = '\0';
		for (token = strtok_r(question, ",", &save); token;
		     token = strtok_r(NULL, ",", &save)) {
			char **next;
			if (!*token) goto bad;
			if (*count == capacity) {
				capacity = capacity ? capacity * 2 : 4;
				next = realloc(*names, capacity * sizeof(**names));
				if (next == NULL) goto memory;
				*names = next;
			}
			(*names)[*count] = strdup(token);
			if ((*names)[*count] == NULL) goto memory;
			(*count)++;
		}
		if (mods && gq_transform_parse(mods, GQ_TRANSFORM_X_MASK, true,
		                                transform, has_sentinel, sentinel,
		                                message, sizeof(message)))
			goto bad_modifier;
	}
	*path = strdup(copy);
	free(copy);
	return *path ? GMT_NOERROR : GMT_MEMORY_ERROR;

bad_modifier:
	GMT_Report(API, GMT_MSG_ERROR,
	           "Unsupported NetCDF selector modifier in %s: %s\n", source, message);
	goto fail;
bad:
	GMT_Report(API, GMT_MSG_ERROR, "Invalid NetCDF selector: %s\n", source);
	goto fail;
memory:
	GMT_Report(API, GMT_MSG_ERROR, "Unable to allocate NetCDF selector\n");
fail:
	if (*names) {
		size_t k;
		for (k = 0; k < *count; k++) free((*names)[k]);
	}
	free(*names);
	*names = NULL;
	*count = 0;
	free(copy);
	gq_transform_free(transform);
	return GMT_PARSE_ERROR;
}

static void merge1d_names_free(char **names, size_t count)
{
	size_t k;
	for (k = 0; k < count; k++) free(names[k]);
	free(names);
}

static int merge1d_is_netcdf(struct GMTAPI_CTRL *API, const char *source)
{
	char *path = strdup(source), *question, *resolved = NULL;
	int ncid, status;

	if (path == NULL) return 0;
	question = strchr(path, '?');
	if (question) *question = '\0';
	if (gq_resolve_remote_path(API, GMT_IS_DATASET, path, &resolved)) {
		free(path);
		return 0;
	}
	status = nc_open(resolved, NC_NOWRITE, &ncid);
	if (status == NC_NOERR) nc_close(ncid);
	free(path);
	free(resolved);
	return status == NC_NOERR;
}

static int merge1d_numeric_type(nc_type type)
{
	return type == NC_BYTE || type == NC_UBYTE || type == NC_SHORT ||
	       type == NC_USHORT || type == NC_INT || type == NC_UINT ||
	       type == NC_INT64 || type == NC_UINT64 || type == NC_FLOAT ||
	       type == NC_DOUBLE;
}

static int merge1d_netcdf_1d_variable(int ncid, int varid, nc_type *type,
                                     int *dimid)
{
	int ndims;

	if (nc_inq_vartype(ncid, varid, type) != NC_NOERR ||
	    nc_inq_varndims(ncid, varid, &ndims) != NC_NOERR ||
	    ndims != 1 ||
	    nc_inq_vardimid(ncid, varid, dimid) != NC_NOERR)
		return GMT_DATA_READ_ERROR;
	return GMT_NOERROR;
}

static char *merge1d_netcdf_text_attribute(int ncid, int varid, const char *name)
{
	size_t length;
	char *value;

	if (nc_inq_attlen(ncid, varid, name, &length) != NC_NOERR) return NULL;
	value = calloc(length + 1, 1);
	if (value == NULL) return NULL;
	if (nc_get_att_text(ncid, varid, name, value) != NC_NOERR) {
		free(value);
		return NULL;
	}
	return value;
}

static int merge1d_netcdf_missing(int ncid, int varid, double value,
                                  bool has_sentinel, double sentinel)
{
	double missing;

	if (isnan(value)) return true;
	if (has_sentinel && value == sentinel) return true;
	if (nc_get_att_double(ncid, varid, "_FillValue", &missing) == NC_NOERR &&
	    (isnan(missing) ? isnan(value) : value == missing))
		return true;
	if (nc_get_att_double(ncid, varid, "missing_value", &missing) == NC_NOERR &&
	    (isnan(missing) ? isnan(value) : value == missing))
		return true;
	return false;
}

static int merge1d_read_netcdf(struct GMTAPI_CTRL *API, const char *source,
                               struct MERGE1D_SERIES *series)
{
	int ncid = -1, nvars, k, dimid = -1, coordinate_var = -1;
	char **selectors = NULL;
	size_t selector_count = 0, field, n, row;
	bool explicit_selector = false, has_sentinel = false;
	double sentinel = 0.0;
	int *varids = NULL;
	int status = GMT_DATA_READ_ERROR;
	char dim_name[NC_MAX_NAME + 1];

	if (merge1d_source_parts(API, source, &series->path, &selectors,
	                         &selector_count, &explicit_selector,
	                         &has_sentinel, &sentinel, &series->transform))
		return GMT_PARSE_ERROR;
	{
		char *resolved = NULL;
		if (gq_resolve_remote_path(API, GMT_IS_DATASET,
		                           series->path, &resolved)) {
			merge1d_names_free(selectors, selector_count);
			return GMT_DATA_READ_ERROR;
		}
		free(series->path);
		series->path = resolved;
	}
	series->source = strdup(source);
	series->format = MERGE1D_FORMAT_NETCDF;
	series->explicit_selector = explicit_selector;
	series->has_sentinel = has_sentinel;
	series->sentinel = sentinel;
	if (series->source == NULL || nc_open(series->path, NC_NOWRITE, &ncid) != NC_NOERR)
		goto cleanup;
	if (nc_inq_nvars(ncid, &nvars) != NC_NOERR) goto cleanup;

	if (selector_count == 0) {
		for (k = 0; k < nvars; k++) {
			int candidate_dim;
			nc_type type;
			char var_name[NC_MAX_NAME + 1], candidate_dim_name[NC_MAX_NAME + 1];
			if (nc_inq_varname(ncid, k, var_name) != NC_NOERR ||
			    merge1d_netcdf_1d_variable(ncid, k, &type,
			                                &candidate_dim) != GMT_NOERROR ||
			    !merge1d_numeric_type(type) ||
			    nc_inq_dimname(ncid, candidate_dim, candidate_dim_name) != NC_NOERR)
				continue;
			if (!strcmp(var_name, candidate_dim_name)) continue;
			selectors = calloc(1, sizeof(*selectors));
			if (selectors == NULL) goto cleanup;
			selectors[0] = strdup(var_name);
			if (selectors[0] == NULL) goto cleanup;
			selector_count = 1;
			break;
		}
		if (selector_count == 0) {
			GMT_Report(API, GMT_MSG_ERROR,
			           "No one-dimensional data variable found in %s\n", series->path);
			goto cleanup;
		}
	}

	varids = calloc(selector_count, sizeof(*varids));
	series->value = calloc(selector_count, sizeof(*series->value));
	series->field_name = calloc(selector_count, sizeof(*series->field_name));
	series->units = calloc(selector_count, sizeof(*series->units));
	if (!varids || !series->value || !series->field_name || !series->units)
		goto cleanup;
	series->n_fields = selector_count;
	{
		char message[GMT_LEN256];
		if (gq_transform_validate_values(&series->transform, selector_count,
		                                  message, sizeof(message))) {
			GMT_Report(API, GMT_MSG_ERROR, "%s: %s\n", source, message);
			goto cleanup;
		}
	}

	for (field = 0; field < selector_count; field++) {
		int field_dim;
		nc_type type;
		if (nc_inq_varid(ncid, selectors[field], &varids[field]) != NC_NOERR ||
		    merge1d_netcdf_1d_variable(ncid, varids[field], &type,
		                                &field_dim) != GMT_NOERROR ||
		    !merge1d_numeric_type(type)) {
			GMT_Report(API, GMT_MSG_ERROR,
			           "%s is not a numeric one-dimensional variable in %s\n",
			           selectors[field], series->path);
			goto cleanup;
		}
		if (field == 0) dimid = field_dim;
		else if (field_dim != dimid) {
			GMT_Report(API, GMT_MSG_ERROR,
			           "Selected variables in %s do not share one coordinate\n",
			           series->path);
			goto cleanup;
		}
		series->field_name[field] = strdup(selectors[field]);
		series->units[field] = merge1d_netcdf_text_attribute(ncid, varids[field],
		                                                     "units");
		if (series->field_name[field] == NULL) goto cleanup;
	}

	if (nc_inq_dim(ncid, dimid, dim_name, &n) != NC_NOERR ||
	    nc_inq_varid(ncid, dim_name, &coordinate_var) != NC_NOERR) {
		GMT_Report(API, GMT_MSG_ERROR,
		           "Coordinate variable for dimension in %s was not found\n",
		           series->path);
		goto cleanup;
	}
	{
		int coordinate_dim;
		nc_type coordinate_type;
		if (merge1d_netcdf_1d_variable(ncid, coordinate_var,
		                                &coordinate_type,
		                                &coordinate_dim) != GMT_NOERROR ||
		    !merge1d_numeric_type(coordinate_type) || coordinate_dim != dimid) {
			GMT_Report(API, GMT_MSG_ERROR,
			           "Invalid coordinate variable %s in %s\n",
			           dim_name, series->path);
			goto cleanup;
		}
	}
	series->n = n;
	series->coordinate_name = strdup(dim_name);
	series->coordinate_units = merge1d_netcdf_text_attribute(ncid, coordinate_var,
	                                                          "units");
	series->x = calloc(n, sizeof(*series->x));
	if (!series->coordinate_name || !series->x ||
	    nc_get_var_double(ncid, coordinate_var, series->x) != NC_NOERR)
		goto cleanup;
	{
		double scale = 1.0, offset = 0.0;
		nc_get_att_double(ncid, coordinate_var, "scale_factor", &scale);
		nc_get_att_double(ncid, coordinate_var, "add_offset", &offset);
		for (row = 0; row < n; row++)
			series->x[row] = (series->x[row] * scale + offset) *
			                 series->transform.axis_scale[GQ_TRANSFORM_X];
	}
	if (series->transform.axis_unit[GQ_TRANSFORM_X]) {
		free(series->coordinate_units);
		series->coordinate_units =
		    strdup(series->transform.axis_unit[GQ_TRANSFORM_X]);
		if (!series->coordinate_units) goto cleanup;
	}
	else if (series->transform.axis_set[GQ_TRANSFORM_X] &&
	         series->transform.axis_scale[GQ_TRANSFORM_X] != 1.0) {
		free(series->coordinate_units);
		series->coordinate_units = NULL;
	}

	for (field = 0; field < selector_count; field++) {
		double scale = 1.0, offset = 0.0;
		series->value[field] = calloc(n, sizeof(*series->value[field]));
		if (!series->value[field] ||
		    nc_get_var_double(ncid, varids[field], series->value[field]) != NC_NOERR)
			goto cleanup;
		nc_get_att_double(ncid, varids[field], "scale_factor", &scale);
		nc_get_att_double(ncid, varids[field], "add_offset", &offset);
		for (row = 0; row < n; row++) {
			double value = series->value[field][row];
			if (merge1d_netcdf_missing(ncid, varids[field], value,
			                           has_sentinel, sentinel))
				series->value[field][row] = NAN;
			else
				series->value[field][row] = (value * scale + offset) *
				                            gq_transform_value_scale(&series->transform,
				                                                         field);
		}
		if (gq_transform_value_unit(&series->transform, field)) {
			free(series->units[field]);
			series->units[field] =
			    strdup(gq_transform_value_unit(&series->transform, field));
			if (!series->units[field]) goto cleanup;
		}
		else if (gq_transform_value_scale(&series->transform, field) != 1.0) {
			free(series->units[field]);
			series->units[field] = NULL;
		}
	}
	status = GMT_NOERROR;

cleanup:
	if (ncid >= 0) nc_close(ncid);
	merge1d_names_free(selectors, selector_count);
	free(varids);
	if (status != GMT_NOERROR)
		GMT_Report(API, GMT_MSG_ERROR, "Unable to read NetCDF series %s\n", source);
	return status;
}

static int merge1d_read_text(struct GMT_CTRL *GMT, const char *source,
                             struct MERGE1D_SERIES *series)
{
	struct GMT_DATASET *D = NULL;
	struct GMT_DATASEGMENT *S;
	size_t field, row;
	int status = GMT_DATA_READ_ERROR;

	D = GMT_Read_Data(GMT->parent, GMT_IS_DATASET, GMT_IS_FILE, GMT_IS_NONE,
	                  GMT_READ_NORMAL, NULL, (void *)source, NULL);
	if (D == NULL) goto cleanup;
	if (D->n_tables != 1 || D->n_segments != 1 || D->n_columns < 2) {
		GMT_Report(GMT->parent, GMT_MSG_ERROR,
		           "Text input %s must contain one segment with at least two columns\n",
		           source);
		goto cleanup;
	}
	S = D->table[0]->segment[0];
	if (S->n_rows < 2) {
		GMT_Report(GMT->parent, GMT_MSG_ERROR,
		           "Text input %s must contain at least two rows\n", source);
		goto cleanup;
	}
	series->source = strdup(source);
	series->path = strdup(source);
	series->format = MERGE1D_FORMAT_TEXT;
	series->n = S->n_rows;
	series->n_fields = S->n_columns - 1;
	series->x = calloc(series->n, sizeof(*series->x));
	series->value = calloc(series->n_fields, sizeof(*series->value));
	series->field_name = calloc(series->n_fields, sizeof(*series->field_name));
	series->units = calloc(series->n_fields, sizeof(*series->units));
	series->coordinate_name = strdup("x");
	if (!series->source || !series->path || !series->x || !series->value ||
	    !series->field_name || !series->units || !series->coordinate_name)
		goto cleanup;
	memcpy(series->x, S->data[0], series->n * sizeof(*series->x));
	if (GMT->common.d.active[GMT_IN])
		for (row = 0; row < series->n; row++)
			if (series->x[row] == GMT->common.d.nan_proxy[GMT_IN])
				series->x[row] = NAN;
	for (field = 0; field < series->n_fields; field++) {
		char name[32];
		series->value[field] = calloc(series->n, sizeof(*series->value[field]));
		snprintf(name, sizeof(name), "z%zu", field + 1);
		series->field_name[field] = strdup(series->n_fields == 1 ? "z" : name);
		if (!series->value[field] || !series->field_name[field]) goto cleanup;
		for (row = 0; row < series->n; row++) {
			double value = S->data[field + 1][row];
			series->value[field][row] =
			    GMT->common.d.active[GMT_IN] &&
			    value == GMT->common.d.nan_proxy[GMT_IN] ? NAN : value;
		}
	}
	status = GMT_NOERROR;

cleanup:
	if (D && GMT_Destroy_Data(GMT->parent, &D) != GMT_NOERROR)
		status = GMT_RUNTIME_ERROR;
	return status;
}

static int merge1d_validate_coordinates(struct GMTAPI_CTRL *API,
                                        const struct MERGE1D_SERIES *series)
{
	size_t k;

	if (series->n < 2) return GMT_DIM_TOO_SMALL;
	for (k = 0; k < series->n; k++) {
		if (!isfinite(series->x[k])) {
			GMT_Report(API, GMT_MSG_ERROR,
			           "Coordinates in %s must be finite (record %zu)\n",
			           series->source, k);
			return GMT_DATA_READ_ERROR;
		}
		if (k && series->x[k] <= series->x[k - 1]) {
			GMT_Report(API, GMT_MSG_ERROR,
			           "Coordinates in %s must be strictly increasing after input "
			           "scaling (record %zu). Input +x scaling does not reorder "
			           "samples; choose +x<scale> so the transformed axis increases, "
			           "then use output -Z+x<scale> to restore the desired output "
			           "convention.\n", series->source, k);
			return GMT_DATA_READ_ERROR;
		}
	}
	return GMT_NOERROR;
}

static int merge1d_read_series(struct GMT_CTRL *GMT, const char *source,
                               struct MERGE1D_SERIES *series)
{
	int status;

	memset(series, 0, sizeof(*series));
	status = merge1d_is_netcdf(GMT->parent, source)
	       ? merge1d_read_netcdf(GMT->parent, source, series)
	       : merge1d_read_text(GMT, source, series);
	if (status != GMT_NOERROR) return status;
	return merge1d_validate_coordinates(GMT->parent, series);
}

static int merge1d_classify_text_file(const char *path, bool *is_mergefile)
{
	FILE *fp = fopen(path, "r");
	char line[GMT_BUFSIZ];

	if (fp == NULL) return GMT_DATA_READ_ERROR;
	while (fgets(line, sizeof(line), fp)) {
		char *text = merge1d_trim(line), *end = NULL;
		if (!*text || *text == '#') continue;
		errno = 0;
		(void)strtod(text, &end);
		*is_mergefile = errno || end == text ||
		                (*end && !isspace((unsigned char)*end));
		fclose(fp);
		return GMT_NOERROR;
	}
	fclose(fp);
	return GMT_DATA_READ_ERROR;
}

static int merge1d_materialize_stdin(struct GMT_CTRL *GMT,
                                     struct MERGE1D_CTRL *Ctrl,
                                     char output[PATH_MAX])
{
	FILE *fp;
	char buffer[8192];
	size_t n, k, stdin_count = 0;

	for (k = 0; k < Ctrl->In.n; k++)
		if (!strcmp(Ctrl->In.file[k], "-")) stdin_count++;
	if (Ctrl->In.n == 0) stdin_count = 1;
	if (stdin_count == 0) return GMT_NOERROR;
	if (stdin_count > 1 || Ctrl->In.n > 1) {
		GMT_Report(GMT->parent, GMT_MSG_ERROR,
		           "Standard input must be the only input source\n");
		return GMT_PARSE_ERROR;
	}
	if (gmt_get_tempname(GMT->parent, "merge1d_stdin", ".txt", output))
		return GMT_RUNTIME_ERROR;
	fp = fopen(output, "wb");
	if (fp == NULL) return GMT_ERROR_ON_FOPEN;
	while ((n = fread(buffer, 1, sizeof(buffer), stdin)) > 0)
		if (fwrite(buffer, 1, n, fp) != n) {
			fclose(fp);
			return GMT_DATA_WRITE_ERROR;
		}
	if (ferror(stdin) || fclose(fp)) return GMT_DATA_READ_ERROR;
	if (Ctrl->In.n == 0) {
		Ctrl->In.file = calloc(1, sizeof(*Ctrl->In.file));
		if (Ctrl->In.file == NULL) return GMT_MEMORY_ERROR;
		Ctrl->In.n = 1;
	}
	else free(Ctrl->In.file[0]);
	Ctrl->In.file[0] = strdup(output);
	return Ctrl->In.file[0] ? GMT_NOERROR : GMT_MEMORY_ERROR;
}

static int merge1d_specs_from_inputs(struct GMT_CTRL *GMT,
                                     struct MERGE1D_CTRL *Ctrl,
                                     struct MERGE1D_JOB *job)
{
	bool is_mergefile = false;
	size_t k;

	for (k = 0; k < Ctrl->In.n; k++) {
		char *resolved = NULL;
		int status = gq_resolve_remote_source(
		    GMT->parent, GMT_IS_DATASET, Ctrl->In.file[k], NULL, &resolved);
		if (status != GMT_NOERROR) return status;
		free(Ctrl->In.file[k]);
		Ctrl->In.file[k] = resolved;
	}
	if (Ctrl->In.n == 1 &&
	    !merge1d_is_netcdf(GMT->parent, Ctrl->In.file[0])) {
		int status = merge1d_classify_text_file(Ctrl->In.file[0], &is_mergefile);
		if (status != GMT_NOERROR) return status;
	}
	if (is_mergefile)
		return merge1d_read_mergefile(GMT->parent, Ctrl->In.file[0], job);
	if (Ctrl->A.active) {
		GMT_Report(GMT->parent, GMT_MSG_ERROR,
		           "Option -A requires a mergefile with explicit supports\n");
		return GMT_PARSE_ERROR;
	}
	for (k = 0; k < Ctrl->In.n; k++) {
		struct MERGE1D_SPEC spec;
		memset(&spec, 0, sizeof(spec));
		spec.primary_source = strdup(Ctrl->In.file[k]);
		spec.function = WFUNC_BOXCAR;
		spec.ratio1 = spec.ratio2 = 0.0;
		if (!spec.primary_source || merge1d_append_spec(job, &spec)) {
			free(spec.primary_source);
			return GMT_MEMORY_ERROR;
		}
	}
	Ctrl->C.active = true;
	return GMT_NOERROR;
}

static int merge1d_interpolate_run(struct GMT_CTRL *GMT,
                                   const struct MERGE1D_SERIES *series,
                                   size_t field, const double *x,
                                   const double *value, size_t n,
                                   const double *axis, size_t n_axis,
                                   unsigned int mode, double fit,
                                   double *output)
{
	size_t first = 0, count;

	while (first < n_axis && axis[first] < x[0]) first++;
	count = first;
	while (count < n_axis && axis[count] <= x[n - 1]) count++;
	count -= first;
	if (n == 1) {
		if (count == 1 &&
		    fabs(axis[first] - x[0]) <=
		    32.0 * DBL_EPSILON * MAX(1.0, fabs(x[0])))
			output[first] = value[0];
	}
	else if (count) {
		int status = gmt_intpol(GMT, (double *)x, (double *)value, NULL,
		                        n, count, (double *)&axis[first],
		                        &output[first], fit, mode);
		if (status != GMT_NOERROR) {
			GMT_Report(GMT->parent, GMT_MSG_ERROR,
			           "GMT interpolation failed for %s field %zu\n",
			           series->source, field + 1);
			return status;
		}
	}
	return GMT_NOERROR;
}

static int merge1d_sample_series(struct GMT_CTRL *GMT,
                                 struct MERGE1D_SERIES *series,
                                 const double *axis, size_t n_axis,
                                 unsigned int mode, double fit,
                                 bool bridge, double max_gap)
{
	size_t field, k;
	double *bridge_x = NULL, *bridge_value = NULL;
	int status = GMT_NOERROR;

	series->sampled = calloc(series->n_fields, sizeof(*series->sampled));
	if (series->sampled == NULL) return GMT_MEMORY_ERROR;
	if (bridge) {
		bridge_x = calloc(series->n, sizeof(*bridge_x));
		bridge_value = calloc(series->n, sizeof(*bridge_value));
		if (!bridge_x || !bridge_value) {
			status = GMT_MEMORY_ERROR;
			goto cleanup;
		}
	}
	for (field = 0; field < series->n_fields; field++) {
		series->sampled[field] = calloc(n_axis, sizeof(*series->sampled[field]));
		if (series->sampled[field] == NULL) {
			status = GMT_MEMORY_ERROR;
			goto cleanup;
		}
		for (k = 0; k < n_axis; k++) series->sampled[field][k] = NAN;
		if (!bridge) {
			size_t start = 0;
			while (start < series->n) {
				size_t stop;
				while (start < series->n &&
				       !isfinite(series->value[field][start]))
					start++;
				if (start == series->n) break;
				stop = start + 1;
				while (stop < series->n &&
				       isfinite(series->value[field][stop]))
					stop++;
				status = merge1d_interpolate_run(
				    GMT, series, field, &series->x[start],
				    &series->value[field][start], stop - start,
				    axis, n_axis, mode, fit, series->sampled[field]);
				if (status != GMT_NOERROR) goto cleanup;
				start = stop;
			}
		}
		else {
			size_t run_count = 0, previous = 0;
			for (k = 0; k < series->n; k++) {
				if (!isfinite(series->value[field][k])) continue;
				if (run_count && k > previous + 1 &&
				    series->x[k] - series->x[previous] > max_gap) {
					status = merge1d_interpolate_run(
					    GMT, series, field, bridge_x, bridge_value,
					    run_count, axis, n_axis, mode, fit,
					    series->sampled[field]);
					if (status != GMT_NOERROR) goto cleanup;
					run_count = 0;
				}
				bridge_x[run_count] = series->x[k];
				bridge_value[run_count++] = series->value[field][k];
				previous = k;
			}
			if (run_count) {
				status = merge1d_interpolate_run(
				    GMT, series, field, bridge_x, bridge_value, run_count,
				    axis, n_axis, mode, fit, series->sampled[field]);
				if (status != GMT_NOERROR) goto cleanup;
			}
		}
	}

cleanup:
	free(bridge_x);
	free(bridge_value);
	return status;
}

static int merge1d_parse_output_fields(struct GMTAPI_CTRL *API,
                                       const struct MERGE1D_CTRL *Ctrl,
                                       struct MERGE1D_JOB *job,
                                       bool netcdf_output)
{
	size_t count = 0, k;
	char *copy = NULL, *save = NULL, *token;

	if (Ctrl->F.active) {
		copy = strdup(Ctrl->F.fields);
		if (copy == NULL) return GMT_MEMORY_ERROR;
		job->output_fields = calloc(job->n_fields, sizeof(*job->output_fields));
		if (job->output_fields == NULL) {
			free(copy);
			return GMT_MEMORY_ERROR;
		}
		for (token = strtok_r(copy, ",", &save); token;
		     token = strtok_r(NULL, ",", &save)) {
			if (!*token) goto bad;
			if (count >= job->n_fields) goto count_error;
			for (k = 0; k < count; k++)
				if (!strcmp(job->output_fields[k], token)) goto duplicate;
			job->output_fields[count] = strdup(token);
			if (job->output_fields[count] == NULL) {
				free(copy);
				return GMT_MEMORY_ERROR;
			}
			count++;
		}
		free(copy);
		if (count != job->n_fields) {
			GMT_Report(API, GMT_MSG_ERROR,
			           "Option -F lists %zu fields, but inputs contain %zu\n",
			           count, job->n_fields);
			return GMT_PARSE_ERROR;
		}
		return GMT_NOERROR;
	}
	if ((netcdf_output || job->format == MERGE1D_FORMAT_NETCDF) &&
	    job->n_fields > 1 && !Ctrl->W.only) {
		GMT_Report(API, GMT_MSG_ERROR,
		           "Multiparameter NetCDF output requires -F\n");
		return GMT_PARSE_ERROR;
	}
	job->output_fields = calloc(job->n_fields, sizeof(*job->output_fields));
	if (job->output_fields == NULL) return GMT_MEMORY_ERROR;
	for (k = 0; k < job->n_fields; k++) {
		const struct MERGE1D_SERIES *first = &job->spec[0].primary;
		char name[32];
		if (job->n_fields == 1 && first->explicit_selector)
			job->output_fields[k] = strdup(first->field_name[k]);
		else {
			snprintf(name, sizeof(name), job->n_fields == 1 ? "z" : "z%zu", k + 1);
			job->output_fields[k] = strdup(name);
		}
		if (job->output_fields[k] == NULL) return GMT_MEMORY_ERROR;
	}
	return GMT_NOERROR;

duplicate:
	GMT_Report(API, GMT_MSG_ERROR, "Duplicate output field name: %s\n", token);
	free(copy);
	return GMT_PARSE_ERROR;
count_error:
	GMT_Report(API, GMT_MSG_ERROR,
	           "Option -F lists more fields than the %zu input fields\n",
	           job->n_fields);
	free(copy);
	return GMT_PARSE_ERROR;
bad:
	GMT_Report(API, GMT_MSG_ERROR, "Invalid output field list\n");
	free(copy);
	return GMT_PARSE_ERROR;
}

static int merge1d_prepare_job(struct GMT_CTRL *GMT, struct MERGE1D_CTRL *Ctrl,
                               struct MERGE1D_JOB *job, const double *axis,
                               bool netcdf_output)
{
	size_t k;
	double tolerance;
	int status;

	for (k = 0; k < job->count; k++) {
		struct MERGE1D_SPEC *spec = &job->spec[k];
		status = merge1d_read_series(GMT, spec->primary_source, &spec->primary);
		if (status != GMT_NOERROR) return status;
		if (job->format == MERGE1D_FORMAT_UNKNOWN) {
			job->format = spec->primary.format;
			job->n_fields = spec->primary.n_fields;
			job->coordinate_name = strdup(spec->primary.coordinate_name);
			job->coordinate_units = spec->primary.coordinate_units
			                      ? strdup(spec->primary.coordinate_units) : NULL;
		}
		if (spec->primary.format != job->format) {
			GMT_Report(GMT->parent, GMT_MSG_ERROR,
			           "Text and NetCDF inputs cannot be mixed in one run\n");
			return GMT_PARSE_ERROR;
		}
		if (spec->primary.n_fields != job->n_fields) {
			GMT_Report(GMT->parent, GMT_MSG_ERROR,
			           "Input %s has %zu fields; expected %zu\n",
			           spec->primary_source, spec->primary.n_fields,
			           job->n_fields);
			return GMT_PARSE_ERROR;
		}
		if (spec->has_secondary) {
			status = merge1d_read_series(GMT, spec->secondary_source,
			                             &spec->secondary);
			if (status != GMT_NOERROR) return status;
			if (spec->secondary.format != job->format ||
			    spec->secondary.n_fields != job->n_fields) {
				GMT_Report(GMT->parent, GMT_MSG_ERROR,
				           "Secondary %s does not match the input format and field count\n",
				           spec->secondary_source);
				return GMT_PARSE_ERROR;
			}
		}
		if (!spec->has_interval) {
			spec->west = spec->primary.x[0];
			spec->east = spec->primary.x[spec->primary.n - 1];
		}
		tolerance = 32.0 * DBL_EPSILON *
		            MAX(1.0, MAX(fabs(spec->west), fabs(spec->east)));
		if (spec->west < spec->primary.x[0] - tolerance ||
		    spec->east > spec->primary.x[spec->primary.n - 1] + tolerance) {
			GMT_Report(GMT->parent, GMT_MSG_ERROR,
			           "Support %.12g/%.12g lies outside primary coordinate "
			           "range %.12g/%.12g for %s\n",
			           spec->west, spec->east, spec->primary.x[0],
			           spec->primary.x[spec->primary.n - 1],
			           spec->primary_source);
			return GMT_PARSE_ERROR;
		}
		spec->nx = (int)floor((spec->east - spec->west) / Ctrl->T.inc + 0.5) + 1;
		if (spec->nx < 2) {
			GMT_Report(GMT->parent, GMT_MSG_ERROR,
			           "Support %.12g/%.12g is shorter than the output increment\n",
			           spec->west, spec->east);
			return GMT_PARSE_ERROR;
		}
		status = merge1d_sample_series(GMT, &spec->primary, axis, Ctrl->T.n,
		                               Ctrl->S.mode, Ctrl->S.fit,
		                               Ctrl->S.bridge, Ctrl->S.max_gap);
		if (status != GMT_NOERROR) return status;
		if (spec->has_secondary) {
			status = merge1d_sample_series(GMT, &spec->secondary, axis,
			                               Ctrl->T.n, Ctrl->S.mode,
			                               Ctrl->S.fit, Ctrl->S.bridge,
			                               Ctrl->S.max_gap);
			if (status != GMT_NOERROR) return status;
		}
	}
	if (job->format == MERGE1D_FORMAT_NETCDF) {
		for (k = 0; k < job->count; k++) {
			size_t field;
			for (field = 0; field < job->n_fields; field++) {
				if (strcmp(job->spec[k].primary.field_name[field],
				           job->spec[0].primary.field_name[field]) &&
				    !Ctrl->F.active && !Ctrl->W.only) {
					GMT_Report(GMT->parent, GMT_MSG_ERROR,
					           "Different NetCDF variable names require -F positional mapping\n");
					return GMT_PARSE_ERROR;
				}
			}
		}
	}
	return merge1d_parse_output_fields(GMT->parent, Ctrl, job, netcdf_output);
}

static int merge1d_support_weight(const struct MERGE1D_SPEC *spec, double x,
                                  double *weight)
{
	window data;
	double scaled, wl, wr;
	int left, right;

	if (x < spec->west || x > spec->east) {
		*weight = 0.0;
		return GMT_NOERROR;
	}
	memset(&data, 0, sizeof(data));
	data.nx = spec->nx;
	data.ratio_x1 = spec->ratio1;
	data.ratio_x2 = spec->ratio2;
	data.x_function = spec->function;
	scaled = (x - spec->west) * (double)(spec->nx - 1) /
	         (spec->east - spec->west);
	left = (int)floor(scaled);
	if (left < 0) left = 0;
	if (left >= spec->nx - 1) left = spec->nx - 1;
	right = left == spec->nx - 1 ? left : left + 1;
	if (embedding_contribution1d(left, &data) != SUCCESS) return GMT_RUNTIME_ERROR;
	wl = data.contribution;
	if (left == right) {
		*weight = wl;
		return GMT_NOERROR;
	}
	if (embedding_contribution1d(right, &data) != SUCCESS) return GMT_RUNTIME_ERROR;
	wr = data.contribution;
	*weight = wl + (scaled - (double)left) * (wr - wl);
	return GMT_NOERROR;
}

/* Select the first support, not its potentially much wider input domain. */
static int merge1d_report_weight(const struct MERGE1D_CTRL *Ctrl,
                                 const struct MERGE1D_JOB *job, double x,
                                 double *weight)
{
	size_t k, owner = SIZE_MAX;
	*weight = 0.0;
	for (k = 0; k < job->count; k++) {
		const struct MERGE1D_SPEC *spec = &job->spec[k];
		if (x < spec->primary.x[0] || x > spec->primary.x[spec->primary.n - 1])
			continue;
		if (!spec->has_secondary) return GMT_NOERROR;
		if (x < spec->west || x > spec->east) continue;
		owner = k;
		if (merge1d_support_weight(spec, x, weight)) return GMT_RUNTIME_ERROR;
		break;
	}
	/* A zero on the selected support boundary is a valid weight. */
	if (!Ctrl->A.active || owner == SIZE_MAX || *weight <= 0.0)
		return GMT_NOERROR;
	for (k = owner + 1; k < job->count; k++) {
		const struct MERGE1D_SPEC *spec = &job->spec[k];
		double other;
		if (!spec->has_secondary ||
		    strcmp(spec->secondary_source, job->spec[owner].secondary_source) ||
		    !strcmp(spec->primary_source, job->spec[owner].secondary_source) ||
		    x < spec->primary.x[0] || x > spec->primary.x[spec->primary.n - 1] ||
		    x < spec->west || x > spec->east) continue;
		if (merge1d_support_weight(spec, x, &other)) return GMT_RUNTIME_ERROR;
		if (other > 0.0) *weight = MIN(1.0, *weight + other);
	}
	return GMT_NOERROR;
}

static int merge1d_validate_overlap_axis(struct GMT_CTRL *GMT,
                                         const struct MERGE1D_JOB *job,
                                         const double *axis, size_t n_axis)
{
	size_t a, b, i;

	for (a = 0; a < job->count; a++) {
		if (!job->spec[a].has_secondary) continue;
		for (b = a + 1; b < job->count; b++) {
			bool overlap = false;
			if (!job->spec[b].has_secondary) continue;
			for (i = 0; i < n_axis; i++) {
				double wa, wb;
				if (merge1d_support_weight(&job->spec[a], axis[i], &wa) ||
				    merge1d_support_weight(&job->spec[b], axis[i], &wb))
					return GMT_RUNTIME_ERROR;
				if (wa > 0.0 && wb > 0.0) {
					overlap = true;
					break;
				}
			}
			if (!overlap) continue;
			if (!strcmp(job->spec[b].primary_source,
			            job->spec[a].secondary_source))
				continue;
			if (strcmp(job->spec[a].secondary_source,
			           job->spec[b].secondary_source)) {
				GMT_Report(GMT->parent, GMT_MSG_ERROR,
				           "Overlapping primary supports %s and %s must use "
				           "the same secondary source with -A\n",
				           job->spec[a].primary_source,
				           job->spec[b].primary_source);
				return GMT_RUNTIME_ERROR;
			}
		}
	}
	return GMT_NOERROR;
}

static bool merge1d_sign_allowed(int sign, double value, bool initialized)
{
	if (!initialized || sign == 0) return true;
	if (sign < 0) return value <= 0.0;
	return value >= 0.0;
}

static int merge1d_compute_clobber(const struct MERGE1D_CTRL *Ctrl,
                                   const struct MERGE1D_JOB *job,
                                   const double *axis,
                                   double **output, double *weights,
                                   size_t n_axis)
{
	size_t field, i, k;

	for (i = 0; i < n_axis; i++) {
		bool covered = false;
		for (k = 0; k < job->count; k++)
			if (axis[i] >= job->spec[k].primary.x[0] &&
			    axis[i] <= job->spec[k].primary.x[job->spec[k].primary.n - 1]) {
				covered = true;
				break;
			}
		for (field = 0; field < job->n_fields; field++) {
			bool set = false;
			double selected = NAN;
			for (k = 0; k < job->count; k++) {
				double value = job->spec[k].primary.sampled[field][i];
				if (!isfinite(value)) continue;
				if (!merge1d_sign_allowed(Ctrl->C.sign, value, set)) continue;
				if (!set) {
					selected = value;
					set = true;
					continue;
				}
				if (Ctrl->C.mode == MERGE1D_FIRST) continue;
				if (Ctrl->C.mode == MERGE1D_LOWER && value >= selected) continue;
				if (Ctrl->C.mode == MERGE1D_UPPER && value <= selected) continue;
				selected = value;
			}
			output[field][i] = selected;
		}
		weights[i] = covered ? 1.0 : 0.0;
	}
	return GMT_NOERROR;
}

static int merge1d_compute_regular(const struct MERGE1D_CTRL *Ctrl,
                                   const struct MERGE1D_JOB *job,
                                   const double *axis, double **output,
                                   double *weights, size_t n_axis)
{
	size_t field, i, k;

	for (i = 0; i < n_axis; i++) {
		if (merge1d_report_weight(Ctrl, job, axis[i], &weights[i]))
			return GMT_RUNTIME_ERROR;
		for (field = 0; field < job->n_fields; field++) {
			output[field][i] = NAN;
			for (k = 0; k < job->count; k++) {
				const struct MERGE1D_SPEC *spec = &job->spec[k];
				double primary, secondary = NAN, weight = 0.0;
				if (axis[i] < spec->primary.x[0] ||
				    axis[i] > spec->primary.x[spec->primary.n - 1])
					continue;
				primary = spec->primary.sampled[field][i];
				if (spec->has_secondary)
					secondary = spec->secondary.sampled[field][i];
				if (merge1d_support_weight(spec, axis[i], &weight))
					return GMT_RUNTIME_ERROR;
				if (!isfinite(primary)) {
					if (!spec->has_secondary) continue;
					if (Ctrl->P.active && isfinite(secondary))
						output[field][i] = secondary;
					break;
				}
				else if (!isfinite(secondary)) output[field][i] = primary;
				else output[field][i] = weight * primary +
				                         (1.0 - weight) * secondary;
				break;
			}
		}
	}
	return GMT_NOERROR;
}

static int merge1d_compute_aggregate(const struct MERGE1D_CTRL *Ctrl,
                                     const struct MERGE1D_JOB *job,
                                     const double *axis, double **output,
                                     double *weights, size_t n_axis)
{
	size_t field, i, k;

	for (i = 0; i < n_axis; i++) {
		size_t owner = SIZE_MAX;
		double sum_geometry = 0.0;

		for (k = 0; k < job->count; k++) {
			const struct MERGE1D_SPEC *spec = &job->spec[k];
			if (axis[i] < spec->primary.x[0] ||
			    axis[i] > spec->primary.x[spec->primary.n - 1])
				continue;
			owner = k;
			break;
		}
		if (owner < job->count && job->spec[owner].has_secondary) {
			const char *secondary = job->spec[owner].secondary_source;
			for (k = owner; k < job->count; k++) {
				double weight;
				const struct MERGE1D_SPEC *spec = &job->spec[k];
				if (!spec->has_secondary ||
				    strcmp(spec->secondary_source, secondary) ||
				    !strcmp(spec->primary_source, secondary))
					continue;
				if (axis[i] < spec->primary.x[0] ||
				    axis[i] > spec->primary.x[spec->primary.n - 1])
					continue;
				if (merge1d_support_weight(spec, axis[i], &weight))
					return GMT_RUNTIME_ERROR;
				sum_geometry += weight;
			}
		}
		if (merge1d_report_weight(Ctrl, job, axis[i], &weights[i]))
			return GMT_RUNTIME_ERROR;
		for (field = 0; field < job->n_fields; field++) {
			bool any_finite_primary = false;
			double sum_valid = 0.0, sum_values = 0.0;
			double secondary_weight = 0.0, secondary_values = 0.0;
			double background = NAN, fallback_primary = NAN;

			if (owner == SIZE_MAX) {
				output[field][i] = NAN;
				continue;
			}
			if (!job->spec[owner].has_secondary) {
				for (k = owner; k < job->count; k++) {
					const struct MERGE1D_SPEC *spec = &job->spec[k];
					if (axis[i] < spec->primary.x[0] ||
					    axis[i] > spec->primary.x[spec->primary.n - 1])
						continue;
					if (isfinite(spec->primary.sampled[field][i])) {
						output[field][i] = spec->primary.sampled[field][i];
						break;
					}
				}
				continue;
			}
			for (k = owner; k < job->count; k++) {
				const struct MERGE1D_SPEC *spec = &job->spec[k];
				double weight, primary, secondary = NAN;
				if (!spec->has_secondary ||
				    strcmp(spec->secondary_source,
				           job->spec[owner].secondary_source) ||
				    !strcmp(spec->primary_source,
				            job->spec[owner].secondary_source))
					continue;
				if (axis[i] < spec->primary.x[0] ||
				    axis[i] > spec->primary.x[spec->primary.n - 1])
					continue;
				if (merge1d_support_weight(spec, axis[i], &weight))
					return GMT_RUNTIME_ERROR;
				primary = spec->primary.sampled[field][i];
				if (isfinite(primary)) {
					any_finite_primary = true;
					if (!isfinite(fallback_primary)) fallback_primary = primary;
				}
				secondary = spec->secondary.sampled[field][i];
				if (isfinite(secondary) && !isfinite(background))
					background = secondary;
				if (isfinite(primary) && weight > 0.0) {
					sum_valid += weight;
					sum_values += weight * primary;
				}
				else if (Ctrl->P.active && isfinite(secondary) && weight > 0.0) {
					secondary_weight += weight;
					secondary_values += weight * secondary;
				}
			}
			{
				double background_weight = isfinite(background)
				                         ? MAX(0.0, 1.0 - sum_geometry) : 0.0;
				double denominator = sum_valid + secondary_weight + background_weight;
				double numerator = sum_values + secondary_values +
				                   background_weight * (isfinite(background) ? background : 0.0);
				if (denominator > 0.0 &&
				    (any_finite_primary || Ctrl->P.active))
					output[field][i] = numerator / denominator;
				else if (any_finite_primary) output[field][i] = fallback_primary;
				else output[field][i] = NAN;
			}
		}
	}
	return GMT_NOERROR;
}

static bool merge1d_netcdf_output(const char *path)
{
	const char *dot;

	if (path == NULL) return false;
	dot = strrchr(path, '.');
	return dot && !strcasecmp(dot, ".nc");
}

static int merge1d_write_text(struct GMTAPI_CTRL *API, const char *path,
                              const struct MERGE1D_CTRL *Ctrl,
                              const struct MERGE1D_JOB *job,
                              const double *axis, double **output,
                              const double *weights)
{
	FILE *fp = stdout;
	size_t i, field;
	double axis_scale = Ctrl->Z.transform.axis_scale[GQ_TRANSFORM_X];

	if (path && strcmp(path, "-")) {
		fp = fopen(path, "w");
		if (fp == NULL) {
			GMT_Report(API, GMT_MSG_ERROR, "Unable to open output %s: %s\n",
			           path, strerror(errno));
			return GMT_ERROR_ON_FOPEN;
		}
	}
	for (i = 0; i < Ctrl->T.n; i++) {
		double coordinate = axis[i] * axis_scale;
		if (coordinate == 0.0) coordinate = 0.0;
		if (fprintf(fp, "%.12g", coordinate) < 0)
			goto write_error;
		if (!Ctrl->W.only)
			for (field = 0; field < job->n_fields; field++)
				if (fprintf(fp, " %.12g", output[field][i] *
				            gq_transform_value_scale(&Ctrl->Z.transform, field)) < 0)
					goto write_error;
		if (Ctrl->W.active && fprintf(fp, " %.12g", weights[i]) < 0)
			goto write_error;
		if (fputc('\n', fp) == EOF) goto write_error;
	}
	if (fp != stdout && fclose(fp)) return GMT_DATA_WRITE_ERROR;
	return GMT_NOERROR;

write_error:
	if (fp != stdout) fclose(fp);
	return GMT_DATA_WRITE_ERROR;
}

static int merge1d_netcdf_valid_name(const char *name)
{
	const unsigned char *p = (const unsigned char *)name;

	if (!p || !*p || !(isalpha(*p) || *p == '_')) return false;
	for (p++; *p; p++)
		if (!(isalnum(*p) || *p == '_')) return false;
	return true;
}

static int merge1d_write_netcdf(struct GMTAPI_CTRL *API, const char *path,
                                const struct MERGE1D_CTRL *Ctrl,
                                const struct MERGE1D_JOB *job,
                                const double *axis, double **output,
                                const double *weights)
{
	int ncid = -1, dimid, coordinate_var, weight_var = -1;
	int *field_vars = NULL;
	size_t field;
	int status = GMT_DATA_WRITE_ERROR;
	double *axis_output = NULL, *field_output = NULL, *weight_output = NULL;
	double fill = NAN;
	float valid[2] = {0.0f, 1.0f};
	const char *coordinate = job->coordinate_name ? job->coordinate_name : "x";
	const char *coordinate_units = Ctrl->Z.transform.axis_unit[GQ_TRANSFORM_X]
	                             ? Ctrl->Z.transform.axis_unit[GQ_TRANSFORM_X]
	                             : (Ctrl->Z.transform.axis_scale[GQ_TRANSFORM_X] == 1.0
	                                ? job->coordinate_units : NULL);
	double axis_scale = Ctrl->Z.transform.axis_scale[GQ_TRANSFORM_X];

	if (!path) {
		GMT_Report(API, GMT_MSG_ERROR, "NetCDF output requires -G<file.nc>\n");
		return GMT_PARSE_ERROR;
	}
	if (!merge1d_netcdf_valid_name(coordinate) || !strcmp(coordinate, "weight"))
		coordinate = "x";
	for (field = 0; !Ctrl->W.only && field < job->n_fields; field++) {
		if (!merge1d_netcdf_valid_name(job->output_fields[field]) ||
		    !strcmp(job->output_fields[field], coordinate) ||
		    !strcmp(job->output_fields[field], "weight")) {
			GMT_Report(API, GMT_MSG_ERROR,
			           "Invalid or reserved NetCDF field name: %s\n",
			           job->output_fields[field]);
			return GMT_PARSE_ERROR;
		}
	}
	if (nc_create(path, NC_NETCDF4 | NC_CLOBBER, &ncid) != NC_NOERR ||
	    nc_def_dim(ncid, coordinate, Ctrl->T.n, &dimid) != NC_NOERR ||
	    nc_def_var(ncid, coordinate, NC_DOUBLE, 1, &dimid,
	               &coordinate_var) != NC_NOERR)
		goto cleanup;
	if (coordinate_units &&
	    nc_put_att_text(ncid, coordinate_var, "units",
	                    strlen(coordinate_units), coordinate_units) != NC_NOERR)
		goto cleanup;
	field_vars = calloc(job->n_fields, sizeof(*field_vars));
	if (field_vars == NULL) goto cleanup;
	if (!Ctrl->W.only) {
		for (field = 0; field < job->n_fields; field++) {
			const char *target = gq_transform_value_unit(&Ctrl->Z.transform, field);
			const char *units = target ? target
			                  : (gq_transform_value_scale(&Ctrl->Z.transform, field) == 1.0
			                     ? job->spec[0].primary.units[field] : NULL);
			if (nc_def_var(ncid, job->output_fields[field], NC_DOUBLE, 1,
			               &dimid, &field_vars[field]) != NC_NOERR ||
			    nc_put_att_double(ncid, field_vars[field], "_FillValue",
			                      NC_DOUBLE, 1, &fill) != NC_NOERR ||
			    nc_put_att_text(ncid, field_vars[field], "long_name",
			                    strlen(job->output_fields[field]),
			                    job->output_fields[field]) != NC_NOERR)
				goto cleanup;
			if (units && nc_put_att_text(ncid, field_vars[field], "units",
			                             strlen(units), units) != NC_NOERR)
				goto cleanup;
			nc_def_var_deflate(ncid, field_vars[field], 0, 1, 2);
		}
	}
	if (Ctrl->W.active) {
		if (nc_def_var(ncid, "weight", NC_DOUBLE, 1, &dimid,
		               &weight_var) != NC_NOERR ||
		    nc_put_att_double(ncid, weight_var, "_FillValue", NC_DOUBLE, 1,
		                      &fill) != NC_NOERR ||
		    nc_put_att_text(ncid, weight_var, "long_name",
		                    strlen("merging weight"),
		                    "merging weight") != NC_NOERR ||
		    nc_put_att_text(ncid, weight_var, "units", 1, "1") != NC_NOERR ||
		    nc_put_att_float(ncid, weight_var, "valid_range", NC_FLOAT, 2,
		                     valid) != NC_NOERR)
			goto cleanup;
		nc_def_var_deflate(ncid, weight_var, 0, 1, 2);
	}
	axis_output = calloc(Ctrl->T.n, sizeof(*axis_output));
	field_output = calloc(Ctrl->T.n, sizeof(*field_output));
	weight_output = calloc(Ctrl->T.n, sizeof(*weight_output));
	if (!axis_output || !field_output || !weight_output) {
		status = GMT_MEMORY_ERROR;
		goto cleanup;
	}
	for (field = 0; field < Ctrl->T.n; field++) {
		axis_output[field] = axis[field] * axis_scale;
		if (axis_output[field] == 0.0) axis_output[field] = 0.0;
		weight_output[field] = weights[field];
	}
	if (nc_put_att_text(ncid, NC_GLOBAL, "Conventions", strlen("CF-1.8"),
	                    "CF-1.8") != NC_NOERR ||
	    nc_put_att_text(ncid, NC_GLOBAL, "history",
	                    strlen("Created by GMT merge1d"),
	                    "Created by GMT merge1d") != NC_NOERR ||
	    nc_enddef(ncid) != NC_NOERR ||
	    nc_put_var_double(ncid, coordinate_var, axis_output) != NC_NOERR)
		goto cleanup;
	if (!Ctrl->W.only)
		for (field = 0; field < job->n_fields; field++) {
			size_t i;
			double scale = gq_transform_value_scale(&Ctrl->Z.transform, field);
			for (i = 0; i < Ctrl->T.n; i++)
				field_output[i] = output[field][i] * scale;
			if (nc_put_var_double(ncid, field_vars[field], field_output) != NC_NOERR)
				goto cleanup;
		}
	if (Ctrl->W.active &&
	    nc_put_var_double(ncid, weight_var, weight_output) != NC_NOERR)
		goto cleanup;
	status = GMT_NOERROR;

cleanup:
	free(axis_output);
	free(field_output);
	free(weight_output);
	free(field_vars);
	if (ncid >= 0 && nc_close(ncid) != NC_NOERR) status = GMT_DATA_WRITE_ERROR;
	if (status != GMT_NOERROR)
		GMT_Report(API, GMT_MSG_ERROR, "Unable to write NetCDF output %s\n", path);
	return status;
}

static int merge1d_run(struct GMT_CTRL *GMT, struct MERGE1D_CTRL *Ctrl)
{
	struct MERGE1D_JOB job;
	double *axis = NULL, **output = NULL, *weights = NULL;
	size_t i, field;
	bool netcdf_output;
	char stdin_temp[PATH_MAX] = {""};
	int status;

	memset(&job, 0, sizeof(job));
	status = merge1d_materialize_stdin(GMT, Ctrl, stdin_temp);
	if (status != GMT_NOERROR) goto cleanup;
	status = merge1d_specs_from_inputs(GMT, Ctrl, &job);
	if (status != GMT_NOERROR) goto cleanup;
	netcdf_output = merge1d_netcdf_output(Ctrl->G.file);
	axis = calloc(Ctrl->T.n, sizeof(*axis));
	if (axis == NULL) {
		status = GMT_MEMORY_ERROR;
		goto cleanup;
	}
	for (i = 0; i < Ctrl->T.n; i++)
		axis[i] = Ctrl->T.min + (double)i * Ctrl->T.inc;
	status = merge1d_prepare_job(GMT, Ctrl, &job, axis, netcdf_output);
	if (status != GMT_NOERROR) goto cleanup;
	{
		char message[GMT_LEN256];
		if (gq_transform_validate_values(&Ctrl->Z.transform, job.n_fields,
		                                  message, sizeof(message))) {
			GMT_Report(GMT->parent, GMT_MSG_ERROR, "Option -Z: %s\n", message);
			status = GMT_PARSE_ERROR;
			goto cleanup;
		}
	}
	if (Ctrl->A.active) {
		status = merge1d_validate_overlap_axis(GMT, &job, axis, Ctrl->T.n);
		if (status != GMT_NOERROR) goto cleanup;
	}
	output = calloc(job.n_fields, sizeof(*output));
	weights = calloc(Ctrl->T.n, sizeof(*weights));
	if (!output || !weights) {
		status = GMT_MEMORY_ERROR;
		goto cleanup;
	}
	for (field = 0; field < job.n_fields; field++) {
		output[field] = calloc(Ctrl->T.n, sizeof(*output[field]));
		if (output[field] == NULL) {
			status = GMT_MEMORY_ERROR;
			goto cleanup;
		}
		for (i = 0; i < Ctrl->T.n; i++) output[field][i] = NAN;
	}
	if (Ctrl->C.active)
		status = merge1d_compute_clobber(Ctrl, &job, axis, output, weights,
		                                 Ctrl->T.n);
	else if (Ctrl->A.active)
		status = merge1d_compute_aggregate(Ctrl, &job, axis, output, weights,
		                                   Ctrl->T.n);
	else
		status = merge1d_compute_regular(Ctrl, &job, axis, output, weights,
		                                 Ctrl->T.n);
	if (status != GMT_NOERROR) goto cleanup;
	status = netcdf_output
	       ? merge1d_write_netcdf(GMT->parent, Ctrl->G.file, Ctrl, &job,
	                              axis, output, weights)
	       : merge1d_write_text(GMT->parent, Ctrl->G.file, Ctrl, &job,
	                           axis, output, weights);

cleanup:
	if (stdin_temp[0]) gmt_remove_file(GMT, stdin_temp);
	if (output)
		for (field = 0; field < job.n_fields; field++) free(output[field]);
	free(output);
	free(weights);
	free(axis);
	merge1d_job_free(&job);
	return status;
}

#define bailout(code) { gmt_M_free_options(mode); return (code); }
#define Return(code) { Free_Ctrl(GMT, Ctrl); gmt_end_module(GMT, GMT_cpy); bailout(code); }

EXTERN_MSC int GMT_merge1d(void *V_API, int mode, void *args)
{
	struct GMTAPI_CTRL *API = gmt_get_api_ptr(V_API);
	struct GMT_CTRL *GMT = NULL, *GMT_cpy = NULL;
	struct GMT_OPTION *options = NULL;
	struct MERGE1D_CTRL *Ctrl = NULL;
	int error, status;

	if (API == NULL) return GMT_NOT_A_SESSION;
	if (mode == GMT_MODULE_PURPOSE) return usage(API, GMT_MODULE_PURPOSE);
	options = GMT_Create_Options(API, mode, args);
	if (API->error) return API->error;
	if ((error = gmt_report_usage(API, options, 0, usage)) != GMT_NOERROR)
		bailout(error);
	if ((GMT = gmt_init_module(API, THIS_MODULE_LIB, THIS_MODULE_CLASSIC_NAME,
	                           THIS_MODULE_KEYS, THIS_MODULE_NEEDS, module_kw,
	                           &options, &GMT_cpy)) == NULL)
		bailout(API->error);
	if (GMT_Parse_Common(API, THIS_MODULE_OPTIONS, options))
		Return(API->error);
	Ctrl = New_Ctrl(GMT);
	if ((error = parse(GMT, Ctrl, options)) != GMT_NOERROR)
		Return(error);
	status = merge1d_run(GMT, Ctrl);
	Return(status);
}
