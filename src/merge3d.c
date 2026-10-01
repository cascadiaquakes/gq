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
/*
 * merge3d combines primary and secondary NetCDF cubes using extruded BLEND
 * supports.  Variables are mapped positionally, source z coordinates may be
 * scaled independently, and output is streamed one horizontal layer at a time.
 */

#include "gmt_dev.h"
#include "gq_remote.h"
#include "gq_transform.h"
#include "merge3d_inc.h"
#include <blend/blend.h>
#include <netcdf.h>

#define THIS_MODULE_CLASSIC_NAME "merge3d"
#define THIS_MODULE_MODERN_NAME "merge3d"
#define THIS_MODULE_LIB "gq"
#define THIS_MODULE_LIB_PURPOSE "The CRESCENT cyberinfrastructure team supplements to the Generic Mapping Tools"
#define THIS_MODULE_PURPOSE "Tile or smoothly merge three-dimensional multiparameter NetCDF cubes"
#define THIS_MODULE_KEYS "<G{+,GG}"
#define THIS_MODULE_NEEDS "R"
#define THIS_MODULE_OPTIONS "RVdfn"

enum MERGE3D_AXIS {
	MERGE3D_X = 0,
	MERGE3D_Y,
	MERGE3D_Z
};

enum MERGE3D_CLOBBER {
	MERGE3D_UPPER = 0,
	MERGE3D_LOWER,
	MERGE3D_FIRST,
	MERGE3D_LAST
};

struct MERGE3D_GAP {
	bool active;
	char method;
	double argument;
	unsigned int sectors;
	bool limited;
	unsigned int max_gap;
};

struct MERGE3D_CTRL {
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
	struct MERGE3D_GAP H;
	struct {
		bool active;
		double inc[2];
	} I;
	struct {
		bool active;
		char method;
		bool write;
	} M;
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

struct MERGE3D_SOURCE_PARTS {
	char *path;
	char **names;
	size_t count;
	bool explicit_selector;
	bool has_sentinel;
	double sentinel;
	struct GQ_TRANSFORM transform;
};

struct MERGE3D_FIELD {
	int varid;
	int dimids[3];
	int axis_position[3];
	char *name;
	char *units;
};

struct MERGE3D_CUBE {
	char *source;
	char *path;
	int ncid;
	bool explicit_selector;
	bool has_sentinel;
	double sentinel;
	struct GQ_TRANSFORM transform;
	size_t n_fields;
	struct MERGE3D_FIELD *field;
	int axis_dimid[3];
	int coordinate_varid[3];
	size_t n[3];
	double *coordinate[3];
	char *coordinate_name[3];
	char *coordinate_units[3];
};

struct MERGE3D_SPEC {
	char *primary_source;
	char *secondary_source;
	char *polygon_file;
	bool has_secondary;
	bool has_polygon;
	bool has_zrange;
	double zlo;
	double zhi;
	blend_window_function function[3];
	double ratio[6];
	struct MERGE3D_CUBE primary;
	struct MERGE3D_CUBE secondary;
	window support;
	polygon real_support;
	bool support_ready;
	int support_i0;
	int support_i1;
	int support_j0;
	int support_j1;
	int support_k0;
	int support_k1;
};

struct MERGE3D_JOB {
	struct MERGE3D_SPEC *spec;
	size_t count;
	bool mergefile;
	size_t n_fields;
	char **output_fields;
	char **output_units;
	double wesn[4];
	double inc[2];
	size_t nx;
	size_t ny;
	double *x;
	double *y;
	double *z;
};

static void merge3d_names_free(char **names, size_t count)
{
	size_t k;
	for (k = 0; k < count; k++) free(names[k]);
	free(names);
}

static void merge3d_source_parts_free(struct MERGE3D_SOURCE_PARTS *parts)
{
	if (parts == NULL) return;
	free(parts->path);
	merge3d_names_free(parts->names, parts->count);
	gq_transform_free(&parts->transform);
	memset(parts, 0, sizeof(*parts));
}

static void merge3d_cube_free(struct MERGE3D_CUBE *cube)
{
	size_t axis, field;

	if (cube == NULL) return;
	if (cube->ncid >= 0) nc_close(cube->ncid);
	free(cube->source);
	free(cube->path);
	for (field = 0; field < cube->n_fields; field++) {
		free(cube->field[field].name);
		free(cube->field[field].units);
	}
	free(cube->field);
	for (axis = 0; axis < 3; axis++) {
		free(cube->coordinate[axis]);
		free(cube->coordinate_name[axis]);
	free(cube->coordinate_units[axis]);
	}
	gq_transform_free(&cube->transform);
	memset(cube, 0, sizeof(*cube));
	cube->ncid = -1;
}

static void merge3d_job_free(struct MERGE3D_JOB *job)
{
	size_t k, field;

	if (job == NULL) return;
	for (k = 0; k < job->count; k++) {
		free(job->spec[k].primary_source);
		free(job->spec[k].secondary_source);
		free(job->spec[k].polygon_file);
		merge3d_cube_free(&job->spec[k].primary);
		merge3d_cube_free(&job->spec[k].secondary);
		if (job->spec[k].support_ready)
			blend_window_boundary_clear(&job->spec[k].support);
		blend_polygon_free(&job->spec[k].real_support);
	}
	free(job->spec);
	for (field = 0; field < job->n_fields; field++) {
		free(job->output_fields ? job->output_fields[field] : NULL);
		free(job->output_units ? job->output_units[field] : NULL);
	}
	free(job->output_fields);
	free(job->output_units);
	free(job->x);
	free(job->y);
	free(job->z);
	memset(job, 0, sizeof(*job));
}

static void *New_Ctrl(struct GMT_CTRL *GMT)
{
	struct MERGE3D_CTRL *Ctrl = gmt_M_memory(GMT, NULL, 1, struct MERGE3D_CTRL);

	Ctrl->C.mode = MERGE3D_FIRST;
	Ctrl->H.method = 'l';
	Ctrl->H.sectors = 4;
	Ctrl->M.method = 'E';
	Ctrl->S.mode = GMT_SPLINE_LINEAR;
	gq_transform_init(&Ctrl->Z.transform);
	return Ctrl;
}

static void Free_Ctrl(struct GMT_CTRL *GMT, struct MERGE3D_CTRL *Ctrl)
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
	          "usage: %s [<mergefile> | <cube1> <cube2> ...] "
	          "-R<w/e/s/n> -I<dx>[/<dy>] -T<zmin>/<zmax>/<dz> -G<output.nc> "
	          "[-A] [-Cf|l|o|u[+n|p]] [-F<fields>] "
	          "[-H[n|l|a|s|m[<arg>]][+m<maxgap>]] [-ME|B[+w]] [-P] "
	          "[-Sa|c|e|l|n|s<p>[+g[<maxgap>]]] [-W[+o]] "
	          "[-Z+x<sx>[+X<xunit>]+y<sy>[+Y<yunit>]"
	          "+z<sz>[+Z<zunit>]+v<scales>[+V<units>]] "
	          "[%s] [%s] [%s]\n",
	          name, GMT_V_OPT, GMT_di_OPT, GMT_n_OPT);
	if (level == GMT_SYNOPSIS) return GMT_MODULE_SYNOPSIS;

	GMT_Message(API, GMT_TIME_NONE, "  REQUIRED ARGUMENTS:\n");
	GMT_Usage(API, 1, "\n<mergefile> | <cube1> <cube2> ...");
	GMT_Usage(API, -2,
	          "Supply one mergefile or list one or more NetCDF cubes directly. "
	          "Omit the input argument to read a mergefile from standard input. "
	          "Direct inputs tile values in availability order and use first-value "
	          "clobbering by default.");
	GMT_Usage(API, 3,
	          "Each non-comment mergefile record contains up to six whitespace-separated fields:");
	GMT_Usage(API, 3,
	          "primary: Required NetCDF source providing the primary field or fields.");
	GMT_Usage(API, 3,
	          "secondary: Optional NetCDF source paired with primary for merging. Use '-' "
	          "for an unpaired fallback tile. Pairing is valid only within the primary "
	          "horizontal and vertical support and does not extend the primary domain.");
	GMT_Usage(API, 3,
	          "polygon: Optional xy polygon defining the horizontal primary support. Use '-' "
	          "or omit it to use the complete primary horizontal domain.");
	GMT_Usage(API, 3,
	          "zlo/zhi: Optional vertical primary support in transformed input coordinates, "
	          "with zlo < zhi. The default is the complete transformed primary z range.");
	GMT_Usage(API, 3,
	          "xwindow/ywindow/zwindow: BLEND window functions. One name applies to all "
	          "axes. With two names, x also applies to z. Three names set x, y, and z. "
	          "The default is cosine/cosine/cosine.");
	GMT_Usage(API, 3,
	          "rx1/rx2/ry1/ry2/rz1/rz2: Dimensionless taper ratios in [0, 0.5). One value applies "
	          "everywhere, three apply symmetrically to x, y, and z, and six set every "
	          "beginning and ending ratio independently. The default is 0.2 everywhere.");
	GMT_Usage(API, 3,
	          "Each ratio sets the fraction of the corresponding support extent used by "
	          "the transition at one boundary. rx1/rx2 apply at west/east (low/high x), "
	          "ry1/ry2 at south/north (low/high y), and rz1/rz2 at zlo/zhi. For a "
	          "vertical support 0/40, rz1/rz2 = 0/0.2 disables the zlo taper and uses "
	          "approximately the final 8 coordinate units for the zhi taper.");
	GMT_Usage(API, 3,
	          "Within each transition, the selected window controls how the primary "
	          "merging-weight factor changes between its boundary value and 1. The paired "
	          "secondary receives the complementary weight. The x, y, and z factors are "
	          "multiplied. Larger ratios give broader transitions and a smaller full-primary "
	          "interior. 0 disables the taper on that side. Boxcar ignores taper ratios and "
	          "has unit weight throughout the support.");
	GMT_Usage(API, 3,
	          "For the polygonal horizontal support, BLEND evaluates x and y tapers along "
	          "local polygon cross-sections and adapts the transition width where a "
	          "cross-section is too narrow for the nominal support-wide taper. The z taper "
	          "uses the complete zlo/zhi support interval.");
	GMT_Usage(API, 3,
	          "Use '-' to skip an optional field when supplying a later field. Trailing "
	          "optional fields may be omitted. Blank lines and text after '#' are ignored.");
	GMT_Usage(API, 3, "Example mergefile:");
	GMT_Usage(API, 3,
	          "  primary.nc?vp,vs secondary.nc?p,s support.txt 0/40 cosine/cosine/cosine 0.2/0.2/0");
	GMT_Usage(API, 3, "  secondary.nc?p,s - - - -");
	GMT_Usage(API, 3,
	          "Select NetCDF fields with file.nc?field1,field2,... . Selected fields "
	          "must share the same x, y, and z coordinates. If ? is omitted, the first "
	          "eligible three-dimensional data variable is used.");
	GMT_Usage(API, 3,
	          "Append input modifiers after the field list. +x, +y, and +z scale source "
	          "coordinates. +v supplies one broadcast field scale or one scale per selected "
	          "field. +X, +Y, +Z, and +V set target-unit metadata. and +n declares an "
	          "additional missing-value sentinel. Input transforms are applied after NetCDF "
	          "packing is decoded and before interpolation, support tests, and merging.");
	GMT_Usage(API, 3,
	          "Use file.nc?<field1,field2,...>+<modifiers> for named fields or "
	          "file.nc?+<modifiers> to transform the default field without naming it.");
	GMT_Usage(API, 3,
	          "Input coordinate scaling does not reorder coordinates or cube data. "
	          "Every transformed input axis must be strictly increasing. For a "
	          "decreasing source axis, use a negative input scale to make it "
	          "increase. use output -Z to restore a decreasing convention after merging.");
	GMT_Usage(API, 3,
	          "Example: model.nc?vp,vs+x0.001+Xkm+y0.001+Ykm+z-1/1000+Zkm"
	          "+v0.001,0.001+Vkm/s,km/s selects vp and vs, scales x and y from m to km, "
	          "converts a source z axis of 4000 ... -16000 m to -4 ... 16 km without "
	          "reordering the cube, and scales both fields from m/s to km/s. Output "
	          "-Z+z-1000+Zm restores the original z convention without reordering.");
	GMT_Option(API, "R");
	GMT_Usage(API, -2, "Set the required horizontal output domain. It must overlap at least one input cube.");
	GMT_Usage(API, 1, "\n-I<dx>[/<dy>]");
	GMT_Usage(API, -2, "Set the required horizontal output increments. One value applies to x and y. "
	          "Two values set them independently.");
	GMT_Usage(API, 1, "\n-T<zmin>/<zmax>/<dz>");
	GMT_Usage(API, -2,
	          "Set the required regular output z axis. zmin must be less than zmax and dz "
	          "is the increment. Values use the standardized coordinate system established "
	          "by input +z transforms. The maximum is adjusted when necessary to fit dz.");
	GMT_Usage(API, 1, "\n-G<output.nc>");
	GMT_Usage(API, -2, "Write a NetCDF cube with every selected field sharing dimensions (z,y,x).");

	GMT_Message(API, GMT_TIME_NONE, "\n  OPTIONAL ARGUMENTS:\n");
	GMT_Usage(API, 1, "\n-A");
	GMT_Usage(API, -2,
	          "Normalize positive weights where primary supports overlap. "
	          "Only primaries that overlap must share the same secondary cube. "
	          "A later record for that secondary starts a lower-priority layer. "
	          "Non-overlapping primaries may use different secondaries, and unpaired "
	          "records remain fallback tiles.");
	GMT_Usage(API, 1, "\n-Cf|l|o|u[+n|p]");
	GMT_Usage(API, -2, "Select clobber/tiling mode instead of weighted merging:");
	GMT_Usage(API, 3,
	          "f: Keep the first available value. This is the default for direct cube lists.");
	GMT_Usage(API, 3, "l: Keep the lowest available value.");
	GMT_Usage(API, 3, "o: Keep the last available value.");
	GMT_Usage(API, 3, "u: Keep the highest available value.");
	GMT_Usage(API, 3, "+n: Only consider non-positive values for clobbering.");
	GMT_Usage(API, 3, "+p: Only consider non-negative values for clobbering.");
	GMT_Usage(API, 1, "\n-F<field1,field2,...>");
	GMT_Usage(API, -2,
	          "Set NetCDF output variable names. -F does not select source variables: "
	          "fields selected with ? map positionally to -F. Thus "
	          "model1.nc?vp,vs,den and model2.nc?p,s,d may be merged with "
	          "-Fvp,vs,rho when the differently named fields are equivalent. Without "
	          "-F, output retains the first primary's field names and every source must "
	          "use those same names.");
	GMT_Usage(API, 1, "\n-H[n|l|a|s|m[<arg>]][+m<maxgap>]");
	GMT_Usage(API, -2,
	          "Fill strictly internal horizontal missing-data holes in every native "
	          "x-y layer before vertical interpolation, resampling, and merging. Original "
	          "non-missing nodes and boundary-connected gaps are preserved. Without -H, "
	          "horizontal holes are not filled. Use linear Delaunay interpolation when -H "
	          "has no directive. Input cubes used with -H must have regularly spaced x "
	          "and y coordinates.");
	GMT_Usage(API, 3, "Nearest neighbor (n). Optionally append a search radius in grid nodes.");
	GMT_Usage(API, 3, "Linear Delaunay interpolation (l). This is the default.");
	GMT_Usage(API, 3,
	          "Local weighted average (a). Append <radius>[/<sectors>] in grid nodes. "
	          "The default is 3/4.");
	GMT_Usage(API, 3,
	          "Spline interpolation (s). Optionally append tension in the range 0-1. "
	          "The default is 0.");
	GMT_Usage(API, 3,
	          "Minimum-curvature interpolation (m). Optionally append tension in the "
	          "range 0-1. The default is 0.");
	GMT_Usage(API, 3,
	          "+m Only fill holes whose horizontal and vertical spans are both no larger "
	          "than <maxgap> grid nodes. The default is to fill all internal holes.");
	GMT_Usage(API, 1, "\n-ME|B[+w]");
	GMT_Usage(API, -2,
	          "Convert non-monotone xy polygons with the strict envelope (E) "
	          "or best piecewise envelope (B). Append +w to write a converted polygon "
	          "with a _monotone suffix. Without -M, supplied polygons must already be "
	          "strictly xy-monotone.");
	GMT_Usage(API, 1, "\n-P");
	GMT_Usage(API, -2,
	          "Fill primary values that remain missing after interpolation from "
	          "their non-missing paired secondary values. By default, primary NaNs "
	          "are preserved.");
	GMT_Usage(API, 1, "\n-Sa|c|e|l|n|s<p>[+g[<maxgap>]]");
	GMT_Usage(API, -2,
	          "Choose GMT vertical interpolation: Akima (a), cubic (c), step-up (e), "
	          "linear (l), nearest (n), or smoothing spline (s<p>) with non-negative "
	          "fit parameter p. Linear (l) is the default. "
	          "Interpolation remains within contiguous non-missing runs. Append +g to bridge "
	          "internal missing layers, optionally only when the bracketing "
	          "z-coordinate distance does not exceed maxgap. Values are not extrapolated "
	          "outside the available vertical range.");
	GMT_Option(API, "V");
	GMT_Usage(API, 1, "\n-W[+o]");
	GMT_Usage(API, -2,
	          "Include the shared NetCDF variable weight with long_name='merging weight' "
	          "and units='1'. Append +o to write only coordinates and weight. All output "
	          "fields use this same weight.");
	GMT_Usage(API, 3, "Weights follow paired supports in mergefile order. The first support "
	          "containing a node supplies its primary weight; outside it, later supports remain "
	          "visible. Zero-valued support boundaries are retained. With -A, sum and cap weights "
	          "at 1 only where positive weights overlap with the same secondary. Unpaired "
	          "background weights are 0. These shared taper weights do not represent final "
	          "per-source fractions or field-specific missing-value replacements.");
	GMT_Usage(API, 1,
	          "\n-Z[+x<sx>][+X<xunit>][+y<sy>][+Y<yunit>]"
	          "[+z<sz>][+Z<zunit>][+v<scales>][+V<units>]");
	GMT_Usage(API, -2,
	          "Transform output coordinates and fields after merging. +x, +y, and +z "
	          "scale output coordinates. +X, +Y, and +Z set coordinate units. +v "
	          "supplies one broadcast scale or one scale per output field. and +V sets "
	          "field units. Field lists follow -F order when -F is used and selector "
	          "order otherwise. -Z scales output coordinates and "
	          "fields in place and does not reorder coordinates, fields, or weight. "
	          "A negative axis scale therefore produces a decreasing output axis. "
	          "Weight is not scaled.");
	GMT_Usage(API, 3,
	          "Example: with -Fvp,vs, -Z+x0.001+Xkm+y0.001+Ykm+z-0.001"
	          "+Zkm+v0.001,0.001+Vkm/s,km/s converts x and y from m to km, "
	          "converts a negative-down z axis in m to positive-down km, and "
	          "converts vp and vs from m/s to km/s without reordering the cube.");
	GMT_Option(API, "di");
	GMT_Option(API, "n");
	if (gmt_M_showusage(API)) {
		GMT_Usage(API, -2,
		          "Control horizontal x-y interpolation after -S has interpolated each "
		          "native vertical column to an output z layer. Use nearest neighbor (-nn), "
		          "bilinear (-nl), bicubic (-nc), or B-spline (-nb). Bicubic is the default.");
		GMT_Usage(API, 3,
		          "Regular horizontal grids use GMT's two-dimensional interpolation and "
		          "the selected -n modifiers. Cubes with irregular x or y coordinates use "
		          "separable x-then-y GMT splines. In that fallback, -nb and -nc both use "
		          "cubic interpolation.");
		GMT_Usage(API, 3,
		          "Option -n does not replace or conflict with -H. Processing follows input "
		          "scaling, -H filling of internal holes in native x-y layers, -S vertical "
		          "interpolation to an output z layer, -n horizontal resampling to the -R/-I "
		          "grid, and then merging. Without -H, -n resamples available values but does "
		          "not deliberately reconstruct internal horizontal holes.");
		GMT_Usage(API, 3,
		          "Example: -Hl -Sl+g -nl fills native x-y holes with linear Delaunay "
		          "interpolation, bridges internal z gaps linearly, and resamples each "
		          "output layer horizontally with bilinear interpolation.");
	}
	GMT_Option(API, ".");
	return GMT_MODULE_USAGE;
}

