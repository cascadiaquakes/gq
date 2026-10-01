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
 * topobath adds, removes, or replaces topography and/or bathymetry in a
 * multiparameter NetCDF model. Transformed input coordinates increase from
 * negative elevation above sea level to positive depth below sea level.
 * Calculations use an equivalent private positive-up coordinate.
 */

#include "gmt_dev.h"
#include "gq_remote.h"
#include "gq_transform.h"
#include "topobath_inc.h"
#include <netcdf.h>

#define THIS_MODULE_CLASSIC_NAME "topobath"
#define THIS_MODULE_MODERN_NAME "topobath"
#define THIS_MODULE_LIB "gq"
#define THIS_MODULE_LIB_PURPOSE "The CRESCENT cyberinfrastructure team supplements to the Generic Mapping Tools"
#define THIS_MODULE_PURPOSE "Add, remove, or replace topography and/or bathymetry in three-dimensional multiparameter NetCDF cubes"
#define THIS_MODULE_KEYS "<G{,GG}"
#define THIS_MODULE_NEEDS ""
#define THIS_MODULE_OPTIONS "RVdfn"

enum TOPOBATH_AXIS {
	TOPOBATH_X = 0,
	TOPOBATH_Y,
	TOPOBATH_Z
};

enum TOPOBATH_METHOD {
	TOPOBATH_PULL = 0,
	TOPOBATH_EXTEND,
	TOPOBATH_LINEAR
};

enum TOPOBATH_OPERATION {
	TOPOBATH_ADD = 0,
	TOPOBATH_REMOVE,
	TOPOBATH_REPLACE
};

enum TOPOBATH_SCOPE {
	TOPOBATH_BOTH = 0,
	TOPOBATH_TOPOGRAPHY,
	TOPOBATH_BATHYMETRY
};

enum TOPOBATH_CLASS_MODE {
	TOPOBATH_CLASS_GMT = 0,
	TOPOBATH_CLASS_MODEL,
	TOPOBATH_CLASS_ALL_LAND,
	TOPOBATH_CLASS_ALL_WET
};

enum TOPOBATH_CLASS {
	TOPOBATH_AMBIGUOUS = -1,
	TOPOBATH_UNRESOLVED = 0,
	TOPOBATH_LAND = 1,
	TOPOBATH_OCEAN = 2
};

struct TOPOBATH_GAP {
	bool active;
	char method;
	double argument;
	unsigned int sectors;
	bool limited;
	unsigned int max_gap;
};

struct TOPOBATH_PARAMETER {
	char *name;
	double value;
	double tolerance;
};

struct TOPOBATH_CTRL {
	struct {
		char **file;
		size_t n;
	} In;
	struct {
		bool active;
		char selection[GMT_LEN256];
	} A;
	struct {
		bool active;
		enum TOPOBATH_CLASS_MODE mode;
	} C;
	struct {
		bool active;
		char resolution;
	} D;
	struct {
		bool active;
		char *file;
	} E;
	struct {
		struct TOPOBATH_PARAMETER *item;
		size_t n;
	} F;
	struct {
		bool active;
		char *file;
	} G;
	struct TOPOBATH_GAP H;
	struct {
		bool active;
		double inc[2];
	} I;
	struct {
		bool active;
		char *file;
	} K;
	struct {
		struct TOPOBATH_PARAMETER *item;
		size_t n;
	} L;
	struct {
		bool active;
		enum TOPOBATH_METHOD mode;
	} M;
	struct {
		bool active;
		enum TOPOBATH_OPERATION mode;
		enum TOPOBATH_SCOPE scope;
	} O;
	struct {
		bool active;
		char *surface;
		char *classification;
	} Q;
	struct {
		bool active;
		unsigned int mode;
		double fit;
		bool bridge;
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
		struct TOPOBATH_PARAMETER *item;
		size_t n;
	} W;
	struct {
		bool active;
		struct GQ_TRANSFORM transform;
	} Z;
};

struct TOPOBATH_SOURCE_PARTS {
	char *path;
	char **names;
	size_t count;
	bool explicit_selector;
	bool has_sentinel;
	double sentinel;
	struct GQ_TRANSFORM transform;
};

struct TOPOBATH_FIELD {
	int varid;
	int dimids[3];
	int axis_position[3];
	char *name;
};

struct TOPOBATH_CUBE {
	char *source;
	char *path;
	int ncid;
	bool explicit_selector;
	bool has_sentinel;
	double sentinel;
	struct GQ_TRANSFORM transform;
	bool reverse[3];
	size_t n_fields;
	struct TOPOBATH_FIELD *field;
	int axis_dimid[3];
	int coordinate_varid[3];
	size_t n[3];
	double *coordinate[3];
	char *coordinate_name[3];
	double **cached_field;
	bool canonical;
	bool horizontal_resampled;
};

struct TOPOBATH_JOB {
	struct TOPOBATH_CUBE cube;
	size_t plane;
	double *old_surface;
	double *new_surface;
	signed char *classification;
	bool *modify;
	double *z;
	size_t nz;
	double dz;
	double vertical_sign;
	bool inferred;
	bool geographic;
	bool *have_water;
	double *water;
	double *water_tolerance;
	bool *have_air;
	double *air;
	bool *have_minimum;
	double *minimum;
};

struct TOPOBATH_OUTPUT {
	int ncid;
	int *field_varid;
	struct {
		int input_varid;
		int output_varid;
		nc_type type;
		size_t count;
		int ndims;
		size_t length[2];
		bool reverse[2];
	} *ancillary;
	size_t n_ancillary;
};

static void topobath_names_free(char **names, size_t count)
{
	size_t k;
	for (k = 0; k < count; k++) free(names[k]);
	free(names);
}

static void topobath_source_parts_free(struct TOPOBATH_SOURCE_PARTS *parts)
{
	if (parts == NULL) return;
	free(parts->path);
	topobath_names_free(parts->names, parts->count);
	gq_transform_free(&parts->transform);
	memset(parts, 0, sizeof(*parts));
}

static void topobath_cube_free(struct TOPOBATH_CUBE *cube)
{
	size_t axis, field;

	if (cube == NULL) return;
	if (cube->ncid >= 0) nc_close(cube->ncid);
	free(cube->source);
	free(cube->path);
	for (field = 0; field < cube->n_fields; field++)
		free(cube->field[field].name);
	if (cube->cached_field)
		for (field = 0; field < cube->n_fields; field++)
			free(cube->cached_field[field]);
	free(cube->cached_field);
	free(cube->field);
	for (axis = 0; axis < 3; axis++) {
		free(cube->coordinate[axis]);
		free(cube->coordinate_name[axis]);
	}
	gq_transform_free(&cube->transform);
	memset(cube, 0, sizeof(*cube));
	cube->ncid = -1;
}

static void topobath_job_free(struct TOPOBATH_JOB *job)
{
	if (job == NULL) return;
	topobath_cube_free(&job->cube);
	free(job->old_surface);
	free(job->new_surface);
	free(job->classification);
	free(job->modify);
	free(job->z);
	free(job->have_water);
	free(job->water);
	free(job->water_tolerance);
	free(job->have_air);
	free(job->air);
	free(job->have_minimum);
	free(job->minimum);
	memset(job, 0, sizeof(*job));
}

static void *New_Ctrl(struct GMT_CTRL *GMT)
{
	struct TOPOBATH_CTRL *Ctrl =
	    gmt_M_memory(GMT, NULL, 1, struct TOPOBATH_CTRL);

	strcpy(Ctrl->A.selection, "0/0/1");
	Ctrl->C.mode = TOPOBATH_CLASS_GMT;
	Ctrl->D.resolution = 'l';
	Ctrl->O.scope = TOPOBATH_BOTH;
	Ctrl->S.mode = GMT_SPLINE_LINEAR;
	gq_transform_init(&Ctrl->Z.transform);
	return Ctrl;
}

static void Free_Ctrl(struct GMT_CTRL *GMT, struct TOPOBATH_CTRL *Ctrl)
{
	size_t k;

	if (Ctrl == NULL) return;
	for (k = 0; k < Ctrl->In.n; k++) free(Ctrl->In.file[k]);
	free(Ctrl->In.file);
	free(Ctrl->E.file);
	for (k = 0; k < Ctrl->F.n; k++) free(Ctrl->F.item[k].name);
	free(Ctrl->F.item);
	free(Ctrl->G.file);
	free(Ctrl->K.file);
	for (k = 0; k < Ctrl->L.n; k++) free(Ctrl->L.item[k].name);
	free(Ctrl->L.item);
	free(Ctrl->Q.surface);
	free(Ctrl->Q.classification);
	for (k = 0; k < Ctrl->W.n; k++) free(Ctrl->W.item[k].name);
	free(Ctrl->W.item);
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
	          "usage: %s <model.nc>[?fields][+n<missing>]"
	          "[+x<sx>][+X<unit>][+y<sy>][+Y<unit>][+z<sz>][+Z<unit>]"
	          "[+v<scales>][+V<units>] "
	          "[<new_surface>[?field][+x<sx>][+X<unit>]"
	          "[+y<sy>][+Y<unit>][+v|z<scale>][+V|Z<unit>]] "
	          "-G<output.nc> -Oa|r|x[+t|b] "
	          "[-A<min_area>[/<min_level>/<max_level>]] [-Cg|m|l|w] "
	          "[-D<a|f|h|i|l|c|n>] [-E<old_surface>] "
	          "[-F<field>/<air>] [-H[n|l|a|s|m[<arg>]][+m<maxgap>]] "
	          "[-K<landmask>[?field][+x<sx>][+X<unit>]"
	          "[+y<sy>][+Y<unit>]] [-L<field>/<minimum>] "
	          "[-Mp|e|l] "
	          "[-Q<surface.nc>[+c<classification.nc>]] "
	          "[-Sa|c|e|l|n|s<p>[+g[<maxgap>]]] [-T<zmin>/<zmax>/<dz>] "
	          "[-W<field>/<value>[+t<tolerance>]] "
	          "[-Z[+x<sx>][+X<xunit>][+y<sy>][+Y<yunit>]"
	          "[+z<sz>][+Z<zunit>][+v<scales>][+V<units>]] "
	          "[%s] [-I<dx>[/<dy>]] "
	          "[%s] [%s] [%s]\n",
	          name, GMT_Rgeo_OPT, GMT_V_OPT, GMT_di_OPT, GMT_n_OPT);
	if (level == GMT_SYNOPSIS) return GMT_MODULE_SYNOPSIS;

	GMT_Message(API, GMT_TIME_NONE, "  REQUIRED ARGUMENTS:\n");
	GMT_Usage(API, 1, "\n<model.nc>[?field1,field2,...][+n<missing>]"
	                       "[+x<sx>][+X<unit>][+y<sy>][+Y<unit>]"
	                       "[+z<sz>][+Z<unit>][+v<scales>][+V<units>]");
	GMT_Usage(API, -2,
	          "Read selected numeric 3-D variables sharing x, y, and z. "
	          "Without a selector, process every compatible 3-D variable. "
	          "Lowercase coordinate and value modifiers scale the unpacked input. "
	          "Uppercase modifiers set output unit metadata. One +v/+V entry is "
	          "broadcast. Otherwise entries follow the selector field order. "
	          "Use +z to standardize the model vertical coordinate. After scaling, "
	          "z must increase from negative elevation above sea level to positive "
	          "depth below sea level. Scaling changes coordinate values but does not "
	          "reorder model layers. For example, +z-0.001+Zkm converts a stored "
	          "z axis of 4000 ... -16000 m to -4 ... 16 km.");
	GMT_Usage(API, 1, "\n<new_surface>[?field][+x<sx>][+X<unit>]"
	                       "[+y<sy>][+Y<unit>]"
	                       "[+v|z<scale>][+V|Z<unit>]");
	GMT_Usage(API, -2,
	          "Supply the requested topographic and/or bathymetric surface for "
	          "-Oa and -Ox. Its transformed values must use the model's working "
	          "units and convention: negative topographic elevation and positive "
	          "bathymetric depth. It is sampled after its coordinate and value "
	          "scales are applied. For a surface grid, +z/+Z are aliases for "
	          "+v/+V. +s<scale> is also accepted for compatibility with older "
	          "commands. New commands should use +v<scale> or +z<scale>. For example, "
	          "+z-0.001+Zkm converts positive-up relief in metres to the required "
	          "signed values in kilometres. Omit this grid with -Or.");
	GMT_Usage(API, 1, "\n-G<output.nc>");
	GMT_Usage(API, -2, "Write selected variables to a NetCDF model ordered (z,y,x).");
	GMT_Usage(API, 1, "\n-Oa|r|x[+t|b]");
	GMT_Usage(API, -2,
	          "Select add (a), remove (r), or replace (x). Add maps a nominally "
	          "sea-level surface to the requested surface and warns when selected "
	          "columns already have nonzero relief. Remove shifts the selected "
	          "free surface or seafloor to zero. Removing bathymetry removes the "
	          "water column. Replace removes the old selected surface and applies "
	          "the requested one. Append +t to operate on dry-land topography only "
	          "or +b to operate on wet-region bathymetry only. Otherwise operate "
	          "on both. The Wet/dry classification rather than the sign of a surface "
	          "value, determines which columns each scope selects.");

	GMT_Message(API, GMT_TIME_NONE, "\n  OPTIONAL ARGUMENTS:\n");
	GMT_Option(API, "R");
	GMT_Usage(API, -2,
	          "Set an output horizontal region contained within the transformed "
	          "model domain. The default uses the complete model region.");
	GMT_Usage(API, 1, "\n-I<dx>[/<dy>]");
	GMT_Usage(API, -2,
	          "Set positive output horizontal increments. By default, retain the "
	          "model increments. "
	          "Selected model fields, the inferred surface, and relief are sampled "
	          "onto this lattice. Additional variables that depend on x or y are "
	          "omitted when -R or -I changes the horizontal grid because topobath "
	          "does not resample them. Single-value metadata, such as map-projection "
	          "information, is retained.");
	GMT_Usage(API, 1,
	          "\n-A<min_area>[/<min_level>/<max_level>][+a<antarctica>]"
	          "[+l|r][+p<percent>]");
	GMT_Usage(API, -2,
	          "Select GSHHG features used by automatic shoreline classification. "
	          "Features below <min_area> km^2 and hierarchy levels outside "
	          "<min_level>/<max_level> are skipped. The default is 0/0/1, which "
	          "uses ocean and land only. "
	          "Levels are 0 ocean, 1 land, 2 lake, 3 island in lake, and 4 pond. "
	          "GMT's +a, +l, +r, and +p modifiers follow grdlandmask.");
	GMT_Usage(API, 1, "\n-Cg|m|l|w");
	GMT_Usage(API, -2,
	          "Choose classification precedence. Use g for GMT shoreline priority "
	          "(the default), m to let model evidence override GMT where model "
	          "evidence resolves a class, l to classify the entire domain as land, "
	          "or w to classify it as wet. Model wet evidence requires matching "
	          "-W signatures. A user -K mask is authoritative and cannot be "
	          "combined with -Cm, -Cl, or -Cw.");
	GMT_Usage(API, 1, "\n-D<a|f|h|i|l|c|n>");
	GMT_Usage(API, -2,
	          "Set the GMT shoreline resolution used by -Cg or as the prior for "
	          "-Cm. The default is low resolution (l). Use n to disable shoreline "
	          "classification. This option "
	          "has no effect for Cartesian models, -K, -Cl, or -Cw.");
	GMT_Usage(API, 1, "\n-E<old_surface>[?field][+x<sx>][+X<unit>]"
	                       "[+y<sy>][+Y<unit>]"
	                       "[+v|z<scale>][+V|Z<unit>]");
	GMT_Usage(API, -2,
	          "Supply the old free surface or seafloor in the transformed model's "
	          "signed vertical convention. Use +v or +z to scale its values. "
	          "Without -E, infer the shallowest boundary identified across the "
	          "selected fields. -W signatures locate wet-region seafloors. If "
	          "fields disagree, use the shallowest boundary and warn. Supplying "
	          "-E is highly recommended when a valid parameter can be missing at "
	          "the free surface, because an unrecognized NaN may otherwise be "
	          "treated as air. +s<scale> is also accepted for compatibility with "
	          "older commands. New commands should use +v<scale> or +z<scale>.");
	GMT_Usage(API, 1, "\n-F<field>/<air>");
	GMT_Usage(API, -2,
	          "Set the value written above the free surface for one selected "
	          "field. Repeat as needed. The default air value is NaN for every "
	          "field. A field that is NaN exactly at a known surface remains NaN. "
	          "the air value applies strictly above that surface.");
	GMT_Usage(API, 1, "\n-H[n|l|a|s|m[<arg>]][+m<maxgap>]");
	GMT_Usage(API, -2,
	          "Fill strictly internal horizontal missing-data holes in every native "
	          "x-y model layer before horizontal resampling, surface inference, and "
	          "the requested topography operation. Original non-missing nodes and "
	          "boundary-connected missing regions are preserved. Without -H, native "
	          "horizontal holes are not filled. Use linear Delaunay interpolation "
	          "when -H is given without a method. Available methods are:");
	GMT_Usage(API, 3,
	          "Nearest neighbor (n). Optionally append a search radius in grid nodes.");
	GMT_Usage(API, 3, "Linear Delaunay interpolation (l). This is the default.");
	GMT_Usage(API, 3,
	          "Local weighted average (a). Optionally append radius[/sectors] in "
	          "grid nodes. The default is 3/4.");
	GMT_Usage(API, 3,
	          "Spline interpolation (s). Optionally append tension in the range 0-1. "
	          "The default is 0.");
	GMT_Usage(API, 3,
	          "Minimum-curvature interpolation (m). Optionally append tension in the "
	          "range 0-1. The default is 0.");
	GMT_Usage(API, 3,
	          "+m Only fill holes whose x and y spans are both no larger than "
	          "<maxgap> grid nodes. The default is to fill all internal holes.");
	GMT_Usage(API, 1, "\n-K<landmask>[?field][+x<sx>][+X<unit>]"
	                       "[+y<sy>][+Y<unit>]");
	GMT_Usage(API, -2,
	          "Supply an authoritative classification grid with wet=0 and land=1. "
	          "It replaces GMT and model classification rather than serving only "
	          "as a prior. Use ?field to select the mask variable in a multi-variable "
	          "file. The +x and +y modifiers scale its coordinates, while +X and +Y "
	          "set their units. Mask values cannot be scaled.");
	GMT_Usage(API, 1, "\n-Mp|e|l");
	GMT_Usage(API, -2,
	          "Choose how -Oa or -Ox constructs selected dry-land topography. "
	          "Pull-up/push-down (p) shifts the whole column. Constant 1-D "
	          "extension (e) first maps the old surface to zero, then extends its "
	          "surface value to the requested elevation. Linear extension (l) "
	          "instead grades from that surface value to -L at the requested "
	          "elevation. Wet columns and dry columns whose requested surface is "
	          "below sea level always use pull-up/push-down. Newly created water "
	          "uses -W. Option -M is required for add/replace and is not used for "
	          "remove.");
	GMT_Usage(API, 1, "\n-L<field>/<minimum>");
	GMT_Usage(API, -2,
	          "Set the value reached at the requested dry-land surface by -Ml. "
	          "Repeat for every selected field when linear extension is used.");
	GMT_Usage(API, 1, "\n-Q<surface.nc>[+c<classification.nc>]");
	GMT_Usage(API, -2,
	          "Write the old surface coordinate used by the operation and, "
	          "optionally, classification codes: 0 unresolved, 1 land, and 2 wet. "
	          "Unresolved columns outside the model coverage remain NaN. Output "
	          "-Z coordinate scales and units apply to these grids. +z scales the "
	          "old surface, but classification codes cannot be scaled.");
	GMT_Usage(API, 1, "\n-Sa|c|e|l|n|s<p>[+g[<maxgap>]]");
	GMT_Usage(API, -2,
	          "Choose GMT vertical interpolation: Akima (a), cubic (c), step-up (e), "
	          "linear (l), nearest (n), or smoothing spline (s<p>) with non-negative "
	          "fit parameter p. Linear (l) is the default. Append +g to bridge "
	          "internal missing layers, optionally only when the bracketing "
	          "z-coordinate distance does not exceed maxgap. This is vertical "
	          "interpolation. Option -H fills enclosed holes in native x-y layers, "
	          "whereas common -n controls horizontal resampling onto the -R/-I lattice.");
	GMT_Usage(API, 1, "\n-T<zmin>/<zmax>/<dz>");
	GMT_Usage(API, -2,
	          "Set the working output vertical axis, where zmin is the top, zmax "
	          "is the bottom, zmin < zmax, and dz > 0. Values above sea level are "
	          "negative and values below it are positive. Without -T, retain the "
	          "smallest transformed input spacing and derive increasing bounds. "
	          "For example, -T-5/20/0.1 spans 5 km elevation to 20 km depth.");
	GMT_Usage(API, 1, "\n-W<field>/<value>[+t<tolerance>]");
	GMT_Usage(API, -2,
	          "Set one field's water value and the tolerance used to recognize old "
	          "water during inference. Repeat for every selected field when an old "
	          "wet surface must be inferred, or when add/replace creates or rebuilds "
	          "water. It is not required merely because wet cells exist when -E "
	          "supplies the old surface and the operation removes or retains their "
	          "water unchanged. NaN is accepted as a water value.");
	GMT_Usage(API, 3,
	          "Example: for model.nc?vp,vs,rho in km/s, km/s, and g/cm^3, "
	          "-Wvp/1.5 -Wvs/0 -Wrho/1.03 assigns those values to newly created "
	          "water and uses them to recognize an existing water column.");
	GMT_Usage(API, 3,
	          "Example: -Wvp/1.5+t0.05 treats existing vp values from 1.45 through "
	          "1.55 as water during surface inference. Newly created water is "
	          "written as exactly 1.5.");
	GMT_Usage(API, 1,
	          "\n-Z[+x<sx>][+X<xunit>][+y<sy>][+Y<yunit>]"
	          "[+z<sz>][+Z<zunit>][+v<scales>][+V<units>]");
	GMT_Usage(API, -2,
	          "Transform output coordinates and selected fields after all topography "
	          "operations. +x, +y, and +z scale output coordinates. +X, +Y, and +Z "
	          "set coordinate units. +v supplies one broadcast scale or one scale "
	          "per selected field. +V sets field units. Field lists follow the "
	          "model selector order. Scaling occurs in place and does not reorder "
	          "coordinates, model layers, fields, or ancillary variables. A negative "
	          "axis scale therefore produces a decreasing output axis.");
	GMT_Usage(API, 3,
	          "Example: for model.nc?vp,vs, -Z+x0.001+Xkm+y0.001+Ykm"
	          "+z-1000+Zm+v0.001,0.001+Vkm/s,km/s converts x and y from m "
	          "to km, restores a positive-up z axis in m, and converts vp and vs "
	          "from m/s to km/s without reordering the cube.");
	GMT_Option(API, "V,di,n,.");
	GMT_Message(API, GMT_TIME_NONE, "\n  OPERATION ORDER AND CAVEATS:\n");
	GMT_Usage(API, -2,
	          "Input coordinate and field scaling occurs first. The transformed z "
	          "axis must increase from top to bottom. Scale a contrary axis with "
	          "the model's +z modifier. Next -H optionally fills strictly internal "
	          "holes in each native x-y model layer, and -n resamples the requested "
	          "-R/-I model lattice. Topobath then classifies wet and dry columns, obtains the "
	          "old surface from -E or model inference, and applies -O/-M. Vertical "
	          "-S interpolation and optional gap bridging are used while sampling "
	          "the moved columns. Output -Z coordinate and field scaling occurs last "
	          "and does not reorder coordinates, layers, fields, or ancillary variables.");
	GMT_Usage(API, -2,
	          "Classification is independent of surface sign, so dry land below "
	          "sea level is valid (e.g., Death Valley or Dead Sea). A requested "
	          "surface above sea level in a classified wet cell is physically "
	          "inconsistent and is clamped to sea level with a warning. This "
	          "accommodates small coastline differences between masks and "
	          "interpolated relief. Below each identified surface, the existing "
	          "column moves with the adjustment. Missing values in one field are "
	          "preserved even when another field identifies the surface. A large "
	          "pull-up can move a column far enough that its output base lies "
	          "outside the source model. Those exposed values are written as NaN "
	          "and a warning is reported.");
	return GMT_MODULE_USAGE;
}