static int merge3d_parse_number(const char *text, double *value)
{
	char copy[GMT_LEN128], *slash, *end = NULL;
	double numerator, denominator = 1.0;

	if (text == NULL || !text[0] || strlen(text) >= sizeof(copy))
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

static int merge3d_parse_increment(struct GMTAPI_CTRL *API, const char *text,
                                   double inc[2])
{
	char copy[GMT_LEN128], *slash;

	if (text == NULL || strlen(text) >= sizeof(copy)) return GMT_PARSE_ERROR;
	strcpy(copy, text);
	slash = strchr(copy, '/');
	if (slash) *slash++ = '\0';
	if (merge3d_parse_number(copy, &inc[0]) ||
	    (slash && merge3d_parse_number(slash, &inc[1]))) {
		GMT_Report(API, GMT_MSG_ERROR, "Option -I: Invalid increment %s\n", text);
		return GMT_PARSE_ERROR;
	}
	if (!slash) inc[1] = inc[0];
	if (inc[0] <= 0.0 || inc[1] <= 0.0) {
		GMT_Report(API, GMT_MSG_ERROR, "Option -I increments must be positive\n");
		return GMT_PARSE_ERROR;
	}
	return GMT_NOERROR;
}

static int merge3d_parse_zrange(struct GMTAPI_CTRL *API, const char *text,
                                struct MERGE3D_CTRL *Ctrl)
{
	char copy[GMT_LEN256], *token = NULL, *save = NULL;
	double value[3], intervals, adjusted, tolerance;
	size_t n = 0;

	if (text == NULL || strlen(text) >= sizeof(copy)) return GMT_PARSE_ERROR;
	strcpy(copy, text);
	for (token = strtok_r(copy, "/", &save); token && n < 3;
	     token = strtok_r(NULL, "/", &save)) {
		if (merge3d_parse_number(token, &value[n])) break;
		n++;
	}
	if (token || n != 3 || value[0] >= value[1] || value[2] <= 0.0) {
		GMT_Report(API, GMT_MSG_ERROR,
		           "Option -T must be zmin/zmax/dz with zmin < zmax and dz > 0\n");
		return GMT_PARSE_ERROR;
	}
	intervals = (value[1] - value[0]) / value[2];
	if (intervals > (double)(SIZE_MAX - 1)) return GMT_DIM_TOO_LARGE;
	Ctrl->T.n = (size_t)floor(intervals + 0.5) + 1;
	if (Ctrl->T.n < 2) return GMT_DIM_TOO_SMALL;
	adjusted = value[0] + (double)(Ctrl->T.n - 1) * value[2];
	tolerance = 32.0 * DBL_EPSILON *
	            MAX(1.0, MAX(fabs(value[1]), fabs(adjusted)));
	if (fabs(adjusted - value[1]) > tolerance)
		GMT_Report(API, GMT_MSG_WARNING,
		           "Option -T: Adjusting maximum from %.12g to %.12g\n",
		           value[1], adjusted);
	Ctrl->T.min = value[0];
	Ctrl->T.max = adjusted;
	Ctrl->T.inc = value[2];
	return GMT_NOERROR;
}

static int merge3d_parse_interpolation(struct GMTAPI_CTRL *API,
                                       const char *text,
                                       struct MERGE3D_CTRL *Ctrl)
{
	char copy[GMT_LEN128], *modifier = NULL, *end = NULL;

	if (text == NULL || !text[0] || strlen(text) >= sizeof(copy)) goto bad;
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
			if (!copy[1]) goto bad;
			errno = 0;
			Ctrl->S.fit = strtod(copy + 1, &end);
			if (errno || end == copy + 1 || *end ||
			    !isfinite(Ctrl->S.fit) || Ctrl->S.fit < 0.0)
				goto bad;
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

static int merge3d_parse_gap_option(struct GMTAPI_CTRL *API, const char *text,
	                                struct MERGE3D_GAP *H)
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
	if (!strchr("nlasm", H->method)) {
		GMT_Report(API, GMT_MSG_ERROR,
		           "Option -H method must be n, l, a, s, or m\n");
		return GMT_PARSE_ERROR;
	}
	modifier = p ? strchr(p, '+') : NULL;
	length = modifier ? (size_t)(modifier - p) : (p ? strlen(p) : 0);
	if (length >= sizeof(value)) return GMT_PARSE_ERROR;
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
					return GMT_PARSE_ERROR;
				H->sectors = (unsigned int)sectors;
			}
		}
		errno = 0;
		H->argument = strtod(value, &end);
		if (errno || end == value || *end || !isfinite(H->argument))
			return GMT_PARSE_ERROR;
	}
	if (H->method == 'a' && H->argument <= 0.0) H->argument = 3.0;
	if (H->method == 'n' && H->argument < 0.0) return GMT_PARSE_ERROR;
	if ((H->method == 's' || H->method == 'm') &&
	    (H->argument < 0.0 || H->argument > 1.0)) {
		GMT_Report(API, GMT_MSG_ERROR,
		           "Option -H spline tension must be between 0 and 1\n");
		return GMT_PARSE_ERROR;
	}
	if (H->method == 'l' && length) {
		GMT_Report(API, GMT_MSG_ERROR,
		           "Option -Hl does not accept a method argument\n");
		return GMT_PARSE_ERROR;
	}
	for (p = modifier; p && *p; ) {
		unsigned long gap;
		if (p[0] != '+' || p[1] != 'm') {
			GMT_Report(API, GMT_MSG_ERROR,
			           "Option -H only accepts the +m<maxgap> modifier\n");
			return GMT_PARSE_ERROR;
		}
		p += 2;
		gap = strtoul(p, &end, 10);
		if (end == p || gap < 1 || gap > UINT_MAX || (*end && *end != '+')) {
			GMT_Report(API, GMT_MSG_ERROR,
			           "Option -H +m requires a positive grid-node span\n");
			return GMT_PARSE_ERROR;
		}
		H->limited = true;
		H->max_gap = (unsigned int)gap;
		p = end;
	}
	return GMT_NOERROR;
}

static int merge3d_parse_output_scale(struct GMTAPI_CTRL *API,
                                      const char *text,
                                      struct MERGE3D_CTRL *Ctrl)
{
	char message[GMT_LEN256];
	if (!text || text[0] != '+' ||
	    gq_transform_parse(text,
	                       GQ_TRANSFORM_X_MASK | GQ_TRANSFORM_Y_MASK |
	                       GQ_TRANSFORM_Z_MASK,
	                       false, &Ctrl->Z.transform, NULL, NULL,
	                       message, sizeof(message))) {
		GMT_Report(API, GMT_MSG_ERROR,
		           "Option -Z requires explicit coordinate/value modifiers: %s\n",
		           message);
		return GMT_PARSE_ERROR;
	}
	return GMT_NOERROR;
}

static int parse(struct GMT_CTRL *GMT, struct MERGE3D_CTRL *Ctrl,
                 struct GMT_OPTION *options)
{
	struct GMT_OPTION *opt;
	struct GMTAPI_CTRL *API = GMT->parent;
	unsigned int n_errors = 0;

	for (opt = options; opt; opt = opt->next) {
		switch (opt->option) {
			case '<': {
				char **next = realloc(Ctrl->In.file,
				                     (Ctrl->In.n + 1) * sizeof(*next));
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
					case 'f': Ctrl->C.mode = MERGE3D_FIRST; break;
					case 'l': Ctrl->C.mode = MERGE3D_LOWER; break;
					case 'o': Ctrl->C.mode = MERGE3D_LAST; break;
					case 'u': Ctrl->C.mode = MERGE3D_UPPER; break;
					default: n_errors++; break;
				}
				if (!opt->arg[1]) Ctrl->C.sign = 0;
				else if (!strcmp(opt->arg + 1, "+n")) Ctrl->C.sign = -1;
				else if (!strcmp(opt->arg + 1, "+p")) Ctrl->C.sign = 1;
				else n_errors++;
				break;
			case 'F':
				n_errors += gmt_M_repeated_module_option(API, Ctrl->F.active);
				if (!opt->arg[0]) n_errors++;
				else Ctrl->F.fields = strdup(opt->arg);
				break;
			case 'G':
				n_errors += gmt_M_repeated_module_option(API, Ctrl->G.active);
				if (!opt->arg[0]) n_errors++;
				else Ctrl->G.file = strdup(opt->arg);
				break;
			case 'H':
				n_errors += gmt_M_repeated_module_option(API, Ctrl->H.active);
				n_errors += merge3d_parse_gap_option(API, opt->arg, &Ctrl->H);
				break;
			case 'I':
				n_errors += gmt_M_repeated_module_option(API, Ctrl->I.active);
				n_errors += merge3d_parse_increment(API, opt->arg, Ctrl->I.inc);
				break;
			case 'M':
				n_errors += gmt_M_repeated_module_option(API, Ctrl->M.active);
				if ((opt->arg[0] != 'E' && opt->arg[0] != 'B') ||
				    (opt->arg[1] && strcmp(opt->arg + 1, "+w")))
					n_errors++;
				else {
					Ctrl->M.method = opt->arg[0];
					Ctrl->M.write = strstr(opt->arg, "+w") != NULL;
				}
				break;
			case 'P':
				n_errors += gmt_M_repeated_module_option(API, Ctrl->P.active);
				n_errors += gmt_get_no_argument(GMT, opt->arg, opt->option, 0);
				break;
			case 'S':
				n_errors += gmt_M_repeated_module_option(API, Ctrl->S.active);
				n_errors += merge3d_parse_interpolation(API, opt->arg, Ctrl);
				break;
			case 'T':
				n_errors += gmt_M_repeated_module_option(API, Ctrl->T.active);
				n_errors += merge3d_parse_zrange(API, opt->arg, Ctrl);
				break;
			case 'W':
				n_errors += gmt_M_repeated_module_option(API, Ctrl->W.active);
				if (opt->arg[0] && strcmp(opt->arg, "+o")) n_errors++;
				Ctrl->W.only = strstr(opt->arg, "+o") != NULL;
				break;
			case 'Z':
				n_errors += gmt_M_repeated_module_option(API, Ctrl->Z.active);
				n_errors += merge3d_parse_output_scale(API, opt->arg, Ctrl);
				break;
			default:
				n_errors += gmt_default_option_error(GMT, opt);
				break;
		}
	}
	n_errors += gmt_M_check_condition(GMT, !GMT->common.R.active[RSET],
	                                  "Option -R is required\n");
	n_errors += gmt_M_check_condition(GMT, !Ctrl->I.active,
	                                  "Option -I is required\n");
	n_errors += gmt_M_check_condition(GMT, !Ctrl->T.active,
	                                  "Option -T is required\n");
	n_errors += gmt_M_check_condition(GMT, !Ctrl->G.active,
	                                  "Option -G is required\n");
	n_errors += gmt_M_check_condition(GMT, Ctrl->A.active && Ctrl->C.active,
	                                  "Options -A and -C are mutually exclusive\n");
	if (n_errors)
		GMT_Report(API, GMT_MSG_ERROR,
		           "Invalid merge3d options; use merge3d -? for usage\n");
	return n_errors ? GMT_PARSE_ERROR : GMT_NOERROR;
}

static char *merge3d_trim(char *text)
{
	char *end;

	while (isspace((unsigned char)*text)) text++;
	if (!*text) return text;
	end = text + strlen(text) - 1;
	while (end >= text && isspace((unsigned char)*end)) *end-- = '\0';
	return text;
}

static char *merge3d_text_attribute(int ncid, int varid, const char *name)
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

static int merge3d_numeric_type(nc_type type)
{
	return type == NC_BYTE || type == NC_UBYTE || type == NC_SHORT ||
	       type == NC_USHORT || type == NC_INT || type == NC_UINT ||
	       type == NC_INT64 || type == NC_UINT64 || type == NC_FLOAT ||
	       type == NC_DOUBLE;
}

static char *merge3d_modifier_end(char *start)
{
	char *p;
	for (p = start; *p; p++) {
		if (*p != '+') continue;
		if (p > start && (p[-1] == 'e' || p[-1] == 'E')) continue;
		return p;
	}
	return p;
}

static int merge3d_source_parts(struct GMTAPI_CTRL *API, const char *source,
                                struct MERGE3D_SOURCE_PARTS *parts)
{
	char *copy = NULL, *question, *modifier = NULL, *save = NULL, *token;
	size_t capacity = 0;
	int status;

	memset(parts, 0, sizeof(*parts));
	gq_transform_init(&parts->transform);
	copy = strdup(source);
	if (copy == NULL) return GMT_MEMORY_ERROR;
	question = strchr(copy, '?');
	if (question) {
		char *names;
		*question++ = '\0';
		names = question;
		modifier = merge3d_modifier_end(names);
		if (*modifier) *modifier++ = '\0';
		if (*names) {
			parts->explicit_selector = true;
			for (token = strtok_r(names, ",", &save); token;
			     token = strtok_r(NULL, ",", &save)) {
				char **next;
				if (!*token) goto bad;
				if (parts->count == capacity) {
					capacity = capacity ? capacity * 2 : 4;
					next = realloc(parts->names,
					               capacity * sizeof(*parts->names));
					if (next == NULL) goto memory;
					parts->names = next;
				}
				parts->names[parts->count] = strdup(token);
				if (parts->names[parts->count] == NULL) goto memory;
				parts->count++;
			}
		}
		if (modifier) {
			char message[GMT_LEN256];
			if (gq_transform_parse(modifier,
			                       GQ_TRANSFORM_X_MASK | GQ_TRANSFORM_Y_MASK |
			                       GQ_TRANSFORM_Z_MASK,
			                       true, &parts->transform,
			                       &parts->has_sentinel, &parts->sentinel,
			                       message, sizeof(message)))
				goto bad_modifier;
		}
	}
	status = gq_resolve_remote_path(API, GMT_IS_GRID, copy, &parts->path);
	free(copy);
	if (status != GMT_NOERROR) merge3d_source_parts_free(parts);
	return status;

bad_modifier:
	GMT_Report(API, GMT_MSG_ERROR,
	           "Unsupported selector transform in %s; use +n, coordinate, "
	           "value, and target-unit modifiers\n",
	           source);
	goto fail;
bad:
	GMT_Report(API, GMT_MSG_ERROR, "Invalid NetCDF selector %s\n", source);
	goto fail;
memory:
	GMT_Report(API, GMT_MSG_ERROR, "Unable to allocate selector for %s\n", source);
fail:
	free(copy);
	merge3d_source_parts_free(parts);
	return GMT_PARSE_ERROR;
}

static int merge3d_is_netcdf(struct GMTAPI_CTRL *API, const char *source)
{
	struct MERGE3D_SOURCE_PARTS parts;
	int ncid, status;

	if (merge3d_source_parts(API, source, &parts)) return false;
	status = nc_open(parts.path, NC_NOWRITE, &ncid);
	if (status == NC_NOERR) nc_close(ncid);
	merge3d_source_parts_free(&parts);
	return status == NC_NOERR;
}

static int merge3d_axis_name(const char *text)
{
	char lower[NC_MAX_NAME + 1];
	size_t k, length;

	if (text == NULL) return -1;
	length = strlen(text);
	if (length > NC_MAX_NAME) return -1;
	for (k = 0; k <= length; k++)
		lower[k] = (char)tolower((unsigned char)text[k]);
	if (!strcmp(lower, "x") || !strcmp(lower, "lon") ||
	    !strcmp(lower, "longitude") || !strcmp(lower, "easting"))
		return MERGE3D_X;
	if (!strcmp(lower, "y") || !strcmp(lower, "lat") ||
	    !strcmp(lower, "latitude") || !strcmp(lower, "northing"))
		return MERGE3D_Y;
	if (!strcmp(lower, "z") || !strcmp(lower, "depth") ||
	    !strcmp(lower, "elevation") || !strcmp(lower, "altitude") ||
	    !strcmp(lower, "level"))
		return MERGE3D_Z;
	return -1;
}

static int merge3d_coordinate_axis(int ncid, int varid, const char *name)
{
	char *attribute;
	int axis = -1;

	attribute = merge3d_text_attribute(ncid, varid, "axis");
	if (attribute) {
		if ((attribute[0] == 'X' || attribute[0] == 'x') && !attribute[1])
			axis = MERGE3D_X;
		else if ((attribute[0] == 'Y' || attribute[0] == 'y') && !attribute[1])
			axis = MERGE3D_Y;
		else if ((attribute[0] == 'Z' || attribute[0] == 'z') && !attribute[1])
			axis = MERGE3D_Z;
		free(attribute);
		if (axis >= 0) return axis;
	}
	attribute = merge3d_text_attribute(ncid, varid, "standard_name");
	if (attribute) {
		if (strstr(attribute, "longitude") ||
		    strstr(attribute, "projection_x_coordinate"))
			axis = MERGE3D_X;
		else if (strstr(attribute, "latitude") ||
		         strstr(attribute, "projection_y_coordinate"))
			axis = MERGE3D_Y;
		else if (strstr(attribute, "depth") || strstr(attribute, "height") ||
		         strstr(attribute, "altitude"))
			axis = MERGE3D_Z;
		free(attribute);
		if (axis >= 0) return axis;
	}
	return merge3d_axis_name(name);
}

static int merge3d_variable_3d(int ncid, int varid, int dimids[3])
{
	nc_type type;
	int ndims;

	if (nc_inq_vartype(ncid, varid, &type) != NC_NOERR ||
	    !merge3d_numeric_type(type) ||
	    nc_inq_varndims(ncid, varid, &ndims) != NC_NOERR || ndims != 3 ||
	    nc_inq_vardimid(ncid, varid, dimids) != NC_NOERR)
		return GMT_DATA_READ_ERROR;
	return GMT_NOERROR;
}

static int merge3d_find_default_variable(struct GMTAPI_CTRL *API, int ncid,
                                         const char *path, char **name)
{
	int nvars, varid;

	if (nc_inq_nvars(ncid, &nvars) != NC_NOERR) return GMT_DATA_READ_ERROR;
	for (varid = 0; varid < nvars; varid++) {
		int dimids[3];
		char candidate[NC_MAX_NAME + 1];
		if (merge3d_variable_3d(ncid, varid, dimids) != GMT_NOERROR ||
		    nc_inq_varname(ncid, varid, candidate) != NC_NOERR)
			continue;
		*name = strdup(candidate);
		return *name ? GMT_NOERROR : GMT_MEMORY_ERROR;
	}
	GMT_Report(API, GMT_MSG_ERROR,
	           "No numeric three-dimensional data variable found in %s\n", path);
	return GMT_DATA_READ_ERROR;
}

static int merge3d_identify_axes(struct GMTAPI_CTRL *API,
                                 struct MERGE3D_CUBE *cube,
                                 const int dimids[3])
{
	bool seen[3] = {false, false, false};
	int position;

	for (position = 0; position < 3; position++) {
		char dim_name[NC_MAX_NAME + 1];
		int coordinate_varid, axis;
		if (nc_inq_dimname(cube->ncid, dimids[position], dim_name) != NC_NOERR ||
		    nc_inq_varid(cube->ncid, dim_name, &coordinate_varid) != NC_NOERR) {
			GMT_Report(API, GMT_MSG_ERROR,
			           "Dimension %d in %s has no coordinate variable\n",
			           dimids[position], cube->path);
			return GMT_DATA_READ_ERROR;
		}
		axis = merge3d_coordinate_axis(cube->ncid, coordinate_varid, dim_name);
		if (axis < 0 || seen[axis]) {
			GMT_Report(API, GMT_MSG_ERROR,
			           "Cannot uniquely identify x, y, and z coordinates in %s\n",
			           cube->path);
			return GMT_DATA_READ_ERROR;
		}
		seen[axis] = true;
		cube->axis_dimid[axis] = dimids[position];
		cube->coordinate_varid[axis] = coordinate_varid;
	}
	return GMT_NOERROR;
}

static int merge3d_validate_axis(struct GMTAPI_CTRL *API,
                                 const struct MERGE3D_CUBE *cube, int axis)
{
	size_t k;

	if (cube->n[axis] < 2) return GMT_DIM_TOO_SMALL;
	for (k = 0; k < cube->n[axis]; k++) {
		if (!isfinite(cube->coordinate[axis][k])) {
			GMT_Report(API, GMT_MSG_ERROR,
			           "%c coordinates in %s must be finite (index %zu)\n",
			           "xyz"[axis], cube->source, k);
			return GMT_DATA_READ_ERROR;
		}
		if (k && cube->coordinate[axis][k] <=
		         cube->coordinate[axis][k - 1]) {
			GMT_Report(API, GMT_MSG_ERROR,
			           "%c coordinates in %s must be strictly increasing after "
			           "input scaling (index %zu). Input +%c scaling does not "
			           "reorder cube data; choose +%c<scale> so the transformed "
			           "axis increases, then use output -Z+%c<scale> to restore "
			           "the desired output convention.\n",
			           "xyz"[axis], cube->source, k, "xyz"[axis],
			           "xyz"[axis], "xyz"[axis]);
			return GMT_DATA_READ_ERROR;
		}
	}
	return GMT_NOERROR;
}

static int merge3d_open_cube(struct GMTAPI_CTRL *API, const char *source,
                             struct MERGE3D_CUBE *cube)
{
	struct MERGE3D_SOURCE_PARTS parts;
	int first_dimids[3], status = GMT_DATA_READ_ERROR;
	size_t field, axis;

	memset(cube, 0, sizeof(*cube));
	cube->ncid = -1;
	if (merge3d_source_parts(API, source, &parts)) return GMT_PARSE_ERROR;
	cube->source = strdup(source);
	cube->path = strdup(parts.path);
	cube->explicit_selector = parts.explicit_selector;
	cube->has_sentinel = parts.has_sentinel;
	cube->sentinel = parts.sentinel;
	cube->transform = parts.transform;
	gq_transform_init(&parts.transform);
	if (!cube->source || !cube->path ||
	    nc_open(cube->path, NC_NOWRITE, &cube->ncid) != NC_NOERR)
		goto cleanup;
	if (parts.count == 0) {
		char *name = NULL;
		if (merge3d_find_default_variable(API, cube->ncid, cube->path, &name))
			goto cleanup;
		parts.names = calloc(1, sizeof(*parts.names));
		if (parts.names == NULL) {
			free(name);
			goto cleanup;
		}
		parts.names[0] = name;
		parts.count = 1;
	}
	cube->n_fields = parts.count;
	{
		char message[GMT_LEN256];
		if (gq_transform_validate_values(&cube->transform, cube->n_fields,
		                                  message, sizeof(message))) {
			GMT_Report(API, GMT_MSG_ERROR, "%s: %s\n", source, message);
			goto cleanup;
		}
	}
	cube->field = calloc(cube->n_fields, sizeof(*cube->field));
	if (cube->field == NULL) goto cleanup;

	for (field = 0; field < cube->n_fields; field++) {
		struct MERGE3D_FIELD *item = &cube->field[field];
		int position;
		if (nc_inq_varid(cube->ncid, parts.names[field], &item->varid) != NC_NOERR ||
		    merge3d_variable_3d(cube->ncid, item->varid, item->dimids)) {
			GMT_Report(API, GMT_MSG_ERROR,
			           "%s is not a numeric three-dimensional variable in %s\n",
			           parts.names[field], cube->path);
			goto cleanup;
		}
		if (field == 0) {
			memcpy(first_dimids, item->dimids, sizeof(first_dimids));
			if (merge3d_identify_axes(API, cube, first_dimids)) goto cleanup;
		}
		for (axis = 0; axis < 3; axis++) {
			item->axis_position[axis] = -1;
			for (position = 0; position < 3; position++)
				if (item->dimids[position] == cube->axis_dimid[axis])
					item->axis_position[axis] = position;
			if (item->axis_position[axis] < 0) {
				GMT_Report(API, GMT_MSG_ERROR,
				           "Selected variables in %s do not share x, y, and z dimensions\n",
				           cube->path);
				goto cleanup;
			}
		}
		item->name = strdup(parts.names[field]);
		item->units = merge3d_text_attribute(cube->ncid, item->varid, "units");
		if (item->name == NULL) goto cleanup;
		if (gq_transform_value_unit(&cube->transform, field)) {
			free(item->units);
			item->units = strdup(gq_transform_value_unit(&cube->transform, field));
			if (!item->units) goto cleanup;
		}
		else if (gq_transform_value_scale(&cube->transform, field) != 1.0) {
			free(item->units);
			item->units = NULL;
		}
	}