static int topobath_parse_number(const char *text, double *value, bool allow_nan)
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
	if (errno || end == copy || *end ||
	    (!isfinite(numerator) && !(allow_nan && isnan(numerator))))
		return GMT_PARSE_ERROR;
	if (slash) {
		errno = 0;
		denominator = strtod(slash, &end);
		if (errno || end == slash || *end || !isfinite(denominator) ||
		    denominator == 0.0)
			return GMT_PARSE_ERROR;
	}
	*value = numerator / denominator;
	return (isfinite(*value) || (allow_nan && isnan(*value)))
	       ? GMT_NOERROR : GMT_PARSE_ERROR;
}

static int topobath_parse_range(struct GMTAPI_CTRL *API, const char *text,
                                struct TOPOBATH_CTRL *Ctrl)
{
	char copy[GMT_LEN256], *token = NULL, *save = NULL;
	double value[3], intervals, adjusted, tolerance;
	size_t n = 0;

	if (text == NULL || strlen(text) >= sizeof(copy)) return GMT_PARSE_ERROR;
	strcpy(copy, text);
	for (token = strtok_r(copy, "/", &save); token && n < 3;
	     token = strtok_r(NULL, "/", &save)) {
		if (topobath_parse_number(token, &value[n], false)) break;
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
		           "Option -T: Adjusting bottom from %.12g to %.12g\n",
		           value[1], adjusted);
	Ctrl->T.min = value[0];
	Ctrl->T.max = adjusted;
	Ctrl->T.inc = value[2];
	return GMT_NOERROR;
}

static int topobath_parse_interpolation(struct GMTAPI_CTRL *API,
                                        const char *text,
                                        struct TOPOBATH_CTRL *Ctrl)
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

static int topobath_parse_gap_option(struct GMTAPI_CTRL *API, const char *text,
                                     struct TOPOBATH_GAP *H)
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

static int topobath_parse_increment(struct GMTAPI_CTRL *API, const char *text,
                                    double inc[2])
{
	char copy[GMT_LEN128], *slash;

	if (text == NULL || strlen(text) >= sizeof(copy)) return GMT_PARSE_ERROR;
	strcpy(copy, text);
	slash = strchr(copy, '/');
	if (slash) {
		*slash++ = '\0';
		if (strchr(slash, '/')) goto bad;
	}
	if (topobath_parse_number(copy, &inc[0], false) ||
	    (slash && topobath_parse_number(slash, &inc[1], false)))
		goto bad;
	if (!slash) inc[1] = inc[0];
	if (inc[0] <= 0.0 || inc[1] <= 0.0) {
		GMT_Report(API, GMT_MSG_ERROR, "Option -I increments must be positive\n");
		return GMT_PARSE_ERROR;
	}
	return GMT_NOERROR;
bad:
	GMT_Report(API, GMT_MSG_ERROR, "Option -I: Invalid increment %s\n",
	           text ? text : "");
	return GMT_PARSE_ERROR;
}

static int topobath_parse_output_scale(struct GMTAPI_CTRL *API,
                                       const char *text,
                                       struct TOPOBATH_CTRL *Ctrl)
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

static int topobath_append_parameter(struct GMTAPI_CTRL *API,
                                     struct TOPOBATH_PARAMETER **items,
                                     size_t *count, const char *text,
                                     const char *kind, bool allow_nan,
                                     bool allow_tolerance)
{
	struct TOPOBATH_PARAMETER item = {0}, *next;
	char copy[GMT_LEN256], *slash, *modifier;
	size_t k;

	if (text == NULL || strlen(text) >= sizeof(copy)) goto bad;
	strcpy(copy, text);
	slash = strchr(copy, '/');
	if (!slash) goto bad;
	*slash++ = '\0';
	if (!copy[0] || !slash[0]) goto bad;
	modifier = strstr(slash, "+t");
	if (modifier) {
		*modifier = '\0';
		modifier += 2;
		if (!allow_tolerance || !modifier[0] ||
		    topobath_parse_number(modifier, &item.tolerance, false) ||
		    item.tolerance < 0.0)
			goto bad;
	}
	if (topobath_parse_number(slash, &item.value, allow_nan)) goto bad;
	if (!allow_nan && !isfinite(item.value)) goto bad;
	if (allow_tolerance && !modifier)
		item.tolerance = isnan(item.value)
		               ? 0.0 : MAX(1.0e-6, fabs(item.value) * 1.0e-4);
	for (k = 0; k < *count; k++)
		if (!strcmp((*items)[k].name, copy)) {
			GMT_Report(API, GMT_MSG_ERROR,
			           "Parameter for field %s was specified more than once\n", copy);
			return GMT_PARSE_ERROR;
		}
	item.name = strdup(copy);
	if (item.name == NULL) return GMT_MEMORY_ERROR;
	next = realloc(*items, (*count + 1) * sizeof(*next));
	if (next == NULL) {
		free(item.name);
		return GMT_MEMORY_ERROR;
	}
	*items = next;
	(*items)[(*count)++] = item;
	return GMT_NOERROR;
bad:
	GMT_Report(API, GMT_MSG_ERROR,
	           "Expected %s as <field>/<value>%s: %s\n",
	           kind, allow_tolerance ? "[+t<tolerance>]" : "",
	           text ? text : "");
	return GMT_PARSE_ERROR;
}

static int topobath_parse_operation(struct GMTAPI_CTRL *API, const char *text,
                                    struct TOPOBATH_CTRL *Ctrl)
{
	char copy[GMT_LEN64], *modifier;

	if (text == NULL || !text[0] || strlen(text) >= sizeof(copy)) goto bad;
	strcpy(copy, text);
	modifier = strchr(copy, '+');
	if (modifier) *modifier++ = '\0';
	if (!strcmp(copy, "a")) Ctrl->O.mode = TOPOBATH_ADD;
	else if (!strcmp(copy, "r")) Ctrl->O.mode = TOPOBATH_REMOVE;
	else if (!strcmp(copy, "x")) Ctrl->O.mode = TOPOBATH_REPLACE;
	else goto bad;
	if (modifier) {
		if (!strcmp(modifier, "t")) Ctrl->O.scope = TOPOBATH_TOPOGRAPHY;
		else if (!strcmp(modifier, "b")) Ctrl->O.scope = TOPOBATH_BATHYMETRY;
		else goto bad;
	}
	return GMT_NOERROR;
bad:
	GMT_Report(API, GMT_MSG_ERROR,
	           "Option -O must be a, r, or x, optionally followed by +t or +b\n");
	return GMT_PARSE_ERROR;
}

static int topobath_parse_q(struct GMTAPI_CTRL *API, const char *text,
                            struct TOPOBATH_CTRL *Ctrl)
{
	char *copy, *modifier;

	if (text == NULL || !text[0]) return GMT_PARSE_ERROR;
	copy = strdup(text);
	if (copy == NULL) return GMT_MEMORY_ERROR;
	modifier = strstr(copy, "+c");
	if (modifier) {
		*modifier = '\0';
		modifier += 2;
		if (!modifier[0] || strchr(modifier, '+')) goto bad;
		Ctrl->Q.classification = strdup(modifier);
		if (Ctrl->Q.classification == NULL) {
			free(copy);
			return GMT_MEMORY_ERROR;
		}
	}
	if (!copy[0]) goto bad;
	Ctrl->Q.surface = strdup(copy);
	free(copy);
	return Ctrl->Q.surface ? GMT_NOERROR : GMT_MEMORY_ERROR;
bad:
	free(copy);
	GMT_Report(API, GMT_MSG_ERROR,
	           "Option -Q must be <surface.nc>[+c<classification.nc>]\n");
	return GMT_PARSE_ERROR;
}