	for (axis = 0; axis < 3; axis++) {
		char name[NC_MAX_NAME + 1];
		int ndims, dimid;
		nc_type type;
		if (nc_inq_dim(cube->ncid, cube->axis_dimid[axis], name,
		               &cube->n[axis]) != NC_NOERR ||
		    nc_inq_vartype(cube->ncid, cube->coordinate_varid[axis], &type) != NC_NOERR ||
		    !merge3d_numeric_type(type) ||
		    nc_inq_varndims(cube->ncid, cube->coordinate_varid[axis], &ndims) != NC_NOERR ||
		    ndims != 1 ||
		    nc_inq_vardimid(cube->ncid, cube->coordinate_varid[axis], &dimid) != NC_NOERR ||
		    dimid != cube->axis_dimid[axis])
			goto cleanup;
		cube->coordinate_name[axis] = strdup(name);
		cube->coordinate_units[axis] =
		    merge3d_text_attribute(cube->ncid, cube->coordinate_varid[axis], "units");
		cube->coordinate[axis] = calloc(cube->n[axis], sizeof(double));
		if (!cube->coordinate_name[axis] || !cube->coordinate[axis] ||
		    nc_get_var_double(cube->ncid, cube->coordinate_varid[axis],
		                      cube->coordinate[axis]) != NC_NOERR)
			goto cleanup;
		{
			double scale = 1.0, offset = 0.0;
			size_t k;
			nc_get_att_double(cube->ncid, cube->coordinate_varid[axis],
			                  "scale_factor", &scale);
			nc_get_att_double(cube->ncid, cube->coordinate_varid[axis],
			                  "add_offset", &offset);
			for (k = 0; k < cube->n[axis]; k++)
				cube->coordinate[axis][k] =
				    (cube->coordinate[axis][k] * scale + offset) *
				    cube->transform.axis_scale[axis];
		}
		if (cube->transform.axis_unit[axis]) {
			free(cube->coordinate_units[axis]);
			cube->coordinate_units[axis] =
			    strdup(cube->transform.axis_unit[axis]);
			if (!cube->coordinate_units[axis]) goto cleanup;
		}
		else if (cube->transform.axis_set[axis] &&
		         cube->transform.axis_scale[axis] != 1.0) {
			free(cube->coordinate_units[axis]);
			cube->coordinate_units[axis] = NULL;
		}
	}
	for (axis = 0; axis < 3; axis++)
		if (merge3d_validate_axis(API, cube, (int)axis)) goto cleanup;
	status = GMT_NOERROR;

cleanup:
	merge3d_source_parts_free(&parts);
	if (status != GMT_NOERROR) {
		GMT_Report(API, GMT_MSG_ERROR, "Unable to read cube metadata from %s\n", source);
		merge3d_cube_free(cube);
	}
	return status;
}

static int merge3d_missing(int ncid, int varid, double value,
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

static int merge3d_read_field(struct GMTAPI_CTRL *API,
                              const struct MERGE3D_CUBE *cube,
                              size_t field, double **values)
{
	const struct MERGE3D_FIELD *item = &cube->field[field];
	size_t total = cube->n[0] * cube->n[1] * cube->n[2], k;
	double scale = 1.0, offset = 0.0;
	double user_scale = gq_transform_value_scale(&cube->transform, field);

	*values = calloc(total, sizeof(**values));
	if (*values == NULL) return GMT_MEMORY_ERROR;
	if (nc_get_var_double(cube->ncid, item->varid, *values) != NC_NOERR) {
		free(*values);
		*values = NULL;
		return GMT_DATA_READ_ERROR;
	}
	nc_get_att_double(cube->ncid, item->varid, "scale_factor", &scale);
	nc_get_att_double(cube->ncid, item->varid, "add_offset", &offset);
	for (k = 0; k < total; k++) {
		double value = (*values)[k];
		if (merge3d_missing(cube->ncid, item->varid, value,
		                   cube->has_sentinel, cube->sentinel) ||
		    (API->GMT->common.d.active[GMT_IN] &&
		     value == API->GMT->common.d.nan_proxy[GMT_IN]))
			(*values)[k] = NAN;
		else
			(*values)[k] = (value * scale + offset) * user_scale;
	}
	GMT_Report(API, GMT_MSG_DEBUG, "Read %s?%s\n",
	           cube->path, item->name);
	return GMT_NOERROR;
}

static int merge3d_append_spec(struct MERGE3D_JOB *job,
                               const struct MERGE3D_SPEC *spec)
{
	struct MERGE3D_SPEC *next =
	    realloc(job->spec, (job->count + 1) * sizeof(*next));
	if (next == NULL) return GMT_MEMORY_ERROR;
	job->spec = next;
	job->spec[job->count++] = *spec;
	return GMT_NOERROR;
}

static int merge3d_parse_pair(struct GMTAPI_CTRL *API, const char *text,
                              double *lo, double *hi)
{
	char copy[GMT_LEN128], *slash;

	if (!text || strlen(text) >= sizeof(copy)) return GMT_PARSE_ERROR;
	strcpy(copy, text);
	slash = strchr(copy, '/');
	if (!slash || strchr(slash + 1, '/')) goto bad;
	*slash++ = '\0';
	if (merge3d_parse_number(copy, lo) || merge3d_parse_number(slash, hi) ||
	    *lo >= *hi)
		goto bad;
	return GMT_NOERROR;
bad:
	GMT_Report(API, GMT_MSG_ERROR,
	           "Vertical support must be zlo/zhi with zlo < zhi: %s\n",
	           text ? text : "");
	return GMT_PARSE_ERROR;
}

static int merge3d_parse_functions(struct GMTAPI_CTRL *API, const char *text,
                                   blend_window_function function[3])
{
	char copy[GMT_LEN256], *token = NULL, *save = NULL;
	size_t n = 0;

	if (!text || strlen(text) >= sizeof(copy)) return GMT_PARSE_ERROR;
	strcpy(copy, text);
	for (token = strtok_r(copy, "/", &save); token && n < 3;
	     token = strtok_r(NULL, "/", &save)) {
		if (blend_window_function_from_name(token, &function[n]) != SUCCESS)
			goto bad;
		n++;
	}
	if (token || n < 1 || n > 3) goto bad;
	if (n == 1) function[1] = function[2] = function[0];
	else if (n == 2) function[2] = function[0];
	return GMT_NOERROR;
bad:
	GMT_Report(API, GMT_MSG_ERROR,
	           "Window functions must be x[/y[/z]]: %s\n", text ? text : "");
	return GMT_PARSE_ERROR;
}

static int merge3d_parse_ratios(struct GMTAPI_CTRL *API, const char *text,
                                double ratio[6])
{
	char copy[GMT_LEN256], *token = NULL, *save = NULL;
	double value[6];
	size_t n = 0, k;

	if (!text || strlen(text) >= sizeof(copy)) return GMT_PARSE_ERROR;
	strcpy(copy, text);
	for (token = strtok_r(copy, "/", &save); token && n < 6;
	     token = strtok_r(NULL, "/", &save)) {
		if (merge3d_parse_number(token, &value[n])) goto bad;
		n++;
	}
	if (token || (n != 1 && n != 3 && n != 6)) goto bad;
	if (n == 1)
		for (k = 0; k < 6; k++) ratio[k] = value[0];
	else if (n == 3) {
		ratio[0] = ratio[1] = value[0];
		ratio[2] = ratio[3] = value[1];
		ratio[4] = ratio[5] = value[2];
	}
	else
		for (k = 0; k < 6; k++) ratio[k] = value[k];
	for (k = 0; k < 6; k++)
		if (ratio[k] < 0.0 || ratio[k] >= 0.5) goto bad;
	return GMT_NOERROR;
bad:
	GMT_Report(API, GMT_MSG_ERROR,
	           "Taper ratios must contain 1, 3, or 6 values in [0, 0.5): %s\n",
	           text ? text : "");
	return GMT_PARSE_ERROR;
}

static void merge3d_spec_defaults(struct MERGE3D_SPEC *spec)
{
	size_t k;

	memset(spec, 0, sizeof(*spec));
	spec->primary.ncid = spec->secondary.ncid = -1;
	for (k = 0; k < 3; k++) spec->function[k] = WFUNC_COSINE;
	for (k = 0; k < 6; k++) spec->ratio[k] = 0.2;
}

static int merge3d_read_mergefile(struct GMTAPI_CTRL *API, const char *path,
                                  struct MERGE3D_JOB *job)
{
	char *resolved = NULL;
	FILE *fp;
	char line[GMT_BUFSIZ];
	size_t line_number = 0;
	int status = GMT_NOERROR;

	if (strcmp(path, "-") &&
	    gq_resolve_remote_path(API, GMT_IS_DATASET, path, &resolved))
		return GMT_DATA_READ_ERROR;
	fp = !strcmp(path, "-") ? stdin : fopen(resolved, "r");
	if (fp == NULL) {
		GMT_Report(API, GMT_MSG_ERROR, "Unable to open mergefile %s: %s\n",
		           path, strerror(errno));
		free(resolved);
		return GMT_DATA_READ_ERROR;
	}
	while (fgets(line, sizeof(line), fp)) {
		char *tokens[7] = {NULL}, *save = NULL, *token, *text, *comment;
		size_t n = 0;
		struct MERGE3D_SPEC spec;

		line_number++;
		comment = strchr(line, '#');
		if (comment) *comment = '\0';
		text = merge3d_trim(line);
		if (!*text) continue;
		for (token = strtok_r(text, " \t\r\n", &save); token && n < 7;
		     token = strtok_r(NULL, " \t\r\n", &save))
			tokens[n++] = token;
		if (n == 0) continue;
		if (n > 6) {
			GMT_Report(API, GMT_MSG_ERROR,
			           "%s:%zu: Expected at most six fields\n", path, line_number);
			status = GMT_PARSE_ERROR;
			break;
		}
		merge3d_spec_defaults(&spec);
		spec.primary_source = strdup(tokens[0]);
		if (spec.primary_source == NULL) {
			status = GMT_MEMORY_ERROR;
			break;
		}
		if (n > 1 && strcmp(tokens[1], "-")) {
			spec.secondary_source = strdup(tokens[1]);
			if (spec.secondary_source == NULL) {
				free(spec.primary_source);
				status = GMT_MEMORY_ERROR;
				break;
			}
			spec.has_secondary = true;
		}
		if (n > 2 && strcmp(tokens[2], "-")) {
			spec.polygon_file = strdup(tokens[2]);
			if (spec.polygon_file == NULL) {
				free(spec.primary_source);
				free(spec.secondary_source);
				status = GMT_MEMORY_ERROR;
				break;
			}
			spec.has_polygon = true;
		}
		if (n > 3 && strcmp(tokens[3], "-")) {
			if (merge3d_parse_pair(API, tokens[3], &spec.zlo, &spec.zhi)) {
				free(spec.primary_source);
				free(spec.secondary_source);
				free(spec.polygon_file);
				status = GMT_PARSE_ERROR;
				break;
			}
			spec.has_zrange = true;
		}
		if (n > 4 && strcmp(tokens[4], "-") &&
		    merge3d_parse_functions(API, tokens[4], spec.function)) {
			free(spec.primary_source);
			free(spec.secondary_source);
			free(spec.polygon_file);
			status = GMT_PARSE_ERROR;
			break;
		}
		if (n > 5 && strcmp(tokens[5], "-") &&
		    merge3d_parse_ratios(API, tokens[5], spec.ratio)) {
			free(spec.primary_source);
			free(spec.secondary_source);
			free(spec.polygon_file);
			status = GMT_PARSE_ERROR;
			break;
		}
		if (merge3d_append_spec(job, &spec)) {
			free(spec.primary_source);
			free(spec.secondary_source);
			free(spec.polygon_file);
			status = GMT_MEMORY_ERROR;
			break;
		}
	}
	if (ferror(fp)) status = GMT_DATA_READ_ERROR;
	if (fp != stdin) fclose(fp);
	free(resolved);
	if (status == GMT_NOERROR && job->count == 0) {
		GMT_Report(API, GMT_MSG_ERROR, "Mergefile %s contains no records\n", path);
		status = GMT_DATA_READ_ERROR;
	}
	job->mergefile = true;
	return status;
}

static int merge3d_read_specs(struct GMTAPI_CTRL *API,
                              const struct MERGE3D_CTRL *Ctrl,
                              struct MERGE3D_JOB *job)
{
	size_t k;

	if (Ctrl->In.n == 0) return merge3d_read_mergefile(API, "-", job);
	if (Ctrl->In.n == 1 && !merge3d_is_netcdf(API, Ctrl->In.file[0]))
		return merge3d_read_mergefile(API, Ctrl->In.file[0], job);
	for (k = 0; k < Ctrl->In.n; k++) {
		struct MERGE3D_SPEC spec;
		merge3d_spec_defaults(&spec);
		spec.primary_source = strdup(Ctrl->In.file[k]);
		if (spec.primary_source == NULL ||
		    merge3d_append_spec(job, &spec)) {
			free(spec.primary_source);
			return GMT_MEMORY_ERROR;
		}
	}
	return GMT_NOERROR;
}

static int merge3d_grid_size(struct GMTAPI_CTRL *API, const char *axis,
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
	tolerance = 32.0 * DBL_EPSILON * MAX(1.0, MAX(fabs(hi), fabs(adjusted)));
	if (fabs(adjusted - hi) > tolerance)
		GMT_Report(API, GMT_MSG_WARNING,
		           "Adjusting %s maximum from %.12g to %.12g to fit increment\n",
		           axis, hi, adjusted);
	*adjusted_hi = adjusted;
	return GMT_NOERROR;
}

static int merge3d_valid_name(const char *name)
{
	const unsigned char *p = (const unsigned char *)name;
	if (!p[0] || !(isalpha(p[0]) || p[0] == '_')) return false;
	for (p++; *p; p++)
		if (!(isalnum(*p) || *p == '_')) return false;
	return true;
}

static int merge3d_parse_output_fields(struct GMTAPI_CTRL *API,
                                       const struct MERGE3D_CTRL *Ctrl,
                                       struct MERGE3D_JOB *job)
{
	char *copy = NULL, *token = NULL, *save = NULL;
	size_t count = 0, field;

	job->output_fields = calloc(job->n_fields, sizeof(*job->output_fields));
	job->output_units = calloc(job->n_fields, sizeof(*job->output_units));
	if (!job->output_fields || !job->output_units) return GMT_MEMORY_ERROR;
	if (Ctrl->F.active) {
		copy = strdup(Ctrl->F.fields);
		if (!copy) return GMT_MEMORY_ERROR;
		for (token = strtok_r(copy, ",", &save); token;
		     token = strtok_r(NULL, ",", &save)) {
			if (count >= job->n_fields) break;
			job->output_fields[count++] = strdup(token);
		}
		if (token || count != job->n_fields) {
			GMT_Report(API, GMT_MSG_ERROR,
			           "Option -F must list exactly %zu output variables\n",
			           job->n_fields);
			free(copy);
			return GMT_PARSE_ERROR;
		}
		free(copy);
	}
	else {
		for (field = 0; field < job->n_fields; field++) {
			job->output_fields[field] =
			    strdup(job->spec[0].primary.field[field].name);
			if (job->output_fields[field] == NULL) return GMT_MEMORY_ERROR;
		}
	}
	for (field = 0; field < job->n_fields; field++) {
		const char *name = job->output_fields[field];
		const char *units = job->spec[0].primary.field[field].units;
		if (!merge3d_valid_name(name) || !strcmp(name, "x") ||
		    !strcmp(name, "y") || !strcmp(name, "z") ||
		    !strcmp(name, "weight")) {
			GMT_Report(API, GMT_MSG_ERROR,
			           "Invalid or reserved output variable name %s; use -F\n", name);
			return GMT_PARSE_ERROR;
		}
		if (units) {
			bool agree = true;
			size_t k;
			for (k = 0; k < job->count && agree; k++) {
				const char *other = job->spec[k].primary.field[field].units;
				if ((other == NULL) != (units == NULL) ||
				    (other && strcmp(other, units)))
					agree = false;
				if (job->spec[k].has_secondary) {
					other = job->spec[k].secondary.field[field].units;
					if ((other == NULL) != (units == NULL) ||
					    (other && strcmp(other, units)))
						agree = false;
				}
			}
			if (agree) job->output_units[field] = strdup(units);
			else GMT_Report(API, GMT_MSG_WARNING,
			                "Omitting units for %s because source units differ\n",
			                name);
		}
	}
	return GMT_NOERROR;
}

static int merge3d_monotone_name(const char *path, char output[PATH_MAX])
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

static int merge3d_polygon_to_real(const polygon *local, double xmin,
                                   double xmax, double ymin, double ymax,
                                   int nx, int ny, polygon *real)
{
	size_t k;

	if (blend_polygon_alloc(real, local->n_vertices) != SUCCESS)
		return GMT_MEMORY_ERROR;
	for (k = 0; k < local->n_vertices; k++) {
		double x = xmin + local->vertices[k].x * (xmax - xmin) /
		                  (double)(nx - 1);
		double y = ymin + local->vertices[k].y * (ymax - ymin) /
		                  (double)(ny - 1);
		if (blend_polygon_set_vertex(real, k, x, y) != SUCCESS) {
			blend_polygon_free(real);
			return GMT_RUNTIME_ERROR;
		}
	}
	return GMT_NOERROR;
}

static int merge3d_support_indices(double origin, double inc,
                                   double lo, double hi,
                                   int *i0, int *i1)
{
	double a = (lo - origin) / inc;
	double b = (hi - origin) / inc;

	*i0 = (int)floor(a + GMT_CONV8_LIMIT);
	*i1 = (int)ceil(b - GMT_CONV8_LIMIT);
	return *i1 > *i0 ? GMT_NOERROR : GMT_DIM_TOO_SMALL;
}

static int merge3d_prepare_support(struct GMT_CTRL *GMT,
                                   const struct MERGE3D_CTRL *Ctrl,
                                   const struct MERGE3D_JOB *job,
                                   struct MERGE3D_SPEC *spec)
{
	polygon input = {0}, local = {0}, monotone = {0}, converted_real = {0};
	permuted_vertex boundary = {0};
	char *polygon_path = NULL;
	double xmin, xmax, ymin, ymax, zlo, zhi;
	int nx, ny, nz, is_strict = 0, converted = 0;
	int status = GMT_RUNTIME_ERROR;
	size_t k;

	if (spec->has_polygon) {
		status = gq_resolve_remote_path(GMT->parent, GMT_IS_DATASET,
		                                spec->polygon_file, &polygon_path);
		if (status != GMT_NOERROR ||
		    blend_polygon_read(polygon_path, &input) != SUCCESS ||
		    blend_polygon_validate(&input) != SUCCESS ||
		    blend_polygon_bounds(&input, &xmin, &xmax, &ymin, &ymax) != SUCCESS) {
			GMT_Report(GMT->parent, GMT_MSG_ERROR,
			           "Unable to read polygon %s\n", spec->polygon_file);
			goto cleanup;
		}
	}
	else {
		xmin = spec->primary.coordinate[MERGE3D_X][0];
		xmax = spec->primary.coordinate[MERGE3D_X][spec->primary.n[MERGE3D_X] - 1];
		ymin = spec->primary.coordinate[MERGE3D_Y][0];
		ymax = spec->primary.coordinate[MERGE3D_Y][spec->primary.n[MERGE3D_Y] - 1];
		if (blend_polygon_alloc(&input, 4) != SUCCESS ||
		    blend_polygon_set_vertex(&input, 0, xmin, ymin) != SUCCESS ||
		    blend_polygon_set_vertex(&input, 1, xmax, ymin) != SUCCESS ||
		    blend_polygon_set_vertex(&input, 2, xmax, ymax) != SUCCESS ||
		    blend_polygon_set_vertex(&input, 3, xmin, ymax) != SUCCESS)
			goto cleanup;
	}
	zlo = spec->has_zrange
	    ? spec->zlo : spec->primary.coordinate[MERGE3D_Z][0];
	zhi = spec->has_zrange
	    ? spec->zhi : spec->primary.coordinate[MERGE3D_Z][spec->primary.n[MERGE3D_Z] - 1];
	if (zlo >= zhi) {
		GMT_Report(GMT->parent, GMT_MSG_ERROR,
		           "Invalid vertical support for %s\n", spec->primary_source);
		goto cleanup;
	}
	if (merge3d_support_indices(job->wesn[XLO], job->inc[0], xmin, xmax,
	                            &spec->support_i0, &spec->support_i1) ||
	    merge3d_support_indices(job->wesn[YLO], job->inc[1], ymin, ymax,
	                            &spec->support_j0, &spec->support_j1) ||
	    merge3d_support_indices(Ctrl->T.min, Ctrl->T.inc, zlo, zhi,
	                            &spec->support_k0, &spec->support_k1))
		goto cleanup;
	nx = spec->support_i1 - spec->support_i0 + 1;
	ny = spec->support_j1 - spec->support_j0 + 1;
	nz = spec->support_k1 - spec->support_k0 + 1;
	xmin = job->wesn[XLO] + (double)spec->support_i0 * job->inc[0];
	xmax = job->wesn[XLO] + (double)spec->support_i1 * job->inc[0];
	ymin = job->wesn[YLO] + (double)spec->support_j0 * job->inc[1];
	ymax = job->wesn[YLO] + (double)spec->support_j1 * job->inc[1];
	zlo = Ctrl->T.min + (double)spec->support_k0 * Ctrl->T.inc;
	zhi = Ctrl->T.min + (double)spec->support_k1 * Ctrl->T.inc;

	if (blend_polygon_map_to_grid(&input, xmin, xmax, ymin, ymax,
	                              nx, ny, &local) != SUCCESS)
		goto cleanup;
	for (k = 0; k < local.n_vertices; k++) {
		local.vertices[k].x = floor(local.vertices[k].x + 0.5);
		local.vertices[k].y = floor(local.vertices[k].y + 0.5);
	}
	if (blend_polygon_validate(&local) != SUCCESS ||
	    blend_polygon_is_xy_monotone_strict(&local, &is_strict) != SUCCESS)
		goto cleanup;
	if (is_strict) {
		if (blend_polygon_copy(&local, &monotone) != SUCCESS) goto cleanup;
	}
	else {
		if (!Ctrl->M.active) {
			GMT_Report(GMT->parent, GMT_MSG_ERROR,
			           "Polygon %s is not strictly xy-monotone; use -ME or -MB\n",
			           spec->polygon_file ? spec->polygon_file : "(rectangular support)");
			status = GMT_PARSE_ERROR;
			goto cleanup;
		}
		if ((Ctrl->M.method == 'E' &&
		     blend_polygon_xy_monotone_envelope_strict(&local, &monotone) != SUCCESS) ||
		    (Ctrl->M.method == 'B' &&
		     blend_polygon_xy_monotone_best_piecewise_envelope_strict(
		         &local, &monotone, 0.0, (double)(nx - 1),
		         0.0, (double)(ny - 1), nx, ny) != SUCCESS))
			goto cleanup;
		converted = 1;
	}
	memset(&spec->support, 0, sizeof(spec->support));
	spec->support.nx = nx;
	spec->support.ny = ny;
	spec->support.nz = nz;
	spec->support.ratio_x1 = spec->ratio[0];
	spec->support.ratio_x2 = spec->ratio[1];
	spec->support.ratio_y1 = spec->ratio[2];
	spec->support.ratio_y2 = spec->ratio[3];
	spec->support.ratio_z1 = spec->ratio[4];
	spec->support.ratio_z2 = spec->ratio[5];
	spec->support.x_function = spec->function[0];
	spec->support.y_function = spec->function[1];
	spec->support.z_function = spec->function[2];
	if (merge3d_polygon_to_real(&monotone, xmin, xmax, ymin, ymax,
	                            nx, ny, &spec->real_support) ||
	    blend_window_set_polygon(&spec->support, &monotone) != SUCCESS ||
	    boundary_assembly(&spec->support, &boundary) != SUCCESS)
		goto cleanup;
	spec->support_ready = true;
	if (converted) {
		GMT_Report(GMT->parent, GMT_MSG_INFORMATION,
		           "Converted %s to a strictly xy-monotone support (%zu -> %zu vertices)\n",
		           spec->polygon_file, local.n_vertices, monotone.n_vertices);
		if (Ctrl->M.write && spec->polygon_file) {
			char output[PATH_MAX];
			if (merge3d_monotone_name(spec->polygon_file, output) ||
			    merge3d_polygon_to_real(&monotone, xmin, xmax, ymin, ymax,
			                            nx, ny, &converted_real) ||
			    blend_polygon_write(output, &converted_real) != SUCCESS)
				goto cleanup;
			GMT_Report(GMT->parent, GMT_MSG_INFORMATION,
			           "Wrote converted polygon %s\n", output);
		}
	}
	status = GMT_NOERROR;

cleanup:
	free(polygon_path);
	blend_permuted_vertex_free(&boundary);
	blend_polygon_free(&converted_real);
	blend_polygon_free(&monotone);
	blend_polygon_free(&local);
	blend_polygon_free(&input);
	return status;
}

static int merge3d_same_secondary(const struct MERGE3D_SPEC *a,
                                  const struct MERGE3D_SPEC *b)
{
	if (!a->has_secondary || !b->has_secondary) return false;
	return !strcmp(a->secondary.source, b->secondary.source);
}

static int merge3d_support_weight(const struct MERGE3D_SPEC *spec,
                                  int col, int row, int layer,
                                  double *weight)
{
	if (!spec->support_ready ||
	    col < spec->support_i0 || col > spec->support_i1 ||
	    row < spec->support_j0 || row > spec->support_j1 ||
	    layer < spec->support_k0 || layer > spec->support_k1) {
		*weight = 0.0;
		return GMT_NOERROR;
	}
	if (embedding_contribution3d(col - spec->support_i0,
	                             row - spec->support_j0,
	                             layer - spec->support_k0,
	                             (window *)&spec->support) != SUCCESS)
		return GMT_RUNTIME_ERROR;
	*weight = spec->support.contribution;
	return GMT_NOERROR;
}

static int merge3d_validate_overlap(struct GMT_CTRL *GMT,
                                    const struct MERGE3D_JOB *job)
{
	size_t a, b;

	for (a = 0; a < job->count; a++) {
		if (!job->spec[a].has_secondary) continue;
		for (b = a + 1; b < job->count; b++) {
			int i0 = MAX(job->spec[a].support_i0, job->spec[b].support_i0);
			int i1 = MIN(job->spec[a].support_i1, job->spec[b].support_i1);
			int j0 = MAX(job->spec[a].support_j0, job->spec[b].support_j0);
			int j1 = MIN(job->spec[a].support_j1, job->spec[b].support_j1);
			int k0 = MAX(job->spec[a].support_k0, job->spec[b].support_k0);
			int k1 = MIN(job->spec[a].support_k1, job->spec[b].support_k1);
			int i, j, k, overlaps = 0;
			if (!job->spec[b].has_secondary) continue;
			if (i0 > i1 || j0 > j1 || k0 > k1) continue;
			for (k = k0; k <= k1 && !overlaps; k++)
				for (j = j0; j <= j1 && !overlaps; j++)
					for (i = i0; i <= i1; i++) {
						double wa, wb;
						if (merge3d_support_weight(&job->spec[a], i, j, k, &wa) ||
						    merge3d_support_weight(&job->spec[b], i, j, k, &wb))
							return GMT_RUNTIME_ERROR;
						if (wa > 0.0 && wb > 0.0) {
							overlaps = 1;
							break;
						}
					}
			if (overlaps && !merge3d_same_secondary(&job->spec[a],
			                                        &job->spec[b])) {
				if (!strcmp(job->spec[b].primary_source,
				            job->spec[a].secondary_source))
					continue;
				GMT_Report(GMT->parent, GMT_MSG_ERROR,
				           "Overlapping primary supports %s and %s must use "
				           "the same secondary cube with -A\n",
				           job->spec[a].primary_source,
				           job->spec[b].primary_source);
				return GMT_RUNTIME_ERROR;
			}
		}
	}
	return GMT_NOERROR;
}

static int merge3d_prepare_job(struct GMT_CTRL *GMT,
                               const struct MERGE3D_CTRL *Ctrl,
                               struct MERGE3D_JOB *job)
{
	size_t k, field;
	double adjusted;
	int status;

	memcpy(job->wesn, GMT->common.R.wesn, sizeof(job->wesn));
	memcpy(job->inc, Ctrl->I.inc, sizeof(job->inc));
	if (merge3d_grid_size(GMT->parent, "x", job->wesn[XLO],
	                      job->wesn[XHI], job->inc[0], &job->nx, &adjusted))
		return GMT_RUNTIME_ERROR;
	job->wesn[XHI] = adjusted;
	if (merge3d_grid_size(GMT->parent, "y", job->wesn[YLO],
	                      job->wesn[YHI], job->inc[1], &job->ny, &adjusted))
		return GMT_RUNTIME_ERROR;
	job->wesn[YHI] = adjusted;
	job->x = calloc(job->nx, sizeof(*job->x));
	job->y = calloc(job->ny, sizeof(*job->y));
	job->z = calloc(Ctrl->T.n, sizeof(*job->z));
	if (!job->x || !job->y || !job->z) return GMT_MEMORY_ERROR;
	for (k = 0; k < job->nx; k++)
		job->x[k] = job->wesn[XLO] + (double)k * job->inc[0];
	for (k = 0; k < job->ny; k++)
		job->y[k] = job->wesn[YLO] + (double)k * job->inc[1];
	for (k = 0; k < Ctrl->T.n; k++)
		job->z[k] = Ctrl->T.min + (double)k * Ctrl->T.inc;

	for (k = 0; k < job->count; k++) {
		struct MERGE3D_SPEC *spec = &job->spec[k];
		if ((status = merge3d_open_cube(GMT->parent, spec->primary_source,
		                                &spec->primary)) != GMT_NOERROR)
			return status;
		if (spec->has_secondary &&
		    (status = merge3d_open_cube(GMT->parent, spec->secondary_source,
		                                &spec->secondary)) != GMT_NOERROR)
			return status;
		if (k == 0) job->n_fields = spec->primary.n_fields;
		if (spec->primary.n_fields != job->n_fields ||
		    (spec->has_secondary &&
		     spec->secondary.n_fields != job->n_fields)) {
			GMT_Report(GMT->parent, GMT_MSG_ERROR,
			           "All primary and secondary selectors must list %zu fields\n",
			           job->n_fields);
			return GMT_PARSE_ERROR;
		}
		if (!Ctrl->F.active && k > 0)
			for (field = 0; field < job->n_fields; field++)
				if (strcmp(spec->primary.field[field].name,
				           job->spec[0].primary.field[field].name)) {
					GMT_Report(GMT->parent, GMT_MSG_ERROR,
					           "Different source variable names require -F positional mapping\n");
					return GMT_PARSE_ERROR;
				}
		if ((status = merge3d_prepare_support(GMT, Ctrl, job, spec)) != GMT_NOERROR)
			return status;
	}
	if (Ctrl->A.active &&
	    (status = merge3d_validate_overlap(GMT, job)) != GMT_NOERROR)
		return status;
	return merge3d_parse_output_fields(GMT->parent, Ctrl, job);
}

static size_t merge3d_field_index(const struct MERGE3D_CUBE *cube,
                                  size_t field, size_t ix, size_t iy, size_t iz)
{
	const struct MERGE3D_FIELD *item = &cube->field[field];
	size_t index[3], length[3], stride = 1, offset = 0;
	int position;

	index[item->axis_position[MERGE3D_X]] = ix;
	index[item->axis_position[MERGE3D_Y]] = iy;
	index[item->axis_position[MERGE3D_Z]] = iz;
	for (position = 0; position < 3; position++) {
		int axis;
		length[position] = 0;
		for (axis = 0; axis < 3; axis++)
			if (item->axis_position[axis] == position)
				length[position] = cube->n[axis];
	}
	for (position = 2; position >= 0; position--) {
		offset += index[position] * stride;
		stride *= length[position];
	}
	return offset;
}

static int merge3d_interpolate_run(struct GMT_CTRL *GMT,
                                   const double *x, const double *value,
                                   size_t n, const double *target,
                                   size_t n_target, double *output,
                                   double fit, unsigned int mode)
{
	size_t first = 0, count;

	while (first < n_target && target[first] < x[0]) first++;
	count = first;
	while (count < n_target && target[count] <= x[n - 1]) count++;
	count -= first;
	if (n == 1) {
		if (count == 1 &&
		    fabs(target[first] - x[0]) <=
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

static int merge3d_interpolate_finite(struct GMT_CTRL *GMT,
                                      const double *x, const double *value,
                                      size_t n, const double *target,
                                      size_t n_target, double *output,
                                      double fit, unsigned int mode,
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
			status = merge3d_interpolate_run(
			    GMT, &x[start], &value[start], stop - start,
			    target, n_target, output, fit, mode);
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
			if (run_count && k > previous + 1 &&
			    x[k] - x[previous] > max_gap) {
				status = merge3d_interpolate_run(
				    GMT, bridge_x, bridge_value, run_count,
				    target, n_target, output, fit, mode);
				if (status != GMT_NOERROR) return status;
				run_count = 0;
			}
			bridge_x[run_count] = x[k];
			bridge_value[run_count++] = value[k];
			previous = k;
		}
		if (run_count)
			return merge3d_interpolate_run(
			    GMT, bridge_x, bridge_value, run_count,
			    target, n_target, output, fit, mode);
	}
	return GMT_NOERROR;
}

static unsigned int merge3d_horizontal_mode(const struct GMT_CTRL *GMT)
{
	switch (GMT->common.n.interpolant) {
		case BCR_NEARNEIGHBOR: return GMT_SPLINE_NN;
		case BCR_BILINEAR: return GMT_SPLINE_LINEAR;
		case BCR_BSPLINE:
		case BCR_BICUBIC:
		default: return GMT_SPLINE_CUBIC;
	}
}

static bool merge3d_regular_coordinate(const double *coordinate, size_t n,
                                       double *increment)
{
	size_t k;
	double tolerance;

	*increment = (coordinate[n - 1] - coordinate[0]) / (double)(n - 1);
	tolerance = 256.0 * DBL_EPSILON *
	            MAX(1.0, MAX(fabs(coordinate[0]), fabs(coordinate[n - 1])));
	for (k = 1; k < n; k++)
		if (fabs((coordinate[k] - coordinate[k - 1]) - *increment) >
		    tolerance)
			return false;
	return true;
}

static int merge3d_horizontal_bcr(struct GMT_CTRL *GMT,
                                  const struct MERGE3D_JOB *job,
                                  const struct MERGE3D_CUBE *cube,
                                  const double *native, double *output)
{
	struct GMT_GRID *Grid = NULL;
	double wesn[4], inc[2];
	size_t row, col, source_row;
	int status = GMT_MEMORY_ERROR;

	if (!merge3d_regular_coordinate(cube->coordinate[MERGE3D_X],
	                                cube->n[MERGE3D_X], &inc[0]) ||
	    !merge3d_regular_coordinate(cube->coordinate[MERGE3D_Y],
	                                cube->n[MERGE3D_Y], &inc[1]))
		return GMT_NOTSET;
	wesn[XLO] = cube->coordinate[MERGE3D_X][0];
	wesn[XHI] = cube->coordinate[MERGE3D_X][cube->n[MERGE3D_X] - 1];
	wesn[YLO] = cube->coordinate[MERGE3D_Y][0];
	wesn[YHI] = cube->coordinate[MERGE3D_Y][cube->n[MERGE3D_Y] - 1];
	Grid = GMT_Create_Data(GMT->parent, GMT_IS_GRID, GMT_IS_SURFACE,
	                       GMT_CONTAINER_AND_DATA, NULL, wesn, inc,
	                       GMT_GRID_NODE_REG, GMT_NOTSET, NULL);
	if (Grid == NULL) return GMT_MEMORY_ERROR;
	for (row = 0; row < cube->n[MERGE3D_Y]; row++) {
		source_row = cube->n[MERGE3D_Y] - 1 - row;
		for (col = 0; col < cube->n[MERGE3D_X]; col++)
			Grid->data[gmt_M_ijp(Grid->header, row, col)] =
			    (gmt_grdfloat)native[source_row * cube->n[MERGE3D_X] + col];
	}
	if (gmt_grd_BC_set(GMT, Grid, GMT_IN) != GMT_NOERROR) goto cleanup;
	for (row = 0; row < job->ny; row++)
		for (col = 0; col < job->nx; col++)
			output[row * job->nx + col] =
			    gmt_bcr_get_z(GMT, Grid, job->x[col], job->y[row]);
	status = GMT_NOERROR;

cleanup:
	if (Grid && GMT_Destroy_Data(GMT->parent, &Grid) != GMT_NOERROR)
		status = GMT_RUNTIME_ERROR;
	return status;
}

static const char *merge3d_gap_method_name(char method)
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

static int merge3d_write_xyz(struct GMT_CTRL *GMT, struct GMT_GRID *Grid,
	                         const char *path)
{
	FILE *fp = fopen(path, "w");
	size_t row, col;

	if (fp == NULL) return GMT_DATA_WRITE_ERROR;
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
	if (fclose(fp) != 0) return GMT_DATA_WRITE_ERROR;
	return GMT_NOERROR;
}

static int merge3d_fill_horizontal_layer(struct GMT_CTRL *GMT,
	                                     const struct MERGE3D_CTRL *Ctrl,
	                                     const struct MERGE3D_CUBE *cube,
	                                     size_t field, size_t iz,
	                                     double *values,
	                                     size_t *hole_count,
	                                     size_t *node_count)
{
	struct GMT_GRID *Grid = NULL, *Candidate = NULL;
	uint8_t *visited = NULL, *eligible = NULL;
	uint64_t *queue = NULL;
	char input[PATH_MAX] = {""}, candidate[PATH_MAX] = {""};
	char xyz[PATH_MAX] = {""}, command[4 * PATH_MAX + GMT_LEN512] = {""};
	static char *V_level = GMT_VERBOSE_CODES;
	double wesn[4], inc[2];
	size_t nx = cube->n[MERGE3D_X], ny = cube->n[MERGE3D_Y];
	size_t nxy, iy, ix, eligible_holes = 0, eligible_nodes = 0;
	size_t filled_nodes = 0;
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
	for (iy = 0; iy < ny; iy++) {
		for (ix = 0; ix < nx; ix++) {
			size_t node = iy * nx + ix, head = 0, tail = 0, q;
			size_t min_y = iy, max_y = iy, min_x = ix, max_x = ix;
			bool boundary = false, accepted;
			if (visited[node] ||
			    isfinite(values[merge3d_field_index(cube, field, ix, iy, iz)]))
				continue;
			visited[node] = 1;
			queue[tail++] = node;
			while (head < tail) {
				size_t current = queue[head++];
				size_t cy = current / nx, cx = current % nx;
				int dy, dx;
				if (cy == 0 || cy + 1 == ny || cx == 0 || cx + 1 == nx)
					boundary = true;
				if (cy < min_y) min_y = cy;
				if (cy > max_y) max_y = cy;
				if (cx < min_x) min_x = cx;
				if (cx > max_x) max_x = cx;
				for (dy = -1; dy <= 1; dy++) {
					for (dx = -1; dx <= 1; dx++) {
						long next_y, next_x;
						size_t next;
						if (dy == 0 && dx == 0) continue;
						next_y = (long)cy + dy;
						next_x = (long)cx + dx;
						if (next_y < 0 || next_x < 0 ||
						    next_y >= (long)ny || next_x >= (long)nx)
							continue;
						next = (size_t)next_y * nx + (size_t)next_x;
						if (visited[next] ||
						    isfinite(values[merge3d_field_index(
						        cube, field, (size_t)next_x, (size_t)next_y, iz)]))
							continue;
						visited[next] = 1;
						queue[tail++] = next;
					}
				}
			}
			accepted = !boundary &&
			           (!Ctrl->H.limited ||
			            (max_x - min_x + 1 <= Ctrl->H.max_gap &&
			             max_y - min_y + 1 <= Ctrl->H.max_gap));
			if (!accepted) continue;
			eligible_holes++;
			eligible_nodes += tail;
			for (q = 0; q < tail; q++) eligible[queue[q]] = 1;
		}
	}
	if (eligible_nodes == 0) {
		status = GMT_NOERROR;
		goto cleanup;
	}
	wesn[XLO] = cube->coordinate[MERGE3D_X][0];
	wesn[XHI] = cube->coordinate[MERGE3D_X][nx - 1];
	wesn[YLO] = cube->coordinate[MERGE3D_Y][0];
	wesn[YHI] = cube->coordinate[MERGE3D_Y][ny - 1];
	if (!merge3d_regular_coordinate(cube->coordinate[MERGE3D_X], nx, &inc[0]) ||
	    !merge3d_regular_coordinate(cube->coordinate[MERGE3D_Y], ny, &inc[1])) {
		GMT_Report(GMT->parent, GMT_MSG_ERROR,
		           "Option -H requires regular x and y coordinates in %s\n",
		           cube->source);
		status = GMT_RUNTIME_ERROR;
		goto cleanup;
	}
	Grid = GMT_Create_Data(GMT->parent, GMT_IS_GRID, GMT_IS_SURFACE,
	                       GMT_CONTAINER_AND_DATA, NULL, wesn, inc,
	                       GMT_GRID_NODE_REG, GMT_NOTSET, NULL);
	if (Grid == NULL) {
		status = GMT_MEMORY_ERROR;
		goto cleanup;
	}
	for (iy = 0; iy < ny; iy++) {
		size_t row = ny - 1 - iy;
		for (ix = 0; ix < nx; ix++)
			Grid->data[gmt_M_ijp(Grid->header, row, ix)] = (gmt_grdfloat)
			    values[merge3d_field_index(cube, field, ix, iy, iz)];
	}
	if (gmt_get_tempname(GMT->parent, "merge3d_gap_input", ".nc", input) ||
	    gmt_get_tempname(GMT->parent, "merge3d_gap_candidate", ".nc", candidate) ||
	    GMT_Write_Data(GMT->parent, GMT_IS_GRID, GMT_IS_FILE,
	                   GMT_IS_SURFACE, GMT_CONTAINER_AND_DATA,
	                   NULL, input, Grid) != GMT_NOERROR) {
		status = GMT_DATA_WRITE_ERROR;
		goto cleanup;
	}
	if (Ctrl->H.method == 'n') {
		if (Ctrl->H.argument > 0.0)
			snprintf(command, sizeof(command),
			         "%s -An%.12g -G%s -V%c --GMT_HISTORY=readonly",
			         input, Ctrl->H.argument, candidate,
			         V_level[GMT->current.setting.verbose]);
		else
			snprintf(command, sizeof(command),
			         "%s -An -G%s -V%c --GMT_HISTORY=readonly",
			         input, candidate, V_level[GMT->current.setting.verbose]);
		status = GMT_Call_Module(GMT->parent, "grdfill", GMT_MODULE_CMD, command);
	}
	else if (Ctrl->H.method == 's') {
		snprintf(command, sizeof(command),
		         "%s -As%.12g -G%s -V%c --GMT_HISTORY=readonly",
		         input, Ctrl->H.argument, candidate,
		         V_level[GMT->current.setting.verbose]);
		status = GMT_Call_Module(GMT->parent, "grdfill", GMT_MODULE_CMD, command);
	}
	else {
		const char *registration = Grid->header->registration ? "-rp" : "-rg";
		const char *geographic = gmt_M_is_geographic(GMT, GMT_IN) ? "-fg" : "";
		double radius = Ctrl->H.argument * MAX(inc[0], inc[1]);
		unsigned int minimum_sectors = MAX(1U, (Ctrl->H.sectors + 1U) / 2U);
		if (gmt_get_tempname(GMT->parent, "merge3d_gap_points", ".txt", xyz) ||
		    merge3d_write_xyz(GMT, Grid, xyz) != GMT_NOERROR) {
			status = GMT_DATA_WRITE_ERROR;
			goto cleanup;
		}
		if (Ctrl->H.method == 'l')
			snprintf(command, sizeof(command),
			         "%s -R%.17g/%.17g/%.17g/%.17g -I%.17g/%.17g %s %s "
			         "-Z -G%s -V%c --GMT_HISTORY=readonly",
			         xyz, wesn[XLO], wesn[XHI], wesn[YLO], wesn[YHI],
			         inc[0], inc[1], registration, geographic, candidate,
			         V_level[GMT->current.setting.verbose]);
		else if (Ctrl->H.method == 'a')
			snprintf(command, sizeof(command),
			         "%s -R%.17g/%.17g/%.17g/%.17g -I%.17g/%.17g "
			         "-S%.17g%s -N%u+m%u %s %s -G%s -V%c --GMT_HISTORY=readonly",
			         xyz, wesn[XLO], wesn[XHI], wesn[YLO], wesn[YHI],
			         inc[0], inc[1], radius, geographic[0] ? "d" : "",
			         Ctrl->H.sectors, minimum_sectors, registration, geographic,
			         candidate, V_level[GMT->current.setting.verbose]);
		else
			snprintf(command, sizeof(command),
			         "%s -R%.17g/%.17g/%.17g/%.17g -I%.17g/%.17g -T%.17g "
			         "%s %s %s -G%s -V%c --GMT_HISTORY=readonly",
			         xyz, wesn[XLO], wesn[XHI], wesn[YLO], wesn[YHI],
			         inc[0], inc[1], Ctrl->H.argument, registration, geographic,
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
	if (Candidate == NULL || Candidate->header->n_columns != nx ||
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
			values[merge3d_field_index(cube, field, ix, iy, iz)] = value;
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

static int merge3d_fill_horizontal_gaps(struct GMT_CTRL *GMT,
	                                    const struct MERGE3D_CTRL *Ctrl,
	                                    const struct MERGE3D_CUBE *cube,
	                                    size_t field, double *values)
{
	size_t iz, holes = 0, nodes = 0;

	for (iz = 0; iz < cube->n[MERGE3D_Z]; iz++) {
		int status = merge3d_fill_horizontal_layer(
		    GMT, Ctrl, cube, field, iz, values, &holes, &nodes);
		if (status != GMT_NOERROR) {
			GMT_Report(GMT->parent, GMT_MSG_ERROR,
			           "Unable to fill horizontal gaps in %s?%s at z=%.12g "
			           "using %s interpolation\n",
			           cube->path, cube->field[field].name,
			           cube->coordinate[MERGE3D_Z][iz],
			           merge3d_gap_method_name(Ctrl->H.method));
			return status;
		}
	}
	GMT_Report(GMT->parent, GMT_MSG_INFORMATION,
	           "Filled %zu nodes in %zu internal horizontal gap%s in %s?%s "
	           "using %s interpolation\n",
	           nodes, holes, holes == 1 ? "" : "s", cube->path,
	           cube->field[field].name, merge3d_gap_method_name(Ctrl->H.method));
	return GMT_NOERROR;
}

static int merge3d_sample_layer(struct GMT_CTRL *GMT,
                                const struct MERGE3D_CTRL *Ctrl,
                                const struct MERGE3D_JOB *job,
                                const struct MERGE3D_CUBE *cube,
                                size_t field, const double *values,
                                double z, double *output)
{
	size_t nx = cube->n[MERGE3D_X], ny = cube->n[MERGE3D_Y];
	size_t ix, iy, iz;
	double *trace = NULL, *native = NULL, *x_sampled = NULL;
	double *column = NULL, *target_column = NULL;
	double *bridge_x = NULL, *bridge_value = NULL;
	unsigned int horizontal_mode = merge3d_horizontal_mode(GMT);
	int status = GMT_MEMORY_ERROR;

	trace = calloc(cube->n[MERGE3D_Z], sizeof(*trace));
	native = calloc(nx * ny, sizeof(*native));
	x_sampled = calloc(job->nx * ny, sizeof(*x_sampled));
	column = calloc(ny, sizeof(*column));
	target_column = calloc(job->ny, sizeof(*target_column));
	if (Ctrl->S.bridge) {
		bridge_x = calloc(cube->n[MERGE3D_Z], sizeof(*bridge_x));
		bridge_value = calloc(cube->n[MERGE3D_Z], sizeof(*bridge_value));
	}
	if (!trace || !native || !x_sampled || !column || !target_column)
		goto cleanup;
	if (Ctrl->S.bridge && (!bridge_x || !bridge_value)) goto cleanup;
	for (iy = 0; iy < ny; iy++) {
		for (ix = 0; ix < nx; ix++) {
			double result;
			for (iz = 0; iz < cube->n[MERGE3D_Z]; iz++)
				trace[iz] = values[merge3d_field_index(cube, field,
				                                      ix, iy, iz)];
			status = merge3d_interpolate_finite(
			    GMT, cube->coordinate[MERGE3D_Z], trace,
			    cube->n[MERGE3D_Z], &z, 1, &result,
			    Ctrl->S.fit, Ctrl->S.mode, Ctrl->S.bridge,
			    Ctrl->S.max_gap, bridge_x, bridge_value);
			if (status != GMT_NOERROR) goto cleanup;
			native[iy * nx + ix] = result;
		}
	}
	status = merge3d_horizontal_bcr(GMT, job, cube, native, output);
	if (status == GMT_NOERROR) goto cleanup;
	if (status != GMT_NOTSET) goto cleanup;
	GMT_Report(GMT->parent, GMT_MSG_INFORMATION,
	           "%s has irregular horizontal coordinates; using separable GMT splines\n",
	           cube->source);
	for (iy = 0; iy < ny; iy++) {
		status = merge3d_interpolate_finite(
		    GMT, cube->coordinate[MERGE3D_X], &native[iy * nx], nx,
		    job->x, job->nx, &x_sampled[iy * job->nx],
		    0.0, horizontal_mode, false, 0.0, NULL, NULL);
		if (status != GMT_NOERROR) goto cleanup;
	}
	for (ix = 0; ix < job->nx; ix++) {
		for (iy = 0; iy < ny; iy++)
			column[iy] = x_sampled[iy * job->nx + ix];
		status = merge3d_interpolate_finite(
		    GMT, cube->coordinate[MERGE3D_Y], column, ny,
		    job->y, job->ny, target_column,
		    0.0, horizontal_mode, false, 0.0, NULL, NULL);
		if (status != GMT_NOERROR) goto cleanup;
		for (iy = 0; iy < job->ny; iy++)
			output[iy * job->nx + ix] = target_column[iy];
	}
	status = GMT_NOERROR;

cleanup:
	free(trace);
	free(native);
	free(x_sampled);
	free(column);
	free(target_column);
	free(bridge_x);
	free(bridge_value);
	return status;
}

static bool merge3d_source_covers(const struct MERGE3D_CUBE *cube,
                                  double x, double y, double z)
{
	return x >= cube->coordinate[MERGE3D_X][0] &&
	       x <= cube->coordinate[MERGE3D_X][cube->n[MERGE3D_X] - 1] &&
	       y >= cube->coordinate[MERGE3D_Y][0] &&
	       y <= cube->coordinate[MERGE3D_Y][cube->n[MERGE3D_Y] - 1] &&
	       z >= cube->coordinate[MERGE3D_Z][0] &&
	       z <= cube->coordinate[MERGE3D_Z][cube->n[MERGE3D_Z] - 1];
}

static bool merge3d_sign_allowed(int sign, double value, bool initialized)
{
	if (!initialized || sign == 0) return true;
	if (sign < 0) return value <= 0.0;
	return value >= 0.0;
}

static bool merge3d_support_contains(const struct MERGE3D_SPEC *spec,
                                      int col, int row, int layer)
{
	const window *support = &spec->support;
	int x = col - spec->support_i0, y = row - spec->support_j0;
	if (!spec->support_ready || col < spec->support_i0 || col > spec->support_i1 ||
	    row < spec->support_j0 || row > spec->support_j1 ||
	    layer < spec->support_k0 || layer > spec->support_k1) return false;
	return x >= support->nnx1[y] && x <= support->nnx2[y] &&
	       y >= support->nny1[x] && y <= support->nny2[x];
}

static int merge3d_weight_layer(const struct MERGE3D_CTRL *Ctrl,
                                const struct MERGE3D_JOB *job,
                                size_t layer, double *weights)
{
	size_t row, col, k, node;

	for (row = 0; row < job->ny; row++) {
		for (col = 0; col < job->nx; col++) {
			double value = 0.0;
			node = row * job->nx + col;
			if (Ctrl->C.active || !job->mergefile) {
				for (k = 0; k < job->count; k++)
					if (merge3d_source_covers(&job->spec[k].primary,
					                          job->x[col], job->y[row],
					                          job->z[layer])) {
						value = 1.0;
						break;
					}
			}
			else {
				size_t owner = SIZE_MAX;
				for (k = 0; k < job->count; k++) {
					if (merge3d_source_covers(&job->spec[k].primary,
					                          job->x[col], job->y[row],
					                          job->z[layer])) {
						if (!job->spec[k].has_secondary) break;
						if (!merge3d_support_contains(&job->spec[k], (int)col,
						                              (int)row, (int)layer)) continue;
						owner = k;
						if (merge3d_support_weight(&job->spec[k], (int)col,
						                           (int)row, (int)layer, &value))
							return GMT_RUNTIME_ERROR;
						break;
					}
				}
				if (Ctrl->A.active && owner < job->count && value > 0.0)
				for (k = owner + 1; k < job->count; k++) {
					double weight;
					if (!merge3d_same_secondary(&job->spec[owner],
					                            &job->spec[k]) ||
					    !strcmp(job->spec[k].primary_source,
					            job->spec[owner].secondary_source))
						continue;
					if (!merge3d_source_covers(&job->spec[k].primary,
					                          job->x[col], job->y[row],
					                          job->z[layer]))
						continue;
					if (!merge3d_support_contains(&job->spec[k], (int)col,
					                              (int)row, (int)layer)) continue;
					if (merge3d_support_weight(&job->spec[k], (int)col,
					                           (int)row, (int)layer,
					                           &weight))
						return GMT_RUNTIME_ERROR;
					if (weight > 0.0) value = MIN(1.0, value + weight);
				}
			}
			weights[node] = value;
		}
	}
	return GMT_NOERROR;
}

static int merge3d_compute_clobber(struct GMT_CTRL *GMT,
                                   const struct MERGE3D_CTRL *Ctrl,
                                   const struct MERGE3D_JOB *job,
                                   size_t field, size_t layer,
                                   double **primary_data, double *output)
{
	double *sample = calloc(job->nx * job->ny, sizeof(*sample));
	size_t k, node, plane = job->nx * job->ny;
	int status = GMT_MEMORY_ERROR;

	if (!sample) return GMT_MEMORY_ERROR;
	for (node = 0; node < plane; node++) output[node] = NAN;
	for (k = 0; k < job->count; k++) {
		status = merge3d_sample_layer(GMT, Ctrl, job,
		                              &job->spec[k].primary, field,
		                              primary_data[k], job->z[layer], sample);
		if (status != GMT_NOERROR) goto cleanup;
		for (node = 0; node < plane; node++) {
			double value = sample[node];
			bool set = isfinite(output[node]);
			if (!isfinite(value) ||
			    !merge3d_sign_allowed(Ctrl->C.sign, value, set))
				continue;
			if (!set) output[node] = value;
			else if (Ctrl->C.mode == MERGE3D_FIRST) continue;
			else if (Ctrl->C.mode == MERGE3D_LOWER &&
			         value >= output[node]) continue;
			else if (Ctrl->C.mode == MERGE3D_UPPER &&
			         value <= output[node]) continue;
			else output[node] = value;
		}
	}
	status = GMT_NOERROR;
cleanup:
	free(sample);
	return status;
}

static int merge3d_compute_regular(struct GMT_CTRL *GMT,
                                   const struct MERGE3D_CTRL *Ctrl,
                                   const struct MERGE3D_JOB *job,
                                   size_t field, size_t layer,
                                   double **primary_data,
                                   double **secondary_data,
                                   double *output)
{
	size_t plane = job->nx * job->ny, k, node;
	double *primary = calloc(plane, sizeof(*primary));
	double *secondary = calloc(plane, sizeof(*secondary));
	bool *claimed = calloc(plane, sizeof(*claimed));
	int status = GMT_MEMORY_ERROR;

	if (!primary || !secondary || !claimed) goto cleanup;
	for (node = 0; node < plane; node++) output[node] = NAN;
	for (k = 0; k < job->count; k++) {
		status = merge3d_sample_layer(GMT, Ctrl, job,
		                              &job->spec[k].primary, field,
		                              primary_data[k], job->z[layer], primary);
		if (status != GMT_NOERROR) goto cleanup;
		if (job->spec[k].has_secondary) {
			status = merge3d_sample_layer(GMT, Ctrl, job,
			                              &job->spec[k].secondary, field,
			                              secondary_data[k], job->z[layer],
			                              secondary);
			if (status != GMT_NOERROR) goto cleanup;
		}
		else
			for (node = 0; node < plane; node++) secondary[node] = NAN;
		for (node = 0; node < plane; node++) {
			size_t row = node / job->nx, col = node % job->nx;
			double p = primary[node], s = secondary[node], weight = 0.0;
			if (claimed[node] ||
			    !merge3d_source_covers(&job->spec[k].primary,
			                           job->x[col], job->y[row], job->z[layer]))
				continue;
			if (!job->spec[k].has_secondary) {
				if (!isfinite(p)) continue;
				claimed[node] = true;
				output[node] = p;
				continue;
			}
			claimed[node] = true;
			if (merge3d_support_weight(&job->spec[k], (int)col, (int)row,
			                           (int)layer, &weight)) {
				status = GMT_RUNTIME_ERROR;
				goto cleanup;
			}
			if (!isfinite(p)) {
				if (Ctrl->P.active && isfinite(s)) output[node] = s;
			}
			else if (!isfinite(s)) output[node] = p;
			else output[node] = weight * p + (1.0 - weight) * s;
		}
	}
	status = GMT_NOERROR;
cleanup:
	free(primary);
	free(secondary);
	free(claimed);
	return status;
}

static int merge3d_compute_aggregate(struct GMT_CTRL *GMT,
                                     const struct MERGE3D_CTRL *Ctrl,
                                     const struct MERGE3D_JOB *job,
                                     size_t field, size_t layer,
                                     double **primary_data,
                                     double **secondary_data,
                                     double *output)
{
	size_t plane = job->nx * job->ny, k, node;
	double *primary = calloc(plane, sizeof(*primary));
	double *secondary = calloc(plane, sizeof(*secondary));
	double *sum_valid = calloc(plane, sizeof(*sum_valid));
	double *sum_values = calloc(plane, sizeof(*sum_values));
	double *sum_geometry = calloc(plane, sizeof(*sum_geometry));
	double *secondary_weight = calloc(plane, sizeof(*secondary_weight));
	double *secondary_values = calloc(plane, sizeof(*secondary_values));
	double *background = calloc(plane, sizeof(*background));
	double *fallback = calloc(plane, sizeof(*fallback));
	bool *any_finite_primary = calloc(plane, sizeof(*any_finite_primary));
	size_t *owner = malloc(plane * sizeof(*owner));
	int status = GMT_MEMORY_ERROR;

	if (!primary || !secondary || !sum_valid || !sum_values || !sum_geometry ||
	    !secondary_weight || !secondary_values || !background || !fallback ||
	    !any_finite_primary || !owner)
		goto cleanup;
	for (node = 0; node < plane; node++) {
		background[node] = fallback[node] = NAN;
		owner[node] = SIZE_MAX;
	}
	for (k = 0; k < job->count; k++) {
		status = merge3d_sample_layer(GMT, Ctrl, job,
		                              &job->spec[k].primary, field,
		                              primary_data[k], job->z[layer], primary);
		if (status != GMT_NOERROR) goto cleanup;
		if (job->spec[k].has_secondary) {
			status = merge3d_sample_layer(GMT, Ctrl, job,
			                              &job->spec[k].secondary, field,
			                              secondary_data[k], job->z[layer],
			                              secondary);
			if (status != GMT_NOERROR) goto cleanup;
		}
		else
			for (node = 0; node < plane; node++) secondary[node] = NAN;
		for (node = 0; node < plane; node++) {
			size_t row = node / job->nx, col = node % job->nx;
			double weight;
			if (!merge3d_source_covers(&job->spec[k].primary,
			                           job->x[col], job->y[row], job->z[layer]))
				continue;
			if (owner[node] == SIZE_MAX) {
				if (!job->spec[k].has_secondary) {
					if (isfinite(primary[node])) {
						fallback[node] = primary[node];
						owner[node] = job->count;
					}
					continue;
				}
				owner[node] = k;
			}
			else if (owner[node] == job->count)
				continue;
			else if (!merge3d_same_secondary(&job->spec[owner[node]],
			                                &job->spec[k]) ||
			         !strcmp(job->spec[k].primary_source,
			                 job->spec[owner[node]].secondary_source))
				continue;
			if (isfinite(primary[node])) {
				any_finite_primary[node] = true;
				if (!isfinite(fallback[node])) fallback[node] = primary[node];
			}
			if (isfinite(secondary[node]) && !isfinite(background[node]))
				background[node] = secondary[node];
			if (merge3d_support_weight(&job->spec[k], (int)col, (int)row,
			                           (int)layer, &weight)) {
				status = GMT_RUNTIME_ERROR;
				goto cleanup;
			}
			sum_geometry[node] += weight;
			if (isfinite(primary[node]) && weight > 0.0) {
				sum_valid[node] += weight;
				sum_values[node] += weight * primary[node];
			}
			else if (Ctrl->P.active && isfinite(secondary[node]) && weight > 0.0) {
				secondary_weight[node] += weight;
				secondary_values[node] += weight * secondary[node];
			}
		}
	}
	for (node = 0; node < plane; node++) {
		if (owner[node] == job->count) output[node] = fallback[node];
		else {
			double bw = isfinite(background[node])
			          ? MAX(0.0, 1.0 - sum_geometry[node]) : 0.0;
			double denominator = sum_valid[node] + secondary_weight[node] + bw;
			double numerator = sum_values[node] + secondary_values[node] +
			                   bw * (isfinite(background[node]) ? background[node] : 0.0);
			if (denominator > 0.0 &&
			    (any_finite_primary[node] || Ctrl->P.active))
				output[node] = numerator / denominator;
			else if (any_finite_primary[node]) output[node] = fallback[node];
			else output[node] = NAN;
		}
	}
	status = GMT_NOERROR;
cleanup:
	free(primary);
	free(secondary);
	free(sum_valid);
	free(sum_values);
	free(sum_geometry);
	free(secondary_weight);
	free(secondary_values);
	free(background);
	free(fallback);
	free(any_finite_primary);
	free(owner);
	return status;
}

static const char *merge3d_output_axis_units(const struct MERGE3D_CTRL *Ctrl,
                                             const struct MERGE3D_JOB *job,
                                             size_t axis)
{
	if (Ctrl->Z.transform.axis_unit[axis])
		return Ctrl->Z.transform.axis_unit[axis];
	if (Ctrl->Z.transform.axis_scale[axis] != 1.0) return NULL;
	return job->spec[0].primary.coordinate_units[axis];
}

static void merge3d_output_plane(const double *input, float *output,
                                 size_t nx, size_t ny, double scale)
{
	size_t row, col;
	for (row = 0; row < ny; row++)
		for (col = 0; col < nx; col++) {
			double value = input[row * nx + col];
			output[row * nx + col] =
			    isfinite(value) ? (float)(value * scale) : NAN;
		}
}

static int merge3d_nc_error(struct GMTAPI_CTRL *API, int code,
                            const char *context)
{
	if (code == NC_NOERR) return GMT_NOERROR;
	GMT_Report(API, GMT_MSG_ERROR, "%s: %s\n", context, nc_strerror(code));
	return GMT_RUNTIME_ERROR;
}

static int merge3d_write_output(struct GMT_CTRL *GMT,
                                const struct MERGE3D_CTRL *Ctrl,
                                const struct MERGE3D_JOB *job)
{
	int ncid = -1, dimid[3], coordinate_varid[3], weight_varid = -1;
	int *field_varid = NULL;
	int dimensions[3];
	size_t field, layer, k, plane = job->nx * job->ny;
	size_t start[3] = {0, 0, 0}, count[3] = {1, 0, 0};
	double *output = NULL, *weights = NULL;
	double *coordinate_output[GQ_TRANSFORM_N_AXES] = {NULL, NULL, NULL};
	float *float_output = NULL;
	double **primary_data = NULL, **secondary_data = NULL;
	float fill = NAN;
	int status = GMT_RUNTIME_ERROR, code;

	field_varid = calloc(job->n_fields, sizeof(*field_varid));
	output = calloc(plane, sizeof(*output));
	weights = calloc(plane, sizeof(*weights));
	float_output = calloc(plane, sizeof(*float_output));
	coordinate_output[MERGE3D_X] = calloc(job->nx, sizeof(double));
	coordinate_output[MERGE3D_Y] = calloc(job->ny, sizeof(double));
	coordinate_output[MERGE3D_Z] = calloc(Ctrl->T.n, sizeof(double));
	primary_data = calloc(job->count, sizeof(*primary_data));
	secondary_data = calloc(job->count, sizeof(*secondary_data));
	if (!field_varid || !output || !weights || !float_output ||
	    !coordinate_output[0] || !coordinate_output[1] || !coordinate_output[2] ||
	    !primary_data || !secondary_data) {
		status = GMT_MEMORY_ERROR;
		goto cleanup;
	}
	code = nc_create(Ctrl->G.file, NC_CLOBBER | NC_NETCDF4, &ncid);
	if (merge3d_nc_error(GMT->parent, code, "Unable to create output cube"))
		goto cleanup;
	if (nc_def_dim(ncid, "x", job->nx, &dimid[MERGE3D_X]) != NC_NOERR ||
	    nc_def_dim(ncid, "y", job->ny, &dimid[MERGE3D_Y]) != NC_NOERR ||
	    nc_def_dim(ncid, "z", Ctrl->T.n, &dimid[MERGE3D_Z]) != NC_NOERR ||
	    nc_def_var(ncid, "x", NC_DOUBLE, 1, &dimid[MERGE3D_X],
	               &coordinate_varid[MERGE3D_X]) != NC_NOERR ||
	    nc_def_var(ncid, "y", NC_DOUBLE, 1, &dimid[MERGE3D_Y],
	               &coordinate_varid[MERGE3D_Y]) != NC_NOERR ||
	    nc_def_var(ncid, "z", NC_DOUBLE, 1, &dimid[MERGE3D_Z],
	               &coordinate_varid[MERGE3D_Z]) != NC_NOERR)
		goto netcdf_error;
	for (field = 0; field < GQ_TRANSFORM_N_AXES; field++) {
		const char *units = merge3d_output_axis_units(Ctrl, job, field);
		if (units)
			nc_put_att_text(ncid, coordinate_varid[field], "units",
			                strlen(units), units);
	}
	nc_put_att_text(ncid, coordinate_varid[MERGE3D_X], "axis", 1, "X");
	nc_put_att_text(ncid, coordinate_varid[MERGE3D_Y], "axis", 1, "Y");
	nc_put_att_text(ncid, coordinate_varid[MERGE3D_Z], "axis", 1, "Z");
	nc_put_att_text(ncid, NC_GLOBAL, "Conventions", 6, "CF-1.8");
	dimensions[0] = dimid[MERGE3D_Z];
	dimensions[1] = dimid[MERGE3D_Y];
	dimensions[2] = dimid[MERGE3D_X];
	if (!Ctrl->W.only) {
		for (field = 0; field < job->n_fields; field++) {
			if (nc_def_var(ncid, job->output_fields[field], NC_FLOAT, 3,
			               dimensions, &field_varid[field]) != NC_NOERR ||
			    nc_put_att_float(ncid, field_varid[field], "_FillValue",
			                     NC_FLOAT, 1, &fill) != NC_NOERR)
				goto netcdf_error;
			nc_def_var_deflate(ncid, field_varid[field], 1, 1, 2);
			{
				const char *target = gq_transform_value_unit(&Ctrl->Z.transform, field);
				const char *units = target ? target
				                  : (gq_transform_value_scale(&Ctrl->Z.transform, field) == 1.0
				                     ? job->output_units[field] : NULL);
				if (units)
				nc_put_att_text(ncid, field_varid[field], "units",
				                strlen(units), units);
			}
		}
	}
	if (Ctrl->W.active) {
		float valid_range[2] = {0.0f, 1.0f};
		if (nc_def_var(ncid, "weight", NC_FLOAT, 3, dimensions,
		               &weight_varid) != NC_NOERR ||
		    nc_put_att_float(ncid, weight_varid, "_FillValue",
		                     NC_FLOAT, 1, &fill) != NC_NOERR ||
		    nc_put_att_text(ncid, weight_varid, "long_name", 14,
		                    "merging weight") != NC_NOERR ||
		    nc_put_att_text(ncid, weight_varid, "units", 1, "1") != NC_NOERR ||
		    nc_put_att_float(ncid, weight_varid, "valid_range",
		                     NC_FLOAT, 2, valid_range) != NC_NOERR)
			goto netcdf_error;
		nc_def_var_deflate(ncid, weight_varid, 1, 1, 2);
	}
	for (k = 0; k < job->nx; k++)
		coordinate_output[MERGE3D_X][k] =
		    job->x[k] *
		    Ctrl->Z.transform.axis_scale[MERGE3D_X];
	for (k = 0; k < job->ny; k++)
		coordinate_output[MERGE3D_Y][k] =
		    job->y[k] *
		    Ctrl->Z.transform.axis_scale[MERGE3D_Y];
	for (layer = 0; layer < Ctrl->T.n; layer++)
		coordinate_output[MERGE3D_Z][layer] =
		    job->z[layer] *
		    Ctrl->Z.transform.axis_scale[MERGE3D_Z];
	for (field = 0; field < GQ_TRANSFORM_N_AXES; field++) {
		size_t length = field == MERGE3D_X ? job->nx
		              : field == MERGE3D_Y ? job->ny : Ctrl->T.n;
		for (k = 0; k < length; k++)
			if (coordinate_output[field][k] == 0.0)
				coordinate_output[field][k] = 0.0;
	}
	if (nc_enddef(ncid) != NC_NOERR ||
	    nc_put_var_double(ncid, coordinate_varid[MERGE3D_X],
	                      coordinate_output[MERGE3D_X]) != NC_NOERR ||
	    nc_put_var_double(ncid, coordinate_varid[MERGE3D_Y],
	                      coordinate_output[MERGE3D_Y]) != NC_NOERR ||
	    nc_put_var_double(ncid, coordinate_varid[MERGE3D_Z],
	                      coordinate_output[MERGE3D_Z]) != NC_NOERR)
		goto netcdf_error;
	count[1] = job->ny;
	count[2] = job->nx;

	if (Ctrl->W.active) {
		for (layer = 0; layer < Ctrl->T.n; layer++) {
			if (merge3d_weight_layer(Ctrl, job, layer, weights))
				goto cleanup;
			merge3d_output_plane(weights, float_output, job->nx, job->ny, 1.0);
			start[0] = layer;
			if (nc_put_vara_float(ncid, weight_varid, start, count,
			                      float_output) != NC_NOERR)
				goto netcdf_error;
		}
	}

	if (!Ctrl->W.only) {
		for (field = 0; field < job->n_fields; field++) {
			for (k = 0; k < job->count; k++) {
				status = merge3d_read_field(GMT->parent,
				                            &job->spec[k].primary,
				                            field, &primary_data[k]);
				if (status != GMT_NOERROR)
					goto cleanup;
				if (Ctrl->H.active) {
					status = merge3d_fill_horizontal_gaps(
					    GMT, Ctrl, &job->spec[k].primary, field,
					    primary_data[k]);
					if (status != GMT_NOERROR) goto cleanup;
				}
				if (job->spec[k].has_secondary) {
					status = merge3d_read_field(GMT->parent,
					                            &job->spec[k].secondary,
					                            field, &secondary_data[k]);
					if (status != GMT_NOERROR) goto cleanup;
					if (Ctrl->H.active) {
						status = merge3d_fill_horizontal_gaps(
						    GMT, Ctrl, &job->spec[k].secondary, field,
						    secondary_data[k]);
						if (status != GMT_NOERROR) goto cleanup;
					}
				}
			}
			for (layer = 0; layer < Ctrl->T.n; layer++) {
				if (Ctrl->C.active || !job->mergefile)
					status = merge3d_compute_clobber(
					    GMT, Ctrl, job, field, layer, primary_data, output);
				else if (Ctrl->A.active)
					status = merge3d_compute_aggregate(
					    GMT, Ctrl, job, field, layer, primary_data,
					    secondary_data, output);
				else
					status = merge3d_compute_regular(
					    GMT, Ctrl, job, field, layer, primary_data,
					    secondary_data, output);
				if (status != GMT_NOERROR) goto cleanup;
				merge3d_output_plane(
				    output, float_output, job->nx, job->ny,
				    gq_transform_value_scale(&Ctrl->Z.transform, field));
				start[0] = layer;
				if (nc_put_vara_float(ncid, field_varid[field], start, count,
				                      float_output) != NC_NOERR)
					goto netcdf_error;
				GMT_Report(GMT->parent, GMT_MSG_INFORMATION,
				           "Processed %s layer %zu of %zu\r",
				           job->output_fields[field], layer + 1, Ctrl->T.n);
			}
			GMT_Report(GMT->parent, GMT_MSG_INFORMATION,
			           "Processed %s: %zu layers\n",
			           job->output_fields[field], Ctrl->T.n);
			for (k = 0; k < job->count; k++) {
				free(primary_data[k]);
				free(secondary_data[k]);
				primary_data[k] = secondary_data[k] = NULL;
			}
		}
	}
	if (nc_close(ncid) != NC_NOERR) {
		ncid = -1;
		goto netcdf_error;
	}
	ncid = -1;
	status = GMT_NOERROR;
	goto cleanup;

netcdf_error:
	status = GMT_RUNTIME_ERROR;
	GMT_Report(GMT->parent, GMT_MSG_ERROR,
	           "NetCDF error while writing %s\n", Ctrl->G.file);
cleanup:
	if (ncid >= 0) nc_close(ncid);
	if (primary_data)
		for (k = 0; k < job->count; k++) {
			free(primary_data[k]);
			free(secondary_data ? secondary_data[k] : NULL);
		}
	free(field_varid);
	free(output);
	free(weights);
	free(float_output);
	for (k = 0; k < GQ_TRANSFORM_N_AXES; k++) free(coordinate_output[k]);
	free(primary_data);
	free(secondary_data);
	return status;
}

static int merge3d_run(struct GMT_CTRL *GMT, struct MERGE3D_CTRL *Ctrl)
{
	struct MERGE3D_JOB job;
	size_t k;
	int status;

	memset(&job, 0, sizeof(job));
	status = merge3d_read_specs(GMT->parent, Ctrl, &job);
	if (status != GMT_NOERROR) goto cleanup;
	status = merge3d_prepare_job(GMT, Ctrl, &job);
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
	for (k = 0; k < job.count; k++) {
		if (!strcmp(Ctrl->G.file, job.spec[k].primary.path) ||
		    (job.spec[k].has_secondary &&
		     !strcmp(Ctrl->G.file, job.spec[k].secondary.path))) {
			GMT_Report(GMT->parent, GMT_MSG_ERROR,
			           "Output file must differ from every input cube\n");
			status = GMT_PARSE_ERROR;
			goto cleanup;
		}
	}
	status = merge3d_write_output(GMT, Ctrl, &job);

cleanup:
	merge3d_job_free(&job);
	return status;
}

#define bailout(code) { gmt_M_free_options(mode); return (code); }
#define Return(code) { Free_Ctrl(GMT, Ctrl); gmt_end_module(GMT, GMT_cpy); bailout(code); }

EXTERN_MSC int GMT_merge3d(void *V_API, int mode, void *args)
{
	struct GMTAPI_CTRL *API = gmt_get_api_ptr(V_API);
	struct GMT_CTRL *GMT = NULL, *GMT_cpy = NULL;
	struct GMT_OPTION *options = NULL;
	struct MERGE3D_CTRL *Ctrl = NULL;
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
	status = merge3d_run(GMT, Ctrl);
	Return(status);
}