static int parse(struct GMT_CTRL *GMT, struct TOPOBATH_CTRL *Ctrl,
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
				if (Ctrl->In.file[Ctrl->In.n - 1] == NULL)
					return GMT_MEMORY_ERROR;
				break;
			}
			case 'A': {
				char selection[GMT_LEN256];
				struct GMT_SHORE_SELECT info = {0};
				n_errors += gmt_M_repeated_module_option(API, Ctrl->A.active);
				if (!opt->arg[0] || strlen(opt->arg) >= sizeof(selection)) {
					n_errors++;
					break;
				}
				strcpy(selection, opt->arg);
				info.high = GSHHS_MAX_LEVEL;
				if (gmt_set_levels(GMT, selection, &info) != GMT_NOERROR)
					n_errors++;
				else
					strcpy(Ctrl->A.selection, opt->arg);
				break;
			}
			case 'C':
				n_errors += gmt_M_repeated_module_option(API, Ctrl->C.active);
				if (!opt->arg[0] || opt->arg[1]) n_errors++;
				else if (opt->arg[0] == 'g') Ctrl->C.mode = TOPOBATH_CLASS_GMT;
				else if (opt->arg[0] == 'm') Ctrl->C.mode = TOPOBATH_CLASS_MODEL;
				else if (opt->arg[0] == 'l') Ctrl->C.mode = TOPOBATH_CLASS_ALL_LAND;
				else if (opt->arg[0] == 'w') Ctrl->C.mode = TOPOBATH_CLASS_ALL_WET;
				else n_errors++;
				break;
			case 'D':
				n_errors += gmt_M_repeated_module_option(API, Ctrl->D.active);
				if (!opt->arg[0] || opt->arg[1] ||
				    strchr("afhilcn", opt->arg[0]) == NULL)
					n_errors++;
				else
					Ctrl->D.resolution = opt->arg[0];
				break;
			case 'E':
				n_errors += gmt_M_repeated_module_option(API, Ctrl->E.active);
				if (!opt->arg[0]) n_errors++;
				else Ctrl->E.file = strdup(opt->arg);
				break;
			case 'F':
				n_errors += topobath_append_parameter(
				    API, &Ctrl->F.item, &Ctrl->F.n, opt->arg,
				    "air value", true, false);
				break;
			case 'G':
				n_errors += gmt_M_repeated_module_option(API, Ctrl->G.active);
				if (!opt->arg[0]) n_errors++;
				else Ctrl->G.file = strdup(opt->arg);
				break;
			case 'H':
				n_errors += gmt_M_repeated_module_option(API, Ctrl->H.active);
				n_errors += topobath_parse_gap_option(API, opt->arg, &Ctrl->H);
				break;
			case 'I':
				n_errors += gmt_M_repeated_module_option(API, Ctrl->I.active);
				n_errors += topobath_parse_increment(API, opt->arg, Ctrl->I.inc);
				break;
			case 'K':
				n_errors += gmt_M_repeated_module_option(API, Ctrl->K.active);
				if (!opt->arg[0]) n_errors++;
				else Ctrl->K.file = strdup(opt->arg);
				break;
			case 'L':
				n_errors += topobath_append_parameter(
				    API, &Ctrl->L.item, &Ctrl->L.n, opt->arg,
				    "linear minimum", false, false);
				break;
			case 'M':
				n_errors += gmt_M_repeated_module_option(API, Ctrl->M.active);
					switch (opt->arg[0]) {
						case 'p': Ctrl->M.mode = TOPOBATH_PULL; break;
						case 'e': Ctrl->M.mode = TOPOBATH_EXTEND; break;
						case 'l': Ctrl->M.mode = TOPOBATH_LINEAR; break;
						default: n_errors++; break;
					}
					if (opt->arg[1]) n_errors++;
					break;
			case 'O':
				n_errors += gmt_M_repeated_module_option(API, Ctrl->O.active);
				n_errors += topobath_parse_operation(API, opt->arg, Ctrl);
				break;
			case 'Q':
				n_errors += gmt_M_repeated_module_option(API, Ctrl->Q.active);
				n_errors += topobath_parse_q(API, opt->arg, Ctrl);
				break;
			case 'S':
				n_errors += gmt_M_repeated_module_option(API, Ctrl->S.active);
				n_errors += topobath_parse_interpolation(API, opt->arg, Ctrl);
				break;
			case 'T':
				n_errors += gmt_M_repeated_module_option(API, Ctrl->T.active);
				n_errors += topobath_parse_range(API, opt->arg, Ctrl);
				break;
			case 'W':
				n_errors += topobath_append_parameter(
				    API, &Ctrl->W.item, &Ctrl->W.n, opt->arg,
				    "water value", true, true);
				break;
			case 'Z':
				n_errors += gmt_M_repeated_module_option(API, Ctrl->Z.active);
				n_errors += topobath_parse_output_scale(API, opt->arg, Ctrl);
				break;
			default:
				n_errors += gmt_default_option_error(GMT, opt);
				break;
		}
	}
	n_errors += gmt_M_check_condition(GMT, Ctrl->In.n < 1 || Ctrl->In.n > 2,
	                                  "Specify one model and at most one new surface grid\n");
	n_errors += gmt_M_check_condition(GMT, !Ctrl->G.active,
	                                  "Option -G is required\n");
	n_errors += gmt_M_check_condition(GMT, !Ctrl->O.active,
	                                  "Option -O is required\n");
	n_errors += gmt_M_check_condition(
	    GMT, Ctrl->O.active && Ctrl->O.mode == TOPOBATH_REMOVE && Ctrl->In.n == 2,
	    "Removing a surface does not accept a new topography grid\n");
	n_errors += gmt_M_check_condition(
	    GMT, Ctrl->O.active && Ctrl->O.mode != TOPOBATH_REMOVE && Ctrl->In.n != 2,
	    "Adding or replacing a surface requires a new topography grid\n");
	n_errors += gmt_M_check_condition(
	    GMT, Ctrl->O.active && Ctrl->O.mode != TOPOBATH_REMOVE && !Ctrl->M.active,
	    "Option -M is required with -Oa and -Ox\n");
	n_errors += gmt_M_check_condition(
	    GMT, Ctrl->O.active && Ctrl->O.mode == TOPOBATH_REMOVE && Ctrl->M.active,
	    "Option -M is not used with -Or\n");
	n_errors += gmt_M_check_condition(
	    GMT, Ctrl->K.active && Ctrl->C.active && Ctrl->C.mode != TOPOBATH_CLASS_GMT,
	    "Option -K is authoritative and cannot be combined with -Cm, -Cl, or -Cw\n");
	if (n_errors)
		GMT_Report(API, GMT_MSG_ERROR,
		           "Invalid topobath options; use topobath -? for usage\n");
	return n_errors ? GMT_PARSE_ERROR : GMT_NOERROR;
}

static char *topobath_text_attribute(int ncid, int varid, const char *name)
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

static int topobath_numeric_type(nc_type type)
{
	return type == NC_BYTE || type == NC_UBYTE || type == NC_SHORT ||
	       type == NC_USHORT || type == NC_INT || type == NC_UINT ||
	       type == NC_INT64 || type == NC_UINT64 || type == NC_FLOAT ||
	       type == NC_DOUBLE;
}

static char *topobath_modifier_end(char *start)
{
	char *p;
	for (p = start; *p; p++) {
		if (*p != '+') continue;
		if (p > start && (p[-1] == 'e' || p[-1] == 'E')) continue;
		return p;
	}
	return p;
}

static int topobath_source_parts(struct GMTAPI_CTRL *API, const char *source,
                                 struct TOPOBATH_SOURCE_PARTS *parts)
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
		modifier = topobath_modifier_end(names);
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
	}
	else {
		modifier = topobath_modifier_end(copy);
		if (*modifier) *modifier++ = '\0';
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
	status = gq_resolve_remote_path(API, GMT_IS_GRID, copy, &parts->path);
	free(copy);
	if (status != GMT_NOERROR) topobath_source_parts_free(parts);
	return status;

bad_modifier:
	GMT_Report(API, GMT_MSG_ERROR,
	           "Unsupported model transform in %s; use +n, coordinate, "
	           "value, and target-unit modifiers\n", source);
	goto fail;
bad:
	GMT_Report(API, GMT_MSG_ERROR, "Invalid NetCDF selector %s\n", source);
	goto fail;
memory:
	GMT_Report(API, GMT_MSG_ERROR, "Unable to allocate selector for %s\n", source);
fail:
	free(copy);
	topobath_source_parts_free(parts);
	return GMT_PARSE_ERROR;
}

static int topobath_axis_name(const char *text)
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
		return TOPOBATH_X;
	if (!strcmp(lower, "y") || !strcmp(lower, "lat") ||
	    !strcmp(lower, "latitude") || !strcmp(lower, "northing"))
		return TOPOBATH_Y;
	if (!strcmp(lower, "z") || !strcmp(lower, "depth") ||
	    !strcmp(lower, "elevation") || !strcmp(lower, "altitude") ||
	    !strcmp(lower, "level"))
		return TOPOBATH_Z;
	return -1;
}

static int topobath_coordinate_axis(int ncid, int varid, const char *name)
{
	char *attribute;
	int axis = -1;

	attribute = topobath_text_attribute(ncid, varid, "axis");
	if (attribute) {
		if ((attribute[0] == 'X' || attribute[0] == 'x') && !attribute[1])
			axis = TOPOBATH_X;
		else if ((attribute[0] == 'Y' || attribute[0] == 'y') && !attribute[1])
			axis = TOPOBATH_Y;
		else if ((attribute[0] == 'Z' || attribute[0] == 'z') && !attribute[1])
			axis = TOPOBATH_Z;
		free(attribute);
		if (axis >= 0) return axis;
	}
	attribute = topobath_text_attribute(ncid, varid, "standard_name");
	if (attribute) {
		if (strstr(attribute, "longitude") ||
		    strstr(attribute, "projection_x_coordinate"))
			axis = TOPOBATH_X;
		else if (strstr(attribute, "latitude") ||
		         strstr(attribute, "projection_y_coordinate"))
			axis = TOPOBATH_Y;
		else if (strstr(attribute, "depth") || strstr(attribute, "height") ||
		         strstr(attribute, "altitude"))
			axis = TOPOBATH_Z;
		free(attribute);
		if (axis >= 0) return axis;
	}
	return topobath_axis_name(name);
}

static int topobath_variable_3d(int ncid, int varid, int dimids[3])
{
	nc_type type;
	int ndims;

	if (nc_inq_vartype(ncid, varid, &type) != NC_NOERR ||
	    !topobath_numeric_type(type) ||
	    nc_inq_varndims(ncid, varid, &ndims) != NC_NOERR || ndims != 3 ||
	    nc_inq_vardimid(ncid, varid, dimids) != NC_NOERR)
		return GMT_DATA_READ_ERROR;
	return GMT_NOERROR;
}

static int topobath_identify_axes(struct GMTAPI_CTRL *API,
                                  struct TOPOBATH_CUBE *cube,
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
		axis = topobath_coordinate_axis(cube->ncid, coordinate_varid, dim_name);
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

static bool topobath_same_dimensions(const int a[3], const int b[3])
{
	int i, j;
	for (i = 0; i < 3; i++) {
		bool found = false;
		for (j = 0; j < 3; j++)
			if (a[i] == b[j]) found = true;
		if (!found) return false;
	}
	return true;
}

static int topobath_default_fields(struct GMTAPI_CTRL *API,
                                   struct TOPOBATH_CUBE *cube,
                                   struct TOPOBATH_SOURCE_PARTS *parts,
                                   int first_dimids[3])
{
	int nvars, varid, first = -1;
	size_t capacity = 0;

	if (nc_inq_nvars(cube->ncid, &nvars) != NC_NOERR)
		return GMT_DATA_READ_ERROR;
	for (varid = 0; varid < nvars; varid++) {
		int dims[3];
		if (topobath_variable_3d(cube->ncid, varid, dims) == GMT_NOERROR) {
			first = varid;
			memcpy(first_dimids, dims, 3 * sizeof(int));
			break;
		}
	}
	if (first < 0) {
		GMT_Report(API, GMT_MSG_ERROR,
		           "No numeric three-dimensional data variable found in %s\n",
		           cube->path);
		return GMT_DATA_READ_ERROR;
	}
	if (topobath_identify_axes(API, cube, first_dimids))
		return GMT_DATA_READ_ERROR;
	for (varid = 0; varid < nvars; varid++) {
		char name[NC_MAX_NAME + 1];
		int dims[3];
		char **next;
		if (topobath_variable_3d(cube->ncid, varid, dims) ||
		    !topobath_same_dimensions(first_dimids, dims))
			continue;
		if (parts->count == capacity) {
			capacity = capacity ? capacity * 2 : 4;
			next = realloc(parts->names, capacity * sizeof(*next));
			if (next == NULL) return GMT_MEMORY_ERROR;
			parts->names = next;
		}
		if (nc_inq_varname(cube->ncid, varid, name) != NC_NOERR)
			return GMT_DATA_READ_ERROR;
		parts->names[parts->count] = strdup(name);
		if (parts->names[parts->count] == NULL) return GMT_MEMORY_ERROR;
		parts->count++;
	}
	return GMT_NOERROR;
}

static int topobath_open_cube(struct GMTAPI_CTRL *API, const char *source,
                              struct TOPOBATH_CUBE *cube)
{
	struct TOPOBATH_SOURCE_PARTS parts;
	int first_dimids[3], status = GMT_DATA_READ_ERROR;
	size_t field, axis;

	memset(cube, 0, sizeof(*cube));
	gq_transform_init(&cube->transform);
	cube->ncid = -1;
	if (topobath_source_parts(API, source, &parts)) return GMT_PARSE_ERROR;
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
		if (topobath_default_fields(API, cube, &parts, first_dimids))
			goto cleanup;
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
		struct TOPOBATH_FIELD *item = &cube->field[field];
		int position;
		if (nc_inq_varid(cube->ncid, parts.names[field], &item->varid) != NC_NOERR ||
		    topobath_variable_3d(cube->ncid, item->varid, item->dimids)) {
			GMT_Report(API, GMT_MSG_ERROR,
			           "%s is not a numeric three-dimensional variable in %s\n",
			           parts.names[field], cube->path);
			goto cleanup;
		}
		if (field == 0 && cube->axis_dimid[0] == 0 &&
		    cube->axis_dimid[1] == 0 && cube->axis_dimid[2] == 0) {
			memcpy(first_dimids, item->dimids, sizeof(first_dimids));
			if (topobath_identify_axes(API, cube, first_dimids)) goto cleanup;
		}
		if (!topobath_same_dimensions(first_dimids, item->dimids)) {
			GMT_Report(API, GMT_MSG_ERROR,
			           "Selected variables in %s do not share x, y, and z dimensions\n",
			           cube->path);
			goto cleanup;
		}
		for (axis = 0; axis < 3; axis++) {
			item->axis_position[axis] = -1;
			for (position = 0; position < 3; position++)
				if (item->dimids[position] == cube->axis_dimid[axis])
					item->axis_position[axis] = position;
			if (item->axis_position[axis] < 0) goto cleanup;
		}
		item->name = strdup(parts.names[field]);
		if (item->name == NULL) goto cleanup;
	}

	for (axis = 0; axis < 3; axis++) {
		char name[NC_MAX_NAME + 1];
		int ndims, dimid;
		nc_type type;
		if (nc_inq_dim(cube->ncid, cube->axis_dimid[axis], name,
		               &cube->n[axis]) != NC_NOERR ||
		    nc_inq_vartype(cube->ncid, cube->coordinate_varid[axis], &type) != NC_NOERR ||
		    !topobath_numeric_type(type) ||
		    nc_inq_varndims(cube->ncid, cube->coordinate_varid[axis], &ndims) != NC_NOERR ||
		    ndims != 1 ||
		    nc_inq_vardimid(cube->ncid, cube->coordinate_varid[axis], &dimid) != NC_NOERR ||
		    dimid != cube->axis_dimid[axis])
			goto cleanup;
		cube->coordinate_name[axis] = strdup(name);
		cube->coordinate[axis] = calloc(cube->n[axis], sizeof(double));
		if (!cube->coordinate_name[axis] || !cube->coordinate[axis] ||
		    nc_get_var_double(cube->ncid, cube->coordinate_varid[axis],
		                      cube->coordinate[axis]) != NC_NOERR)
			goto cleanup;
	}
	for (axis = 0; axis < 3; axis++) {
		size_t k;
		double scale = 1.0, offset = 0.0;
		nc_get_att_double(cube->ncid, cube->coordinate_varid[axis],
		                  "scale_factor", &scale);
		nc_get_att_double(cube->ncid, cube->coordinate_varid[axis],
		                  "add_offset", &offset);
		for (k = 0; k < cube->n[axis]; k++)
			cube->coordinate[axis][k] =
			    (cube->coordinate[axis][k] * scale + offset) *
			    cube->transform.axis_scale[axis];
		if (cube->n[axis] < 2) goto cleanup;
		{
			double *coordinate = cube->coordinate[axis];
			size_t n = cube->n[axis];
			bool increasing = coordinate[n - 1] > coordinate[0];
			for (k = 1; k < n; k++)
				if (!isfinite(coordinate[k]) ||
				    (increasing ? coordinate[k] <= coordinate[k - 1]
				                : coordinate[k] >= coordinate[k - 1])) {
				GMT_Report(API, GMT_MSG_ERROR,
				           "Scaled %c coordinates must be strictly monotonic\n",
				           "xyz"[axis]);
					goto cleanup;
				}
			if (!increasing) {
				if (axis == TOPOBATH_Z) {
					GMT_Report(API, GMT_MSG_ERROR,
					           "Transformed z coordinates must increase from top to "
					           "bottom; use +z<scale> to establish the required "
					           "negative-elevation, positive-depth convention\n");
					goto cleanup;
				}
				for (k = 0; k < n / 2; k++) {
					double swap = coordinate[k];
					coordinate[k] = coordinate[n - 1 - k];
					coordinate[n - 1 - k] = swap;
				}
				cube->reverse[axis] = true;
			}
		}
	}
	status = GMT_NOERROR;

cleanup:
	topobath_source_parts_free(&parts);
	if (status != GMT_NOERROR) {
		GMT_Report(API, GMT_MSG_ERROR,
		           "Unable to read cube metadata from %s\n", source);
		topobath_cube_free(cube);
	}
	return status;
}

static void topobath_normalize_vertical(struct GMTAPI_CTRL *API,
                                        struct TOPOBATH_JOB *job)
{
	struct TOPOBATH_CUBE *cube = &job->cube;
	size_t k, n = cube->n[TOPOBATH_Z];

	job->vertical_sign = -1.0;
	for (k = 0; k < n; k++)
		cube->coordinate[TOPOBATH_Z][k] *= job->vertical_sign;
	for (k = 0; k < n / 2; k++) {
		double swap = cube->coordinate[TOPOBATH_Z][k];
		cube->coordinate[TOPOBATH_Z][k] =
		    cube->coordinate[TOPOBATH_Z][n - 1 - k];
		cube->coordinate[TOPOBATH_Z][n - 1 - k] = swap;
	}
	cube->reverse[TOPOBATH_Z] = !cube->reverse[TOPOBATH_Z];
	GMT_Report(API, GMT_MSG_INFORMATION,
	           "Working z convention: increasing from negative elevation "
	           "to positive depth\n");
}

static size_t topobath_field_index(const struct TOPOBATH_CUBE *cube,
                                   size_t field, size_t ix, size_t iy, size_t iz)
{
	const struct TOPOBATH_FIELD *item = &cube->field[field];
	size_t index[3], length[3], stride = 1, offset = 0;
	int position;

	if (cube->canonical)
		return (iz * cube->n[TOPOBATH_Y] + iy) * cube->n[TOPOBATH_X] + ix;

	if (cube->reverse[TOPOBATH_X]) ix = cube->n[TOPOBATH_X] - 1 - ix;
	if (cube->reverse[TOPOBATH_Y]) iy = cube->n[TOPOBATH_Y] - 1 - iy;
	if (cube->reverse[TOPOBATH_Z]) iz = cube->n[TOPOBATH_Z] - 1 - iz;
	index[item->axis_position[TOPOBATH_X]] = ix;
	index[item->axis_position[TOPOBATH_Y]] = iy;
	index[item->axis_position[TOPOBATH_Z]] = iz;
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

static int topobath_missing(int ncid, int varid, double value,
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

static int topobath_read_field(struct GMTAPI_CTRL *API,
                               const struct TOPOBATH_CUBE *cube,
                               size_t field, double **values)
{
	const struct TOPOBATH_FIELD *item = &cube->field[field];
	size_t total = cube->n[0] * cube->n[1] * cube->n[2], k;
	double scale = 1.0, offset = 0.0;
	double user_scale = gq_transform_value_scale(&cube->transform, field);

	*values = calloc(total, sizeof(**values));
	if (*values == NULL) return GMT_MEMORY_ERROR;
	if (cube->cached_field) {
		memcpy(*values, cube->cached_field[field], total * sizeof(**values));
		return GMT_NOERROR;
	}
	if (nc_get_var_double(cube->ncid, item->varid, *values) != NC_NOERR) {
		free(*values);
		*values = NULL;
		return GMT_DATA_READ_ERROR;
	}
	nc_get_att_double(cube->ncid, item->varid, "scale_factor", &scale);
	nc_get_att_double(cube->ncid, item->varid, "add_offset", &offset);
	for (k = 0; k < total; k++) {
		double value = (*values)[k];
		if (topobath_missing(cube->ncid, item->varid, value,
		                    cube->has_sentinel, cube->sentinel) ||
		    (API->GMT->common.d.active[GMT_IN] &&
		     value == API->GMT->common.d.nan_proxy[GMT_IN]))
			(*values)[k] = NAN;
		else
			(*values)[k] = (value * scale + offset) * user_scale;
	}
	return GMT_NOERROR;
}

static char *topobath_grid_modifier(char *text)
{
	char *p;
	for (p = text; *p; p++) {
		if (*p != '+') continue;
		if (p > text && (p[-1] == 'e' || p[-1] == 'E')) continue;
		if (strchr("sxyXYZzvV", p[1])) return p;
	}
	return p;
}

static int topobath_grid_source(struct GMTAPI_CTRL *API, const char *source,
	                            bool allow_values, char **clean,
	                            struct GQ_TRANSFORM *transform)
{
	char *copy, *modifier, *p, message[GMT_LEN256];

	*clean = NULL;
	gq_transform_init(transform);
	copy = strdup(source);
	if (copy == NULL) return GMT_MEMORY_ERROR;
	modifier = topobath_grid_modifier(copy);
	if (*modifier) *modifier++ = '\0';
	for (p = modifier; p && *p; p = strchr(p, '+')) {
		if (*p == '+') p++;
		if (*p == 's') *p = 'v';
		else if (*p == 'z') *p = 'v';
		else if (*p == 'Z') *p = 'V';
	}
	if (modifier &&
	    gq_transform_parse(modifier,
	                       GQ_TRANSFORM_X_MASK | GQ_TRANSFORM_Y_MASK,
	                       false, transform, NULL, NULL,
	                       message, sizeof(message))) {
		GMT_Report(API, GMT_MSG_ERROR,
		           "Invalid ancillary-grid transform in %s: %s\n",
		           source, message);
		free(copy);
		gq_transform_free(transform);
		return GMT_PARSE_ERROR;
	}
	if (gq_transform_validate_values(transform, 1, message, sizeof(message)) ||
	    (!allow_values && (transform->values_set ||
	                       transform->value_units_set))) {
		GMT_Report(API, GMT_MSG_ERROR,
		           allow_values ? "Invalid ancillary-grid transform in %s: %s\n"
		                        : "Mask grids do not accept +s/+v/+V: %s\n",
		           source, message);
		free(copy);
		gq_transform_free(transform);
		return GMT_PARSE_ERROR;
	}
	*clean = copy;
	return GMT_NOERROR;
}

static int topobath_sample_grid(struct GMT_CTRL *GMT, const char *source,
                                const struct TOPOBATH_CUBE *cube,
                                bool allow_values, double *output)
{
	struct GMT_GRID *Grid = NULL;
	struct GQ_TRANSFORM transform;
	char *clean = NULL;
	double wesn[4], x0, x1, y0, y1;
	size_t row, col;
	int status;

	status = topobath_grid_source(GMT->parent, source, allow_values,
	                              &clean, &transform);
	if (status != GMT_NOERROR) return status;
	x0 = cube->coordinate[TOPOBATH_X][0] /
	     transform.axis_scale[GQ_TRANSFORM_X];
	x1 = cube->coordinate[TOPOBATH_X][cube->n[TOPOBATH_X] - 1] /
	     transform.axis_scale[GQ_TRANSFORM_X];
	y0 = cube->coordinate[TOPOBATH_Y][0] /
	     transform.axis_scale[GQ_TRANSFORM_Y];
	y1 = cube->coordinate[TOPOBATH_Y][cube->n[TOPOBATH_Y] - 1] /
	     transform.axis_scale[GQ_TRANSFORM_Y];
	wesn[XLO] = MIN(x0, x1);
	wesn[XHI] = MAX(x0, x1);
	wesn[YLO] = MIN(y0, y1);
	wesn[YHI] = MAX(y0, y1);
	Grid = GMT_Read_Data(GMT->parent, GMT_IS_GRID, GMT_IS_FILE,
	                     GMT_IS_SURFACE, GMT_CONTAINER_AND_DATA,
	                     wesn, clean, NULL);
	free(clean);
	if (Grid == NULL) {
		gq_transform_free(&transform);
		return GMT->parent->error;
	}
	if (gmt_grd_BC_set(GMT, Grid, GMT_IN) != GMT_NOERROR) {
		status = GMT_RUNTIME_ERROR;
		goto cleanup;
	}
	for (row = 0; row < cube->n[TOPOBATH_Y]; row++) {
		for (col = 0; col < cube->n[TOPOBATH_X]; col++) {
			double value = gmt_bcr_get_z(
			    GMT, Grid,
			    cube->coordinate[TOPOBATH_X][col] /
			        transform.axis_scale[GQ_TRANSFORM_X],
			    cube->coordinate[TOPOBATH_Y][row] /
			        transform.axis_scale[GQ_TRANSFORM_Y]);
			output[row * cube->n[TOPOBATH_X] + col] =
			    isfinite(value)
			        ? value * gq_transform_value_scale(&transform, 0) : NAN;
		}
	}
	status = GMT_NOERROR;
cleanup:
	if (GMT_Destroy_Data(GMT->parent, &Grid) != GMT_NOERROR &&
	    status == GMT_NOERROR)
		status = GMT_RUNTIME_ERROR;
	gq_transform_free(&transform);
	return status;
}

static bool topobath_regular_coordinate(const double *coordinate, size_t n,
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

static bool topobath_same_coordinate(const double *first,
                                     const double *second, size_t n)
{
	size_t k;

	for (k = 0; k < n; k++) {
		double tolerance = 256.0 * DBL_EPSILON *
		                   MAX(1.0, MAX(fabs(first[k]), fabs(second[k])));
		if (fabs(first[k] - second[k]) > tolerance) return false;
	}
	return true;
}

static bool topobath_is_geographic(const struct TOPOBATH_CUBE *cube)
{
	char *x_units = cube->transform.axis_unit[TOPOBATH_X]
	              ? strdup(cube->transform.axis_unit[TOPOBATH_X])
	              : topobath_text_attribute(
	                    cube->ncid, cube->coordinate_varid[TOPOBATH_X], "units");
	char *y_units = cube->transform.axis_unit[TOPOBATH_Y]
	              ? strdup(cube->transform.axis_unit[TOPOBATH_Y])
	              : topobath_text_attribute(
	                    cube->ncid, cube->coordinate_varid[TOPOBATH_Y], "units");
	bool geographic =
	    topobath_axis_name(cube->coordinate_name[TOPOBATH_X]) == TOPOBATH_X &&
	    topobath_axis_name(cube->coordinate_name[TOPOBATH_Y]) == TOPOBATH_Y &&
	    ((!strcasecmp(cube->coordinate_name[TOPOBATH_X], "lon") ||
	      !strcasecmp(cube->coordinate_name[TOPOBATH_X], "longitude")) &&
	     (!strcasecmp(cube->coordinate_name[TOPOBATH_Y], "lat") ||
	      !strcasecmp(cube->coordinate_name[TOPOBATH_Y], "latitude")));
	if (x_units && y_units &&
	    strstr(x_units, "degree") && strstr(y_units, "degree"))
		geographic = true;
	free(x_units);
	free(y_units);
	return geographic;
}

static int topobath_gshhg_prior(struct GMT_CTRL *GMT,
                                const struct TOPOBATH_CTRL *Ctrl,
                                const struct TOPOBATH_CUBE *cube,
                                signed char *prior)
{
	struct GMT_GRID *Grid = NULL;
	char virtual_file[GMT_VF_LEN] = {0}, command[GMT_LEN512];
	double dx, dy;
	size_t row, col;
	int status;

	if (Ctrl->D.resolution == 'n' || !topobath_is_geographic(cube) ||
	    !topobath_regular_coordinate(cube->coordinate[TOPOBATH_X],
	                                 cube->n[TOPOBATH_X], &dx) ||
	    !topobath_regular_coordinate(cube->coordinate[TOPOBATH_Y],
	                                 cube->n[TOPOBATH_Y], &dy))
		return GMT_NOTSET;
	if (GMT_Open_VirtualFile(GMT->parent, GMT_IS_GRID, GMT_IS_SURFACE,
	                         GMT_OUT, NULL, virtual_file))
		return GMT_NOTSET;
	snprintf(command, sizeof(command),
	         "-G%s -R%.16g/%.16g/%.16g/%.16g -I%.16g/%.16g -D%c "
	         "-A%s -N0/1/2/3/4 --GMT_HISTORY=false",
	         virtual_file,
	         cube->coordinate[TOPOBATH_X][0],
	         cube->coordinate[TOPOBATH_X][cube->n[TOPOBATH_X] - 1],
	         cube->coordinate[TOPOBATH_Y][0],
	         cube->coordinate[TOPOBATH_Y][cube->n[TOPOBATH_Y] - 1],
	         dx, dy, Ctrl->D.resolution, Ctrl->A.selection);
	status = GMT_Call_Module(GMT->parent, "grdlandmask", GMT_MODULE_CMD, command);
	if (status != GMT_NOERROR) {
		GMT_Report(GMT->parent, GMT_MSG_WARNING,
		           "GMT shoreline classification was unavailable; "
		           "using model-only inference\n");
		GMT_Close_VirtualFile(GMT->parent, virtual_file);
		return GMT_NOTSET;
	}
	Grid = GMT_Read_VirtualFile(GMT->parent, virtual_file);
	if (Grid == NULL) {
		GMT_Close_VirtualFile(GMT->parent, virtual_file);
		return GMT_NOTSET;
	}
	if (gmt_grd_BC_set(GMT, Grid, GMT_IN) != GMT_NOERROR) {
		GMT_Close_VirtualFile(GMT->parent, virtual_file);
		return GMT_NOTSET;
	}
	for (row = 0; row < cube->n[TOPOBATH_Y]; row++) {
		for (col = 0; col < cube->n[TOPOBATH_X]; col++) {
			double value = gmt_bcr_get_z(
			    GMT, Grid, cube->coordinate[TOPOBATH_X][col],
			    cube->coordinate[TOPOBATH_Y][row]);
			size_t k = row * cube->n[TOPOBATH_X] + col;
			if (!isfinite(value))
				prior[k] = TOPOBATH_UNRESOLVED;
			else if ((int)lrint(value) == 0 || (int)lrint(value) == 2 ||
			         (int)lrint(value) == 4)
				prior[k] = TOPOBATH_OCEAN;
			else if ((int)lrint(value) == 1 || (int)lrint(value) == 3)
				prior[k] = TOPOBATH_LAND;
			else
				prior[k] = TOPOBATH_UNRESOLVED;
		}
	}
	GMT_Close_VirtualFile(GMT->parent, virtual_file);
	GMT_Report(GMT->parent, GMT_MSG_INFORMATION,
	           "Used GMT shoreline data as a land/ocean inference prior\n");
	return GMT_NOERROR;
}

static int topobath_prepare_prior(struct GMT_CTRL *GMT,
                                  const struct TOPOBATH_CTRL *Ctrl,
                                  const struct TOPOBATH_CUBE *cube,
                                  signed char *prior)
{
	size_t k, plane = cube->n[TOPOBATH_X] * cube->n[TOPOBATH_Y];

	for (k = 0; k < plane; k++) prior[k] = TOPOBATH_UNRESOLVED;
	if (Ctrl->C.mode == TOPOBATH_CLASS_ALL_LAND ||
	    Ctrl->C.mode == TOPOBATH_CLASS_ALL_WET) {
		signed char value = Ctrl->C.mode == TOPOBATH_CLASS_ALL_LAND
		                  ? TOPOBATH_LAND : TOPOBATH_OCEAN;
		for (k = 0; k < plane; k++) prior[k] = value;
		return GMT_NOERROR;
	}
	if (Ctrl->K.active) {
		double *mask = calloc(plane, sizeof(*mask));
		int status;
		if (mask == NULL) return GMT_MEMORY_ERROR;
		status = topobath_sample_grid(GMT, Ctrl->K.file, cube, false, mask);
		if (status != GMT_NOERROR) {
			free(mask);
			return status;
		}
		for (k = 0; k < plane; k++) {
			if (!isfinite(mask[k]))
				prior[k] = TOPOBATH_UNRESOLVED;
			else
				prior[k] = mask[k] >= 0.5 ? TOPOBATH_LAND : TOPOBATH_OCEAN;
		}
		free(mask);
		return GMT_NOERROR;
	}
	topobath_gshhg_prior(GMT, Ctrl, cube, prior);
	return GMT_NOERROR;
}

static bool topobath_matches_water(double value, double water, double tolerance)
{
	if (isnan(water)) return !isfinite(value);
	return isfinite(value) && fabs(value - water) <= tolerance;
}

static int topobath_parameter_index(const struct TOPOBATH_CUBE *cube,
                                    const char *name)
{
	size_t field;
	for (field = 0; field < cube->n_fields; field++)
		if (!strcmp(cube->field[field].name, name)) return (int)field;
	return -1;
}

static int topobath_map_parameters(struct GMTAPI_CTRL *API,
                                   const struct TOPOBATH_CTRL *Ctrl,
                                   struct TOPOBATH_JOB *job)
{
	size_t field, k, n = job->cube.n_fields;

	job->have_water = calloc(n, sizeof(*job->have_water));
	job->water = calloc(n, sizeof(*job->water));
	job->water_tolerance = calloc(n, sizeof(*job->water_tolerance));
	job->have_air = calloc(n, sizeof(*job->have_air));
	job->air = malloc(n * sizeof(*job->air));
	job->have_minimum = calloc(n, sizeof(*job->have_minimum));
	job->minimum = calloc(n, sizeof(*job->minimum));
	if (!job->have_water || !job->water || !job->water_tolerance ||
	    !job->have_air || !job->air || !job->have_minimum || !job->minimum)
		return GMT_MEMORY_ERROR;
	for (field = 0; field < n; field++) job->air[field] = NAN;
	for (k = 0; k < Ctrl->W.n; k++) {
		int index = topobath_parameter_index(&job->cube, Ctrl->W.item[k].name);
		if (index < 0) {
			GMT_Report(API, GMT_MSG_ERROR,
			           "Option -W names unselected field %s\n",
			           Ctrl->W.item[k].name);
			return GMT_PARSE_ERROR;
		}
		job->have_water[index] = true;
		job->water[index] = Ctrl->W.item[k].value;
		job->water_tolerance[index] = Ctrl->W.item[k].tolerance;
	}
	for (k = 0; k < Ctrl->F.n; k++) {
		int index = topobath_parameter_index(&job->cube, Ctrl->F.item[k].name);
		if (index < 0) {
			GMT_Report(API, GMT_MSG_ERROR,
			           "Option -F names unselected field %s\n",
			           Ctrl->F.item[k].name);
			return GMT_PARSE_ERROR;
		}
		job->have_air[index] = true;
		job->air[index] = Ctrl->F.item[k].value;
	}
	for (k = 0; k < Ctrl->L.n; k++) {
		int index = topobath_parameter_index(&job->cube, Ctrl->L.item[k].name);
		if (index < 0) {
			GMT_Report(API, GMT_MSG_ERROR,
			           "Option -L names unselected field %s\n",
			           Ctrl->L.item[k].name);
			return GMT_PARSE_ERROR;
		}
		job->have_minimum[index] = true;
		job->minimum[index] = Ctrl->L.item[k].value;
	}
	for (field = 0; field < n; field++)
		GMT_Report(API, GMT_MSG_DEBUG, "Selected field %s\n",
		           job->cube.field[field].name);
	return GMT_NOERROR;
}

static void topobath_update_candidate(double candidate, double *value,
                                      bool *conflict)
{
	double tolerance;
	if (!isfinite(candidate)) return;
	if (!isfinite(*value)) {
		*value = candidate;
		return;
	}
	tolerance = 64.0 * DBL_EPSILON *
	            MAX(1.0, MAX(fabs(*value), fabs(candidate)));
	if (fabs(*value - candidate) > tolerance) *conflict = true;
	if (candidate > *value) *value = candidate;
}

static int topobath_infer_surface(struct GMT_CTRL *GMT,
                                  const struct TOPOBATH_CTRL *Ctrl,
                                  struct TOPOBATH_JOB *job,
                                  bool infer_surface)
{
	const struct TOPOBATH_CUBE *cube = &job->cube;
	size_t plane = job->plane, nz = cube->n[TOPOBATH_Z];
	double *land = NULL, *water = NULL, *values = NULL, *trace = NULL;
	bool *land_conflict = NULL, *water_conflict = NULL;
	signed char *prior = NULL, *model = NULL;
	size_t field, col, iz, unresolved = 0, land_differences = 0;
	size_t water_differences = 0, class_conflicts = 0;
	int status = GMT_MEMORY_ERROR;

	land = malloc(plane * sizeof(*land));
	water = malloc(plane * sizeof(*water));
	land_conflict = calloc(plane, sizeof(*land_conflict));
	water_conflict = calloc(plane, sizeof(*water_conflict));
	prior = calloc(plane, sizeof(*prior));
	model = calloc(plane, sizeof(*model));
	trace = calloc(nz, sizeof(*trace));
	if (!land || !water || !land_conflict || !water_conflict ||
	    !prior || !model || !trace)
		goto cleanup;
	for (col = 0; col < plane; col++) land[col] = water[col] = NAN;
	if ((status = topobath_prepare_prior(GMT, Ctrl, cube, prior)) != GMT_NOERROR)
		goto cleanup;

	for (field = 0; field < cube->n_fields; field++) {
		if ((status = topobath_read_field(GMT->parent, cube, field, &values))
		    != GMT_NOERROR)
			goto cleanup;
		for (col = 0; col < plane; col++) {
			size_t ix = col % cube->n[TOPOBATH_X];
			size_t iy = col / cube->n[TOPOBATH_X];
			double candidate = NAN;
			for (iz = 0; iz < nz; iz++)
				trace[iz] = values[topobath_field_index(
				    cube, field, ix, iy, iz)];
			for (iz = nz; iz-- > 0;)
				if (isfinite(trace[iz])) {
					candidate = cube->coordinate[TOPOBATH_Z][iz];
					break;
				}
			topobath_update_candidate(candidate, &land[col],
			                          &land_conflict[col]);

			if (job->have_water[field]) {
				size_t top = nz, cursor;
				while (top > 0 &&
				       cube->coordinate[TOPOBATH_Z][top - 1] > 0.0)
					top--;
				if (top == 0) continue;
				cursor = top - 1;
				if (!topobath_matches_water(
				        trace[cursor], job->water[field],
				        job->water_tolerance[field]))
					continue;
				while (cursor > 0 &&
				       topobath_matches_water(
				           trace[cursor], job->water[field],
				           job->water_tolerance[field]))
					cursor--;
				if (topobath_matches_water(
				        trace[cursor], job->water[field],
				        job->water_tolerance[field]) ||
				    !isfinite(trace[cursor]))
					water_conflict[col] = true;
				else
					topobath_update_candidate(
					    cube->coordinate[TOPOBATH_Z][cursor],
					    &water[col], &water_conflict[col]);
			}
		}
		free(values);
		values = NULL;
	}

	for (col = 0; col < plane; col++) {
		bool authoritative = Ctrl->K.active ||
		    Ctrl->C.mode == TOPOBATH_CLASS_ALL_LAND ||
		    Ctrl->C.mode == TOPOBATH_CLASS_ALL_WET;
		model[col] = isfinite(water[col]) ? TOPOBATH_OCEAN
		           : isfinite(land[col]) ? TOPOBATH_LAND
		           : TOPOBATH_UNRESOLVED;
		if (prior[col] != TOPOBATH_UNRESOLVED &&
		    model[col] != TOPOBATH_UNRESOLVED && prior[col] != model[col])
			class_conflicts++;
		if (authoritative)
			job->classification[col] = prior[col];
		else if (Ctrl->C.mode == TOPOBATH_CLASS_MODEL &&
		         model[col] != TOPOBATH_UNRESOLVED)
			job->classification[col] = model[col];
		else if (prior[col] != TOPOBATH_UNRESOLVED)
			job->classification[col] = prior[col];
		else
			job->classification[col] = model[col];
		if (infer_surface) {
			if (job->classification[col] == TOPOBATH_OCEAN &&
			    isfinite(water[col]))
				job->old_surface[col] = water[col];
			else if (isfinite(land[col]))
				job->old_surface[col] = land[col];
		}
		if (land_conflict[col]) land_differences++;
		if (water_conflict[col]) water_differences++;
		if (job->classification[col] == TOPOBATH_UNRESOLVED ||
		    !isfinite(job->old_surface[col]))
			unresolved++;
	}
	job->inferred = infer_surface;
	if (land_differences)
		GMT_Report(GMT->parent, GMT_MSG_WARNING,
		           "Selected fields identify different shallowest finite levels "
		           "in %zu columns; using the shallowest level. Supply -E when "
		           "the true old surface is known\n", land_differences);
	if (water_differences)
		GMT_Report(GMT->parent, GMT_MSG_WARNING,
		           "Water signatures identify different seafloor levels in %zu "
		           "columns; using the shallowest level\n", water_differences);
	if (class_conflicts)
		GMT_Report(GMT->parent, GMT_MSG_WARNING,
		           "Model evidence disagrees with the classification prior in %zu "
		           "columns; %s classification takes precedence\n",
		           class_conflicts,
		           Ctrl->C.mode == TOPOBATH_CLASS_MODEL && !Ctrl->K.active
		               ? "model" : "prior");
	if (unresolved == plane) {
		GMT_Report(GMT->parent, GMT_MSG_ERROR,
		           "Old-surface inference could not resolve any model columns\n");
		status = GMT_DATA_READ_ERROR;
	}
	else {
		if (unresolved)
			GMT_Report(GMT->parent, GMT_MSG_WARNING,
			           "%s left %zu columns outside the model coverage unresolved\n",
			           infer_surface ? "Old-surface inference"
			                         : "The supplied old surface",
			           unresolved);
		GMT_Report(GMT->parent, GMT_MSG_INFORMATION,
		           "%s the old surface for %zu columns\n",
		           infer_surface ? "Inferred" : "Classified",
		           plane - unresolved);
		status = GMT_NOERROR;
	}

cleanup:
	free(values);
	free(trace);
	free(land);
	free(water);
	free(land_conflict);
	free(water_conflict);
	free(prior);
	free(model);
	return status;
}

static int topobath_write_grid(struct GMTAPI_CTRL *API,
                               const struct TOPOBATH_CTRL *Ctrl,
                               const char *path,
                               const struct TOPOBATH_CUBE *cube,
                               const double *values, const signed char *classes,
                               bool classification, double vertical_sign)
{
	int ncid = -1, xdim, ydim, xvar, yvar, zvar, dims[2];
	size_t plane = cube->n[TOPOBATH_X] * cube->n[TOPOBATH_Y], k;
	float *data = NULL, fill = NAN;
	double *x = NULL, *y = NULL, x_range[2], y_range[2];
	const char *x_units = Ctrl->Z.transform.axis_unit[TOPOBATH_X];
	const char *y_units = Ctrl->Z.transform.axis_unit[TOPOBATH_Y];
	const char *z_units = Ctrl->Z.transform.axis_unit[TOPOBATH_Z];
	char *owned_x_units = NULL, *owned_y_units = NULL, *owned_z_units = NULL;
	int node_offset = 0, status = GMT_RUNTIME_ERROR;

	data = calloc(plane, sizeof(*data));
	x = calloc(cube->n[TOPOBATH_X], sizeof(*x));
	y = calloc(cube->n[TOPOBATH_Y], sizeof(*y));
	if (data == NULL || x == NULL || y == NULL) {
		status = GMT_MEMORY_ERROR;
		goto cleanup;
	}
	for (k = 0; k < cube->n[TOPOBATH_X]; k++)
		x[k] = cube->coordinate[TOPOBATH_X][k] *
		       Ctrl->Z.transform.axis_scale[TOPOBATH_X];
	for (k = 0; k < cube->n[TOPOBATH_Y]; k++)
		y[k] = cube->coordinate[TOPOBATH_Y][k] *
		       Ctrl->Z.transform.axis_scale[TOPOBATH_Y];
	x_range[0] = MIN(x[0], x[cube->n[TOPOBATH_X] - 1]);
	x_range[1] = MAX(x[0], x[cube->n[TOPOBATH_X] - 1]);
	y_range[0] = MIN(y[0], y[cube->n[TOPOBATH_Y] - 1]);
	y_range[1] = MAX(y[0], y[cube->n[TOPOBATH_Y] - 1]);
	for (k = 0; k < plane; k++)
		data[k] = classification
		        ? (float)classes[k]
		        : (float)(vertical_sign * values[k] *
		                  Ctrl->Z.transform.axis_scale[TOPOBATH_Z]);
	if (!x_units && Ctrl->Z.transform.axis_scale[TOPOBATH_X] == 1.0) {
		x_units = cube->transform.axis_unit[TOPOBATH_X];
		if (!x_units && (!cube->transform.axis_set[TOPOBATH_X] ||
		                 cube->transform.axis_scale[TOPOBATH_X] == 1.0))
			x_units = owned_x_units = topobath_text_attribute(
			    cube->ncid, cube->coordinate_varid[TOPOBATH_X], "units");
	}
	if (!y_units && Ctrl->Z.transform.axis_scale[TOPOBATH_Y] == 1.0) {
		y_units = cube->transform.axis_unit[TOPOBATH_Y];
		if (!y_units && (!cube->transform.axis_set[TOPOBATH_Y] ||
		                 cube->transform.axis_scale[TOPOBATH_Y] == 1.0))
			y_units = owned_y_units = topobath_text_attribute(
			    cube->ncid, cube->coordinate_varid[TOPOBATH_Y], "units");
	}
	if (!z_units && Ctrl->Z.transform.axis_scale[TOPOBATH_Z] == 1.0) {
		z_units = cube->transform.axis_unit[TOPOBATH_Z];
		if (!z_units && (!cube->transform.axis_set[TOPOBATH_Z] ||
		                 cube->transform.axis_scale[TOPOBATH_Z] == 1.0))
			z_units = owned_z_units = topobath_text_attribute(
			    cube->ncid, cube->coordinate_varid[TOPOBATH_Z], "units");
	}
	if (nc_create(path, NC_CLOBBER | NC_NETCDF4, &ncid) != NC_NOERR ||
	    nc_def_dim(ncid, cube->coordinate_name[TOPOBATH_X],
	               cube->n[TOPOBATH_X], &xdim) != NC_NOERR ||
	    nc_def_dim(ncid, cube->coordinate_name[TOPOBATH_Y],
	               cube->n[TOPOBATH_Y], &ydim) != NC_NOERR ||
	    nc_def_var(ncid, cube->coordinate_name[TOPOBATH_X], NC_DOUBLE,
	               1, &xdim, &xvar) != NC_NOERR ||
	    nc_def_var(ncid, cube->coordinate_name[TOPOBATH_Y], NC_DOUBLE,
	               1, &ydim, &yvar) != NC_NOERR ||
	    nc_put_att_text(ncid, xvar, "axis", 1, "X") != NC_NOERR ||
	    nc_put_att_text(ncid, yvar, "axis", 1, "Y") != NC_NOERR ||
	    (x_units && nc_put_att_text(ncid, xvar, "units",
	                                strlen(x_units), x_units) != NC_NOERR) ||
	    (y_units && nc_put_att_text(ncid, yvar, "units",
	                                strlen(y_units), y_units) != NC_NOERR) ||
	    nc_put_att_double(ncid, xvar, "actual_range", NC_DOUBLE, 2,
	                      x_range) != NC_NOERR ||
	    nc_put_att_double(ncid, yvar, "actual_range", NC_DOUBLE, 2,
	                      y_range) != NC_NOERR ||
	    nc_put_att_int(ncid, NC_GLOBAL, "node_offset", NC_INT, 1,
	                   &node_offset) != NC_NOERR ||
	    nc_put_att_text(ncid, NC_GLOBAL, "Conventions", 6,
	                    "CF-1.8") != NC_NOERR)
		goto cleanup;
	dims[0] = ydim;
	dims[1] = xdim;
	if (nc_def_var(ncid, classification ? "classification" : "elevation",
	               NC_FLOAT, 2, dims, &zvar) != NC_NOERR ||
	    nc_put_att_float(ncid, zvar, "_FillValue", NC_FLOAT, 1, &fill) != NC_NOERR)
		goto cleanup;
	if (classification)
		nc_put_att_text(ncid, zvar, "long_name",
		                strlen("wet/dry classification"),
		                "wet/dry classification");
	else
		nc_put_att_text(ncid, zvar, "long_name",
		                strlen("existing elevation surface"),
		                "existing elevation surface");
	if (!classification && z_units)
		nc_put_att_text(ncid, zvar, "units", strlen(z_units), z_units);
	if (nc_enddef(ncid) != NC_NOERR ||
	    nc_put_var_double(ncid, xvar, x) != NC_NOERR ||
	    nc_put_var_double(ncid, yvar, y) != NC_NOERR ||
	    nc_put_var_float(ncid, zvar, data) != NC_NOERR ||
	    nc_close(ncid) != NC_NOERR)
		goto cleanup_closed;
	ncid = -1;
	status = GMT_NOERROR;
	goto cleanup;
cleanup_closed:
	ncid = -1;
cleanup:
	if (ncid >= 0) nc_close(ncid);
	if (status != GMT_NOERROR)
		GMT_Report(API, GMT_MSG_ERROR,
		           "Unable to write diagnostic grid %s\n", path);
	free(data);
	free(x);
	free(y);
	free(owned_x_units);
	free(owned_y_units);
	free(owned_z_units);
	return status;
}

static int topobath_grid_size(struct GMTAPI_CTRL *API, const char *axis,
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

static int topobath_horizontal_bcr(struct GMT_CTRL *GMT,
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

	if (!topobath_regular_coordinate(source_x, source_nx, &inc[0]) ||
	    !topobath_regular_coordinate(source_y, source_ny, &inc[1])) {
		GMT_Report(GMT->parent, GMT_MSG_ERROR,
		           "Options -R and -I require regular model x and y coordinates\n");
		return GMT_RUNTIME_ERROR;
	}
	wesn[XLO] = source_x[0];
	wesn[XHI] = source_x[source_nx - 1];
	wesn[YLO] = source_y[0];
	wesn[YHI] = source_y[source_ny - 1];
	Grid = GMT_Create_Data(GMT->parent, GMT_IS_GRID, GMT_IS_SURFACE,
	                       GMT_CONTAINER_AND_DATA, NULL, wesn, inc,
	                       GMT_GRID_NODE_REG, GMT_NOTSET, NULL);
	if (Grid == NULL) return GMT_MEMORY_ERROR;
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
	if (Grid && GMT_Destroy_Data(GMT->parent, &Grid) != GMT_NOERROR)
		status = GMT_RUNTIME_ERROR;
	return status;
}

static const char *topobath_gap_method_name(char method)
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

static int topobath_write_xyz(struct GMT_CTRL *GMT, struct GMT_GRID *Grid,
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

static int topobath_fill_horizontal_layer(struct GMT_CTRL *GMT,
                                          const struct TOPOBATH_CTRL *Ctrl,
                                          const struct TOPOBATH_CUBE *cube,
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
	size_t nx = cube->n[TOPOBATH_X], ny = cube->n[TOPOBATH_Y];
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
			    isfinite(values[topobath_field_index(cube, field, ix, iy, iz)]))
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
						    isfinite(values[topobath_field_index(
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
	wesn[XLO] = cube->coordinate[TOPOBATH_X][0];
	wesn[XHI] = cube->coordinate[TOPOBATH_X][nx - 1];
	wesn[YLO] = cube->coordinate[TOPOBATH_Y][0];
	wesn[YHI] = cube->coordinate[TOPOBATH_Y][ny - 1];
	if (!topobath_regular_coordinate(cube->coordinate[TOPOBATH_X], nx, &inc[0]) ||
	    !topobath_regular_coordinate(cube->coordinate[TOPOBATH_Y], ny, &inc[1])) {
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
			    values[topobath_field_index(cube, field, ix, iy, iz)];
	}
	if (gmt_get_tempname(GMT->parent, "topobath_gap_input", ".nc", input) ||
	    gmt_get_tempname(GMT->parent, "topobath_gap_candidate", ".nc", candidate) ||
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
		const char *geographic = topobath_is_geographic(cube) ? "-fg" : "";
		double radius = Ctrl->H.argument * MAX(inc[0], inc[1]);
		unsigned int minimum_sectors = MAX(1U, (Ctrl->H.sectors + 1U) / 2U);
		if (gmt_get_tempname(GMT->parent, "topobath_gap_points", ".txt", xyz) ||
		    topobath_write_xyz(GMT, Grid, xyz) != GMT_NOERROR) {
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
			values[topobath_field_index(cube, field, ix, iy, iz)] = value;
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

static int topobath_fill_horizontal_gaps(struct GMT_CTRL *GMT,
                                         const struct TOPOBATH_CTRL *Ctrl,
                                         struct TOPOBATH_CUBE *cube)
{
	double **cached = NULL;
	size_t field, iz;
	int status = GMT_MEMORY_ERROR;

	cached = calloc(cube->n_fields, sizeof(*cached));
	if (!cached) return GMT_MEMORY_ERROR;
	for (field = 0; field < cube->n_fields; field++) {
		size_t holes = 0, nodes = 0;
		status = topobath_read_field(GMT->parent, cube, field, &cached[field]);
		if (status != GMT_NOERROR) goto cleanup;
		for (iz = 0; iz < cube->n[TOPOBATH_Z]; iz++) {
			status = topobath_fill_horizontal_layer(
			    GMT, Ctrl, cube, field, iz, cached[field], &holes, &nodes);
			if (status != GMT_NOERROR) {
				GMT_Report(GMT->parent, GMT_MSG_ERROR,
				           "Unable to fill horizontal gaps in %s?%s at z=%.12g "
				           "using %s interpolation\n",
				           cube->path, cube->field[field].name,
				           cube->coordinate[TOPOBATH_Z][iz],
				           topobath_gap_method_name(Ctrl->H.method));
				goto cleanup;
			}
		}
		GMT_Report(GMT->parent, GMT_MSG_INFORMATION,
		           "Filled %zu nodes in %zu internal horizontal gap%s in %s?%s "
		           "using %s interpolation\n",
		           nodes, holes, holes == 1 ? "" : "s", cube->path,
		           cube->field[field].name,
		           topobath_gap_method_name(Ctrl->H.method));
	}
	cube->cached_field = cached;
	return GMT_NOERROR;

cleanup:
	for (field = 0; field < cube->n_fields; field++) free(cached[field]);
	free(cached);
	return status;
}

static int topobath_prepare_horizontal(struct GMT_CTRL *GMT,
                                       const struct TOPOBATH_CTRL *Ctrl,
                                       struct TOPOBATH_JOB *job)
{
	struct TOPOBATH_CUBE *cube = &job->cube;
	bool use_region = GMT->common.R.active[RSET];
	bool use_increment = Ctrl->I.active;
	double wesn[4], inc[2], source_inc[2], adjusted, tolerance_x, tolerance_y;
	double *x = NULL, *y = NULL, *native = NULL, *sampled = NULL;
	double **cached = NULL;
	size_t nx, ny, source_nx = cube->n[TOPOBATH_X];
	size_t source_ny = cube->n[TOPOBATH_Y], source_plane;
	size_t target_plane, field, iz, ix, iy, k;
	int status = GMT_MEMORY_ERROR;

	if (!use_region && !use_increment) return GMT_NOERROR;
	if (!topobath_regular_coordinate(cube->coordinate[TOPOBATH_X], source_nx,
	                                 &source_inc[0]) ||
	    !topobath_regular_coordinate(cube->coordinate[TOPOBATH_Y], source_ny,
	                                 &source_inc[1])) {
		GMT_Report(GMT->parent, GMT_MSG_ERROR,
		           "Options -R and -I require regular model x and y coordinates\n");
		return GMT_RUNTIME_ERROR;
	}
	wesn[XLO] = use_region ? GMT->common.R.wesn[XLO]
	                       : cube->coordinate[TOPOBATH_X][0];
	wesn[XHI] = use_region ? GMT->common.R.wesn[XHI]
	                       : cube->coordinate[TOPOBATH_X][source_nx - 1];
	wesn[YLO] = use_region ? GMT->common.R.wesn[YLO]
	                       : cube->coordinate[TOPOBATH_Y][0];
	wesn[YHI] = use_region ? GMT->common.R.wesn[YHI]
	                       : cube->coordinate[TOPOBATH_Y][source_ny - 1];
	inc[0] = use_increment ? Ctrl->I.inc[0] : source_inc[0];
	inc[1] = use_increment ? Ctrl->I.inc[1] : source_inc[1];
	tolerance_x = 64.0 * DBL_EPSILON *
	              MAX(1.0, MAX(fabs(cube->coordinate[TOPOBATH_X][0]),
	                           fabs(cube->coordinate[TOPOBATH_X][source_nx - 1])));
	if (wesn[XLO] < cube->coordinate[TOPOBATH_X][0] - tolerance_x ||
	    wesn[XHI] > cube->coordinate[TOPOBATH_X][source_nx - 1] + tolerance_x) {
		GMT_Report(GMT->parent, GMT_MSG_ERROR,
		           "Option -R x range must remain within %.12g/%.12g\n",
		           cube->coordinate[TOPOBATH_X][0],
		           cube->coordinate[TOPOBATH_X][source_nx - 1]);
		return GMT_RUNTIME_ERROR;
	}
	tolerance_y = 64.0 * DBL_EPSILON *
	              MAX(1.0, MAX(fabs(cube->coordinate[TOPOBATH_Y][0]),
	                           fabs(cube->coordinate[TOPOBATH_Y][source_ny - 1])));
	if (wesn[YLO] < cube->coordinate[TOPOBATH_Y][0] - tolerance_y ||
	    wesn[YHI] > cube->coordinate[TOPOBATH_Y][source_ny - 1] + tolerance_y) {
		GMT_Report(GMT->parent, GMT_MSG_ERROR,
		           "Option -R y range must remain within %.12g/%.12g\n",
		           cube->coordinate[TOPOBATH_Y][0],
		           cube->coordinate[TOPOBATH_Y][source_ny - 1]);
		return GMT_RUNTIME_ERROR;
	}
	if (topobath_grid_size(GMT->parent, "x", wesn[XLO], wesn[XHI], inc[0],
	                      &nx, &adjusted))
		return GMT_RUNTIME_ERROR;
	wesn[XHI] = adjusted;
	if (topobath_grid_size(GMT->parent, "y", wesn[YLO], wesn[YHI], inc[1],
	                      &ny, &adjusted))
		return GMT_RUNTIME_ERROR;
	wesn[YHI] = adjusted;
	if (wesn[XHI] > cube->coordinate[TOPOBATH_X][source_nx - 1] + tolerance_x ||
	    wesn[YHI] > cube->coordinate[TOPOBATH_Y][source_ny - 1] + tolerance_y) {
		GMT_Report(GMT->parent, GMT_MSG_ERROR,
		           "The adjusted -R/-I lattice extends beyond the model domain\n");
		return GMT_RUNTIME_ERROR;
	}
	x = calloc(nx, sizeof(*x));
	y = calloc(ny, sizeof(*y));
	if (!x || !y) goto cleanup;
	for (k = 0; k < nx; k++) x[k] = wesn[XLO] + (double)k * inc[0];
	for (k = 0; k < ny; k++) y[k] = wesn[YLO] + (double)k * inc[1];
	if (nx == source_nx && ny == source_ny &&
	    topobath_same_coordinate(x, cube->coordinate[TOPOBATH_X], nx) &&
	    topobath_same_coordinate(y, cube->coordinate[TOPOBATH_Y], ny)) {
		status = GMT_NOERROR;
		goto cleanup;
	}
	source_plane = source_nx * source_ny;
	target_plane = nx * ny;
	native = calloc(source_plane, sizeof(*native));
	sampled = calloc(target_plane, sizeof(*sampled));
	cached = calloc(cube->n_fields, sizeof(*cached));
	if (!native || !sampled || !cached) goto cleanup;
	for (field = 0; field < cube->n_fields; field++) {
		double *values = NULL;
		cached[field] = calloc(cube->n[TOPOBATH_Z] * target_plane,
		                       sizeof(*cached[field]));
		if (!cached[field]) goto cleanup;
		status = topobath_read_field(GMT->parent, cube, field, &values);
		if (status != GMT_NOERROR) goto cleanup;
		for (iz = 0; iz < cube->n[TOPOBATH_Z]; iz++) {
			for (iy = 0; iy < source_ny; iy++)
				for (ix = 0; ix < source_nx; ix++)
					native[iy * source_nx + ix] =
					    values[topobath_field_index(cube, field, ix, iy, iz)];
			status = topobath_horizontal_bcr(
			    GMT, cube->coordinate[TOPOBATH_X], source_nx,
			    cube->coordinate[TOPOBATH_Y], source_ny, native,
			    x, nx, y, ny, sampled);
			if (status != GMT_NOERROR) {
				free(values);
				goto cleanup;
			}
			memcpy(&cached[field][iz * target_plane], sampled,
			       target_plane * sizeof(*sampled));
		}
		free(values);
	}
	free(cube->coordinate[TOPOBATH_X]);
	free(cube->coordinate[TOPOBATH_Y]);
	cube->coordinate[TOPOBATH_X] = x;
	cube->coordinate[TOPOBATH_Y] = y;
	x = y = NULL;
	cube->n[TOPOBATH_X] = nx;
	cube->n[TOPOBATH_Y] = ny;
	if (cube->cached_field) {
		for (field = 0; field < cube->n_fields; field++)
			free(cube->cached_field[field]);
		free(cube->cached_field);
	}
	cube->cached_field = cached;
	cached = NULL;
	cube->canonical = true;
	cube->horizontal_resampled = true;
	cube->reverse[TOPOBATH_X] = cube->reverse[TOPOBATH_Y] = false;
	cube->reverse[TOPOBATH_Z] = false;
	job->plane = target_plane;
	GMT_Report(GMT->parent, GMT_MSG_INFORMATION,
	           "Resampled model horizontally to %zu by %zu nodes\n", nx, ny);
	status = GMT_NOERROR;

cleanup:
	if (cached)
		for (field = 0; field < cube->n_fields; field++) free(cached[field]);
	free(cached);
	free(x);
	free(y);
	free(native);
	free(sampled);
	return status;
}

static int topobath_prepare_old_surface(struct GMT_CTRL *GMT,
                                        const struct TOPOBATH_CTRL *Ctrl,
                                        struct TOPOBATH_JOB *job)
{
	size_t k;
	int status;

	job->old_surface = calloc(job->plane, sizeof(*job->old_surface));
	job->classification = calloc(job->plane, sizeof(*job->classification));
	if (!job->old_surface || !job->classification)
		return GMT_MEMORY_ERROR;
	for (k = 0; k < job->plane; k++) job->old_surface[k] = NAN;
	if (Ctrl->E.active) {
		status = topobath_sample_grid(GMT, Ctrl->E.file, &job->cube, true,
		                             job->old_surface);
		if (status != GMT_NOERROR) return status;
		for (k = 0; k < job->plane; k++) {
			if (isfinite(job->old_surface[k]))
				job->old_surface[k] *= job->vertical_sign;
		}
	}
	status = topobath_infer_surface(GMT, Ctrl, job, !Ctrl->E.active);
	if (status != GMT_NOERROR) return status;
	return GMT_NOERROR;
}

static int topobath_finish_surfaces(struct GMT_CTRL *GMT,
                                    const struct TOPOBATH_CTRL *Ctrl,
                                    struct TOPOBATH_JOB *job)
{
	double *requested = NULL;
	size_t k, nonzero_add = 0, wet_clamped = 0;
	int status;

	if (Ctrl->Q.active) {
		status = topobath_write_grid(GMT->parent, Ctrl, Ctrl->Q.surface, &job->cube,
		                             job->old_surface, job->classification, false,
		                             job->vertical_sign);
		if (status != GMT_NOERROR) return status;
		if (Ctrl->Q.classification)
			if ((status = topobath_write_grid(
			         GMT->parent, Ctrl, Ctrl->Q.classification, &job->cube,
			         job->old_surface, job->classification, true,
			         job->vertical_sign)) != GMT_NOERROR)
				return status;
	}
	job->new_surface = calloc(job->plane, sizeof(*job->new_surface));
	job->modify = calloc(job->plane, sizeof(*job->modify));
	if (!job->new_surface || !job->modify) return GMT_MEMORY_ERROR;
	if (Ctrl->O.mode != TOPOBATH_REMOVE) {
		requested = calloc(job->plane, sizeof(*requested));
		if (!requested) return GMT_MEMORY_ERROR;
		status = topobath_sample_grid(GMT, Ctrl->In.file[1], &job->cube, true,
		                             requested);
		if (status != GMT_NOERROR) {
			free(requested);
			return status;
		}
		for (k = 0; k < job->plane; k++)
			requested[k] *= job->vertical_sign;
	}
	for (k = 0; k < job->plane; k++) {
		bool land = job->classification[k] == TOPOBATH_LAND;
		bool wet = job->classification[k] == TOPOBATH_OCEAN;
		bool selected = Ctrl->O.scope == TOPOBATH_BOTH ||
		                (Ctrl->O.scope == TOPOBATH_TOPOGRAPHY && land) ||
		                (Ctrl->O.scope == TOPOBATH_BATHYMETRY && wet);
		job->modify[k] = selected && isfinite(job->old_surface[k]) && (land || wet);
		job->new_surface[k] = job->old_surface[k];
		if (!job->modify[k]) continue;
		if (Ctrl->O.mode == TOPOBATH_REMOVE)
			job->new_surface[k] = 0.0;
		else {
			if (!isfinite(requested[k])) {
				GMT_Report(GMT->parent, GMT_MSG_ERROR,
				           "New surface is undefined at selected model column %zu\n", k);
				free(requested);
				return GMT_DATA_READ_ERROR;
			}
			job->new_surface[k] = requested[k];
			if (wet && job->new_surface[k] > 0.0) {
				job->new_surface[k] = 0.0;
				wet_clamped++;
			}
			if (Ctrl->O.mode == TOPOBATH_ADD &&
			    fabs(job->old_surface[k]) > 64.0 * DBL_EPSILON)
				nonzero_add++;
		}
	}
	free(requested);
	if (nonzero_add)
		GMT_Report(GMT->parent, GMT_MSG_WARNING,
		           "Add operation found %zu selected columns whose old surface is "
		           "not sea level; mapping those columns directly to the new surface. "
		           "Use -Ox when replacing an existing surface intentionally\n",
		           nonzero_add);
	if (wet_clamped)
		GMT_Report(GMT->parent, GMT_MSG_WARNING,
		           "Clamped the requested surface to sea level in %zu wet columns "
		           "where relief and classification coastlines disagree\n",
		           wet_clamped);
	return GMT_NOERROR;
}

static int topobath_validate_field_parameters(struct GMTAPI_CTRL *API,
                                               const struct TOPOBATH_CTRL *Ctrl,
                                               const struct TOPOBATH_JOB *job)
{
	bool water_needed = false, minimum_needed = false;
	size_t k, field;

	for (k = 0; k < job->plane; k++) {
		bool wet = job->classification[k] == TOPOBATH_OCEAN;
		bool land = job->classification[k] == TOPOBATH_LAND;
		if (wet && (job->inferred ||
		            (job->modify[k] && Ctrl->O.mode != TOPOBATH_REMOVE)))
			water_needed = true;
		if (land && job->modify[k] && Ctrl->O.mode != TOPOBATH_REMOVE &&
		    Ctrl->M.mode == TOPOBATH_LINEAR && job->new_surface[k] > 0.0)
			minimum_needed = true;
	}
	for (field = 0; field < job->cube.n_fields; field++) {
		if (water_needed && !job->have_water[field]) {
			GMT_Report(API, GMT_MSG_ERROR,
			           "Field %s needs a -W water value\n",
			           job->cube.field[field].name);
			return GMT_PARSE_ERROR;
		}
		if (minimum_needed && !job->have_minimum[field]) {
			GMT_Report(API, GMT_MSG_ERROR,
			           "Field %s needs a -L linear minimum\n",
			           job->cube.field[field].name);
			return GMT_PARSE_ERROR;
		}
	}
	return GMT_NOERROR;
}

static int topobath_prepare_axis(struct GMTAPI_CTRL *API,
                                 const struct TOPOBATH_CTRL *Ctrl,
                                 struct TOPOBATH_JOB *job)
{
	const double *input = job->cube.coordinate[TOPOBATH_Z];
	size_t input_n = job->cube.n[TOPOBATH_Z], k;
	double min = DBL_MAX, max = -DBL_MAX, dz = DBL_MAX;

	if (Ctrl->T.active) {
		job->nz = Ctrl->T.n;
		job->dz = Ctrl->T.inc;
		min = job->vertical_sign * Ctrl->T.max;
	}
	else {
		for (k = 1; k < input_n; k++)
			dz = MIN(dz, input[k] - input[k - 1]);
		for (k = 0; k < job->plane; k++) {
			double old = job->old_surface[k];
			double new = job->new_surface[k], low, high;
			bool wet = job->classification[k] == TOPOBATH_OCEAN;
			bool pull;
			if (!isfinite(old)) continue;
			if (!job->modify[k]) {
				low = input[0];
				high = input[input_n - 1];
			}
			else {
				pull = Ctrl->O.mode == TOPOBATH_REMOVE || wet || new <= 0.0 ||
				       Ctrl->M.mode == TOPOBATH_PULL;
				low = pull ? input[0] + new - old : input[0] - old;
				high = MAX(0.0, new);
			}
			min = MIN(min, low);
			max = MAX(max, high);
		}
		if (!isfinite(min) || !isfinite(max) || min >= max) {
			GMT_Report(API, GMT_MSG_ERROR,
			           "Unable to derive an output vertical axis\n");
			return GMT_RUNTIME_ERROR;
		}
		min = floor(min / dz) * dz;
		max = ceil(max / dz) * dz;
		job->dz = dz;
		job->nz = (size_t)floor((max - min) / dz + 0.5) + 1;
	}
	job->z = calloc(job->nz, sizeof(*job->z));
	if (job->z == NULL) return GMT_MEMORY_ERROR;
	for (k = 0; k < job->nz; k++) job->z[k] = min + (double)k * job->dz;
	GMT_Report(API, GMT_MSG_INFORMATION,
	           "Output vertical axis, zmin/zmax/dz: %.12g/%.12g/%.12g "
	           "(%zu levels)\n",
	           job->vertical_sign * job->z[job->nz - 1],
	           job->vertical_sign * job->z[0], job->dz, job->nz);
	return GMT_NOERROR;
}

static int topobath_interpolate_run(struct GMT_CTRL *GMT,
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

static int topobath_interpolate_finite(struct GMT_CTRL *GMT,
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
			status = topobath_interpolate_run(
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
				status = topobath_interpolate_run(
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
			return topobath_interpolate_run(
			    GMT, bridge_x, bridge_value, run_count,
			    target, n_target, output, fit, mode);
	}
	return GMT_NOERROR;
}

static bool topobath_above_surface(double coordinate, double surface)
{
	double tolerance = 1024.0 * DBL_EPSILON *
	                   MAX(1.0, MAX(fabs(coordinate), fabs(surface)));
	return coordinate > surface + tolerance;
}

static int topobath_transform_field(struct GMT_CTRL *GMT,
                                    const struct TOPOBATH_CTRL *Ctrl,
                                    const struct TOPOBATH_JOB *job,
                                    size_t field, const double *input,
                                    float *output)
{
	const struct TOPOBATH_CUBE *cube = &job->cube;
	size_t input_nz = cube->n[TOPOBATH_Z], output_nz = job->nz;
	double *trace = NULL, *target = NULL, *sampled = NULL;
	double *bridge_x = NULL, *bridge_value = NULL;
	size_t *map = NULL;
	size_t col, iz, bottom_loss = 0;
	int status = GMT_MEMORY_ERROR;

	trace = calloc(input_nz, sizeof(*trace));
	target = calloc(output_nz, sizeof(*target));
	sampled = calloc(output_nz, sizeof(*sampled));
	map = calloc(output_nz, sizeof(*map));
	if (Ctrl->S.bridge) {
		bridge_x = calloc(input_nz, sizeof(*bridge_x));
		bridge_value = calloc(input_nz, sizeof(*bridge_value));
	}
	if (!trace || !target || !sampled || !map ||
	    (Ctrl->S.bridge && (!bridge_x || !bridge_value)))
		goto cleanup;

	for (col = 0; col < job->plane; col++) {
		size_t ix = col % cube->n[TOPOBATH_X];
		size_t iy = col / cube->n[TOPOBATH_X];
		double old = job->old_surface[col];
		double new = job->new_surface[col];
		double surface_value = NAN;
		bool wet = job->classification[col] == TOPOBATH_OCEAN;
		bool land = job->classification[col] == TOPOBATH_LAND;
		bool modified = job->modify[col];
		bool pull = modified &&
		            (Ctrl->O.mode == TOPOBATH_REMOVE || wet || new <= 0.0 ||
		             Ctrl->M.mode == TOPOBATH_PULL);
		bool lost_bottom = false;
		size_t count = 0;
		if (!isfinite(old) || (!land && !wet)) {
			for (iz = 0; iz < output_nz; iz++)
				output[iz * job->plane + col] = NAN;
			continue;
		}

		for (iz = 0; iz < input_nz; iz++)
			trace[iz] = input[topobath_field_index(cube, field, ix, iy, iz)];
		if (modified) {
			for (iz = 0; iz < input_nz; iz++)
				if (topobath_above_surface(
				        cube->coordinate[TOPOBATH_Z][iz], old))
					trace[iz] = NAN;
		}
		if (modified && land && !pull &&
		    (Ctrl->M.mode == TOPOBATH_EXTEND ||
		     Ctrl->M.mode == TOPOBATH_LINEAR)) {
			topobath_interpolate_finite(
			    GMT, cube->coordinate[TOPOBATH_Z], trace, input_nz,
			    &old, 1, &surface_value,
			    Ctrl->S.fit, Ctrl->S.mode, Ctrl->S.bridge,
			    Ctrl->S.max_gap, bridge_x, bridge_value);
		}
		for (iz = 0; iz < output_nz; iz++) {
			double z = job->z[iz];
			float value = NAN;
			bool sample = false;
			double source = NAN;

			if (!modified) {
				source = z;
				sample = true;
			}
			else if (wet) {
				if (z > 0.0)
					value = (float)job->air[field];
				else if (Ctrl->O.mode != TOPOBATH_REMOVE && z > new)
					value = (float)job->water[field];
				else if (z <= new) {
					source = z - new + old;
					sample = true;
				}
			}
			else if (pull) {
				if (z > new)
					value = (float)job->air[field];
				else {
					source = z - new + old;
					sample = true;
				}
			}
			else if (z > new)
				value = (float)job->air[field];
			else if (z <= 0.0) {
				source = z + old;
				sample = true;
			}
			else if (Ctrl->M.mode == TOPOBATH_EXTEND ||
			         !isfinite(surface_value) ||
			         surface_value <= job->minimum[field])
				value = (float)surface_value;
			else
				value = (float)(surface_value +
				        (job->minimum[field] - surface_value) * z / new);
			output[iz * job->plane + col] = value;
			if (sample) {
				if (source < cube->coordinate[TOPOBATH_Z][0])
					lost_bottom = true;
				target[count] = source;
				map[count++] = iz;
			}
		}
		if (count) {
			status = topobath_interpolate_finite(
			    GMT, cube->coordinate[TOPOBATH_Z], trace, input_nz,
			    target, count, sampled, Ctrl->S.fit, Ctrl->S.mode,
			    Ctrl->S.bridge, Ctrl->S.max_gap,
			    bridge_x, bridge_value);
			if (status != GMT_NOERROR) goto cleanup;
			for (iz = 0; iz < count; iz++)
				output[map[iz] * job->plane + col] =
				    (float)sampled[iz];
		}
		if (lost_bottom) bottom_loss++;
	}
	if (bottom_loss)
		GMT_Report(GMT->parent, GMT_MSG_WARNING,
		           "Field %s has %zu columns whose transformed base extends "
		           "below the source model; those output values are NaN\n",
		           cube->field[field].name, bottom_loss);
	status = GMT_NOERROR;
cleanup:
	free(trace);
	free(target);
	free(sampled);
	free(bridge_x);
	free(bridge_value);
	free(map);
	return status;
}

static bool topobath_skip_attribute(const char *name)
{
	return !strcmp(name, "_FillValue") || !strcmp(name, "missing_value") ||
	       !strcmp(name, "scale_factor") || !strcmp(name, "add_offset");
}

static int topobath_copy_attributes(int input, int input_var,
                                    int output, int output_var,
                                    bool unpacked)
{
	int natts, k;

	if (nc_inq_varnatts(input, input_var, &natts) != NC_NOERR)
		return NC_EINVAL;
	for (k = 0; k < natts; k++) {
		char name[NC_MAX_NAME + 1];
		if (nc_inq_attname(input, input_var, k, name) != NC_NOERR)
			return NC_EINVAL;
		if (unpacked && topobath_skip_attribute(name)) continue;
		if (nc_copy_att(input, input_var, name, output, output_var) != NC_NOERR)
			return NC_EINVAL;
	}
	return NC_NOERR;
}

static int topobath_replace_units(int ncid, int varid, const char *units,
                                  bool invalidate)
{
	if (units) {
		nc_del_att(ncid, varid, "units");
		return nc_put_att_text(ncid, varid, "units", strlen(units), units);
	}
	if (invalidate) nc_del_att(ncid, varid, "units");
	return NC_NOERR;
}

static const char *topobath_output_axis_units(
    const struct TOPOBATH_CTRL *Ctrl, const struct TOPOBATH_CUBE *cube,
    size_t axis)
{
	if (Ctrl->Z.transform.axis_unit[axis])
		return Ctrl->Z.transform.axis_unit[axis];
	if (Ctrl->Z.transform.axis_scale[axis] != 1.0) return NULL;
	return cube->transform.axis_unit[axis];
}

static const char *topobath_output_field_units(
    const struct TOPOBATH_CTRL *Ctrl, const struct TOPOBATH_CUBE *cube,
    size_t field)
{
	const char *units = gq_transform_value_unit(&Ctrl->Z.transform, field);
	if (units) return units;
	if (gq_transform_value_scale(&Ctrl->Z.transform, field) != 1.0)
		return NULL;
	return gq_transform_value_unit(&cube->transform, field);
}

static bool topobath_selected_varid(const struct TOPOBATH_CUBE *cube, int varid)
{
	size_t field;
	for (field = 0; field < cube->n_fields; field++)
		if (cube->field[field].varid == varid) return true;
	return false;
}

static int topobath_define_ancillary(const struct TOPOBATH_CUBE *cube,
                                     const int output_dimid[3],
                                     struct TOPOBATH_OUTPUT *output)
{
	int nvars, varid;

	if (nc_inq_nvars(cube->ncid, &nvars) != NC_NOERR) return NC_EINVAL;
	for (varid = 0; varid < nvars; varid++) {
		char name[NC_MAX_NAME + 1];
		int ndims, input_dims[NC_MAX_VAR_DIMS], output_dims[NC_MAX_VAR_DIMS];
		nc_type type;
		size_t count = 1;
		size_t axis_length[2] = {1, 1};
		bool axis_reverse[2] = {false, false};
		bool compatible = true;
		int position, output_varid;

		if (varid == cube->coordinate_varid[TOPOBATH_X] ||
		    varid == cube->coordinate_varid[TOPOBATH_Y] ||
		    varid == cube->coordinate_varid[TOPOBATH_Z] ||
		    topobath_selected_varid(cube, varid))
			continue;
		if (nc_inq_var(cube->ncid, varid, name, &type, &ndims,
		               input_dims, NULL) != NC_NOERR ||
		    ndims > 2 || type == NC_STRING)
			continue;
		if (cube->horizontal_resampled && ndims > 0) continue;
		for (position = 0; position < ndims; position++) {
			size_t length;
			int axis;
			if (input_dims[position] == cube->axis_dimid[TOPOBATH_X]) {
				output_dims[position] = output_dimid[TOPOBATH_X];
				axis = TOPOBATH_X;
			}
			else if (input_dims[position] == cube->axis_dimid[TOPOBATH_Y]) {
				output_dims[position] = output_dimid[TOPOBATH_Y];
				axis = TOPOBATH_Y;
			}
			else {
				compatible = false;
				break;
			}
			if (nc_inq_dimlen(cube->ncid, input_dims[position], &length)
			    != NC_NOERR) {
				compatible = false;
				break;
			}
			count *= length;
			axis_length[position] = length;
			axis_reverse[position] = cube->reverse[axis];
		}
		if (!compatible) continue;
		if (nc_def_var(output->ncid, name, type, ndims, output_dims,
		               &output_varid) != NC_NOERR ||
		    topobath_copy_attributes(cube->ncid, varid, output->ncid,
		                             output_varid, false) != NC_NOERR)
			return NC_EINVAL;
		{
			void *next = realloc(
			    output->ancillary,
			    (output->n_ancillary + 1) * sizeof(*output->ancillary));
			if (next == NULL) return NC_ENOMEM;
			output->ancillary = next;
			output->ancillary[output->n_ancillary].input_varid = varid;
			output->ancillary[output->n_ancillary].output_varid = output_varid;
			output->ancillary[output->n_ancillary].type = type;
			output->ancillary[output->n_ancillary].count = count;
			output->ancillary[output->n_ancillary].ndims = ndims;
			for (position = 0; position < ndims; position++) {
				output->ancillary[output->n_ancillary].length[position] =
				    axis_length[position];
				output->ancillary[output->n_ancillary].reverse[position] =
				    axis_reverse[position];
			}
			output->n_ancillary++;
		}
	}
	return NC_NOERR;
}

static int topobath_copy_ancillary(const struct TOPOBATH_CUBE *cube,
                                   struct TOPOBATH_OUTPUT *output)
{
	size_t k;

	for (k = 0; k < output->n_ancillary; k++) {
		size_t type_size, target;
		void *data, *ordered = NULL;
		bool reordered = false;
		int position;
		if (nc_inq_type(cube->ncid, output->ancillary[k].type,
		                NULL, &type_size) != NC_NOERR)
			return NC_EINVAL;
		data = malloc(output->ancillary[k].count * type_size);
		if (data == NULL) return NC_ENOMEM;
		for (position = 0; position < output->ancillary[k].ndims; position++)
			if (output->ancillary[k].reverse[position]) reordered = true;
		if (nc_get_var(cube->ncid, output->ancillary[k].input_varid,
		               data) != NC_NOERR) {
			free(data);
			return NC_EINVAL;
		}
		if (reordered) {
			ordered = malloc(output->ancillary[k].count * type_size);
			if (!ordered) {
				free(data);
				return NC_ENOMEM;
			}
			for (target = 0; target < output->ancillary[k].count; target++) {
				size_t remainder = target, source = 0, stride = 1;
				for (position = output->ancillary[k].ndims - 1;
				     position >= 0; position--) {
					size_t index = remainder % output->ancillary[k].length[position];
					remainder /= output->ancillary[k].length[position];
					if (output->ancillary[k].reverse[position])
						index = output->ancillary[k].length[position] - 1 - index;
					source += index * stride;
					stride *= output->ancillary[k].length[position];
				}
				memcpy((char *)ordered + target * type_size,
				       (char *)data + source * type_size, type_size);
			}
		}
		if (nc_put_var(output->ncid, output->ancillary[k].output_varid,
		               reordered ? ordered : data) != NC_NOERR) {
			free(ordered);
			free(data);
			return NC_EINVAL;
		}
		free(ordered);
		free(data);
	}
	return NC_NOERR;
}

static int topobath_create_output(struct GMT_CTRL *GMT,
                                  const struct TOPOBATH_CTRL *Ctrl,
                                  const struct TOPOBATH_JOB *job,
                                  struct TOPOBATH_OUTPUT *output)
{
	const struct TOPOBATH_CUBE *cube = &job->cube;
	int dimid[3], coordinate_varid[3], dimensions[3], ngatts, k;
	int status = GMT_RUNTIME_ERROR;
	double *coordinate_output[GQ_TRANSFORM_N_AXES] = {NULL, NULL, NULL};
	float fill = NAN;
	size_t field, node;

	memset(output, 0, sizeof(*output));
	output->ncid = -1;
	output->field_varid = calloc(cube->n_fields, sizeof(*output->field_varid));
	coordinate_output[TOPOBATH_X] = calloc(cube->n[TOPOBATH_X], sizeof(double));
	coordinate_output[TOPOBATH_Y] = calloc(cube->n[TOPOBATH_Y], sizeof(double));
	coordinate_output[TOPOBATH_Z] = calloc(job->nz, sizeof(double));
	if (output->field_varid == NULL || !coordinate_output[TOPOBATH_X] ||
	    !coordinate_output[TOPOBATH_Y] || !coordinate_output[TOPOBATH_Z]) {
		status = GMT_MEMORY_ERROR;
		goto error;
	}
	for (node = 0; node < cube->n[TOPOBATH_X]; node++)
		coordinate_output[TOPOBATH_X][node] =
		    cube->coordinate[TOPOBATH_X][node] *
		    Ctrl->Z.transform.axis_scale[TOPOBATH_X];
	for (node = 0; node < cube->n[TOPOBATH_Y]; node++)
		coordinate_output[TOPOBATH_Y][node] =
		    cube->coordinate[TOPOBATH_Y][node] *
		    Ctrl->Z.transform.axis_scale[TOPOBATH_Y];
	for (node = 0; node < job->nz; node++)
		coordinate_output[TOPOBATH_Z][node] =
		    job->vertical_sign * job->z[job->nz - 1 - node] *
		    Ctrl->Z.transform.axis_scale[TOPOBATH_Z];
	for (field = 0; field < GQ_TRANSFORM_N_AXES; field++) {
		size_t length = field == TOPOBATH_X ? cube->n[TOPOBATH_X]
		              : field == TOPOBATH_Y ? cube->n[TOPOBATH_Y] : job->nz;
		for (node = 0; node < length; node++)
			if (coordinate_output[field][node] == 0.0)
				coordinate_output[field][node] = 0.0;
	}
	if (nc_create(Ctrl->G.file, NC_CLOBBER | NC_NETCDF4, &output->ncid) != NC_NOERR)
		goto error;
	for (k = 0; k < 3; k++) {
		size_t n = k == TOPOBATH_Z ? job->nz : cube->n[k];
		double actual_range[2] = {
			MIN(coordinate_output[k][0], coordinate_output[k][n - 1]),
			MAX(coordinate_output[k][0], coordinate_output[k][n - 1])
		};
		if (nc_def_dim(output->ncid, cube->coordinate_name[k], n,
		               &dimid[k]) != NC_NOERR ||
		    nc_def_var(output->ncid, cube->coordinate_name[k], NC_DOUBLE,
		               1, &dimid[k], &coordinate_varid[k]) != NC_NOERR)
			goto error;
		if (topobath_copy_attributes(cube->ncid, cube->coordinate_varid[k],
		                             output->ncid, coordinate_varid[k],
		                             true) != NC_NOERR)
			goto error;
		if (topobath_replace_units(
		        output->ncid, coordinate_varid[k],
		        topobath_output_axis_units(Ctrl, cube, (size_t)k),
		        (cube->transform.axis_set[k] &&
		         cube->transform.axis_scale[k] != 1.0) ||
		        Ctrl->Z.transform.axis_scale[k] != 1.0) != NC_NOERR)
			goto error;
		nc_del_att(output->ncid, coordinate_varid[k], "actual_range");
		if (nc_put_att_double(output->ncid, coordinate_varid[k], "actual_range",
		                      NC_DOUBLE, 2, actual_range) != NC_NOERR)
			goto error;
	}
	if (nc_inq_natts(cube->ncid, &ngatts) == NC_NOERR)
		for (k = 0; k < ngatts; k++) {
			char name[NC_MAX_NAME + 1];
			if (nc_inq_attname(cube->ncid, NC_GLOBAL, k, name) == NC_NOERR &&
			    strcmp(name, "_NCProperties") &&
			    strcmp(name, "_SuperblockVersion"))
				nc_copy_att(cube->ncid, NC_GLOBAL, name,
				            output->ncid, NC_GLOBAL);
		}
	nc_put_att_text(output->ncid, NC_GLOBAL, "Conventions", 6, "CF-1.8");
	nc_put_att_text(output->ncid, NC_GLOBAL, "source",
	                strlen("Created by GMT topobath"),
	                "Created by GMT topobath");
	nc_del_att(output->ncid, coordinate_varid[TOPOBATH_Z], "positive");
	nc_put_att_text(output->ncid, coordinate_varid[TOPOBATH_Z], "positive",
	                job->vertical_sign *
	                    Ctrl->Z.transform.axis_scale[TOPOBATH_Z] > 0.0 ? 2 : 4,
	                job->vertical_sign *
	                    Ctrl->Z.transform.axis_scale[TOPOBATH_Z] > 0.0
	                    ? "up" : "down");
	dimensions[0] = dimid[TOPOBATH_Z];
	dimensions[1] = dimid[TOPOBATH_Y];
	dimensions[2] = dimid[TOPOBATH_X];
	if (topobath_define_ancillary(cube, dimid, output) != NC_NOERR)
		goto error;
	for (field = 0; field < cube->n_fields; field++) {
		if (nc_def_var(output->ncid, cube->field[field].name, NC_FLOAT, 3,
		               dimensions, &output->field_varid[field]) != NC_NOERR ||
		    nc_put_att_float(output->ncid, output->field_varid[field],
		                     "_FillValue", NC_FLOAT, 1, &fill) != NC_NOERR ||
		    topobath_copy_attributes(cube->ncid, cube->field[field].varid,
		                             output->ncid,
		                             output->field_varid[field], true) != NC_NOERR)
			goto error;
		if (topobath_replace_units(
		        output->ncid, output->field_varid[field],
		        topobath_output_field_units(Ctrl, cube, field),
		        gq_transform_value_scale(&cube->transform, field) != 1.0 ||
		        gq_transform_value_scale(&Ctrl->Z.transform, field) != 1.0) != NC_NOERR)
			goto error;
		nc_def_var_deflate(output->ncid, output->field_varid[field], 1, 1, 2);
	}
	if (nc_enddef(output->ncid) != NC_NOERR ||
	    nc_put_var_double(output->ncid, coordinate_varid[TOPOBATH_X],
	                      coordinate_output[TOPOBATH_X]) != NC_NOERR ||
	    nc_put_var_double(output->ncid, coordinate_varid[TOPOBATH_Y],
	                      coordinate_output[TOPOBATH_Y]) != NC_NOERR ||
	    nc_put_var_double(output->ncid, coordinate_varid[TOPOBATH_Z],
	                      coordinate_output[TOPOBATH_Z]) != NC_NOERR ||
	    topobath_copy_ancillary(cube, output) != NC_NOERR)
		goto error;
	for (field = 0; field < GQ_TRANSFORM_N_AXES; field++)
		free(coordinate_output[field]);
	return GMT_NOERROR;
error:
	GMT_Report(GMT->parent, GMT_MSG_ERROR,
	           "NetCDF error while creating %s\n", Ctrl->G.file);
	if (output->ncid >= 0) nc_close(output->ncid);
	output->ncid = -1;
	free(output->field_varid);
	output->field_varid = NULL;
	free(output->ancillary);
	output->ancillary = NULL;
	output->n_ancillary = 0;
	for (field = 0; field < GQ_TRANSFORM_N_AXES; field++)
		free(coordinate_output[field]);
	return status;
}

static int topobath_write_fields(struct GMT_CTRL *GMT,
                                 const struct TOPOBATH_CTRL *Ctrl,
                                 const struct TOPOBATH_JOB *job,
                                 struct TOPOBATH_OUTPUT *output)
{
	size_t total_output = job->nz * job->plane, field;
	double *input = NULL;
	float *transformed = calloc(total_output, sizeof(*transformed));
	int status = GMT_MEMORY_ERROR;

	if (transformed == NULL) return GMT_MEMORY_ERROR;
	for (field = 0; field < job->cube.n_fields; field++) {
		status = topobath_read_field(GMT->parent, &job->cube, field, &input);
		if (status != GMT_NOERROR) goto cleanup;
		status = topobath_transform_field(GMT, Ctrl, job, field,
		                                  input, transformed);
		free(input);
		input = NULL;
		if (status != GMT_NOERROR) goto cleanup;
		{
			double scale = gq_transform_value_scale(&Ctrl->Z.transform, field);
			size_t node;
			if (scale != 1.0)
				for (node = 0; node < total_output; node++)
					if (isfinite(transformed[node])) transformed[node] *= (float)scale;
		}
		for (size_t layer = 0; layer < job->nz / 2; layer++) {
			size_t opposite = job->nz - 1 - layer, node;
			for (node = 0; node < job->plane; node++) {
				float swap = transformed[layer * job->plane + node];
				transformed[layer * job->plane + node] =
				    transformed[opposite * job->plane + node];
				transformed[opposite * job->plane + node] = swap;
			}
		}
		if (nc_put_var_float(output->ncid, output->field_varid[field],
		                     transformed) != NC_NOERR) {
			status = GMT_RUNTIME_ERROR;
			goto cleanup;
		}
		GMT_Report(GMT->parent, GMT_MSG_INFORMATION,
		           "Processed %s: %zu layers\n",
		           job->cube.field[field].name, job->nz);
	}
	if (nc_close(output->ncid) != NC_NOERR) {
		output->ncid = -1;
		status = GMT_RUNTIME_ERROR;
		goto cleanup;
	}
	output->ncid = -1;
	status = GMT_NOERROR;
cleanup:
	free(input);
	free(transformed);
	if (status != GMT_NOERROR)
		GMT_Report(GMT->parent, GMT_MSG_ERROR,
		           "Unable to write transformed fields to %s\n", Ctrl->G.file);
	return status;
}

static int topobath_run(struct GMT_CTRL *GMT, const struct TOPOBATH_CTRL *Ctrl)
{
	struct TOPOBATH_JOB job;
	struct TOPOBATH_OUTPUT output;
	int status;

	memset(&job, 0, sizeof(job));
	job.cube.ncid = -1;
	memset(&output, 0, sizeof(output));
	output.ncid = -1;
	status = topobath_open_cube(GMT->parent, Ctrl->In.file[0], &job.cube);
	if (status != GMT_NOERROR) goto cleanup;
	{
		char message[GMT_LEN256];
		if (gq_transform_validate_values(&Ctrl->Z.transform, job.cube.n_fields,
		                                 message, sizeof(message))) {
			GMT_Report(GMT->parent, GMT_MSG_ERROR, "Option -Z: %s\n", message);
			status = GMT_PARSE_ERROR;
			goto cleanup;
		}
	}
	topobath_normalize_vertical(GMT->parent, &job);
	job.plane = job.cube.n[TOPOBATH_X] * job.cube.n[TOPOBATH_Y];
	job.geographic = topobath_is_geographic(&job.cube);
	status = topobath_map_parameters(GMT->parent, Ctrl, &job);
	if (status != GMT_NOERROR) goto cleanup;
	if (Ctrl->H.active) {
		status = topobath_fill_horizontal_gaps(GMT, Ctrl, &job.cube);
		if (status != GMT_NOERROR) goto cleanup;
	}
	status = topobath_prepare_horizontal(GMT, Ctrl, &job);
	if (status != GMT_NOERROR) goto cleanup;
	status = topobath_prepare_old_surface(GMT, Ctrl, &job);
	if (status != GMT_NOERROR) goto cleanup;
	status = topobath_finish_surfaces(GMT, Ctrl, &job);
	if (status != GMT_NOERROR) goto cleanup;
	status = topobath_validate_field_parameters(GMT->parent, Ctrl, &job);
	if (status != GMT_NOERROR) goto cleanup;
	status = topobath_prepare_axis(GMT->parent, Ctrl, &job);
	if (status != GMT_NOERROR) goto cleanup;
	status = topobath_create_output(GMT, Ctrl, &job, &output);
	if (status != GMT_NOERROR) goto cleanup;
	status = topobath_write_fields(GMT, Ctrl, &job, &output);

cleanup:
	if (output.ncid >= 0) nc_close(output.ncid);
	free(output.field_varid);
	free(output.ancillary);
	topobath_job_free(&job);
	return status;
}

#define bailout(code) { gmt_M_free_options(mode); return (code); }
#define Return(code) { Free_Ctrl(GMT, Ctrl); gmt_end_module(GMT, GMT_cpy); bailout(code); }

EXTERN_MSC int GMT_topobath(void *V_API, int mode, void *args)
{
	struct GMTAPI_CTRL *API = gmt_get_api_ptr(V_API);
	struct GMT_CTRL *GMT = NULL, *GMT_cpy = NULL;
	struct GMT_OPTION *options = NULL;
	struct TOPOBATH_CTRL *Ctrl = NULL;
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
	gmt_grd_set_datapadding(GMT, true);
	status = topobath_run(GMT, Ctrl);
	Return(status);
}
