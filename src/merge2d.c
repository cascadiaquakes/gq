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
 * merge2d combines primary and secondary netCDF grids using 2-D BLEND
 * windows. Supports may be defined by strict xy-monotone polygons, with
 * optional envelope conversion, and may use independent beginning and ending
 * taper ratios in both dimensions. Multiparameter variables are mapped by
 * position and written with user-selected names. Overlapping primary supports
 * may be aggregated with normalized weights when they share a secondary grid.
 * Grid data are processed row by row.
 */ 

#include "gmt_dev.h"
#include "gq_remote.h"
#include "gq_transform.h"
#include "merge2d_inc.h"
#include <blend/blend.h>
#include <netcdf.h>

#define THIS_MODULE_CLASSIC_NAME "merge2d"
#define THIS_MODULE_MODERN_NAME "merge2d"
#define THIS_MODULE_LIB "gq"
#define THIS_MODULE_LIB_PURPOSE "The CRESCENT cyberinfrastructure team supplements to the Generic Mapping Tools"
#define THIS_MODULE_PURPOSE "Tile or smoothly merge two-dimensional multiparameter NetCDF grids"
#define THIS_MODULE_KEYS "<G{+,GG}"
#define THIS_MODULE_NEEDS "R"
#define THIS_MODULE_OPTIONS "-:RVdfnr"

enum MERGE2D_CLOBBER {
	MERGE2D_UPPER = 0,
	MERGE2D_LOWER,
	MERGE2D_FIRST,
	MERGE2D_LAST
};

struct MERGE2D_GAP {
	bool active;
	char method;
	double argument;
	unsigned int sectors;
	bool limited;
	unsigned int max_gap;
};

struct MERGE2D_CTRL {
	struct {	/* Input files */
		bool active;
		char **file;
		unsigned int n;
	} In;
	struct {	/* -G<grdfile> */
		bool active;
		char *file; 
	} G;
	struct {	/* -Cf|l|o|u[+n|p] */
		bool active;
		unsigned int mode;
		int sign;
	} C;
	struct {	/* -A */
		bool active;
	} A;
	struct {	/* -F<field1,field2,...> */
		bool active;
		char *fields;
	} F;
	struct MERGE2D_GAP H;	/* -Hn|l|a|s|m[<arg>][+m<maxgap>] */
	struct {	/* -I (for checking only) */
		bool active;
	} I;
	struct {	/* -ME|B[+w] */
		bool active;
		char method;
		bool write;
	} M;
	struct {	/* -P */
		bool active;
	} P;
	struct {	/* -Z with explicit transform modifiers */
		bool active;
		struct GQ_TRANSFORM transform;
	} Z;
	struct {	/* -W */
		bool active;
		bool only;
	} W;
	struct {	/* Internal single-field execution (-E) */
		bool active;
	} X;
};

static int merge2d_resolve_inputs(struct GMTAPI_CTRL *API,
                                  struct MERGE2D_CTRL *Ctrl)
{
	unsigned int k;
	for (k = 0; k < Ctrl->In.n; k++) {
		char *resolved = NULL;
		int status = gq_resolve_remote_source(
		    API, GMT_IS_GRID, Ctrl->In.file[k], NULL, &resolved);
		if (status != GMT_NOERROR) return status;
		free(Ctrl->In.file[k]);
		Ctrl->In.file[k] = resolved;
	}
	return GMT_NOERROR;
}

struct MERGE2D_PAIR {							/* Structure with info about each input [primary] grid file */
	struct GMT_GRID *G;							/* I/O structure for grid files, including grd header */
	struct GMT_GRID_ROWBYROW *RbR;				/* structure for row-by-row operations */
	int out_i0, out_i1;							/* Input x bounds in output-grid coordinates */
	int out_j0, out_j1;							/* Input y bounds in output-grid coordinates */
	int support_i0, support_i1, support_j0, support_j1; /* BLEND support bounds in output coordinates */
	off_t offset;								/* grid offset when the grid extends beyond north */
	bool ignore;								/* true if the grid is entirely outside desired region */
	bool outside;								/* true if the current output row is outside the range of this grid */
	bool open;									/* true if file is currently open */
	bool delete;								/* true if file was produced by grdsample to deal with different registration/increments */
	bool memory;								/* true if grid is a in memory array */
	char file[PATH_MAX];						/* Name of grid file */
	char source[PATH_MAX];					/* Original primary grid selector */
	double wesn[4];								/* Boundaries of inner region */
	gmt_grdfloat *z;							/* Row vector holding the current row from this file */
	/* BLEND support and window. */
	window *v_data; 							/* BLEND window for the primary-grid support */
	polygon v_support;							/* Real-coordinate support polygon */
	/* Associated secondary grid. */
	bool secondary; 							/* true if this primary grid is associated with a working secondary grid file but is not used if the primary grid fails any checks */
	struct GMT_GRID *s_G;						/* I/O structure for secondary grid files, including grd header */
	struct GMT_GRID_ROWBYROW *s_RbR;			/* structure for row-by-row operations secondary grid files */
	int s_out_i0, s_out_i1;						/* Secondary x bounds in output-grid coordinates */
	int s_out_j0, s_out_j1;						/* Secondary y bounds in output-grid coordinates */
	off_t s_offset;								/* grid offset when the grid extends beyond north for secondary grid */
	bool s_ignore;								/* true if the grid is entirely outside desired region for secondary grid */
	bool s_outside;								/* true if the current output row is outside the range of this grid for secondary grid */
	bool s_open;								/* true if file is currently open for secondary grid */
	bool s_delete;								/* true if file was produced by grdsample to deal with different registration/increments for secondary grid */
	bool s_memory;								/* true if grid is a in memory array for secondary grid */
	char s_file[PATH_MAX];						/* Name of grid file for secondary grid */
	char s_source[PATH_MAX];					/* Original secondary grid selector */
	double s_wesn[4];							/* Secondary-grid bounds */
	gmt_grdfloat *s_z;							/* Row vector holding the current row from this file for secondary grid */
};

struct MERGE2D_SPEC {
	char *file;
	char *secondary;
	char *polygon;
	char *functions;
	double wesn[4];
	double taper_ratio[4];
	bool have_secondary;
	bool have_polygon;
};

struct MERGE2D_FIELDS {
	char **names;
	size_t count;
};

static void merge2d_specs_free(struct GMT_CTRL *GMT,
                               struct MERGE2D_SPEC *specs,
                               unsigned int count)
{
	unsigned int k;
	if (specs == NULL) return;
	for (k = 0; k < count; k++) {
		gmt_M_str_free(specs[k].file);
		gmt_M_str_free(specs[k].secondary);
		gmt_M_str_free(specs[k].polygon);
		gmt_M_str_free(specs[k].functions);
	}
	gmt_M_free(GMT, specs);
}

static void *New_Ctrl(struct GMT_CTRL *GMT)
{
	struct MERGE2D_CTRL *C = gmt_M_memory(GMT, NULL, 1, struct MERGE2D_CTRL);

	gq_transform_init(&C->Z.transform);
	C->M.method = 'E';
	return C;
}

static void Free_Ctrl(struct GMT_CTRL *GMT, struct MERGE2D_CTRL *C)
{
	unsigned int k;

	if (C == NULL) return;
	for (k = 0; k < C->In.n; k++) gmt_M_str_free (C->In.file[k]);
	gmt_M_free (GMT, C->In.file);
	gmt_M_str_free (C->G.file);
	gmt_M_str_free (C->F.fields);
	gq_transform_free(&C->Z.transform);
	gmt_M_free (GMT, C);
}

static int usage (struct GMTAPI_CTRL *API, int level) {
	const char *name = gmt_show_name_and_purpose (API, THIS_MODULE_LIB, THIS_MODULE_CLASSIC_NAME, THIS_MODULE_PURPOSE);
	if (level == GMT_MODULE_PURPOSE) return (GMT_NOERROR);
	GMT_Usage (API, 0, "usage: %s [<mergefile> | <grid1> <grid2> ...] -G<output.nc> "
		"[%s] [%s] [-A] [-Cf|l|o|u[+n|p]] [-F<fields>] [-H[n|l|a|s|m[<arg>]][+m<maxgap>]] "
		"[-ME|B[+w]] [-P] [%s] [-W[+o]] "
		"[-Z+x<sx>[+X<xunit>]+y<sy>[+Y<yunit>]+v<scales>[+V<units>]] "
		"[%s] [%s] [%s] [%s] [%s]\n",
		name, GMT_Rgeo_OPT, GMT_I_OPT, GMT_V_OPT, GMT_di_OPT, GMT_f_OPT, GMT_n_OPT, GMT_r_OPT, GMT_PAR_OPT);

	if (level == GMT_SYNOPSIS) return (GMT_MODULE_SYNOPSIS);

	GMT_Message (API, GMT_TIME_NONE, "  REQUIRED ARGUMENTS:\n");
	GMT_Usage (API, 1, "\n<mergefile> | <grid1> <grid2> ...");
	GMT_Usage (API, -2, "Supply one mergefile or list at least two NetCDF grids directly. "
		"Omit the input argument to read a mergefile from standard input. Direct inputs "
		"tile values in availability order and use first-value clobbering by default.");
	GMT_Usage (API, 3, "Each non-comment mergefile record contains up to five whitespace-separated fields:");
	GMT_Usage (API, 3, "primary: Required NetCDF source providing the primary field or fields.");
	GMT_Usage (API, 3, "secondary: Optional NetCDF source paired with primary for merging. Use '-' for an "
		"unpaired fallback tile. Pairing is valid only within the primary support and does not extend "
		"the primary domain.");
	GMT_Usage (API, 3, "clipfile: Optional xy polygon defining the primary support. Use '-' or omit it to use "
		"the complete primary-grid domain.");
	GMT_Usage (API, 3, "xwindow/ywindow: BLEND window functions for the x and y dimensions. The default is "
		"cosine/cosine.");
	GMT_Usage (API, 3, "rx1/rx2/ry1/ry2: Dimensionless taper ratios in [0, 0.5). One value applies to all sides, two values "
		"apply symmetrically to x and y, and four set the beginning and ending ratios independently. "
		"The default is 0.2 on every side.");
	GMT_Usage (API, 3, "Each ratio sets the fraction of the corresponding support extent used by the transition "
		"at one boundary: rx1/rx2 apply at west/east (low/high x), and ry1/ry2 apply at south/north "
		"(low/high y). For a rectangular support 100 coordinate units wide, rx1 = 0.2 gives a west "
		"transition approximately 20 units wide.");
	GMT_Usage (API, 3, "Within each transition, the selected window controls how the primary merging-weight "
		"factor changes between its boundary value and 1. The paired secondary receives the complementary "
		"weight. The x and y factors are multiplied. Larger ratios give broader transitions and a smaller "
		"full-primary interior. 0 disables the taper on that side. Boxcar ignores taper ratios and has unit "
		"weight throughout the support.");
	GMT_Usage (API, 3, "For a polygon support, BLEND evaluates the x and y tapers along local cross-sections of "
		"the polygon and adapts the transition width where a cross-section is too narrow for the nominal "
		"support-wide taper.");
	GMT_Usage (API, 3, "Use '-' to skip an optional field when supplying a later field. Trailing optional "
		"fields may be omitted. Blank and comment records are ignored.");
	GMT_Usage (API, 3, "Example mergefile:");
	GMT_Usage (API, 3, "  primary.nc?vp secondary.nc?p support.txt cosine/cosine 0.25/0.25/0.25/0.25");
	GMT_Usage (API, 3, "  secondary.nc?p - - - -");
	GMT_Usage (API, 3, "Select NetCDF fields with file.nc?field1,field2,... . Selected fields must share "
		"the same horizontal coordinates. If ? is omitted, the default grid variable is used.");
	GMT_Usage (API, 3, "Select a layer from a 3-D variable with model.nc?vp[<index>] or "
		"model.nc?vp(<level>). Indices are zero-based. A coordinate value selects the nearest layer "
		"without vertical interpolation. Specify one layer selector for every selected field.");
	GMT_Usage (API, 3, "Append input modifiers after the field list. +x, +y, and +z scale source coordinates. "
		"+v supplies one broadcast field scale or one scale per selected field. +X, +Y, +Z, and +V set "
		"target-unit metadata. +n declares an additional missing-value sentinel. Coordinate-value "
		"layer selection occurs after +z scaling, while index selection remains positional.");
	GMT_Usage (API, 3, "Use file.nc?<field1,field2,...>+<modifiers> for named fields or "
		"file.nc?+<modifiers> to transform the default field without naming it. Input transforms are "
		"applied before gap filling, resampling, and merging.");
	GMT_Usage (API, 3, "Input coordinate scaling does not reorder grid data. Unchanged x and y axes must "
		"remain strictly increasing after scaling. Because GMT supplies them in increasing order, input "
		"+x and +y scales must be positive. Use output -Z to write a decreasing final axis if desired.");
	GMT_Usage (API, 3, "Example: model.nc?vp,vs+x0.001+Xkm+y0.001+Ykm"
		"+v0.001,0.001+Vkm/s,km/s selects vp and vs, scales x and y from m to km, and scales "
		"both fields from m/s to km/s.");
	GMT_Usage (API, 1, "\n-G<output.nc>");
	GMT_Usage (API, -2, "Write the final NetCDF grid. Multiparameter output stores all selected fields "
		"on the same coordinates.");
	GMT_Message (API, GMT_TIME_NONE, "\n  OPTIONAL ARGUMENTS:\n");
	GMT_Option (API, "R");
	GMT_Usage (API, -2, "Set the output region. If -R is omitted, use the union of the input-grid domains.");
	GMT_Option (API, "I");
	GMT_Usage (API, -2, "Set the output increments. If -I is omitted, use the common input increments. "
		"Specify -I when input increments differ and -r when input registrations differ. Inputs not "
		"co-registered with the output geometry are resampled using GMT -n.");
	GMT_Usage (API, 1, "\n-A");
	GMT_Usage (API, -2, "Normalize positive weights where primary supports overlap. Only primaries that "
		"overlap must share the same secondary. A later record for that secondary starts a lower-priority "
		"layer. Non-overlapping primaries may use different secondaries, and unpaired records remain "
		"fallback tiles.");
	GMT_Usage (API, 1, "\n-Cf|l|o|u[+n|p]");
	GMT_Usage (API, -2, "Select clobber/tiling mode instead of weighted merging:");
	GMT_Usage (API, 3, "f: Keep the first available value. This is the default for direct grid lists.");
	GMT_Usage (API, 3, "l: Keep the lowest available value.");
	GMT_Usage (API, 3, "o: Keep the last available value.");
	GMT_Usage (API, 3, "u: Keep the highest available value.");
	GMT_Usage (API, 3, "+n: Only consider non-positive values for clobbering.");
	GMT_Usage (API, 3, "+p: Only consider non-negative values for clobbering.");
	GMT_Usage (API, 1, "\n-F<field1,field2,...>");
	GMT_Usage (API, -2, "Set NetCDF output variable names. -F does not select source variables: fields "
		"selected with ? map positionally to -F. Thus model1.nc?vp,vs,den and model2.nc?p,s,d may be "
		"merged with -Fvp,vs,rho when the differently named fields are equivalent.");
	GMT_Usage (API, 1, "\n-H[n|l|a|s|m[<arg>]][+m<maxgap>]");
	GMT_Usage (API, -2, "Fill strictly internal missing-data holes in every input field before "
		"resampling and merging. Original non-missing nodes and boundary-connected gaps are preserved. "
		"Without -H, internal holes are not filled. Use linear Delaunay interpolation when -H has no directive.");
	GMT_Usage (API, 3, "Nearest neighbor (n). Optionally append a search radius in grid nodes.");
	GMT_Usage (API, 3, "Linear Delaunay interpolation (l). This is the default.");
	GMT_Usage (API, 3, "Local weighted average (a). Append <radius>[/<sectors>] in grid nodes. "
		"The default is 3/4.");
	GMT_Usage (API, 3, "Spline interpolation (s). Optionally append tension in the range 0-1. "
		"The default is 0.");
	GMT_Usage (API, 3, "Minimum-curvature interpolation (m). Optionally append tension in the range 0-1. "
		"The default is 0.");
	GMT_Usage (API, 3, "+m Only fill holes whose horizontal and vertical spans are both no larger "
		"than <maxgap> grid nodes. The default is to fill all internal holes.");
	GMT_Usage (API, 1, "\n-ME|B[+w]");
	GMT_Usage (API, -2, "Convert non-monotone clip polygons to strict xy-monotone supports using "
		"the envelope (E) or best-piecewise envelope (B) method. Append +w to write a converted polygon "
		"with a _monotone suffix. Without -M, supplied polygons must already be strictly xy-monotone.");
	GMT_Usage (API, 1, "\n-P");
	GMT_Usage (API, -2, "Fill primary values that remain missing after interpolation from "
		"their non-missing paired secondary values. By default, primary NaNs are preserved.");
	GMT_Option (API, "V");
	GMT_Usage (API, 1, "\n-W[+o]");
	GMT_Usage (API, -2, "Include the shared NetCDF variable weight with long_name='merging weight' and "
		"units='1'. Append +o to write only the coordinates and weight. All output fields use this same weight.");
	GMT_Usage(API, 3, "Weights follow paired supports in mergefile order. The first support "
		"containing a node supplies its primary weight; outside it, later supports remain visible. "
		"Zero-valued support boundaries are retained. With -A, sum and cap weights at 1 only where "
		"positive weights overlap with the same secondary. Unpaired background weights are 0. "
		"These shared taper weights do not represent final per-source fractions or field-specific "
		"missing-value replacements.");
	GMT_Usage (API, 1, "\n-Z[+x<sx>][+X<xunit>][+y<sy>][+Y<yunit>]"
		"[+v<scales>][+V<units>]");
	GMT_Usage (API, -2, "Transform output coordinates and fields after merging. +x and +y scale output "
		"coordinates. +X and +Y set coordinate units. +v supplies one broadcast scale or one scale per "
		"output field. +V sets field units. Field lists follow -F order when -F is used and selector "
		"order otherwise. -Z scales output "
		"coordinates and fields in place and does not reorder coordinates, fields, or weight. "
		"A negative axis scale therefore produces a decreasing output axis. Weight is not scaled.");
	GMT_Usage (API, 3, "Example: with -Fvp,vs, -Z+x0.001+Xkm+y0.001+Ykm"
		"+v0.001,0.001+Vkm/s,km/s converts x and y from m to km and converts "
		"vp and vs from m/s to km/s.");
	GMT_Option (API, "di");
	if (gmt_M_showusage (API)) GMT_Usage (API, -2, "Also sets value for nodes without constraints. The default is NaN.");
	GMT_Option (API, "f,n");
	if (gmt_M_showusage (API)) GMT_Usage (API, -2, "(-n is passed to grdsample if grids are not co-registered).");
	GMT_Option (API, "r,.");

	return (GMT_MODULE_USAGE);
}

static int merge2d_validate_grid_format(struct GMT_CTRL *GMT,
                                        struct GMT_GRID_HEADER *h,
                                        const char *file)
{
	char family = GMT->session.grdformat[h->type][0];
	if (family != 'n' && family != 'c') {
		GMT_Report(GMT->parent, GMT_MSG_ERROR,
		           "Only netCDF grids are supported; %s uses grid format %s\n",
		           file, GMT->session.grdformat[h->type]);
		return GMT_GRDIO_UNKNOWN_FORMAT;
	}
	return GMT_NOERROR;
}

static bool merge2d_out_of_phase (struct GMT_GRID_HEADER *g, struct GMT_GRID_HEADER *h) {
	/* Look for phase shifts in w/e/s/n between the two grids */
	unsigned int way, side;
	double a;
	for (side = 0; side < 4; side++) {
		way = side / 2;
		a = fmod (fabs (((g->wesn[side] + g->xy_off * g->inc[way]) - (h->wesn[side] + h->xy_off * h->inc[way])) / h->inc[way]), 1.0);
		if (a < GMT_CONV8_LIMIT) continue;
		a = 1.0 - a;
		if (a < GMT_CONV8_LIMIT) continue;
		return true;
	}
	return false;
}

static bool merge2d_overlap_check (struct GMT_CTRL *GMT, struct MERGE2D_PAIR *B, struct GMT_GRID_HEADER *h, unsigned int mode) {
	double w, e, shift = 720.0;
	char *type[2] = {"grid", "inner grid"};

	if (gmt_grd_is_global (GMT, h)) return false;	/* Not possible to be outside the final grids longitude range if global */
	/* check for the primary grid */
	if (gmt_grd_is_global (GMT, B->G->header)) return false;	/* Not possible to overlap with the final grid in longitude range if your are a global grid */
	/* Here the grids are not global so we must carefully check for overlap while being aware of periodicity in 360 degrees */
	w = ((mode) ? B->wesn[XLO] : B->G->header->wesn[XLO]) - shift;	e = ((mode) ? B->wesn[XHI] : B->G->header->wesn[XHI]) - shift;
	while (e < h->wesn[XLO]) { w += 360.0; e += 360.0; shift -= 360.0; }
	if ((h->registration == GMT_GRID_NODE_REG && w > h->wesn[XHI]) || (h->registration == GMT_GRID_PIXEL_REG && w >= h->wesn[XHI]))  {
		GMT_Report (GMT->parent, GMT_MSG_WARNING, "File %s entirely outside longitude range of final grid region (skipped)\n", B->file);
		B->ignore = true;
		return true;
	}
	if (! (gmt_M_is_zero (shift))) {	/* Must modify region */
		if (mode) {
			B->wesn[XLO] = w;	B->wesn[XHI] = e;
		}
		else {
			B->G->header->wesn[XLO] = w;	B->G->header->wesn[XHI] = e;
		}
		GMT_Report (GMT->parent, GMT_MSG_INFORMATION, "File %s %s region needed longitude adjustment to fit final grid region\n", B->file, type[mode]);
	}

	return false;
}

/* false is good here */
static bool merge2d_secondary_overlap_check (struct GMT_CTRL *GMT, struct MERGE2D_PAIR *B, struct GMT_GRID_HEADER *h, unsigned int mode) {
	double w, e, shift = 720.0;
	char *type[2] = {"grid", "inner grid"};

	if (B->secondary) {
		if (gmt_grd_is_global (GMT, B->s_G->header)) {
			B->secondary = true; /* Not possible to overlap with the final grid in longitude range if your are a global grid */
			return false;
		}
		/* Here the grids are not global so we must carefully check for overlap while being aware of periodicity in 360 degrees */
		w = ((mode) ? B->s_wesn[XLO] : B->s_G->header->wesn[XLO]) - shift;	e = ((mode) ? B->s_wesn[XHI] : B->s_G->header->wesn[XHI]) - shift;
		while (e < h->wesn[XLO]) { w += 360.0; e += 360.0; shift -= 360.0; }
		if ((h->registration == GMT_GRID_NODE_REG && w > h->wesn[XHI]) || (h->registration == GMT_GRID_PIXEL_REG && w >= h->wesn[XHI]))  {
			GMT_Report (GMT->parent, GMT_MSG_WARNING, "File %s entirely outside longitude range of final grid region (skipped)\n", B->s_file);
			B->s_ignore = true;
			B->secondary = false;
			return true;
		}
		if (! (gmt_M_is_zero (shift))) {	/* Must modify region */
			if (mode) {
				B->s_wesn[XLO] = w;	B->s_wesn[XHI] = e;
			}
			else {
				B->s_G->header->wesn[XLO] = w;	B->s_G->header->wesn[XHI] = e;
			}
			GMT_Report (GMT->parent, GMT_MSG_INFORMATION, "File %s %s region needed longitude adjustment to fit final grid region\n", B->s_file, type[mode]);
		}
	}

	return false;
}

static bool merge2d_outside_y_range (struct GMT_GRID_HEADER *h, double *wesn) {
	if (h->registration == GMT_GRID_NODE_REG) {
		if (h->wesn[YLO] > wesn[YHI] || h->wesn[YHI] < wesn[YLO]) return true;
	}
	else {	/* Pixel registration */
		if (h->wesn[YLO] >= wesn[YHI] || h->wesn[YHI] <= wesn[YLO]) return true;
	}
	return false;
}

static bool merge2d_outside_cartesian_x_range (struct GMT_GRID_HEADER *h, double *wesn) {
	if (h->registration == GMT_GRID_NODE_REG) {
		if (h->wesn[XLO] > wesn[XHI] || h->wesn[XHI] < wesn[XLO] || h->wesn[YLO] > wesn[YHI] || h->wesn[YHI] < wesn[YLO]) return true;
	}
	else{	/* Pixel registration */
		if (h->wesn[XLO] >= wesn[XHI] || h->wesn[XHI] <= wesn[XLO] || h->wesn[YLO] >= wesn[YHI] || h->wesn[YHI] <= wesn[YLO]) return true;
	}
	return false;
}

EXTERN_MSC void gmtlib_close_grd (struct GMT_CTRL *GMT, struct GMT_GRID *G);

static int merge2d_parse_taper_ratios (struct GMTAPI_CTRL *API, const char *text, double ratio[4]) {
	double values[4];
	char copy[GMT_LEN256] = {""};
	char *token = NULL, *save = NULL;
	unsigned int n = 0, k;

	if (text == NULL || !text[0] || strlen(text) >= sizeof(copy)) return GMT_PARSE_ERROR;
	strncpy(copy, text, sizeof(copy) - 1);
	for (token = strtok_r(copy, "/", &save); token && n < 4; token = strtok_r(NULL, "/", &save)) {
		char *end = NULL;
		errno = 0;
		values[n] = strtod(token, &end);
		if (errno || end == token || *end || !isfinite(values[n])) {
			GMT_Report(API, GMT_MSG_ERROR, "Invalid taper ratio: %s\n", text);
			return GMT_PARSE_ERROR;
		}
		n++;
	}
	if (token || (n != 1 && n != 2 && n != 4)) {
		GMT_Report(API, GMT_MSG_ERROR,
		           "Taper ratios must contain 1, 2, or 4 slash-separated values: %s\n", text);
		return GMT_PARSE_ERROR;
	}
	if (n == 1) {
		for (k = 0; k < 4; k++) ratio[k] = values[0];
	}
	else if (n == 2) {
		ratio[0] = ratio[1] = values[0];
		ratio[2] = ratio[3] = values[1];
	}
	else {
		for (k = 0; k < 4; k++) ratio[k] = values[k];
	}
	for (k = 0; k < 4; k++) {
		if (ratio[k] < 0.0 || ratio[k] >= 0.5) {
			GMT_Report(API, GMT_MSG_ERROR,
			           "Taper ratios must be >= 0 and < 0.5: %s\n", text);
			return GMT_PARSE_ERROR;
		}
	}
	return GMT_NOERROR;
}

static int merge2d_parse_gap_option(struct GMTAPI_CTRL *API, const char *text,
                                    struct MERGE2D_GAP *H) {
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

static int parse (struct GMT_CTRL *GMT, struct MERGE2D_CTRL *Ctrl, struct GMT_OPTION *options) {
	/* This parses the options provided to merge2d and sets parameters in CTRL.
	 * Any GMT common options will override values set previously by other commands.
	 * It also replaces any file names specified as input or output with the data ID
	 * returned when registering these sources/destinations with the API.
	 */

 	unsigned int n_errors = 0;
	size_t n_alloc = 0;
	struct GMT_OPTION *opt = NULL;
	struct GMTAPI_CTRL *API = GMT->parent;

	for (opt = options; opt; opt = opt->next) {
		switch (opt->option) {

			case '<':	/* Collect input files */
				Ctrl->In.active = true;
				if (n_alloc <= Ctrl->In.n)
					Ctrl->In.file = gmt_M_memory (GMT, Ctrl->In.file, n_alloc += GMT_SMALL_CHUNK, char *);
				Ctrl->In.file[Ctrl->In.n] = strdup(opt->arg);
				Ctrl->In.n++;
				break;

			/* Processes program-specific parameters */

			case 'A':
				n_errors += gmt_M_repeated_module_option (API, Ctrl->A.active);
				n_errors += gmt_get_no_argument (GMT, opt->arg, opt->option, 0);
				break;
			case 'C':	/* Clobber mode */
				n_errors += gmt_M_repeated_module_option (API, Ctrl->C.active);
				switch (opt->arg[0]) {
					case 'f': Ctrl->C.mode = MERGE2D_FIRST; break;
					case 'l': Ctrl->C.mode = MERGE2D_LOWER; break;
					case 'o': Ctrl->C.mode = MERGE2D_LAST;  break;
					case 'u': Ctrl->C.mode = MERGE2D_UPPER; break;
					default:
						GMT_Report (API, GMT_MSG_ERROR, "Option -C option: Modifiers are f|l|o|u only\n");
						n_errors++;
						break;
				}
				if (strstr (opt->arg, "+p"))		/* Only use nodes >= 0 in the updates */
					Ctrl->C.sign = +1;
				else if (strstr (opt->arg, "+n"))	/* Only use nodes <= 0 in the updates */
					Ctrl->C.sign = -1;
				else {	/* May be nothing of old-style trailing - or + */
					switch (opt->arg[1]) {	/* Any restriction due to sign */
						case '-':  Ctrl->C.sign = -1; break;
						case '+':  Ctrl->C.sign = +1; break;
						case '\0': Ctrl->C.sign =  0; break;
						default:
							GMT_Report (API, GMT_MSG_ERROR, "Option -C%c option: Sign modifiers are +n|p\n", opt->arg[0]);
							n_errors++;
							break;
					}
				}
				break;
			case 'F':
				n_errors += gmt_M_repeated_module_option (API, Ctrl->F.active);
				if (!opt->arg[0]) {
					GMT_Report(API, GMT_MSG_ERROR, "Option -F requires a comma-separated field list\n");
					n_errors++;
				}
				else Ctrl->F.fields = strdup(opt->arg);
				break;
			case 'G':	/* Output filename */
				n_errors += gmt_M_repeated_module_option (API, Ctrl->G.active);
				n_errors += gmt_get_required_file (GMT, opt->arg, opt->option, 0, GMT_IS_GRID, GMT_OUT, GMT_FILE_LOCAL, &(Ctrl->G.file));
				break;
			case 'H':	/* Fill internal holes before merging */
				n_errors += gmt_M_repeated_module_option (API, Ctrl->H.active);
				n_errors += merge2d_parse_gap_option(API, opt->arg, &Ctrl->H);
				break;
			case 'I':	/* Grid spacings */
				n_errors += gmt_M_repeated_module_option (API, Ctrl->I.active);
				n_errors += gmt_parse_inc_option (GMT, 'I', opt->arg);
				break;
			case 'M':
				n_errors += gmt_M_repeated_module_option (API, Ctrl->M.active);
				if (opt->arg[0] != 'E' && opt->arg[0] != 'B') {
					GMT_Report(API, GMT_MSG_ERROR, "Option -M directive must be E or B\n");
					n_errors++;
				}
				else Ctrl->M.method = opt->arg[0];
				Ctrl->M.write = strstr(opt->arg, "+w") != NULL;
				break;
			case 'P':
				n_errors += gmt_M_repeated_module_option(API, Ctrl->P.active);
				n_errors += gmt_get_no_argument(GMT, opt->arg, opt->option, 0);
				break;
			case 'W':	/* Write weights instead */
				n_errors += gmt_M_repeated_module_option (API, Ctrl->W.active);
				if (opt->arg[0] && strcmp(opt->arg, "+o")) {
					GMT_Report(API, GMT_MSG_ERROR, "Option -W only accepts the +o modifier\n");
					n_errors++;
				}
				Ctrl->W.only = strstr(opt->arg, "+o") != NULL;
				break;
			case 'E':
				n_errors += gmt_M_repeated_module_option(API, Ctrl->X.active);
				n_errors += gmt_get_no_argument(GMT, opt->arg, opt->option, 0);
				break;
			case 'Z': {
				char message[GMT_LEN256];
				n_errors += gmt_M_repeated_module_option (API, Ctrl->Z.active);
				if (!opt->arg[0] || opt->arg[0] != '+' ||
				    gq_transform_parse(opt->arg,
				                       GQ_TRANSFORM_X_MASK | GQ_TRANSFORM_Y_MASK,
				                       false, &Ctrl->Z.transform, NULL, NULL,
				                       message, sizeof(message))) {
					GMT_Report(API, GMT_MSG_ERROR,
					           "Option -Z requires explicit coordinate/value modifiers: %s\n",
					           message);
					n_errors++;
				}
				break;
			}

			default:	/* Report bad options */
				n_errors += gmt_default_option_error (GMT, opt);
				break;
		}
	}

	n_errors += gmt_M_check_condition (GMT, GMT->common.R.active[ISET] && (GMT->common.R.inc[GMT_X] <= 0.0 || GMT->common.R.inc[GMT_Y] <= 0.0),
	            "Option -I: Must specify positive increments\n");
	n_errors += gmt_M_check_condition (GMT, !Ctrl->G.active,
	            "Option -G: Must specify output file\n");
	return (n_errors ? GMT_PARSE_ERROR : GMT_NOERROR);
}
static int merge2d_polygon_bounds_file (const char *path, double *xmin, double *xmax,
                                            double *ymin, double *ymax) {
	polygon poly = {0};
	int status = GMT_DATA_READ_ERROR;
	if (blend_polygon_read(path, &poly) == SUCCESS &&
	    blend_polygon_validate(&poly) == SUCCESS &&
	    blend_polygon_bounds(&poly, xmin, xmax, ymin, ymax) == SUCCESS)
		status = GMT_NOERROR;
	blend_polygon_free(&poly);
	return status;
}

static int merge2d_polygon_to_real (const polygon *local, const double wesn[4],
                                        int nx, int ny, polygon *real) {
	size_t k;
	if (blend_polygon_alloc(real, local->n_vertices) != SUCCESS) return GMT_MEMORY_ERROR;
	for (k = 0; k < local->n_vertices; k++) {
		double x = wesn[XLO] + local->vertices[k].x * (wesn[XHI] - wesn[XLO]) / (double)(nx - 1);
		double y = wesn[YLO] + local->vertices[k].y * (wesn[YHI] - wesn[YLO]) / (double)(ny - 1);
		if (blend_polygon_set_vertex(real, k, x, y) != SUCCESS) {
			blend_polygon_free(real);
			return GMT_RUNTIME_ERROR;
		}
	}
	return GMT_NOERROR;
}

static int merge2d_monotone_name (const char *path, char output[PATH_MAX]) {
	const char *slash = strrchr(path, '/');
	const char *dot = strrchr(path, '.');
	size_t stem;
	if (dot && slash && dot < slash) dot = NULL;
	stem = dot ? (size_t)(dot - path) : strlen(path);
	if (stem + strlen("_monotone") + (dot ? strlen(dot) : 0) + 1 > PATH_MAX) return GMT_RUNTIME_ERROR;
	memcpy(output, path, stem);
	output[stem] = '\0';
	strcat(output, "_monotone");
	if (dot) strcat(output, dot);
	return GMT_NOERROR;
}

static int merge2d_prepare_support (struct GMT_CTRL *GMT, const char *clip_file,
                                       const double wesn[4], int nx, int ny,
                                       const struct MERGE2D_CTRL *Ctrl,
                                       window *data, polygon *real_support) {
	polygon input = {0}, local = {0}, monotone = {0}, converted_real = {0};
	permuted_vertex boundary = {0};
	char *polygon_path = NULL;
	int is_strict = 0, converted = 0, status = GMT_RUNTIME_ERROR;
	size_t k;

	if (clip_file) {
		status = gq_resolve_remote_path(GMT->parent, GMT_IS_DATASET,
		                                clip_file, &polygon_path);
		if (status != GMT_NOERROR ||
		    blend_polygon_read(polygon_path, &input) != SUCCESS) {
			GMT_Report(GMT->parent, GMT_MSG_ERROR, "Unable to read polygon %s\n", clip_file);
			goto cleanup;
		}
	}
	else {
		if (blend_polygon_alloc(&input, 4) != SUCCESS ||
		    blend_polygon_set_vertex(&input, 0, wesn[XLO], wesn[YLO]) != SUCCESS ||
		    blend_polygon_set_vertex(&input, 1, wesn[XHI], wesn[YLO]) != SUCCESS ||
		    blend_polygon_set_vertex(&input, 2, wesn[XHI], wesn[YHI]) != SUCCESS ||
		    blend_polygon_set_vertex(&input, 3, wesn[XLO], wesn[YHI]) != SUCCESS) {
			GMT_Report(GMT->parent, GMT_MSG_ERROR, "Unable to create rectangular support\n");
			goto cleanup;
		}
	}
	if (blend_polygon_validate(&input) != SUCCESS ||
	    blend_polygon_map_to_grid(&input, wesn[XLO], wesn[XHI], wesn[YLO], wesn[YHI],
	                              nx, ny, &local) != SUCCESS) {
		GMT_Report(GMT->parent, GMT_MSG_ERROR, "Invalid polygon support%s%s\n",
		           clip_file ? ": " : "", clip_file ? clip_file : "");
		goto cleanup;
	}
	for (k = 0; k < local.n_vertices; k++) {
		local.vertices[k].x = floor(local.vertices[k].x + 0.5);
		local.vertices[k].y = floor(local.vertices[k].y + 0.5);
	}
	if (blend_polygon_validate(&local) != SUCCESS ||
	    blend_polygon_is_xy_monotone_strict(&local, &is_strict) != SUCCESS) {
		GMT_Report(GMT->parent, GMT_MSG_ERROR, "Polygon is invalid after mapping to the local grid%s%s\n",
		           clip_file ? ": " : "", clip_file ? clip_file : "");
		goto cleanup;
	}
	if (is_strict) {
		if (blend_polygon_copy(&local, &monotone) != SUCCESS) goto cleanup;
	}
	else {
		if (!Ctrl->M.active) {
			GMT_Report(GMT->parent, GMT_MSG_ERROR,
			           "Polygon is not strictly xy-monotone; use -ME or -MB%s%s\n",
			           clip_file ? ": " : "", clip_file ? clip_file : "");
			status = GMT_PARSE_ERROR;
			goto cleanup;
		}
		if ((Ctrl->M.method == 'E' &&
		     blend_polygon_xy_monotone_envelope_strict(&local, &monotone) != SUCCESS) ||
		    (Ctrl->M.method == 'B' &&
		     blend_polygon_xy_monotone_best_piecewise_envelope_strict(
		         &local, &monotone, 0.0, (double)(nx - 1), 0.0, (double)(ny - 1), nx, ny) != SUCCESS)) {
			GMT_Report(GMT->parent, GMT_MSG_ERROR, "Unable to convert polygon with -M%c%s%s\n",
			           Ctrl->M.method, clip_file ? ": " : "", clip_file ? clip_file : "");
			goto cleanup;
		}
		converted = 1;
		GMT_Report(GMT->parent, GMT_MSG_INFORMATION,
		           "Converted polygon to a strictly xy-monotone support (%zu -> %zu vertices)%s%s\n",
		           local.n_vertices, monotone.n_vertices,
		           clip_file ? ": " : "", clip_file ? clip_file : "");
	}
	if (blend_polygon_is_xy_monotone_strict(&monotone, &is_strict) != SUCCESS || !is_strict ||
	    merge2d_polygon_to_real(&monotone, wesn, nx, ny, real_support) != GMT_NOERROR ||
	    blend_window_set_polygon(data, &monotone) != SUCCESS ||
	    boundary_assembly(data, &boundary) != SUCCESS) {
		GMT_Report(GMT->parent, GMT_MSG_ERROR, "Unable to assemble BLEND support%s%s\n",
		           clip_file ? ": " : "", clip_file ? clip_file : "");
		goto cleanup;
	}
	if (converted && Ctrl->M.write && clip_file) {
		char output[PATH_MAX] = {""};
		if (merge2d_monotone_name(clip_file, output) != GMT_NOERROR ||
		    merge2d_polygon_to_real(&monotone, wesn, nx, ny, &converted_real) != GMT_NOERROR ||
		    blend_polygon_write(output, &converted_real) != SUCCESS) {
			GMT_Report(GMT->parent, GMT_MSG_ERROR, "Unable to write converted polygon for %s\n", clip_file);
			goto cleanup;
		}
		GMT_Report(GMT->parent, GMT_MSG_INFORMATION, "Wrote converted polygon %s\n", output);
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

static int merge2d_support_weight_at (struct MERGE2D_PAIR *info, int col, int row,
                                         double *weight) {
	int x, y;
	if (info == NULL || info->v_data == NULL || weight == NULL) return GMT_RUNTIME_ERROR;
	if (col < info->support_i0 || col > info->support_i1 ||
	    row < info->support_j0 || row > info->support_j1) {
		*weight = 0.0;
		return GMT_NOERROR;
	}
	x = col - info->support_i0;
	y = info->support_j1 - row;
	if (embedding_contribution2d(x, y, info->v_data) != SUCCESS) return GMT_RUNTIME_ERROR;
	*weight = info->v_data->contribution;
	return GMT_NOERROR;
}

static bool merge2d_support_contains(const struct MERGE2D_PAIR *info, int col, int row) {
	const window *support = info->v_data;
	int x = col - info->support_i0, y = info->support_j1 - row;
	if (!support || col < info->support_i0 || col > info->support_i1 ||
	    row < info->support_j0 || row > info->support_j1) return false;
	return x >= support->nnx1[y] && x <= support->nnx2[y] &&
	       y >= support->nny1[x] && y <= support->nny2[x];
}

/* Diagnostic weights follow support priority independently of data fields. */
static int merge2d_report_weight(const struct MERGE2D_CTRL *Ctrl,
                                  struct MERGE2D_PAIR *items, unsigned int count,
                                  int col, int row, bool wrap_x, int nx_360,
                                  double *weight) {
	unsigned int k, owner = count;
	*weight = 0.0;
	for (k = 0; k < count; k++) {
		int local_col = col;
		double value;
		if (items[k].ignore || items[k].outside) continue;
		if (wrap_x) {
			local_col += nx_360;
			while (local_col > items[k].out_i1) local_col -= nx_360;
		}
		if (local_col < items[k].out_i0 || local_col > items[k].out_i1) continue;
		if (owner == count && !items[k].secondary) break;
		if (!items[k].secondary || !merge2d_support_contains(&items[k], local_col, row))
			continue;
		if (owner < count &&
		    (strcmp(items[k].s_source, items[owner].s_source) ||
		     !strcmp(items[k].source, items[owner].s_source))) continue;
		if (merge2d_support_weight_at(&items[k], local_col, row, &value) != GMT_NOERROR)
			return GMT_RUNTIME_ERROR;
		if (owner == count) {
			owner = k;
			*weight = value;
			if (!Ctrl->A.active || value <= 0.0) break;
		}
		else if (value > 0.0) *weight = MIN(1.0, *weight + value);
	}
	return GMT_NOERROR;
}

static bool merge2d_has_slice_selector (const char *source) {
	const char *question, *end, *open;
	if (source == NULL || (question = strchr(source, '?')) == NULL) return false;
	end = strchr(question + 1, '+');
	if (end == NULL) end = source + strlen(source);
	open = memchr(question + 1, '[', (size_t)(end - question - 1));
	if (open && memchr(open + 1, ']', (size_t)(end - open - 1))) return true;
	open = memchr(question + 1, '(', (size_t)(end - question - 1));
	return open && memchr(open + 1, ')', (size_t)(end - open - 1));
}

static int merge2d_parse_source_transform (const char *source,
                                               char clean[PATH_MAX],
                                               struct GQ_TRANSFORM *transform,
                                               bool *has_missing,
                                               double *missing) {
	const char *question, *mods = NULL;
	char message[GMT_LEN256], suffix[GMT_LEN256] = {""};
	size_t prefix;

	gq_transform_init(transform);
	*has_missing = false;
	*missing = 0.0;
	if (!source || strlen(source) >= PATH_MAX) return GMT_PARSE_ERROR;
	question = strchr(source, '?');
	if (question) mods = strchr(question + 1, '+');
	prefix = mods ? (size_t)(mods - source) : strlen(source);
	if (question && mods == question + 1) prefix = (size_t)(question - source);
	memcpy(clean, source, prefix);
	clean[prefix] = '\0';
	if (!mods) return GMT_NOERROR;
	if (gq_transform_parse(mods,
	                       GQ_TRANSFORM_X_MASK | GQ_TRANSFORM_Y_MASK |
	                       GQ_TRANSFORM_Z_MASK,
	                       true, transform, has_missing, missing,
	                       message, sizeof(message)))
		return GMT_PARSE_ERROR;
	if (*has_missing) snprintf(suffix, sizeof(suffix), "+n%.17g", *missing);
	if (strlen(clean) + strlen(suffix) + 1 >= PATH_MAX) return GMT_DIM_TOO_SMALL;
	strcat(clean, suffix);
	return GMT_NOERROR;
}

static int merge2d_validate_input_axis_scales (
    struct GMTAPI_CTRL *API, const char *source,
    const struct GQ_TRANSFORM *transform) {
	static const char axis_name[2] = {'x', 'y'};
	size_t axis;

	for (axis = GQ_TRANSFORM_X; axis <= GQ_TRANSFORM_Y; axis++) {
		if (transform->axis_scale[axis] > 0.0) continue;
		GMT_Report(API, GMT_MSG_ERROR,
		           "Transformed %c coordinates in %s must be strictly increasing. "
		           "GMT supplies retained grid axes in increasing order, and input "
		           "+%c scaling does not reorder grid data. Choose a positive "
		           "+%c<scale>; use output -Z+%c<scale> to write a decreasing "
		           "output axis if desired.\n",
		           axis_name[axis], source, axis_name[axis], axis_name[axis],
		           axis_name[axis]);
		return GMT_DATA_READ_ERROR;
	}
	return GMT_NOERROR;
}

static void merge2d_reverse_grid (struct GMT_GRID *Grid, bool x, bool y) {
	uint64_t left, right;
	unsigned int row, col;
	if (x)
		for (row = 0; row < Grid->header->n_rows; row++)
			for (col = 0; col < Grid->header->n_columns / 2; col++) {
				gmt_grdfloat swap;
				left = gmt_M_ijp(Grid->header, row, col);
				right = gmt_M_ijp(Grid->header, row,
				                  Grid->header->n_columns - 1 - col);
				swap = Grid->data[left];
				Grid->data[left] = Grid->data[right];
				Grid->data[right] = swap;
			}
	if (y)
		for (row = 0; row < Grid->header->n_rows / 2; row++)
			for (col = 0; col < Grid->header->n_columns; col++) {
				gmt_grdfloat swap;
				left = gmt_M_ijp(Grid->header, row, col);
				right = gmt_M_ijp(Grid->header,
				                  Grid->header->n_rows - 1 - row, col);
				swap = Grid->data[left];
				Grid->data[left] = Grid->data[right];
				Grid->data[right] = swap;
			}
}

static int merge2d_apply_transform (struct GMT_CTRL *GMT,
                                       struct GMT_GRID *Grid,
                                       const struct GQ_TRANSFORM *transform,
                                       bool include_values) {
	double xscale = transform->axis_scale[GQ_TRANSFORM_X];
	double yscale = transform->axis_scale[GQ_TRANSFORM_Y];
	double value_scale = include_values
	                   ? gq_transform_value_scale(transform, 0) : 1.0;
	double old[4], zmin = DBL_MAX, zmax = -DBL_MAX;
	unsigned int row, col;

	memcpy(old, Grid->header->wesn, sizeof(old));
	merge2d_reverse_grid(Grid, xscale < 0.0, yscale < 0.0);
	Grid->header->wesn[XLO] = MIN(old[XLO] * xscale, old[XHI] * xscale);
	Grid->header->wesn[XHI] = MAX(old[XLO] * xscale, old[XHI] * xscale);
	Grid->header->wesn[YLO] = MIN(old[YLO] * yscale, old[YHI] * yscale);
	Grid->header->wesn[YHI] = MAX(old[YLO] * yscale, old[YHI] * yscale);
	Grid->header->inc[GMT_X] *= fabs(xscale);
	Grid->header->inc[GMT_Y] *= fabs(yscale);
	if (transform->axis_unit[GQ_TRANSFORM_X])
		strncpy(Grid->header->x_units, transform->axis_unit[GQ_TRANSFORM_X],
		        sizeof(Grid->header->x_units) - 1);
	else if (transform->axis_set[GQ_TRANSFORM_X] && xscale != 1.0)
		Grid->header->x_units[0] = '\0';
	if (transform->axis_unit[GQ_TRANSFORM_Y])
		strncpy(Grid->header->y_units, transform->axis_unit[GQ_TRANSFORM_Y],
		        sizeof(Grid->header->y_units) - 1);
	else if (transform->axis_set[GQ_TRANSFORM_Y] && yscale != 1.0)
		Grid->header->y_units[0] = '\0';
	if (include_values) {
		for (row = 0; row < Grid->header->n_rows; row++)
			for (col = 0; col < Grid->header->n_columns; col++) {
				uint64_t node = gmt_M_ijp(Grid->header, row, col);
				if (!isfinite(Grid->data[node])) continue;
				Grid->data[node] *= (gmt_grdfloat)value_scale;
				zmin = MIN(zmin, Grid->data[node]);
				zmax = MAX(zmax, Grid->data[node]);
			}
		if (zmin <= zmax) Grid->header->z_min = zmin, Grid->header->z_max = zmax;
		if (gq_transform_value_unit(transform, 0))
			strncpy(Grid->header->z_units, gq_transform_value_unit(transform, 0),
			        sizeof(Grid->header->z_units) - 1);
		else if (value_scale != 1.0)
			Grid->header->z_units[0] = '\0';
	}
	gmt_set_grddim(GMT, Grid->header);
	return GMT_NOERROR;
}

static void merge2d_reverse_row (double *values, size_t length) {
	size_t k;
	for (k = 0; k < length / 2; k++) {
		double swap = values[k];
		values[k] = values[length - 1 - k];
		values[length - 1 - k] = swap;
	}
}

static int merge2d_reverse_netcdf_coordinate (int ncid, int varid,
                                                  size_t length) {
	double *values = calloc(length, sizeof(*values));
	size_t k;
	int status = GMT_RUNTIME_ERROR;
	if (!values) return GMT_MEMORY_ERROR;
	if (nc_get_var_double(ncid, varid, values) != NC_NOERR) goto cleanup;
	merge2d_reverse_row(values, length);
	for (k = 0; k < length; k++)
		if (values[k] == 0.0) values[k] = 0.0;
	if (nc_put_var_double(ncid, varid, values) != NC_NOERR) goto cleanup;
	status = GMT_NOERROR;

cleanup:
	free(values);
	return status;
}

static int merge2d_restore_netcdf_grid_order (int ncid, int varid,
                                                  size_t ny, size_t nx,
                                                  bool reverse_x,
                                                  bool reverse_y) {
	double *first = NULL, *second = NULL;
	size_t start[2] = {0, 0}, count[2] = {1, nx}, row;
	int status = GMT_RUNTIME_ERROR;

	if (!reverse_x && !reverse_y) return GMT_NOERROR;
	first = calloc(nx, sizeof(*first));
	second = reverse_y ? calloc(nx, sizeof(*second)) : NULL;
	if (!first || (reverse_y && !second)) {
		status = GMT_MEMORY_ERROR;
		goto cleanup;
	}
	if (reverse_y) {
		for (row = 0; row < ny / 2; row++) {
			start[0] = row;
			if (nc_get_vara_double(ncid, varid, start, count, first) != NC_NOERR)
				goto cleanup;
			start[0] = ny - 1 - row;
			if (nc_get_vara_double(ncid, varid, start, count, second) != NC_NOERR)
				goto cleanup;
			if (reverse_x) {
				merge2d_reverse_row(first, nx);
				merge2d_reverse_row(second, nx);
			}
			start[0] = row;
			if (nc_put_vara_double(ncid, varid, start, count, second) != NC_NOERR)
				goto cleanup;
			start[0] = ny - 1 - row;
			if (nc_put_vara_double(ncid, varid, start, count, first) != NC_NOERR)
				goto cleanup;
		}
		if (ny % 2 && reverse_x) {
			start[0] = ny / 2;
			if (nc_get_vara_double(ncid, varid, start, count, first) != NC_NOERR)
				goto cleanup;
			merge2d_reverse_row(first, nx);
			if (nc_put_vara_double(ncid, varid, start, count, first) != NC_NOERR)
				goto cleanup;
		}
	}
	else {
		for (row = 0; row < ny; row++) {
			start[0] = row;
			if (nc_get_vara_double(ncid, varid, start, count, first) != NC_NOERR)
				goto cleanup;
			merge2d_reverse_row(first, nx);
			if (nc_put_vara_double(ncid, varid, start, count, first) != NC_NOERR)
				goto cleanup;
		}
	}
	status = GMT_NOERROR;

cleanup:
	free(first);
	free(second);
	return status;
}

static int merge2d_finalize_transform_units (
    const char *path, const struct GQ_TRANSFORM *transform,
    bool include_values, bool preserve_output_order) {
	int ncid = -1, nvars, varid, ndims;
	int grid_var = -1, grid_dims[2] = {-1, -1}, status = GMT_RUNTIME_ERROR;
	int coordinate_var[2] = {-1, -1};
	size_t axis_length[2] = {0, 0};
	int k;

	if (nc_open(path, NC_WRITE, &ncid) != NC_NOERR ||
	    nc_inq_nvars(ncid, &nvars) != NC_NOERR)
		goto cleanup;
	for (varid = 0; varid < nvars; varid++) {
		if (nc_inq_varndims(ncid, varid, &ndims) != NC_NOERR || ndims != 2)
			continue;
		if (nc_inq_vardimid(ncid, varid, grid_dims) != NC_NOERR)
			goto cleanup;
		grid_var = varid;
		break;
	}
	if (grid_var < 0 || nc_redef(ncid) != NC_NOERR) goto cleanup;
	for (k = 0; k < 2; k++) {
		int axis = k == 0 ? GQ_TRANSFORM_Y : GQ_TRANSFORM_X;
		char name[NC_MAX_NAME + 1];
		const char *unit = transform->axis_unit[axis];
		if (nc_inq_dimname(ncid, grid_dims[k], name) != NC_NOERR ||
		    nc_inq_dimlen(ncid, grid_dims[k], &axis_length[k]) != NC_NOERR ||
		    nc_inq_varid(ncid, name, &coordinate_var[k]) != NC_NOERR)
			goto cleanup;
		if (unit) {
			if (nc_put_att_text(ncid, coordinate_var[k], "units", strlen(unit), unit) != NC_NOERR)
				goto cleanup;
		}
		else if (transform->axis_set[axis] &&
		         transform->axis_scale[axis] != 1.0) {
			int error = nc_del_att(ncid, coordinate_var[k], "units");
			if (error != NC_NOERR && error != NC_ENOTATT) goto cleanup;
		}
	}
	if (include_values) {
		const char *unit = gq_transform_value_unit(transform, 0);
		double scale = gq_transform_value_scale(transform, 0);
		if (unit) {
			if (nc_put_att_text(ncid, grid_var, "units", strlen(unit), unit) != NC_NOERR)
				goto cleanup;
		}
		else if (scale != 1.0) {
			int error = nc_del_att(ncid, grid_var, "units");
			if (error != NC_NOERR && error != NC_ENOTATT) goto cleanup;
		}
	}
	if (nc_enddef(ncid) != NC_NOERR) goto cleanup;
	if (preserve_output_order) {
		bool reverse_x = transform->axis_scale[GQ_TRANSFORM_X] < 0.0;
		bool reverse_y = transform->axis_scale[GQ_TRANSFORM_Y] < 0.0;
		if ((reverse_y && merge2d_reverse_netcdf_coordinate(
		                      ncid, coordinate_var[0], axis_length[0])) ||
		    (reverse_x && merge2d_reverse_netcdf_coordinate(
		                      ncid, coordinate_var[1], axis_length[1])) ||
		    merge2d_restore_netcdf_grid_order(
		        ncid, grid_var, axis_length[0], axis_length[1],
		        reverse_x, reverse_y))
			goto cleanup;
	}
	status = GMT_NOERROR;

cleanup:
	if (ncid >= 0) nc_close(ncid);
	return status;
}

static int merge2d_transform_file (struct GMT_CTRL *GMT,
                                      const char *input, const char *output,
                                      const struct GQ_TRANSFORM *transform,
                                      bool include_values) {
	struct GMT_GRID *Grid = GMT_Read_Data(
	    GMT->parent, GMT_IS_GRID, GMT_IS_FILE, GMT_IS_SURFACE,
	    GMT_CONTAINER_AND_DATA, NULL, input, NULL);
	int status = GMT_RUNTIME_ERROR;
	if (!Grid) return GMT_DATA_READ_ERROR;
	if (merge2d_apply_transform(GMT, Grid, transform, include_values) == GMT_NOERROR &&
	    GMT_Write_Data(GMT->parent, GMT_IS_GRID, GMT_IS_FILE, GMT_IS_SURFACE,
	                   GMT_CONTAINER_AND_DATA, NULL, output, Grid) == GMT_NOERROR &&
	    merge2d_finalize_transform_units(output, transform, include_values,
	                                     true) == GMT_NOERROR)
		status = GMT_NOERROR;
	GMT_Destroy_Data(GMT->parent, &Grid);
	return status;
}

static int merge2d_resolve_level_selector (struct GMT_CTRL *GMT, const char *source,
                                               const struct GQ_TRANSFORM *transform,
                                               char output[PATH_MAX]) {
	const char *question, *open, *close, *mods;
	char path[PATH_MAX] = {""}, variable[NC_MAX_NAME + 1] = {""}, *resolved = NULL, *end = NULL;
	double requested, *levels = NULL, nearest = HUGE_VAL;
	int ncid = -1, varid, coordinate_varid, coordinate_ndims, coordinate_dimid;
	int ndims, dimids[NC_MAX_DIMS], status = GMT_RUNTIME_ERROR;
	size_t path_length, variable_length, n_levels, index = 0, k;
	char dimension[NC_MAX_NAME + 1] = {""};

	if (source == NULL || strlen(source) >= PATH_MAX) return GMT_PARSE_ERROR;
	strcpy(output, source);
	question = strchr(source, '?');
	if (question == NULL) return GMT_NOERROR;
	mods = strchr(question + 1, '+');
	if (mods == NULL) mods = source + strlen(source);
	open = memchr(question + 1, '(', (size_t)(mods - question - 1));
	if (open == NULL) return GMT_NOERROR;
	close = memchr(open + 1, ')', (size_t)(mods - open - 1));
	if (close == NULL) return GMT_PARSE_ERROR;
	requested = strtod(open + 1, &end);
	if (end != close || !isfinite(requested)) {
		GMT_Report(GMT->parent, GMT_MSG_ERROR, "Invalid NetCDF layer level in %s\n", source);
		return GMT_PARSE_ERROR;
	}
	path_length = (size_t)(question - source);
	variable_length = (size_t)(open - question - 1);
	if (path_length == 0 || path_length >= sizeof(path) || variable_length == 0 ||
	    variable_length > NC_MAX_NAME) return GMT_PARSE_ERROR;
	memcpy(path, source, path_length);
	memcpy(variable, question + 1, variable_length);
	if (gq_resolve_remote_path(GMT->parent, GMT_IS_GRID, path, &resolved) != GMT_NOERROR ||
	    nc_open(resolved, NC_NOWRITE, &ncid) != NC_NOERR ||
	    nc_inq_varid(ncid, variable, &varid) != NC_NOERR ||
	    nc_inq_varndims(ncid, varid, &ndims) != NC_NOERR || ndims != 3 ||
	    nc_inq_vardimid(ncid, varid, dimids) != NC_NOERR ||
	    nc_inq_dim(ncid, dimids[0], dimension, &n_levels) != NC_NOERR || n_levels == 0 ||
	    nc_inq_varid(ncid, dimension, &coordinate_varid) != NC_NOERR ||
	    nc_inq_varndims(ncid, coordinate_varid, &coordinate_ndims) != NC_NOERR ||
	    coordinate_ndims != 1 ||
	    nc_inq_vardimid(ncid, coordinate_varid, &coordinate_dimid) != NC_NOERR ||
	    coordinate_dimid != dimids[0]) {
		GMT_Report(GMT->parent, GMT_MSG_ERROR,
		           "Unable to inspect the third coordinate for %s\n", source);
		goto cleanup;
	}
	levels = malloc(n_levels * sizeof(*levels));
	if (levels == NULL) {
		status = GMT_MEMORY_ERROR;
		goto cleanup;
	}
	if (nc_get_var_double(ncid, coordinate_varid, levels) != NC_NOERR) {
		GMT_Report(GMT->parent, GMT_MSG_ERROR,
		           "Unable to read the third coordinate for %s\n", source);
		goto cleanup;
	}
	{
		double scale = 1.0, offset = 0.0;
		nc_get_att_double(ncid, coordinate_varid, "scale_factor", &scale);
		nc_get_att_double(ncid, coordinate_varid, "add_offset", &offset);
		for (k = 0; k < n_levels; k++)
			levels[k] = (levels[k] * scale + offset) *
			            transform->axis_scale[GQ_TRANSFORM_Z];
	}
	for (k = 0; k < n_levels; k++) {
		double distance;
		if (!isfinite(levels[k])) continue;
		distance = fabs(levels[k] - requested);
		if (distance < nearest) {
			nearest = distance;
			index = k;
		}
	}
	if (!isfinite(nearest) ||
	    snprintf(output, PATH_MAX, "%.*s[%zu]%s", (int)(open - source), source,
	             index, close + 1) >= PATH_MAX) {
		status = GMT_PARSE_ERROR;
		goto cleanup;
	}
	GMT_Report(GMT->parent, GMT_MSG_INFORMATION,
	           "Selected %s level %.12g at layer %zu (coordinate %.12g)\n",
	           variable, requested, index, levels[index]);
	status = GMT_NOERROR;

cleanup:
	free(levels);
	if (ncid >= 0) nc_close(ncid);
	free(resolved);
	return status;
}

static int merge2d_materialize_slice (struct GMT_CTRL *GMT, const char *source,
                                         char output[PATH_MAX]) {
	static char *V_level = GMT_VERBOSE_CODES;
	char clean[PATH_MAX] = {""}, selected[PATH_MAX] = {""};
	char raw[PATH_MAX] = {""};
	char command[2 * PATH_MAX + GMT_LEN256];
	struct GQ_TRANSFORM transform;
	struct GMT_GRID *Grid = NULL;
	bool has_missing, slice;
	double missing;
	int status = GMT_RUNTIME_ERROR;
	output[0] = '\0';
	if (merge2d_parse_source_transform(source, clean, &transform,
	                                   &has_missing, &missing)) {
		gq_transform_free(&transform);
		return GMT_PARSE_ERROR;
	}
	if (gq_transform_validate_values(&transform, 1, command, sizeof(command)))
		goto cleanup;
	if (merge2d_validate_input_axis_scales(GMT->parent, source, &transform))
		goto cleanup;
	slice = merge2d_has_slice_selector(clean);
	if ((transform.axis_set[GQ_TRANSFORM_Z] ||
	     transform.axis_unit[GQ_TRANSFORM_Z]) && !slice) {
		GMT_Report(GMT->parent, GMT_MSG_ERROR,
		           "+z/+Z is only valid for a 3-D source selected as a 2-D slice\n");
		goto cleanup;
	}
	strcpy(selected, clean);
	if (slice && merge2d_resolve_level_selector(GMT, clean, &transform, selected))
		goto cleanup;
	if (slice) {
		if (gmt_get_tempname(GMT->parent, "merge2d_slice", ".nc", raw)) goto cleanup;
		snprintf(command, sizeof(command), "%s %s -V%c --GMT_HISTORY=readonly",
		         selected, raw, V_level[GMT->current.setting.verbose]);
		if (GMT_Call_Module(GMT->parent, "grdconvert", GMT_MODULE_CMD, command))
			goto cleanup;
	}
	if (!gq_transform_active(&transform) ||
	    (!transform.axis_set[GQ_TRANSFORM_X] &&
	     !transform.axis_set[GQ_TRANSFORM_Y] &&
	     !transform.axis_unit[GQ_TRANSFORM_X] &&
	     !transform.axis_unit[GQ_TRANSFORM_Y] &&
	     !transform.values_set && !transform.value_units_set)) {
		if (slice) strcpy(output, raw), raw[0] = '\0';
		status = GMT_NOERROR;
		goto cleanup;
	}
	Grid = GMT_Read_Data(GMT->parent, GMT_IS_GRID, GMT_IS_FILE, GMT_IS_SURFACE,
	                     GMT_CONTAINER_AND_DATA, NULL, slice ? raw : clean, NULL);
	if (!Grid || merge2d_apply_transform(GMT, Grid, &transform, true)) goto cleanup;
	if (gmt_get_tempname(GMT->parent, "merge2d_scaled", ".nc", output)) goto cleanup;
	if (GMT_Write_Data(GMT->parent, GMT_IS_GRID, GMT_IS_FILE, GMT_IS_SURFACE,
	                   GMT_CONTAINER_AND_DATA, NULL, output, Grid))
		goto cleanup;
	if (merge2d_finalize_transform_units(output, &transform, true, false)) goto cleanup;
	status = GMT_NOERROR;

cleanup:
	if (Grid) GMT_Destroy_Data(GMT->parent, &Grid);
	if (raw[0]) gmt_remove_file(GMT, raw);
	if (status != GMT_NOERROR && output[0]) {
		gmt_remove_file(GMT, output);
		output[0] = '\0';
	}
	gq_transform_free(&transform);
	return status;
}

static int merge2d_validate_overlap_secondaries (struct GMT_CTRL *GMT,
                                                     struct MERGE2D_PAIR *items,
                                                     unsigned int count) {
	unsigned int a, b;
	for (a = 0; a < count; a++) {
		if (items[a].v_data == NULL) continue;
		for (b = a + 1; b < count; b++) {
			if (items[b].v_data == NULL) continue;
			int col0 = MAX(items[a].support_i0, items[b].support_i0);
			int col1 = MIN(items[a].support_i1, items[b].support_i1);
			int row0 = MAX(items[a].support_j0, items[b].support_j0);
			int row1 = MIN(items[a].support_j1, items[b].support_j1);
			int col, row, overlaps = 0;
			if (col0 > col1 || row0 > row1) continue;
			for (row = row0; row <= row1 && !overlaps; row++) {
				for (col = col0; col <= col1; col++) {
					double wa = 0.0, wb = 0.0;
					if (merge2d_support_weight_at(&items[a], col, row, &wa) != GMT_NOERROR ||
					    merge2d_support_weight_at(&items[b], col, row, &wb) != GMT_NOERROR)
						return GMT_RUNTIME_ERROR;
					if (wa > 0.0 && wb > 0.0) {
						overlaps = 1;
						break;
					}
				}
			}
			if (!overlaps) continue;
			if (!strcmp(items[b].source, items[a].s_source)) continue;
			if (!items[a].secondary || !items[b].secondary ||
			    strcmp(items[a].s_source, items[b].s_source)) {
				GMT_Report(GMT->parent, GMT_MSG_ERROR,
				           "Overlapping primary supports %s and %s must use the same secondary grid with -A\n",
				           items[a].file, items[b].file);
				return GMT_RUNTIME_ERROR;
			}
		}
	}
	return GMT_NOERROR;
}

static int merge2d_sanitize_resample_source (struct GMT_CTRL *GMT,
                                                 const char *source,
                                                 char output[GMT_BUFSIZ]) {
	char command[GMT_BUFSIZ];
	int status;

	output[0] = '\0';
	if (!GMT->common.d.active[GMT_IN]) return GMT_NOERROR;
	if (gmt_get_tempname (GMT->parent, "merge2d_nodata", ".nc", output))
		return GMT_RUNTIME_ERROR;
	snprintf (command, sizeof(command), "%s %.17g NAN = %s -V%c --GMT_HISTORY=readonly",
	          source, GMT->common.d.nan_proxy[GMT_IN], output,
	          GMT_VERBOSE_CODES[GMT->current.setting.verbose]);
	status = GMT_Call_Module (GMT->parent, "grdmath", GMT_MODULE_CMD, command);
	if (status != GMT_NOERROR) {
		gmt_remove_file (GMT, output);
		output[0] = '\0';
		GMT_Report (GMT->parent, GMT_MSG_ERROR,
		            "Unable to replace input nodata values in %s\n", source);
	}
	return status;
}

static const char *merge2d_gap_method_name(char method) {
	switch (method) {
		case 'n': return "nearest neighbor";
		case 'l': return "linear Delaunay";
		case 'a': return "local weighted average";
		case 's': return "spline";
		case 'm': return "minimum curvature";
		default: return "unknown";
	}
}

static int merge2d_write_xyz(struct GMT_CTRL *GMT, struct GMT_GRID *Grid,
                                 const char *path) {
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

static int merge2d_fill_internal_gaps(struct GMT_CTRL *GMT,
                                          const struct MERGE2D_CTRL *Ctrl,
                                          const char *source,
                                          char output[PATH_MAX]) {
	struct GMT_GRID *Grid = NULL, *Candidate = NULL;
	uint8_t *visited = NULL, *eligible = NULL;
	uint64_t *queue = NULL;
	char clean[GMT_BUFSIZ] = {""}, candidate[PATH_MAX] = {""};
	char xyz[PATH_MAX] = {""}, command[4 * PATH_MAX + GMT_LEN512] = {""};
	const char *input = source;
	static char *V_level = GMT_VERBOSE_CODES;
	size_t nx, ny, nxy, row, col, eligible_holes = 0, eligible_nodes = 0;
	size_t filled_nodes = 0;
	int status = GMT_RUNTIME_ERROR;

	output[0] = '\0';
	if (merge2d_sanitize_resample_source(GMT, source, clean) != GMT_NOERROR)
		goto cleanup;
	if (clean[0]) input = clean;
	Grid = GMT_Read_Data(GMT->parent, GMT_IS_GRID, GMT_IS_FILE,
	                     GMT_IS_SURFACE, GMT_CONTAINER_AND_DATA,
	                     NULL, input, NULL);
	if (Grid == NULL) {
		status = GMT_DATA_READ_ERROR;
		goto cleanup;
	}
	nx = Grid->header->n_columns;
	ny = Grid->header->n_rows;
	if (ny && nx > SIZE_MAX / ny) {
		status = GMT_MEMORY_ERROR;
		goto cleanup;
	}
	nxy = nx * ny;
	visited = calloc(nxy, sizeof(*visited));
	eligible = calloc(nxy, sizeof(*eligible));
	queue = calloc(nxy, sizeof(*queue));
	if (!visited || !eligible || !queue) {
		status = GMT_MEMORY_ERROR;
		goto cleanup;
	}

	for (row = 0; row < ny; row++) {
		for (col = 0; col < nx; col++) {
			size_t node = row * nx + col, head = 0, tail = 0, q;
			size_t min_row = row, max_row = row, min_col = col, max_col = col;
			bool boundary = false, accepted;
			if (visited[node] ||
		    isfinite(Grid->data[gmt_M_ijp(Grid->header, row, col)]))
				continue;
			visited[node] = 1;
			queue[tail++] = node;
			while (head < tail) {
				size_t current = queue[head++];
				size_t current_row = current / nx;
				size_t current_col = current % nx;
				int dr, dc;
				if (current_row == 0 || current_row + 1 == ny ||
				    current_col == 0 || current_col + 1 == nx)
					boundary = true;
				if (current_row < min_row) min_row = current_row;
				if (current_row > max_row) max_row = current_row;
				if (current_col < min_col) min_col = current_col;
				if (current_col > max_col) max_col = current_col;
				for (dr = -1; dr <= 1; dr++) {
					for (dc = -1; dc <= 1; dc++) {
						long next_row, next_col;
						size_t next;
						if (dr == 0 && dc == 0) continue;
						next_row = (long)current_row + dr;
						next_col = (long)current_col + dc;
						if (next_row < 0 || next_col < 0 ||
						    next_row >= (long)ny || next_col >= (long)nx)
							continue;
						next = (size_t)next_row * nx + (size_t)next_col;
						if (visited[next] ||
						    isfinite(Grid->data[gmt_M_ijp(Grid->header,
						                                  (size_t)next_row,
						                                  (size_t)next_col)]))
							continue;
						visited[next] = 1;
						queue[tail++] = next;
					}
				}
			}
			accepted = !boundary &&
			           (!Ctrl->H.limited ||
			            (max_col - min_col + 1 <= Ctrl->H.max_gap &&
			             max_row - min_row + 1 <= Ctrl->H.max_gap));
			if (!accepted) continue;
			eligible_holes++;
			eligible_nodes += tail;
			for (q = 0; q < tail; q++) eligible[queue[q]] = 1;
		}
	}

	if (eligible_nodes == 0) {
		GMT_Report(GMT->parent, GMT_MSG_INFORMATION,
		           "No eligible internal gaps in %s\n", source);
		if (clean[0]) {
			strncpy(output, clean, PATH_MAX - 1);
			clean[0] = '\0';
		}
		status = GMT_NOERROR;
		goto cleanup;
	}
	if (gmt_get_tempname(GMT->parent, "merge2d_gap_candidate", ".nc", candidate) ||
	    gmt_get_tempname(GMT->parent, "merge2d_gap_filled", ".nc", output)) {
		status = GMT_RUNTIME_ERROR;
		goto cleanup;
	}

	if (Ctrl->H.method == 'n') {
		if (Ctrl->H.argument > 0.0)
			snprintf(command, sizeof(command), "%s -An%.12g -G%s -V%c --GMT_HISTORY=readonly",
			         input, Ctrl->H.argument, candidate,
			         V_level[GMT->current.setting.verbose]);
		else
			snprintf(command, sizeof(command), "%s -An -G%s -V%c --GMT_HISTORY=readonly",
			         input, candidate, V_level[GMT->current.setting.verbose]);
		status = GMT_Call_Module(GMT->parent, "grdfill", GMT_MODULE_CMD, command);
	}
	else if (Ctrl->H.method == 's') {
		snprintf(command, sizeof(command), "%s -As%.12g -G%s -V%c --GMT_HISTORY=readonly",
		         input, Ctrl->H.argument, candidate,
		         V_level[GMT->current.setting.verbose]);
		status = GMT_Call_Module(GMT->parent, "grdfill", GMT_MODULE_CMD, command);
	}
	else {
		const char *registration = Grid->header->registration ? "-rp" : "-rg";
		const char *geographic = gmt_M_is_geographic(GMT, GMT_IN) ? "-fg" : "";
		double radius = Ctrl->H.argument *
		                MAX(Grid->header->inc[GMT_X], Grid->header->inc[GMT_Y]);
		unsigned int minimum_sectors = MAX(1U, (Ctrl->H.sectors + 1U) / 2U);
		if (gmt_get_tempname(GMT->parent, "merge2d_gap_points", ".txt", xyz) ||
		    merge2d_write_xyz(GMT, Grid, xyz) != GMT_NOERROR) {
			status = GMT_DATA_WRITE_ERROR;
			goto cleanup;
		}
		if (Ctrl->H.method == 'l')
			snprintf(command, sizeof(command),
			         "%s -R%.17g/%.17g/%.17g/%.17g -I%.17g/%.17g %s %s -Z -G%s -V%c --GMT_HISTORY=readonly",
			         xyz, Grid->header->wesn[XLO], Grid->header->wesn[XHI],
			         Grid->header->wesn[YLO], Grid->header->wesn[YHI],
			         Grid->header->inc[GMT_X], Grid->header->inc[GMT_Y],
			         registration, geographic, candidate,
			         V_level[GMT->current.setting.verbose]);
		else if (Ctrl->H.method == 'a')
			snprintf(command, sizeof(command),
			         "%s -R%.17g/%.17g/%.17g/%.17g -I%.17g/%.17g -S%.17g%s -N%u+m%u %s %s -G%s -V%c --GMT_HISTORY=readonly",
			         xyz, Grid->header->wesn[XLO], Grid->header->wesn[XHI],
			         Grid->header->wesn[YLO], Grid->header->wesn[YHI],
			         Grid->header->inc[GMT_X], Grid->header->inc[GMT_Y], radius,
			         geographic[0] ? "d" : "", Ctrl->H.sectors, minimum_sectors,
			         registration, geographic, candidate,
			         V_level[GMT->current.setting.verbose]);
		else
			snprintf(command, sizeof(command),
			         "%s -R%.17g/%.17g/%.17g/%.17g -I%.17g/%.17g -T%.17g %s %s %s -G%s -V%c --GMT_HISTORY=readonly",
			         xyz, Grid->header->wesn[XLO], Grid->header->wesn[XHI],
			         Grid->header->wesn[YLO], Grid->header->wesn[YHI],
			         Grid->header->inc[GMT_X], Grid->header->inc[GMT_Y],
			         Ctrl->H.argument, registration, geographic,
			         geographic[0] ? "-Am" : "", candidate,
			         V_level[GMT->current.setting.verbose]);
		status = GMT_Call_Module(GMT->parent,
		                         Ctrl->H.method == 'l' ? "triangulate" :
		                         Ctrl->H.method == 'a' ? "nearneighbor" : "surface",
		                         GMT_MODULE_CMD, command);
	}
	if (status != GMT_NOERROR) {
		GMT_Report(GMT->parent, GMT_MSG_ERROR,
		           "Unable to fill internal gaps in %s with %s interpolation\n",
		           source, merge2d_gap_method_name(Ctrl->H.method));
		goto cleanup;
	}
	Candidate = GMT_Read_Data(GMT->parent, GMT_IS_GRID, GMT_IS_FILE,
	                          GMT_IS_SURFACE, GMT_CONTAINER_AND_DATA,
	                          NULL, candidate, NULL);
	if (Candidate == NULL || Candidate->header->n_columns != nx ||
	    Candidate->header->n_rows != ny) {
		status = GMT_DATA_READ_ERROR;
		goto cleanup;
	}
	for (row = 0; row < ny; row++) {
		for (col = 0; col < nx; col++) {
			size_t node = row * nx + col;
			gmt_grdfloat value;
			if (!eligible[node]) continue;
			value = Candidate->data[gmt_M_ijp(Candidate->header, row, col)];
			if (!isfinite(value)) continue;
			Grid->data[gmt_M_ijp(Grid->header, row, col)] = value;
			filled_nodes++;
		}
	}
	if (GMT_Write_Data(GMT->parent, GMT_IS_GRID, GMT_IS_FILE,
	                   GMT_IS_SURFACE, GMT_CONTAINER_AND_DATA,
	                   NULL, output, Grid) != GMT_NOERROR) {
		status = GMT_DATA_WRITE_ERROR;
		goto cleanup;
	}
	GMT_Report(GMT->parent, GMT_MSG_INFORMATION,
	           "Filled %zu nodes in %zu internal gap%s in %s using %s interpolation\n",
	           filled_nodes, eligible_holes, eligible_holes == 1 ? "" : "s",
	           source, merge2d_gap_method_name(Ctrl->H.method));
	status = GMT_NOERROR;

cleanup:
	if (Candidate) GMT_Destroy_Data(GMT->parent, &Candidate);
	if (Grid) GMT_Destroy_Data(GMT->parent, &Grid);
	free(visited);
	free(eligible);
	free(queue);
	if (candidate[0]) gmt_remove_file(GMT, candidate);
	if (xyz[0]) gmt_remove_file(GMT, xyz);
	if (clean[0]) gmt_remove_file(GMT, clean);
	if (status != GMT_NOERROR && output[0]) {
		gmt_remove_file(GMT, output);
		output[0] = '\0';
	}
	return status;
}

static int merge2d_prepare_job(struct GMT_CTRL *GMT, char **files,
                               unsigned int n_files,
                               struct GMT_GRID_HEADER **h_ptr,
                               struct MERGE2D_PAIR **merge, bool delayed,
                               struct GMT_GRID *Grid,
                               const struct MERGE2D_CTRL *Ctrl)
{
	/* Returns how many primary grid files or a negative error value if something went wrong */
	int status;
	unsigned int one_or_zero, n = 0, n_scanned;
	bool common_inc = true, common_reg = true;
	bool do_sample, s_do_sample;
	struct MERGE2D_PAIR *B = NULL;
	struct GMT_GRID_HEADER *h = *h_ptr;	/* Input header may be NULL or preset */
	struct GMT_GRID_HIDDEN *GH = NULL; /* primary */
	struct GMT_GRID_HIDDEN *s_GH = NULL; /* secondary */
	struct GMT_GRID_HEADER_HIDDEN *HH = NULL;
	struct GMT_GRID_HEADER_HIDDEN *HHG = NULL; /* primary */
	struct GMT_GRID_HEADER_HIDDEN *s_HHG = NULL; /* secondary */

	char buffer[GMT_BUFSIZ] = {""}, s_buffer[GMT_BUFSIZ] = {""}, res[4] = {""};
	static char *V_level = GMT_VERBOSE_CODES;
	char Iargs[GMT_LEN256] = {""}, s_Iargs[GMT_LEN256] = {""}, Rargs[GMT_LEN256] = {""}, s_Rargs[GMT_LEN256] = {""};
	char cmd[GMT_BUFSIZ] = {""}, s_cmd[GMT_BUFSIZ] = {""};
	double wesn[4];
	struct MERGE2D_SPEC *L = NULL;

	if (n_files > 1) {	/* Got a bunch of grid files - no merging takes place here */
		L = gmt_M_memory (GMT, NULL, n_files, struct MERGE2D_SPEC);
		for (n = 0; n < n_files; n++) {
			if (gq_resolve_remote_source(GMT->parent, GMT_IS_GRID,
			                             files[n], NULL, &L[n].file))
				return (-GMT_DATA_READ_ERROR);
			L[n].have_secondary = false;	/* no secondary files for all primary grid files */
			L[n].have_polygon = false;
		}
	
	}
	else {	/* Must read merge file */
		size_t n_alloc = 0;
		struct GMT_RECORD *In = NULL;
		char file[PATH_MAX] = {""};
		char s_file[PATH_MAX] = {""}; /* secondary grid filename */
		char c_file[PATH_MAX] = {""}; /* clip filename */
		char w_in[GMT_LEN256] = {""}; /* for the window function paramter, e.g., cosine/cosine */
		char taper_in[GMT_LEN256] = {""};
		gmt_set_meminc(GMT, GMT_SMALL_CHUNK);

		do {	/* Keep returning records until we reach EOF */
			if ((In = GMT_Get_Record (GMT->parent, GMT_READ_TEXT, NULL)) == NULL) {	/* Read next record, get NULL if special case */
				if (gmt_M_rec_is_error (GMT)) 		/* Bail if there are any read errors */
					return (-GMT_DATA_READ_ERROR);
				else if (gmt_M_rec_is_eof (GMT)) 		/* Reached end of file */
					break;
				continue;							/* Go back and read the next record */
			}
			/* Data record to process */

			/* Data record to process.  We permit this kind of records:
			 * file [s_file] [c_file] [window_x/window_y] [taper_ratio]
			 * i.e., file (primary grid file) is required but the s_file 
			 * (secondary grid file), c_file (clip file), w_in (window 
			 * functions) and taper_ratio are all optional.
			 */

			n_scanned = sscanf (In->text, "%s %s %s %s %255s", file, s_file, c_file, w_in, taper_in);
			if (n_scanned < 1) {
				GMT_Report (GMT->parent, GMT_MSG_ERROR, "Read error for merging parameters near row %d\n", n);
				gmt_M_free (GMT, L);
				return (-GMT_DATA_READ_ERROR);
			}
			if (n == n_alloc) L = gmt_M_malloc (GMT, L, n, &n_alloc, struct MERGE2D_SPEC);
			if (gq_resolve_remote_source(GMT->parent, GMT_IS_GRID,
			                             file, NULL, &L[n].file))
				return (-GMT_DATA_READ_ERROR);
			L[n].functions = strdup("cosine/cosine");
			L[n].taper_ratio[0] = L[n].taper_ratio[1] =
			L[n].taper_ratio[2] = L[n].taper_ratio[3] = 0.2;
			if (n_scanned > 1 && strcmp(s_file, "-")) {
				L[n].have_secondary = true;
				if (gq_resolve_remote_source(GMT->parent, GMT_IS_GRID,
				                             s_file, NULL, &L[n].secondary))
					return (-GMT_DATA_READ_ERROR);
			}
			if (n_scanned > 2 && strcmp(c_file, "-")) {
				double minx = 0.0, maxx = 0.0, miny = 0.0, maxy = 0.0;
				L[n].have_polygon = true;
				if (gq_resolve_remote_path(GMT->parent, GMT_IS_DATASET,
				                           c_file, &L[n].polygon))
					return (-GMT_DATA_READ_ERROR);
				if (merge2d_polygon_bounds_file(L[n].polygon, &minx, &maxx, &miny, &maxy) != GMT_NOERROR) {
					GMT_Report(GMT->parent, GMT_MSG_ERROR, "Read error for clip file %s\n", L[n].polygon);
					return (-GMT_DATA_READ_ERROR);
				}
				L[n].wesn[XLO] = minx;
				L[n].wesn[XHI] = maxx;
				L[n].wesn[YLO] = miny;
				L[n].wesn[YHI] = maxy;
				GMT_Report(GMT->parent, GMT_MSG_INFORMATION,
				           "Support bounds are %.17g/%.17g/%.17g/%.17g from %s\n",
				           minx, maxx, miny, maxy, L[n].polygon);
			}
			if (n_scanned > 3 && strcmp(w_in, "-")) {
				gmt_M_str_free(L[n].functions);
				L[n].functions = strdup(w_in);
			}
			if (n_scanned > 4 && strcmp(taper_in, "-") &&
			    merge2d_parse_taper_ratios(GMT->parent, taper_in, L[n].taper_ratio) != GMT_NOERROR)
				return (-GMT_PARSE_ERROR);
			n++;
		} while (true);
		gmt_reset_meminc (GMT);
		n_files = n;
	}

	B = gmt_M_memory (GMT, NULL, n_files, struct MERGE2D_PAIR);

	wesn[XLO] = wesn[YLO] = DBL_MAX;	wesn[XHI] = wesn[YHI] = -DBL_MAX;
	if (!delayed) sprintf (res, "-r%c", (Grid->header->registration == GMT_GRID_PIXEL_REG) ? 'p' : 'g');	/* We know the required registration up front */


	for (n = 0; n < n_files; n++) {	/* Process each input grid */
		strncpy (B[n].source, L[n].file, PATH_MAX-1);
		{
			char slice[PATH_MAX] = {""};
			if (merge2d_materialize_slice(GMT, L[n].file, slice) != GMT_NOERROR)
				return (-GMT_DATA_READ_ERROR);
			strncpy(B[n].file, slice[0] ? slice : L[n].file, PATH_MAX - 1);
			B[n].delete = slice[0] != '\0';
		}
		if (Ctrl->H.active) {
			char filled[PATH_MAX] = {""}, previous[PATH_MAX] = {""};
			bool previous_delete = B[n].delete;
			if (previous_delete) strncpy(previous, B[n].file, PATH_MAX - 1);
			if (merge2d_fill_internal_gaps(GMT, Ctrl, B[n].file, filled) != GMT_NOERROR)
				return (-GMT_RUNTIME_ERROR);
			if (filled[0]) {
				strncpy(B[n].file, filled, PATH_MAX - 1);
				B[n].delete = true;
				if (previous_delete) gmt_remove_file(GMT, previous);
			}
		}
		B[n].memory = gmt_M_file_is_memory (B[n].file);	/* If grid in memory then we only read once and have everything at once */
		if ((B[n].G = GMT_Read_Data (GMT->parent, GMT_IS_GRID, GMT_IS_FILE, GMT_IS_SURFACE, GMT_CONTAINER_ONLY|GMT_GRID_ROW_BY_ROW, NULL, B[n].file, NULL)) == NULL) {
			if (B[n].delete) gmt_remove_file(GMT, B[n].file);
			/* Failure somehow, free all grids read so far and bail */
			for (n = 0; n < n_files; n++) {
				gmt_M_str_free(L[n].file);
			}
			gmt_M_free (GMT, L);	gmt_M_free (GMT, B);
			return (-GMT_DATA_READ_ERROR);
		}
		if (merge2d_validate_grid_format(GMT, B[n].G->header, B[n].file) != GMT_NOERROR) {
			GMT_Report(GMT->parent, GMT_MSG_ERROR,
			           "Unable to validate input grid %s\n", B[n].file);
			return (-GMT_NOT_A_VALID_LOGMODE);
		}
		if (!L[n].have_polygon)
			gmt_M_memcpy (B[n].wesn, B[n].G->header->wesn, 4, double);	/* Set inner = outer region */
		else
			gmt_M_memcpy(B[n].wesn, L[n].wesn, 4, double);
		if (h == NULL) {	/* Was not given -R, determine it from the min-max extents of all input grids */
			if (B[n].G->header->wesn[YLO] < wesn[YLO]) wesn[YLO] = B[n].G->header->wesn[YLO];
			if (B[n].G->header->wesn[YHI] > wesn[YHI]) wesn[YHI] = B[n].G->header->wesn[YHI];
			if (B[n].G->header->wesn[XLO] < wesn[XLO]) wesn[XLO] = B[n].G->header->wesn[XLO];
			if (B[n].G->header->wesn[XHI] > wesn[XHI]) wesn[XHI] = B[n].G->header->wesn[XHI];
			if (n > 0) {
				if (fabs((B[n].G->header->inc[GMT_X] - B[0].G->header->inc[GMT_X]) / B[0].G->header->inc[GMT_X]) > 0.002 ||
					fabs((B[n].G->header->inc[GMT_Y] - B[0].G->header->inc[GMT_Y]) / B[0].G->header->inc[GMT_Y]) > 0.002)
						common_inc = false;
			}
			if (B[n].G->header->registration != B[0].G->header->registration)
				common_reg = false;
		}

		/* Prepare the optional secondary without rejecting a valid primary. */
		if (L[n].have_secondary) {
			strncpy (B[n].s_source, L[n].secondary, PATH_MAX-1);
			{
				char slice[PATH_MAX] = {""};
				if (merge2d_materialize_slice(GMT, L[n].secondary, slice) != GMT_NOERROR) {
					B[n].secondary = false;
					continue;
				}
				strncpy(B[n].s_file, slice[0] ? slice : L[n].secondary, PATH_MAX - 1);
				B[n].s_delete = slice[0] != '\0';
			}
			if (Ctrl->H.active) {
				char filled[PATH_MAX] = {""}, previous[PATH_MAX] = {""};
				bool previous_delete = B[n].s_delete;
				if (previous_delete) strncpy(previous, B[n].s_file, PATH_MAX - 1);
				if (merge2d_fill_internal_gaps(GMT, Ctrl, B[n].s_file, filled) != GMT_NOERROR) {
					B[n].secondary = false;
					continue;
				}
				if (filled[0]) {
					strncpy(B[n].s_file, filled, PATH_MAX - 1);
					B[n].s_delete = true;
					if (previous_delete) gmt_remove_file(GMT, previous);
				}
			}
			B[n].s_memory = gmt_M_file_is_memory (B[n].s_file);	/* If grid in memory then we only read once and have everything at once */
			if ((B[n].s_G = GMT_Read_Data (GMT->parent, GMT_IS_GRID, GMT_IS_FILE, GMT_IS_SURFACE, GMT_CONTAINER_ONLY|GMT_GRID_ROW_BY_ROW, NULL, B[n].s_file, NULL)) == NULL) {
				if (B[n].s_delete) gmt_remove_file(GMT, B[n].s_file);
				GMT_Report(GMT->parent, GMT_MSG_WARNING,
				           "Ignoring unreadable secondary grid %s\n", B[n].s_file);
				gmt_M_str_free (L[n].secondary);
				B[n].secondary = false; 
				continue;
			}
			if (merge2d_validate_grid_format(GMT, B[n].s_G->header, B[n].s_file) != GMT_NOERROR) {
				GMT_Report(GMT->parent, GMT_MSG_WARNING,
				           "Ignoring unsupported secondary grid %s\n", B[n].s_file);
				gmt_M_str_free (L[n].secondary);	/* Done with these now */
				B[n].secondary = false; 
				continue;
			}
			gmt_M_memcpy (B[n].s_wesn, B[n].s_G->header->wesn, 4, double);	/* Set inner = outer region always for secondary grids */
			B[n].secondary = true; /* things are going well so far with the secondary grid */
			
		}
	}

	if (h == NULL) {	/* Must use the common region from the tiles and require -I -r if not common increments or registration */
		uint64_t pp;
		double *inc = NULL;
		if (!common_inc && !GMT->common.R.active[ISET])
			GMT_Report (GMT->parent, GMT_MSG_ERROR, "Must specify -I if input grids have different increments\n");
		if (!common_reg && !GMT->common.R.active[GSET])
			GMT_Report (GMT->parent, GMT_MSG_ERROR, "Must specify -r if input grids have different registrations\n");
		if ((!common_inc && !GMT->common.R.active[ISET]) || (!common_reg && !GMT->common.R.active[GSET]))
			return (-GMT_RUNTIME_ERROR);
		/* While the inc may be fixed, our wesn may not be in phase, so since gmt_set_grddim
		 * will plow through and modify inc if it does not fit, we don't want that here. */
		if (GMT->common.R.active[ISET])	/* Got -I [-R -r] */
			gmt_increment_adjust (GMT, wesn, GMT->common.R.inc, (GMT->common.R.active[GSET]) ? GMT->common.R.registration : B[0].G->header->registration);	/* In case user specified incs using distance units we must call this here before adjusting wesn */
		inc = (GMT->common.R.active[ISET]) ? GMT->common.R.inc : B[0].G->header->inc;	/* Either use -I if given else they all have the same increments */
		pp = (uint64_t)ceil ((wesn[XHI] - wesn[XLO])/inc[GMT_X] - GMT_CONV6_LIMIT);
		wesn[XHI] = wesn[XLO] + pp * B[0].G->header->inc[GMT_X];
		pp = (uint64_t)ceil ((wesn[YHI] - wesn[YLO])/inc[GMT_Y] - GMT_CONV6_LIMIT);
		wesn[YHI] = wesn[YLO] + pp * B[0].G->header->inc[GMT_Y];
		/* Create the h structure and initialize it */
		h = gmt_get_header (GMT);
		gmt_M_memcpy (h->wesn, wesn, 4, double);
		gmt_M_memcpy(h->inc, inc, 2, double);
		h->registration = (GMT->common.R.active[GSET]) ? GMT->common.R.registration : B[0].G->header->registration;	/* Either use -r if given else they all have the same registration */
		gmt_M_grd_setpad (GMT, h, GMT->current.io.pad); /* Assign default pad */
		gmt_set_grddim (GMT, h);	/* Update dimensions */
		*h_ptr = h;			/* Pass out the updated settings */
		GMT_Report (GMT->parent, GMT_MSG_INFORMATION,
			"We determined the region %.12g/%.12g/%.12g/%.12g from the given grids\n", h->wesn[XLO], h->wesn[XHI], h->wesn[YLO], h->wesn[YHI]);
	}
	HH = gmt_get_H_hidden (h);
	one_or_zero = !h->registration;

	for (n = 0; n < n_files; n++) {	/* Process each input grid */
		struct GMT_GRID_HEADER *t = B[n].G->header;	/* Shortcut for this tile header */

		/* Skip the file if its outer region does not lie within the final grid region */
		if (merge2d_outside_y_range (h, B[n].wesn)) {
			GMT_Report (GMT->parent, GMT_MSG_WARNING,
			            "File %s entirely outside y-range of final grid region (skipped)\n", B[n].file);
			B[n].ignore = true;
			continue;
		}
		if (gmt_M_x_is_lon (GMT, GMT_IN)) {	/* Must carefully check the longitude overlap */
			if (merge2d_overlap_check (GMT, &B[n], h, 0)) continue;	/* Check header for -+360 issues and overlap  */
			if (merge2d_overlap_check (GMT, &B[n], h, 1)) continue;	/* Check inner region for -+360 issues and overlap  */
		}
		else if (merge2d_outside_cartesian_x_range (h, B[n].wesn)) {
			GMT_Report (GMT->parent, GMT_MSG_WARNING,
			            "File %s entirely outside x-range of final grid region (skipped)\n", B[n].file);
			B[n].ignore = true;
			continue;
		}

		/* If input grids have different spacing or registration we must resample */

		Iargs[0] = Rargs[0] = '\0';
		do_sample = 0;
		if (fabs((t->inc[GMT_X] - h->inc[GMT_X]) / h->inc[GMT_X]) > 0.002 ||
			fabs((t->inc[GMT_Y] - h->inc[GMT_Y]) / h->inc[GMT_Y]) > 0.002) {
			sprintf (Iargs, "-I%.12g/%.12g", h->inc[GMT_X], h->inc[GMT_Y]);
			GMT_Report (GMT->parent, GMT_MSG_WARNING, "File %s has different increments (%.12g/%.12g) than the output grid (%.12g/%.12g) - must resample\n",
				B[n].file, t->inc[GMT_X], t->inc[GMT_Y], h->inc[GMT_X], h->inc[GMT_Y]);
			do_sample |= 1;
		}
		if (merge2d_out_of_phase (t, h)) {	/* Set explicit -R for resampling that is multiple of desired increments AND inside both original grid and desired grid */
			double wesn[4];	/* Make sure wesn is equal to or larger than B[n].G->header->wesn so all points are included */
			unsigned int k;
			/* The goal below is to come up with a wesn that a proper subset of the output grid while still not
			 * exceeding the bounds of the current tile grid. The wesn below will be compatible with the output grid
			 * but is not compatible with the input grid.  grdsample will make the adjustment */
			k = (unsigned int)rint ((MAX (h->wesn[XLO], t->wesn[XLO]) - h->wesn[XLO]) / h->inc[GMT_X] - h->xy_off);
			wesn[XLO] = h->wesn[XLO] + k * h->inc[GMT_X];
			while (wesn[XLO] < t->wesn[XLO]) wesn[XLO] += t->inc[GMT_X];	/* Make sure we are not outside this grid */
			k = (unsigned int)rint ((MIN (h->wesn[XHI], t->wesn[XHI]) - h->wesn[XLO]) / h->inc[GMT_X] - h->xy_off);
			wesn[XHI] = h->wesn[XLO] + k * h->inc[GMT_X];
			while (wesn[XHI] > t->wesn[XHI]) wesn[XHI] -= t->inc[GMT_X];	/* Make sure we are not outside this grid */
			k = (unsigned int)rint ((MAX (h->wesn[YLO], t->wesn[YLO]) - h->wesn[YLO]) / h->inc[GMT_Y] - h->xy_off);
			wesn[YLO] = h->wesn[YLO] + k * h->inc[GMT_Y];
			while (wesn[YLO] < t->wesn[YLO]) wesn[YLO] += t->inc[GMT_Y];	/* Make sure we are not outside this grid */
			k = (unsigned int)rint  ((MIN (h->wesn[YHI], t->wesn[YHI]) - h->wesn[YLO]) / h->inc[GMT_Y] - h->xy_off);
			wesn[YHI] = h->wesn[YLO] + k * h->inc[GMT_Y];
			while (wesn[YHI] > t->wesn[YHI]) wesn[YHI] -= t->inc[GMT_Y];	/* Make sure we are not outside this grid */
			sprintf (Rargs, "-R%.12g/%.12g/%.12g/%.12g", wesn[XLO], wesn[XHI], wesn[YLO], wesn[YHI]);
			GMT_Report (GMT->parent, GMT_MSG_WARNING, "File %s coordinates are phase-shifted w.r.t. the output grid - must resample\n", B[n].file);
			do_sample |= 1;
		}
		else if (do_sample) {	/* Set explicit -R to handle possible subsetting */
			double wesn[4];
			gmt_M_memcpy (wesn, h->wesn, 4, double);
			if (wesn[XLO] < t->wesn[XLO]) wesn[XLO] = t->wesn[XLO];
			if (wesn[XHI] > t->wesn[XHI]) wesn[XHI] = t->wesn[XHI];
			if (wesn[YLO] < t->wesn[YLO]) wesn[YLO] = t->wesn[YLO];
			if (wesn[YHI] > t->wesn[YHI]) wesn[YHI] = t->wesn[YHI];
			sprintf (Rargs, "-R%.12g/%.12g/%.12g/%.12g", wesn[XLO], wesn[XHI], wesn[YLO], wesn[YHI]);
			GMT_Report (GMT->parent, GMT_MSG_DEBUG, "File %s is sampled using region %s\n", B[n].file, Rargs);
		}
		if (do_sample) {	/* One or more reasons to call upon grdsample before using this grid */
			char previous_file[PATH_MAX] = {""};
			bool previous_delete = B[n].delete;
			if (previous_delete) strncpy(previous_file, B[n].file, PATH_MAX - 1);
			gmt_filename_set (B[n].file);	/* Replace any spaces in filename with ASCII 29 */
			GMT->common.V.active = false;	/* Since we will parse again below */
			{	/* Resample the grid into a netCDF grid. */
				char clean[GMT_BUFSIZ] = {""};
				const char *sample_source = B[n].file;

				if (gmt_get_tempname (GMT->parent, "merge2d_resampled", ".nc", buffer))
					return GMT_RUNTIME_ERROR;
				if (merge2d_sanitize_resample_source(GMT, B[n].file, clean) != GMT_NOERROR)
					return (-GMT_RUNTIME_ERROR);
				if (clean[0]) sample_source = clean;
				snprintf (cmd, sizeof(cmd), "%s %s %s %s -G%s -V%c", sample_source, res,
				         Iargs, Rargs, buffer, V_level[GMT->current.setting.verbose]);
				if (gmt_M_is_geographic (GMT, GMT_IN)) strcat (cmd, " -fg");
				strcat (cmd, " --GMT_HISTORY=readonly");
				GMT_Report (GMT->parent, GMT_MSG_INFORMATION, "Resample %s via grdsample %s\n", B[n].file, cmd);
				status = GMT_Call_Module (GMT->parent, "grdsample", GMT_MODULE_CMD, cmd);
				if (clean[0]) gmt_remove_file (GMT, clean);
				if (status) {	/* Resample the file */
					GMT_Report (GMT->parent, GMT_MSG_ERROR, "Unable to resample file %s - exiting\n", B[n].file);
					return (-GMT_RUNTIME_ERROR);
				}
			}
			strncpy (B[n].file, buffer, PATH_MAX-1);	/* Use the temporary file instead */
			B[n].delete = true;		/* Flag to delete this temporary file when done */
			if (GMT_Destroy_Data (GMT->parent, &B[n].G))
				return (-GMT_RUNTIME_ERROR);
			if (previous_delete) gmt_remove_file(GMT, previous_file);
			t = NULL;	/* To remind us that this is now gone */
			if ((B[n].G = GMT_Read_Data (GMT->parent, GMT_IS_GRID, GMT_IS_FILE, GMT_IS_SURFACE, GMT_CONTAINER_ONLY|GMT_GRID_ROW_BY_ROW, NULL, B[n].file, NULL)) == NULL) {
				return (-GMT_DATA_READ_ERROR);
			}
			t = B[n].G->header;	/* Since it got reallocated - now the new resampled wesn region */
			if (merge2d_overlap_check (GMT, &B[n], h, 0)) continue;	/* In case grdconvert changed the region - checks for the secondary grid too */
		}
		B[n].RbR = gmt_M_memory (GMT, NULL, 1, struct GMT_GRID_ROWBYROW);		/* Allocate structure */
		GH = gmt_get_G_hidden (B[n].G);
		if (GH->extra != NULL)	/* Only memory grids will fail this test */
			gmt_M_memcpy (B[n].RbR, GH->extra, 1, struct GMT_GRID_ROWBYROW);	/* Duplicate, since GMT_Destroy_Data will free the header->extra */

		/* Here, i0, j0 is the very first col, row to read, while i1, j1 is the very last col, row to read .
		 * Weights at the outside i,j should be 0, and reach 1 at the edge of the inside block */

		/* The following works for both pixel and grid-registered grids since we are here using the i,j to measure the width of the
		 * taper zone in units of dx, dy. */

		B[n].out_i0 = irint ((t->wesn[XLO] - h->wesn[XLO]) * HH->r_inc[GMT_X]);
		B[n].out_i1 = irint ((t->wesn[XHI] - h->wesn[XLO]) * HH->r_inc[GMT_X]) - t->registration;
		B[n].out_j0 = irint ((h->wesn[YHI] - t->wesn[YHI]) * HH->r_inc[GMT_Y]);
		B[n].out_j1 = irint ((h->wesn[YHI] - t->wesn[YLO]) * HH->r_inc[GMT_Y]) - t->registration;
    
		if (B[n].out_j0 < 0) {	/* Must skip to first row inside the present -R */
			if (GMT->session.grdformat[t->type][0] == 'c')	/* Classic 1-D netCDF grid */
				B[n].offset = t->n_columns * abs (B[n].out_j0);
			else	/* 2-D netCDF grid */
				B[n].offset = B[n].out_j0;
		}
		GMT_Report (GMT->parent, GMT_MSG_DEBUG, "Grid %s: out: %d/%d/%d/%d offset: %d\n",
			B[n].file, B[n].out_i0, B[n].out_i1, B[n].out_j1, B[n].out_j0, (int)B[n].offset);

		/* Allocate space for one entire row for this grid */

		B[n].z = gmt_M_memory (GMT, NULL, t->n_columns, gmt_grdfloat);
		HHG = gmt_get_H_hidden (t);

		GMT_Report (GMT->parent, GMT_MSG_INFORMATION, "Merge file %s in %.12g/%.12g/%.12g/%.12g [%d-%d]\n",
			HHG->name, B[n].wesn[XLO], B[n].wesn[XHI], B[n].wesn[YLO], B[n].wesn[YHI], B[n].out_j0, B[n].out_j1);

		if (!B[n].memory) {	/* Free grid unless it is a memory grid */
			gmtlib_close_grd (GMT, B[n].G);	/* Close the grid file so we don't have lots of them open */
			if (GMT_Destroy_Data (GMT->parent, &B[n].G)) return (-GMT_RUNTIME_ERROR);
		}

		/* Align the optional secondary and prepare the BLEND window. */
		if (B[n].secondary) {

			struct GMT_GRID_HEADER *s_t = B[n].s_G->header;	/* Shortcut for this tile header */

			/* Skip the file if its outer region does not lie within the final grid region */
			if (merge2d_outside_y_range (h, B[n].s_wesn)) {
				GMT_Report (GMT->parent, GMT_MSG_WARNING,
							"File %s entirely outside y-range of final grid region (skipped)\n", B[n].s_file);
				B[n].s_ignore = true;
				B[n].secondary = false;
				gmt_M_str_free (L[n].secondary); 
				continue;
			}
			/* The gmt_M_x_is_lon | merge2d_overlap_check for the secondary grid is already done within the function
			called above - No call ours */
			if (gmt_M_x_is_lon (GMT, GMT_IN)) {	/* Must carefully check the longitude overlap */
				if (merge2d_secondary_overlap_check (GMT, &B[n], h, 0)) continue;	/* Check header for -+360 issues and overlap  */
				if (merge2d_secondary_overlap_check (GMT, &B[n], h, 1)) continue;	/* Check inner region for -+360 issues and overlap  */
			}	

			if (merge2d_outside_cartesian_x_range (h, B[n].s_wesn)) {
				GMT_Report (GMT->parent, GMT_MSG_WARNING,
							"File %s entirely outside x-range of final grid region (skipped)\n", B[n].s_file);
				B[n].s_ignore = true;
				B[n].secondary = false;
				gmt_M_str_free (L[n].secondary); 
				continue;
			}

			/* If input grids have different spacing or registration we must resample */

			s_Iargs[0] = s_Rargs[0] = '\0';
			s_do_sample = 0;
			if (fabs((s_t->inc[GMT_X] - h->inc[GMT_X]) / h->inc[GMT_X]) > 0.002 ||
				fabs((s_t->inc[GMT_Y] - h->inc[GMT_Y]) / h->inc[GMT_Y]) > 0.002) {
				sprintf (s_Iargs, "-I%.12g/%.12g", h->inc[GMT_X], h->inc[GMT_Y]);
				GMT_Report (GMT->parent, GMT_MSG_WARNING, "File %s has different increments (%.12g/%.12g) than the output grid (%.12g/%.12g) - must resample\n",
					B[n].s_file, s_t->inc[GMT_X], s_t->inc[GMT_Y], h->inc[GMT_X], h->inc[GMT_Y]);
				s_do_sample |= 1;
				
			}

			if (merge2d_out_of_phase (s_t, h)) {	/* Set explicit -R for resampling that is multiple of desired increments AND inside both original grid and desired grid */
				double wesn[4];	/* Make sure wesn is equal to or larger than B[n].G->header->wesn so all points are included */
				unsigned int k;
				/* The goal below is to come up with a wesn that a proper subset of the output grid while still not
				* exceeding the bounds of the current tile grid. The wesn below will be compatible with the output grid
				* but is not compatible with the input grid.  grdsample will make the adjustment */
				k = (unsigned int)rint ((MAX (h->wesn[XLO], s_t->wesn[XLO]) - h->wesn[XLO]) / h->inc[GMT_X] - h->xy_off);
				wesn[XLO] = h->wesn[XLO] + k * h->inc[GMT_X];
				while (wesn[XLO] < s_t->wesn[XLO]) wesn[XLO] += s_t->inc[GMT_X];	/* Make sure we are not outside this grid */
				k = (unsigned int)rint ((MIN (h->wesn[XHI], s_t->wesn[XHI]) - h->wesn[XLO]) / h->inc[GMT_X] - h->xy_off);
				wesn[XHI] = h->wesn[XLO] + k * h->inc[GMT_X];
				while (wesn[XHI] > s_t->wesn[XHI]) wesn[XHI] -= s_t->inc[GMT_X];	/* Make sure we are not outside this grid */
				k = (unsigned int)rint ((MAX (h->wesn[YLO], s_t->wesn[YLO]) - h->wesn[YLO]) / h->inc[GMT_Y] - h->xy_off);
				wesn[YLO] = h->wesn[YLO] + k * h->inc[GMT_Y];
				while (wesn[YLO] < s_t->wesn[YLO]) wesn[YLO] += s_t->inc[GMT_Y];	/* Make sure we are not outside this grid */
				k = (unsigned int)rint  ((MIN (h->wesn[YHI], s_t->wesn[YHI]) - h->wesn[YLO]) / h->inc[GMT_Y] - h->xy_off);
				wesn[YHI] = h->wesn[YLO] + k * h->inc[GMT_Y];
				while (wesn[YHI] > s_t->wesn[YHI]) wesn[YHI] -= s_t->inc[GMT_Y];	/* Make sure we are not outside this grid */
				sprintf (s_Rargs, "-R%.12g/%.12g/%.12g/%.12g", wesn[XLO], wesn[XHI], wesn[YLO], wesn[YHI]);
				GMT_Report (GMT->parent, GMT_MSG_WARNING, "File %s coordinates are phase-shifted w.r.t. the output grid - must resample\n", B[n].s_file);
				s_do_sample |= 1;
			}
			else if (s_do_sample) {	/* Set explicit -R to handle possible subsetting */
				double wesn[4];
				gmt_M_memcpy (wesn, h->wesn, 4, double);
				if (wesn[XLO] < s_t->wesn[XLO]) wesn[XLO] = s_t->wesn[XLO];
				if (wesn[XHI] > s_t->wesn[XHI]) wesn[XHI] = s_t->wesn[XHI];
				if (wesn[YLO] < s_t->wesn[YLO]) wesn[YLO] = s_t->wesn[YLO];
				if (wesn[YHI] > s_t->wesn[YHI]) wesn[YHI] = s_t->wesn[YHI];
				sprintf (s_Rargs, "-R%.12g/%.12g/%.12g/%.12g", wesn[XLO], wesn[XHI], wesn[YLO], wesn[YHI]);
				GMT_Report (GMT->parent, GMT_MSG_DEBUG, "File %s is sampled using region %s\n", B[n].s_file, s_Rargs);
			}
			if (s_do_sample) {	/* One or more reasons to call upon grdsample before using this grid */
				char previous_file[PATH_MAX] = {""};
				bool previous_delete = B[n].s_delete;
				if (previous_delete) strncpy(previous_file, B[n].s_file, PATH_MAX - 1);
				gmt_filename_set (B[n].s_file);	/* Replace any spaces in filename with ASCII 29 */
				GMT->common.V.active = false;	/* Since we will parse again below */
				{	/* Resample the grid into a netCDF grid. */
					char clean[GMT_BUFSIZ] = {""};
					const char *sample_source = B[n].s_file;
					if (gmt_get_tempname (GMT->parent, "merge2d_secondary_resampled", ".nc", s_buffer)) {
						B[n].s_ignore = true;
						B[n].secondary = false;
						gmt_M_str_free (L[n].secondary); 
						continue;
					}
					if (merge2d_sanitize_resample_source(GMT, B[n].s_file, clean) != GMT_NOERROR) {
						B[n].s_ignore = true;
						B[n].secondary = false;
						gmt_M_str_free (L[n].secondary);
						continue;
					}
					if (clean[0]) sample_source = clean;
					snprintf (s_cmd, sizeof(s_cmd), "%s %s %s %s -G%s -V%c", sample_source, res,
							s_Iargs, s_Rargs, s_buffer, V_level[GMT->current.setting.verbose]);
					if (gmt_M_is_geographic (GMT, GMT_IN)) strcat (s_cmd, " -fg");
					strcat (s_cmd, " --GMT_HISTORY=readonly");
					GMT_Report (GMT->parent, GMT_MSG_INFORMATION, "Resample %s via grdsample %s\n", B[n].s_file, s_cmd);
					status = GMT_Call_Module (GMT->parent, "grdsample", GMT_MODULE_CMD, s_cmd);
					if (clean[0]) gmt_remove_file (GMT, clean);
					if (status) {	/* Resample the file */
						GMT_Report (GMT->parent, GMT_MSG_ERROR, "Unable to resample file %s - exiting\n", B[n].s_file);
						B[n].s_ignore = true;
						B[n].secondary = false;
						gmt_M_str_free (L[n].secondary); 
						continue;
					}
				}
				strncpy (B[n].s_file, s_buffer, PATH_MAX-1);	/* Use the temporary file instead */
				B[n].s_delete = true;		/* Flag to delete this temporary file when done */
				if (GMT_Destroy_Data (GMT->parent, &B[n].s_G)) {
					B[n].s_ignore = true;
					B[n].secondary = false;
					gmt_M_str_free (L[n].secondary); 
					continue;
				}	
				if (previous_delete) gmt_remove_file(GMT, previous_file);
				s_t = NULL;	/* To remind us that this is now gone */
				if ((B[n].s_G = GMT_Read_Data (GMT->parent, GMT_IS_GRID, GMT_IS_FILE, GMT_IS_SURFACE, GMT_CONTAINER_ONLY|GMT_GRID_ROW_BY_ROW, NULL, B[n].s_file, NULL)) == NULL) {
					B[n].s_ignore = true;
					B[n].secondary = false;
					gmt_M_str_free (L[n].secondary); 
					continue;
				}
				s_t = B[n].s_G->header;	/* Since it got reallocated */
				if (merge2d_secondary_overlap_check(GMT, &B[n], h, 0)) continue;
			}
			B[n].s_RbR = gmt_M_memory (GMT, NULL, 1, struct GMT_GRID_ROWBYROW);		/* Allocate structure */
			s_GH = gmt_get_G_hidden (B[n].s_G);
			if (s_GH->extra != NULL)	/* Only memory grids will fail this test */
				gmt_M_memcpy (B[n].s_RbR, s_GH->extra, 1, struct GMT_GRID_ROWBYROW);	/* Duplicate, since GMT_Destroy_Data will free the header->extra */

			/* Here, i0, j0 is the very first col, row to read, while i1, j1 is the very last col, row to read .
			* Weights at the outside i,j should be 0, and reach 1 at the edge of the inside block */

			/* The following works for both pixel and grid-registered grids since we are here using the i,j to measure the width of the
			* taper zone in units of dx, dy. */

			B[n].s_out_i0 = irint ((s_t->wesn[XLO] - h->wesn[XLO]) * HH->r_inc[GMT_X]);
			B[n].s_out_i1 = irint ((s_t->wesn[XHI] - h->wesn[XLO]) * HH->r_inc[GMT_X]) - s_t->registration;
			B[n].s_out_j0 = irint ((h->wesn[YHI] - s_t->wesn[YHI]) * HH->r_inc[GMT_Y]);
			B[n].s_out_j1 = irint ((h->wesn[YHI] - s_t->wesn[YLO]) * HH->r_inc[GMT_Y]) - s_t->registration;
		
			if (B[n].s_out_j0 < 0) {	/* Must skip to first row inside the present -R */
				if (GMT->session.grdformat[s_t->type][0] == 'c')	/* Classic 1-D netCDF grid */
					B[n].s_offset = s_t->n_columns * abs (B[n].s_out_j0);
				else	/* 2-D netCDF grid */
					B[n].s_offset = B[n].s_out_j0;
			}
			GMT_Report (GMT->parent, GMT_MSG_DEBUG, "Secondary grid %s: out: %d/%d/%d/%d offset: %d\n",
				B[n].s_file, B[n].s_out_i0, B[n].s_out_i1, B[n].s_out_j1, B[n].s_out_j0, (int)B[n].s_offset);

			/* Allocate space for one entire row for this grid */

			B[n].s_z = gmt_M_memory (GMT, NULL, s_t->n_columns, gmt_grdfloat);
			s_HHG = gmt_get_H_hidden (s_t);

			GMT_Report (GMT->parent, GMT_MSG_INFORMATION, "Secondary merge file %s in %.12g/%.12g/%.12g/%.12g [%d-%d]\n",
				s_HHG->name, B[n].s_wesn[XLO], B[n].s_wesn[XHI], B[n].s_wesn[YLO], B[n].s_wesn[YHI], B[n].s_out_j0, B[n].s_out_j1);

			if (!B[n].s_memory) {	/* Free grid unless it is a memory grid */
				gmtlib_close_grd (GMT, B[n].s_G);	/* Close the grid file so we don't have lots of them open */
				if (GMT_Destroy_Data (GMT->parent, &B[n].s_G)) {
					B[n].s_ignore = true;
					B[n].secondary = false;
					gmt_M_str_free (L[n].secondary); 
					continue;
				} 
			}

			/* GET WINDOW FUNCTION PARAMETERS FOR PRIMARY GRID
			if things are good with the secondary grid, now we can populate the blending  
			parameters */

			/* First initialize */
			B[n].v_data = calloc(1, sizeof(window));
			if (B[n].v_data == NULL) return (-GMT_MEMORY_ERROR);
			B[n].support_i0 = L[n].have_polygon
			                  ? irint((B[n].wesn[XLO] - h->wesn[XLO]) * HH->r_inc[GMT_X]) - 1
			                  : B[n].out_i0;
			B[n].support_i1 = L[n].have_polygon
			                  ? irint((B[n].wesn[XHI] - h->wesn[XLO]) * HH->r_inc[GMT_X]) + one_or_zero
			                  : B[n].out_i1;
			B[n].support_j0 = L[n].have_polygon
			                  ? irint((h->wesn[YHI] - B[n].wesn[YHI]) * HH->r_inc[GMT_Y]) - 1
			                  : B[n].out_j0;
			B[n].support_j1 = L[n].have_polygon
			                  ? irint((h->wesn[YHI] - B[n].wesn[YLO]) * HH->r_inc[GMT_Y]) + one_or_zero
			                  : B[n].out_j1;
			B[n].v_data->nx = B[n].support_i1 - B[n].support_i0 + 1;
			B[n].v_data->ny = B[n].support_j1 - B[n].support_j0 + 1;

			/* (2) window functions */
			/* set the window functions in the x and y directions */
			{
				char x_name[GMT_LEN64] = {""}, y_name[GMT_LEN64] = {""};
				if (sscanf(L[n].functions, "%63[^/]/%63s", x_name, y_name) != 2 ||
				    blend_window_function_from_name(x_name, &B[n].v_data->x_function) != SUCCESS ||
				    blend_window_function_from_name(y_name, &B[n].v_data->y_function) != SUCCESS) {
					GMT_Report(GMT->parent, GMT_MSG_ERROR,
					           "Invalid window-function pair %s for %s\n", L[n].functions, L[n].file);
					return (-GMT_PARSE_ERROR);
				}
			}

			/* (3) taper ratios */
			B[n].v_data->ratio_x1 = L[n].taper_ratio[0];
			B[n].v_data->ratio_x2 = L[n].taper_ratio[1];
			B[n].v_data->ratio_y1 = L[n].taper_ratio[2];
			B[n].v_data->ratio_y2 = L[n].taper_ratio[3];

			/* (4) validate, optionally convert, and assemble the BLEND support. */
			if (merge2d_prepare_support(GMT, L[n].have_polygon ? L[n].polygon : NULL,
			                            B[n].wesn, B[n].v_data->nx, B[n].v_data->ny,
			                            Ctrl, B[n].v_data, &B[n].v_support) != GMT_NOERROR)
				return (-GMT_RUNTIME_ERROR);
		}
	}

	if (Ctrl->A.active && merge2d_validate_overlap_secondaries(GMT, B, n_files) != GMT_NOERROR)
		return (-GMT_RUNTIME_ERROR);

	merge2d_specs_free(GMT, L, n_files);
	*merge = B;
	return n_files;
}

static int merge2d_sync_primary_rows (struct GMT_CTRL *GMT, int row, struct MERGE2D_PAIR *B, unsigned int n_merge) {
	unsigned int k;
	int G_row;
	uint64_t col, node;

	for (k = 0; k < n_merge; k++) {	/* Get every input grid ready for the new row */
		if (B[k].ignore) continue;	/* This grid is not even inside our area */
		if (row < B[k].out_j0 || row > B[k].out_j1) {	/* Either done with grid or haven't gotten to this range yet */
			B[k].outside = true;
			if (B[k].open) {	/* If an open file then we wipe */
				gmtlib_close_grd (GMT, B[k].G);	/* Close the grid file */
				if (GMT_Destroy_Data (GMT->parent, &B[k].G)) return GMT_RUNTIME_ERROR;
				B[k].open = false;
				gmt_M_free (GMT, B[k].z);
				gmt_M_free (GMT, B[k].RbR);
				if (B[k].delete && gmt_remove_file (GMT, B[k].file))	/* Delete the temporary resampled file, but it failed */
					GMT_Report (GMT->parent, GMT_MSG_ERROR, "Failed to delete file %s\n", B[k].file);
			}
			continue;
		}
		B[k].outside = false;		/* Here we know the row is inside this grid */

		if (B[k].memory) {	/* Grid already in memory, just copy the relevant row - no reading needed */
			G_row = row - B[k].out_j0;	/* The corresponding row number in the k'th grid */
			node = gmt_M_ijp (B[k].G->header, G_row, 0);	/* Start of our row at col = 0 */
			gmt_M_memcpy (B[k].z, &B[k].G->data[node], B[k].G->header->n_columns, gmt_grdfloat);	/* Copy that row */
		}
		else {	/* Deal with files that may need to be opened the first time we access it */
			if (!B[k].open) {
				struct GMT_GRID_HIDDEN *GH = NULL;
				if ((B[k].G = GMT_Read_Data (GMT->parent, GMT_IS_GRID, GMT_IS_FILE, GMT_IS_SURFACE, GMT_CONTAINER_ONLY|GMT_GRID_ROW_BY_ROW, NULL, B[k].file, NULL)) == NULL) {
					return GMT_GRID_READ_ERROR;
				}
				GH = gmt_get_G_hidden (B[k].G);
				gmt_M_memcpy (B[k].RbR, GH->extra, 1, struct GMT_GRID_ROWBYROW);	/* Duplicate, since GMT_Destroy_Data will free the header->extra */
				B[k].RbR->start[0] += B[k].offset;
				gmt_M_memcpy (GH->extra, B[k].RbR, 1, struct GMT_GRID_ROWBYROW);
				B[k].open = true;
			}
			GMT_Get_Row (GMT->parent, 0, B[k].G, B[k].z);	/* Get one row from this file */
		}
		if (GMT->common.d.active[GMT_IN])
			for (col = 0; col < B[k].G->header->n_columns; col++)
				if (B[k].z[col] == (gmt_grdfloat)GMT->common.d.nan_proxy[GMT_IN])
					B[k].z[col] = GMT->session.f_NaN;
	}
	return GMT_NOERROR;
}

static int merge2d_sync_secondary_rows (struct GMT_CTRL *GMT, int row, struct MERGE2D_PAIR *B, unsigned int n_merge) {
	unsigned int k;
	int s_G_row;
	uint64_t col, s_node;

	for (k = 0; k < n_merge; k++) {	/* Get every input grid ready for the new row */

		if (B[k].secondary) {
			if (B[k].s_ignore) {
				B[k].secondary = false;
				continue;
			}
			if (row < B[k].s_out_j0 || row > B[k].s_out_j1) {	/* Either done with grid or haven't gotten to this range yet */
				B[k].s_outside = true;
				if (B[k].s_open) {	/* If an open file then we wipe */
					gmtlib_close_grd (GMT, B[k].s_G);	/* Close the grid file */
					if (GMT_Destroy_Data (GMT->parent, &B[k].s_G)) return GMT_RUNTIME_ERROR;
					B[k].s_open = false;
					gmt_M_free (GMT, B[k].s_z);
					gmt_M_free (GMT, B[k].s_RbR);
					if (B[k].s_delete && gmt_remove_file (GMT, B[k].s_file))	/* Delete the temporary resampled file, but it failed */
						GMT_Report (GMT->parent, GMT_MSG_ERROR, "[Secondary Grid] Failed to delete file %s\n", B[k].s_file);
				}
				continue;
			}

			B[k].s_outside = false;		/* Here we know the row is inside this grid */

			if (B[k].s_memory) {	/* Grid already in memory, just copy the relevant row - no reading needed */
				s_G_row = row - B[k].s_out_j0;	/* The corresponding row number in the k'th grid */
				s_node = gmt_M_ijp (B[k].s_G->header, s_G_row, 0);	/* Start of our row at col = 0 */
				gmt_M_memcpy (B[k].s_z, &B[k].s_G->data[s_node], B[k].s_G->header->n_columns, gmt_grdfloat);	/* Copy that row */
			}
			else {	/* Deal with files that may need to be opened the first time we access it */
				if (!B[k].s_open) {
					struct GMT_GRID_HIDDEN *s_GH = NULL;
					if ((B[k].s_G = GMT_Read_Data (GMT->parent, GMT_IS_GRID, GMT_IS_FILE, GMT_IS_SURFACE, GMT_CONTAINER_ONLY|GMT_GRID_ROW_BY_ROW, NULL, B[k].s_file, NULL)) == NULL) {
						B[k].secondary = false;
						continue;
					}
					s_GH = gmt_get_G_hidden (B[k].s_G);
					gmt_M_memcpy (B[k].s_RbR, s_GH->extra, 1, struct GMT_GRID_ROWBYROW);	/* Duplicate, since GMT_Destroy_Data will free the header->extra */
					B[k].s_RbR->start[0] += B[k].s_offset;
					gmt_M_memcpy (s_GH->extra, B[k].s_RbR, 1, struct GMT_GRID_ROWBYROW);
					B[k].s_open = true;
				}
				GMT_Get_Row (GMT->parent, 0, B[k].s_G, B[k].s_z);	 /*Get one row from this file */

			}
			if (GMT->common.d.active[GMT_IN])
				for (col = 0; col < B[k].s_G->header->n_columns; col++)
					if (B[k].s_z[col] == (gmt_grdfloat)GMT->common.d.nan_proxy[GMT_IN])
						B[k].s_z[col] = GMT->session.f_NaN;

		}

	}
	return GMT_NOERROR;
}

static bool merge2d_got_plot_domain (struct GMTAPI_CTRL *API, const char *file, char *code) {
	/* If given an =tiled_<ID>_[P|G][L|O|X].xxxxxx list then we return true if the region given when the list was assembled
	 * was a plot domain (give to a PS producer) or a grid domain (given to a grid producer). */
	char region;
	*code = 0;
	if (!gmt_file_is_tiled_list (API, file, NULL, code, &region)) return false;	/* Not a valid tiled list file */

	if (region == 'P') {
		GMT_Report (API, GMT_MSG_DEBUG, "Got tiled list determined from a plot region\n");
		return true;
	}
	return false;
}

static void merge2d_fields_free (struct MERGE2D_FIELDS *fields) {
	size_t k;
	if (fields == NULL) return;
	for (k = 0; k < fields->count; k++) free(fields->names[k]);
	free(fields->names);
	memset(fields, 0, sizeof(*fields));
}

static int merge2d_fields_parse (struct GMTAPI_CTRL *API, const char *text,
                                    struct MERGE2D_FIELDS *fields) {
	char *copy = NULL, *token = NULL, *save = NULL;
	size_t capacity = 0, k;
	if (text == NULL || !text[0]) return GMT_PARSE_ERROR;
	copy = strdup(text);
	if (copy == NULL) return GMT_MEMORY_ERROR;
	for (token = strtok_r(copy, ",", &save); token; token = strtok_r(NULL, ",", &save)) {
		if (!token[0] || !strcmp(token, "weight")) {
			GMT_Report(API, GMT_MSG_ERROR, "Invalid or reserved output field name: %s\n", token);
			free(copy);
			merge2d_fields_free(fields);
			return GMT_PARSE_ERROR;
		}
		for (k = 0; k < fields->count; k++) {
			if (!strcmp(fields->names[k], token)) {
				GMT_Report(API, GMT_MSG_ERROR, "Duplicate output field name: %s\n", token);
				free(copy);
				merge2d_fields_free(fields);
				return GMT_PARSE_ERROR;
			}
		}
		if (fields->count == capacity) {
			char **names;
			capacity = capacity ? capacity * 2 : 4;
			names = realloc(fields->names, capacity * sizeof(*names));
			if (names == NULL) {
				free(copy);
				merge2d_fields_free(fields);
				return GMT_MEMORY_ERROR;
			}
			fields->names = names;
		}
		fields->names[fields->count] = strdup(token);
		if (fields->names[fields->count] == NULL) {
			free(copy);
			merge2d_fields_free(fields);
			return GMT_MEMORY_ERROR;
		}
		fields->count++;
	}
	free(copy);
	return fields->count ? GMT_NOERROR : GMT_PARSE_ERROR;
}

static size_t merge2d_selector_count (const char *source, char first[NC_MAX_NAME + 1]) {
	const char *question = source ? strchr(source, '?') : NULL;
	const char *end, *p;
	size_t count = 0, length;
	if (first) first[0] = '\0';
	if (question == NULL) return 0;
	end = strchr(question + 1, '+');
	if (end == NULL) end = source + strlen(source);
	if (end == question + 1) return 0;
	p = question + 1;
	while (p < end) {
		const char *comma = memchr(p, ',', (size_t)(end - p));
		const char *stop = comma ? comma : end;
		if (stop == p) return 0;
		if (count == 0 && first) {
			const char *slice = memchr(p, '[', (size_t)(stop - p));
			const char *level = memchr(p, '(', (size_t)(stop - p));
			const char *name_end = stop;
			if (slice && slice < name_end) name_end = slice;
			if (level && level < name_end) name_end = level;
			length = MIN((size_t)(name_end - p), (size_t)NC_MAX_NAME);
			memcpy(first, p, length);
			first[length] = '\0';
		}
		count++;
		p = comma ? comma + 1 : end;
	}
	return count;
}

static int merge2d_select_source (struct GMTAPI_CTRL *API, const char *source,
                                     size_t field, size_t n_fields,
                                     char output[PATH_MAX]) {
	const char *question, *mods, *vars_end, *start, *stop;
	size_t count, k, prefix_length, var_length;
	char first[NC_MAX_NAME + 1] = {""};
	char modifiers[GMT_LEN512] = {""}, message[GMT_LEN256];
	struct GQ_TRANSFORM transform;
	bool has_missing = false;
	double missing = 0.0;
	if (source == NULL || !source[0] || strlen(source) >= PATH_MAX) return GMT_PARSE_ERROR;
	if (!strcmp(source, "-")) {
		strcpy(output, source);
		return GMT_NOERROR;
	}
	count = merge2d_selector_count(source, first);
	if (count == 0) {
		if (n_fields != 1) {
			GMT_Report(API, GMT_MSG_ERROR,
			           "Input %s must list %zu variables after '?' to match -F\n", source, n_fields);
			return GMT_PARSE_ERROR;
		}
		strcpy(output, source);
		return GMT_NOERROR;
	}
	if (count != n_fields) {
		GMT_Report(API, GMT_MSG_ERROR,
		           "Input %s lists %zu variables but %zu output fields were requested\n",
		           source, count, n_fields);
		return GMT_PARSE_ERROR;
	}
	question = strchr(source, '?');
	mods = strchr(question + 1, '+');
	vars_end = mods ? mods : source + strlen(source);
	start = question + 1;
	for (k = 0; k < field; k++) {
		start = memchr(start, ',', (size_t)(vars_end - start));
		if (start == NULL) return GMT_PARSE_ERROR;
		start++;
	}
	stop = memchr(start, ',', (size_t)(vars_end - start));
	if (stop == NULL) stop = vars_end;
	prefix_length = (size_t)(question - source);
	var_length = (size_t)(stop - start);
	if (prefix_length + 1 + var_length + 1 > PATH_MAX)
		return GMT_PARSE_ERROR;
	memcpy(output, source, prefix_length);
	output[prefix_length] = '?';
	memcpy(output + prefix_length + 1, start, var_length);
	output[prefix_length + 1 + var_length] = '\0';
	if (mods) {
		gq_transform_init(&transform);
		if (gq_transform_parse(mods,
		                       GQ_TRANSFORM_X_MASK | GQ_TRANSFORM_Y_MASK |
		                       GQ_TRANSFORM_Z_MASK,
		                       true, &transform, &has_missing, &missing,
		                       message, sizeof(message)) ||
		    gq_transform_format(&transform, has_missing, missing,
		                         field, n_fields, true,
		                         modifiers, sizeof(modifiers))) {
			GMT_Report(API, GMT_MSG_ERROR, "Invalid transform in %s: %s\n",
			           source, message);
			gq_transform_free(&transform);
			return GMT_PARSE_ERROR;
		}
		gq_transform_free(&transform);
		if (strlen(output) + strlen(modifiers) + 1 > PATH_MAX)
			return GMT_DIM_TOO_SMALL;
		strcat(output, modifiers);
	}
	return GMT_NOERROR;
}

static int merge2d_first_primary (const struct MERGE2D_CTRL *Ctrl, char source[PATH_MAX]) {
	FILE *fp;
	char line[GMT_BUFSIZ];
	if (Ctrl->In.n > 1) {
		strncpy(source, Ctrl->In.file[0], PATH_MAX - 1);
		return GMT_NOERROR;
	}
	if (Ctrl->In.n != 1 || !strcmp(Ctrl->In.file[0], "-")) return GMT_PARSE_ERROR;
	fp = fopen(Ctrl->In.file[0], "r");
	if (fp == NULL) return GMT_DATA_READ_ERROR;
	while (fgets(line, sizeof(line), fp)) {
		char *save = NULL, *token;
		for (token = line; *token && isspace((unsigned char)*token); token++);
		if (!*token || *token == '#') continue;
		token = strtok_r(token, " \t\r\n", &save);
		if (token) {
			strncpy(source, token, PATH_MAX - 1);
			fclose(fp);
			return GMT_NOERROR;
		}
	}
	fclose(fp);
	return GMT_DATA_READ_ERROR;
}

static int merge2d_determine_fields (struct GMTAPI_CTRL *API,
                                        const struct MERGE2D_CTRL *Ctrl,
                                        struct MERGE2D_FIELDS *fields) {
	char source[PATH_MAX] = {""}, first[NC_MAX_NAME + 1] = {""};
	size_t selectors;
	if (Ctrl->F.active)
		return merge2d_fields_parse(API, Ctrl->F.fields, fields);
	if (merge2d_first_primary(Ctrl, source) != GMT_NOERROR) {
		fields->names = calloc(1, sizeof(*fields->names));
		if (fields->names == NULL) return GMT_MEMORY_ERROR;
		fields->names[0] = strdup("z");
		fields->count = 1;
		return fields->names[0] ? GMT_NOERROR : GMT_MEMORY_ERROR;
	}
	selectors = merge2d_selector_count(source, first);
	if (selectors > 1) {
		if (Ctrl->W.only) {
			fields->names = calloc(selectors, sizeof(*fields->names));
			if (fields->names == NULL) return GMT_MEMORY_ERROR;
			fields->count = selectors;
			return GMT_NOERROR;
		}
		GMT_Report(API, GMT_MSG_ERROR,
		           "Multiple input variables require -F to name the output fields\n");
		return GMT_PARSE_ERROR;
	}
	fields->names = calloc(1, sizeof(*fields->names));
	if (fields->names == NULL) return GMT_MEMORY_ERROR;
	fields->names[0] = strdup(selectors == 1 ? first : "z");
	fields->count = 1;
	return fields->names[0] ? GMT_NOERROR : GMT_MEMORY_ERROR;
}

static int merge2d_write_field_mergefile (struct GMTAPI_CTRL *API, const char *input,
                                              const char *output, size_t field,
                                              size_t n_fields) {
	FILE *in = NULL, *out = NULL;
	char line[GMT_BUFSIZ];
	int status = GMT_DATA_READ_ERROR;
	in = fopen(input, "r");
	if (in == NULL) goto cleanup;
	out = fopen(output, "w");
	if (out == NULL) goto cleanup;
	while (fgets(line, sizeof(line), in)) {
		char copy[GMT_BUFSIZ], *tokens[5] = {NULL}, *save = NULL, *token;
		char primary[PATH_MAX] = {""}, secondary[PATH_MAX] = {""};
		size_t n = 0, k;
		strncpy(copy, line, sizeof(copy) - 1);
		for (token = strtok_r(copy, " \t\r\n", &save); token && n < 5;
		     token = strtok_r(NULL, " \t\r\n", &save)) {
			if (token[0] == '#') break;
			tokens[n++] = token;
		}
		if (n == 0) continue;
		if (merge2d_select_source(API, tokens[0], field, n_fields, primary) != GMT_NOERROR)
			goto cleanup;
		if (n > 1 && merge2d_select_source(API, tokens[1], field, n_fields, secondary) != GMT_NOERROR)
			goto cleanup;
		if (fprintf(out, "%s", primary) < 0) goto cleanup;
		if (n > 1 && fprintf(out, " %s", secondary) < 0) goto cleanup;
		for (k = 2; k < n; k++) if (fprintf(out, " %s", tokens[k]) < 0) goto cleanup;
		if (fputc('\n', out) == EOF) goto cleanup;
	}
	status = ferror(in) ? GMT_DATA_READ_ERROR : GMT_NOERROR;

cleanup:
	if (in) fclose(in);
	if (out && fclose(out) != 0) status = GMT_DATA_WRITE_ERROR;
	return status;
}

static int merge2d_command_append (char **command, size_t *length, size_t *capacity,
                                      const char *text) {
	size_t add = strlen(text);
	if (*length + add + 2 > *capacity) {
		char *next;
		while (*length + add + 2 > *capacity) *capacity = *capacity ? *capacity * 2 : 1024;
		next = realloc(*command, *capacity);
		if (next == NULL) return GMT_MEMORY_ERROR;
		*command = next;
	}
	if (*length) (*command)[(*length)++] = ' ';
	memcpy(*command + *length, text, add);
	*length += add;
	(*command)[*length] = '\0';
	return GMT_NOERROR;
}

static int merge2d_build_single_command (struct GMTAPI_CTRL *API,
                                             struct GMT_OPTION *options,
                                             const struct MERGE2D_CTRL *Ctrl,
                                             const char *input,
                                             size_t field, size_t n_fields,
                                             const char *output, int weights,
                                             char **command) {
	struct GMT_OPTION *opt;
	size_t length = 0, capacity = 0, k;
	char item[PATH_MAX + GMT_LEN256];
	if (Ctrl->In.n > 1) {
		for (k = 0; k < Ctrl->In.n; k++) {
			if (merge2d_select_source(API, Ctrl->In.file[k], field, n_fields, item) != GMT_NOERROR ||
			    merge2d_command_append(command, &length, &capacity, item) != GMT_NOERROR)
				return GMT_PARSE_ERROR;
		}
	}
	else if (merge2d_command_append(command, &length, &capacity, input) != GMT_NOERROR)
		return GMT_MEMORY_ERROR;
	for (opt = options; opt; opt = opt->next) {
		if (opt->option == '<' || opt->option == 'F' || opt->option == 'G' ||
		    opt->option == 'W' || opt->option == 'E' ||
		    opt->option == 'Z')
			continue;
		if (!isprint((unsigned char)opt->option)) continue;
		snprintf(item, sizeof(item), "-%c%s", opt->option, opt->arg ? opt->arg : "");
		if (merge2d_command_append(command, &length, &capacity, item) != GMT_NOERROR)
			return GMT_MEMORY_ERROR;
	}
	if (Ctrl->Z.active) {
		char modifiers[GMT_LEN512] = {""};
		if (gq_transform_format(&Ctrl->Z.transform, false, 0.0,
		                         field, n_fields, !weights,
		                         modifiers, sizeof(modifiers)))
			return GMT_PARSE_ERROR;
		if (modifiers[0]) {
			snprintf(item, sizeof(item), "-Z%s", modifiers);
			if (merge2d_command_append(command, &length, &capacity, item) != GMT_NOERROR)
				return GMT_MEMORY_ERROR;
		}
	}
	snprintf(item, sizeof(item), "-G%s", output);
	if (merge2d_command_append(command, &length, &capacity, item) != GMT_NOERROR ||
	    merge2d_command_append(command, &length, &capacity, "-E") != GMT_NOERROR)
		return GMT_MEMORY_ERROR;
	if (weights && merge2d_command_append(command, &length, &capacity, "-W+o") != GMT_NOERROR)
		return GMT_MEMORY_ERROR;
	return GMT_NOERROR;
}

static int merge2d_netcdf_find_grid (int ncid, int *varid, int dims[2], size_t sizes[2]) {
	int nvars, k, ndims;
	if (nc_inq_nvars(ncid, &nvars) != NC_NOERR) return GMT_RUNTIME_ERROR;
	for (k = 0; k < nvars; k++) {
		if (nc_inq_varndims(ncid, k, &ndims) != NC_NOERR || ndims != 2) continue;
		if (nc_inq_vardimid(ncid, k, dims) != NC_NOERR ||
		    nc_inq_dimlen(ncid, dims[0], &sizes[0]) != NC_NOERR ||
		    nc_inq_dimlen(ncid, dims[1], &sizes[1]) != NC_NOERR)
			return GMT_RUNTIME_ERROR;
		*varid = k;
		return GMT_NOERROR;
	}
	return GMT_RUNTIME_ERROR;
}

static int merge2d_netcdf_copy_attribute (int input, int input_var,
                                              int output, int output_var,
                                              const char *name) {
	if (nc_inq_att(input, input_var, name, NULL, NULL) != NC_NOERR) return GMT_NOERROR;
	return nc_copy_att(input, input_var, name, output, output_var) == NC_NOERR
	       ? GMT_NOERROR : GMT_RUNTIME_ERROR;
}

static int merge2d_package_netcdf (struct GMTAPI_CTRL *API, const char *output,
                                      char **data_files, const struct MERGE2D_FIELDS *fields,
                                      const char *weight_file, int include_data,
                                      int include_weight) {
	int input = -1, result = -1, source_var, source_dims[2], output_dims[2];
	int coord_vars[2] = {-1, -1}, output_coord_vars[2] = {-1, -1};
	int *output_vars = NULL, weight_var = -1, status = GMT_RUNTIME_ERROR;
	size_t sizes[2], chunks[2], k, row;
	float *buffer = NULL;
	char dim_names[2][NC_MAX_NAME + 1];
	if (nc_open(include_data ? data_files[0] : weight_file, NC_NOWRITE, &input) != NC_NOERR ||
	    merge2d_netcdf_find_grid(input, &source_var, source_dims, sizes) != GMT_NOERROR ||
	    nc_inq_dimname(input, source_dims[0], dim_names[0]) != NC_NOERR ||
	    nc_inq_dimname(input, source_dims[1], dim_names[1]) != NC_NOERR ||
	    nc_create(output, NC_NETCDF4 | NC_CLOBBER, &result) != NC_NOERR)
		goto cleanup;
	{
		int natts = 0, att;
		if (nc_inq_natts(input, &natts) != NC_NOERR) goto cleanup;
		for (att = 0; att < natts; att++) {
			char name[NC_MAX_NAME + 1];
			if (nc_inq_attname(input, NC_GLOBAL, att, name) != NC_NOERR ||
			    nc_copy_att(input, NC_GLOBAL, name, result, NC_GLOBAL) != NC_NOERR)
				goto cleanup;
		}
		if (nc_put_att_text(result, NC_GLOBAL, "history",
		                    strlen("Created by GMT merge2d"),
		                    "Created by GMT merge2d") != NC_NOERR)
			goto cleanup;
	}
	for (k = 0; k < 2; k++) {
		nc_type type = NC_DOUBLE;
		int dim;
		if (nc_def_dim(result, dim_names[k], sizes[k], &output_dims[k]) != NC_NOERR) goto cleanup;
		if (nc_inq_varid(input, dim_names[k], &coord_vars[k]) == NC_NOERR)
			nc_inq_vartype(input, coord_vars[k], &type);
		dim = output_dims[k];
		if (nc_def_var(result, dim_names[k], type, 1, &dim, &output_coord_vars[k]) != NC_NOERR)
			goto cleanup;
		if (coord_vars[k] >= 0) {
			if (merge2d_netcdf_copy_attribute(input, coord_vars[k], result, output_coord_vars[k],
			                                  "long_name") != GMT_NOERROR ||
			    merge2d_netcdf_copy_attribute(input, coord_vars[k], result, output_coord_vars[k],
			                                  "units") != GMT_NOERROR ||
			    merge2d_netcdf_copy_attribute(input, coord_vars[k], result, output_coord_vars[k],
			                                  "actual_range") != GMT_NOERROR)
				goto cleanup;
		}
	}
	chunks[0] = sizes[0] < 64 ? sizes[0] : 64;
	chunks[1] = sizes[1] < 1024 ? sizes[1] : 1024;
	if (include_data) {
		output_vars = calloc(fields->count, sizeof(*output_vars));
		if (output_vars == NULL) goto cleanup;
		for (k = 0; k < fields->count; k++) {
			float fill = NAN;
			if (nc_def_var(result, fields->names[k], NC_FLOAT, 2, output_dims, &output_vars[k]) != NC_NOERR ||
			    nc_put_att_float(result, output_vars[k], "_FillValue", NC_FLOAT, 1, &fill) != NC_NOERR)
				goto cleanup;
			{
				int metadata_input = -1, metadata_var, metadata_dims[2];
				size_t metadata_sizes[2];
				if (nc_open(data_files[k], NC_NOWRITE, &metadata_input) != NC_NOERR ||
				    merge2d_netcdf_find_grid(metadata_input, &metadata_var,
				                             metadata_dims, metadata_sizes) != GMT_NOERROR) {
					if (metadata_input >= 0) nc_close(metadata_input);
					goto cleanup;
				}
				if (merge2d_netcdf_copy_attribute(metadata_input, metadata_var, result,
				                                  output_vars[k], "units") != GMT_NOERROR) {
					nc_close(metadata_input);
					goto cleanup;
				}
				if (nc_put_att_text(result, output_vars[k], "long_name",
				                    strlen(fields->names[k]), fields->names[k]) != NC_NOERR) {
					nc_close(metadata_input);
					goto cleanup;
				}
				nc_close(metadata_input);
			}
			if (nc_def_var_chunking(result, output_vars[k], NC_CHUNKED, chunks) != NC_NOERR ||
			    nc_def_var_deflate(result, output_vars[k], 0, 1, 2) != NC_NOERR)
				goto cleanup;
		}
	}
	if (include_weight) {
		float fill = NAN, valid[2] = {0.0f, 1.0f};
		if (nc_def_var(result, "weight", NC_FLOAT, 2, output_dims, &weight_var) != NC_NOERR ||
		    nc_put_att_float(result, weight_var, "_FillValue", NC_FLOAT, 1, &fill) != NC_NOERR ||
		    nc_put_att_text(result, weight_var, "long_name", strlen("merging weight"),
		                    "merging weight") != NC_NOERR ||
		    nc_put_att_text(result, weight_var, "units", 1, "1") != NC_NOERR ||
		    nc_put_att_float(result, weight_var, "valid_range", NC_FLOAT, 2, valid) != NC_NOERR)
			goto cleanup;
		if (nc_def_var_chunking(result, weight_var, NC_CHUNKED, chunks) != NC_NOERR ||
		    nc_def_var_deflate(result, weight_var, 0, 1, 2) != NC_NOERR)
			goto cleanup;
	}
	if (nc_enddef(result) != NC_NOERR) goto cleanup;
	for (k = 0; k < 2; k++) {
		double *coordinates = calloc(sizes[k], sizeof(*coordinates));
		if (coordinates == NULL) goto cleanup;
		if (coord_vars[k] >= 0) {
			if (nc_get_var_double(input, coord_vars[k], coordinates) != NC_NOERR ||
			    nc_put_var_double(result, output_coord_vars[k], coordinates) != NC_NOERR) {
				free(coordinates);
				goto cleanup;
			}
		}
		else {
			size_t j;
			for (j = 0; j < sizes[k]; j++) coordinates[j] = (double)j;
			if (nc_put_var_double(result, output_coord_vars[k], coordinates) != NC_NOERR) {
				free(coordinates);
				goto cleanup;
			}
		}
		free(coordinates);
	}
	buffer = calloc(chunks[0] * sizes[1], sizeof(*buffer));
	if (buffer == NULL) goto cleanup;
	if (include_data) {
		for (k = 0; k < fields->count; k++) {
			int field_input = -1, field_var, field_dims[2];
			size_t field_sizes[2], start[2] = {0, 0}, count[2] = {0, sizes[1]};
			if (nc_open(data_files[k], NC_NOWRITE, &field_input) != NC_NOERR ||
			    merge2d_netcdf_find_grid(field_input, &field_var, field_dims, field_sizes) != GMT_NOERROR ||
			    field_sizes[0] != sizes[0] || field_sizes[1] != sizes[1]) {
				if (field_input >= 0) nc_close(field_input);
				goto cleanup;
			}
			for (row = 0; row < sizes[0]; row += chunks[0]) {
				start[0] = row;
				count[0] = sizes[0] - row < chunks[0] ? sizes[0] - row : chunks[0];
				if (nc_get_vara_float(field_input, field_var, start, count, buffer) != NC_NOERR ||
				    nc_put_vara_float(result, output_vars[k], start, count, buffer) != NC_NOERR) {
					nc_close(field_input);
					goto cleanup;
				}
			}
			nc_close(field_input);
		}
	}
	if (include_weight) {
		int weight_input = -1, input_weight_var, weight_dims[2];
		size_t weight_sizes[2], start[2] = {0, 0}, count[2] = {0, sizes[1]};
		if (nc_open(weight_file, NC_NOWRITE, &weight_input) != NC_NOERR ||
		    merge2d_netcdf_find_grid(weight_input, &input_weight_var, weight_dims, weight_sizes) != GMT_NOERROR ||
		    weight_sizes[0] != sizes[0] || weight_sizes[1] != sizes[1]) {
			if (weight_input >= 0) nc_close(weight_input);
			goto cleanup;
		}
		for (row = 0; row < sizes[0]; row += chunks[0]) {
			start[0] = row;
			count[0] = sizes[0] - row < chunks[0] ? sizes[0] - row : chunks[0];
			if (nc_get_vara_float(weight_input, input_weight_var, start, count, buffer) != NC_NOERR ||
			    nc_put_vara_float(result, weight_var, start, count, buffer) != NC_NOERR) {
				nc_close(weight_input);
				goto cleanup;
			}
		}
		nc_close(weight_input);
	}
	status = GMT_NOERROR;

cleanup:
	if (input >= 0) nc_close(input);
	if (result >= 0 && nc_close(result) != NC_NOERR) status = GMT_RUNTIME_ERROR;
	free(output_vars);
	free(buffer);
	if (status != GMT_NOERROR)
		GMT_Report(API, GMT_MSG_ERROR, "Unable to assemble multiparameter netCDF output %s\n", output);
	return status;
}

EXTERN_MSC int GMT_merge2d (void *V_API, int mode, void *args);

static int merge2d_execute_internal (const char *command) {
	void *session = GMT_Create_Session("merge2d-field", 2U, 0U, NULL);
	int status;
	if (session == NULL) return GMT_RUNTIME_ERROR;
	status = GMT_merge2d(session, GMT_MODULE_CMD, (void *)command);
	GMT_Destroy_Session(session);
	return status;
}

static int merge2d_run_multiparameter (struct GMT_CTRL *GMT,
                                          struct GMT_OPTION *options,
                                          struct MERGE2D_CTRL *Ctrl) {
	struct MERGE2D_FIELDS fields = {0};
	char **data_files = NULL, **merge_files = NULL;
	char weight_file[PATH_MAX] = {""};
	size_t k, runs = 0;
	int include_data = !Ctrl->W.only, include_weight = Ctrl->W.active;
	int status = GMT_RUNTIME_ERROR;
	if (merge2d_determine_fields(GMT->parent, Ctrl, &fields) != GMT_NOERROR)
		goto cleanup;
	{
		char message[GMT_LEN256];
		if (gq_transform_validate_values(&Ctrl->Z.transform, fields.count,
		                                  message, sizeof(message))) {
			GMT_Report(GMT->parent, GMT_MSG_ERROR, "Option -Z: %s\n", message);
			status = GMT_PARSE_ERROR;
			goto cleanup;
		}
	}
	runs = include_data ? fields.count : 1;
	data_files = calloc(fields.count, sizeof(*data_files));
	merge_files = calloc(runs, sizeof(*merge_files));
	if (data_files == NULL || merge_files == NULL) goto cleanup;
	for (k = 0; k < runs; k++) {
		char command_input[PATH_MAX] = {""}, *command = NULL;
		merge_files[k] = calloc(PATH_MAX, 1);
		if (merge_files[k] == NULL ||
		    gmt_get_tempname(GMT->parent, "merge2d_fields", ".txt", merge_files[k]))
			goto cleanup;
		if (Ctrl->In.n == 1) {
			if (merge2d_write_field_mergefile(GMT->parent, Ctrl->In.file[0], merge_files[k],
			                                  k, fields.count) != GMT_NOERROR)
				goto cleanup;
			strncpy(command_input, merge_files[k], PATH_MAX - 1);
		}
		data_files[k] = calloc(PATH_MAX, 1);
		if (data_files[k] == NULL ||
		    gmt_get_tempname(GMT->parent, "merge2d_field", ".nc", data_files[k]))
			goto cleanup;
		if (merge2d_build_single_command(GMT->parent, options, Ctrl, command_input,
		                                 k, fields.count, data_files[k], 0,
		                                 &command) != GMT_NOERROR) {
			free(command);
			goto cleanup;
		}
		GMT_Report(GMT->parent, GMT_MSG_DEBUG, "Internal field command: %s\n", command);
		status = merge2d_execute_internal(command);
		free(command);
		if (status != GMT_NOERROR) goto cleanup;
	}
	if (include_weight) {
		char command_input[PATH_MAX] = {""}, *command = NULL;
		if (gmt_get_tempname(GMT->parent, "merge2d_weight", ".nc", weight_file))
			goto cleanup;
		if (Ctrl->In.n == 1) strncpy(command_input, merge_files[0], PATH_MAX - 1);
		if (merge2d_build_single_command(GMT->parent, options, Ctrl, command_input,
		                                 0, fields.count, weight_file, 1,
		                                 &command) != GMT_NOERROR) {
			free(command);
			goto cleanup;
		}
		GMT_Report(GMT->parent, GMT_MSG_DEBUG, "Internal weight command: %s\n", command);
		status = merge2d_execute_internal(command);
		free(command);
		if (status != GMT_NOERROR) goto cleanup;
	}
	status = merge2d_package_netcdf(GMT->parent, Ctrl->G.file, data_files, &fields,
	                                weight_file, include_data, include_weight);

cleanup:
	if (data_files) {
		for (k = 0; k < fields.count; k++) {
			if (data_files[k]) {
				if (data_files[k][0]) gmt_remove_file(GMT, data_files[k]);
				free(data_files[k]);
			}
		}
	}
	if (merge_files) {
		for (k = 0; k < runs; k++) {
			if (merge_files[k]) {
				if (merge_files[k][0]) gmt_remove_file(GMT, merge_files[k]);
				free(merge_files[k]);
			}
		}
	}
	if (weight_file[0]) gmt_remove_file(GMT, weight_file);
	free(data_files);
	free(merge_files);
	merge2d_fields_free(&fields);
	return status;
}

static int merge2d_run(struct GMT_CTRL *GMT, struct MERGE2D_CTRL *Ctrl,
                       struct GMT_OPTION *options)
{
	struct GMTAPI_CTRL *API = GMT->parent;
	struct MERGE2D_PAIR *merge = NULL;
	struct GMT_GRID *Grid = NULL;
	struct GMT_GRID_HEADER_HIDDEN *HH = NULL;
	struct GMT_GRID_HEADER *h_region = NULL;
	unsigned int col, row, nx_360 = 0, k, kk, s_kk, m, n_merge;
	unsigned int nx_final, ny_final, write_mode;
	int status, pcol, err, error;
	bool transform_output, wrap_x, write_all_at_once = false;
	bool first_grid, delayed = true, not_nan;
	uint64_t ij, n_fill, n_tot;
	double w;
	gmt_grdfloat *z = NULL, no_data_f;
	char zcase;
	char *outfile = NULL, outtemp[PATH_MAX];
	if ((error = merge2d_resolve_inputs(API, Ctrl)) != GMT_NOERROR)
		return (error);
	if (!Ctrl->X.active) {
		char first_source[PATH_MAX] = {""};
		int selected = merge2d_first_primary(Ctrl, first_source) == GMT_NOERROR &&
		               merge2d_selector_count(first_source, NULL) > 0;
		if (Ctrl->F.active || Ctrl->W.active || selected) {
			status = merge2d_run_multiparameter(GMT, options, Ctrl);
			return (status);
		}
	}
	{
		char message[GMT_LEN256];
		if (gq_transform_validate_values(&Ctrl->Z.transform, 1,
		                                  message, sizeof(message))) {
			GMT_Report(API, GMT_MSG_ERROR, "Option -Z: %s\n", message);
			return (GMT_PARSE_ERROR);
		}
	}

	gmt_grd_set_datapadding (GMT, true);	/* Turn on gridpadding when reading a subset */

	if (Ctrl->In.n <= 1) {	/* Got a merge file (or stdin) */
		if (Ctrl->In.n) {	/* Check if user mistakenly gave a single grid as input */
			int test_ncid = -1;
			if (nc_open(Ctrl->In.file[0], NC_NOWRITE, &test_ncid) == NC_NOERR) {
				nc_close(test_ncid);
				GMT_Report (API, GMT_MSG_ERROR, "Only a single grid found; no merging can take place\n");
				return (GMT_RUNTIME_ERROR);
			}
		}
		if (GMT_Init_IO (API, GMT_IS_DATASET, GMT_IS_TEXT, GMT_IN, GMT_ADD_DEFAULT, 0, options) != GMT_NOERROR) {	/* Register data input */
			return (API->error);
		}
		if (GMT_Begin_IO (API, GMT_IS_DATASET, GMT_IN, GMT_HEADER_ON) != GMT_NOERROR) {	/* Enables data input and sets access mode */
			return (API->error);
		}
	}

	if (GMT->common.R.active[RSET] && GMT->common.R.active[ISET]) {	/* Set output grid via -R -I [-r] */
		double *region = NULL, wesn[4];
		if (Ctrl->In.n == 1 && merge2d_got_plot_domain (API, Ctrl->In.file[0], &zcase)) {	/* Must adjust -R to be exact multiple of desired grid inc */
			double inc15[2] = {15.0 / 3600.0, 15.0/ 3600.0};
			double *inc = (zcase == 'O') ? inc15 : GMT->common.R.inc;	/* Pointer to the grid increment */
			gmt_increment_adjust (GMT, GMT->common.R.wesn, inc, GMT_GRID_DEFAULT_REG);	/* In case user specified incs using distance units we must call this here before adjusting wesn */
			wesn[XLO] = floor ((GMT->common.R.wesn[XLO] / inc[GMT_X]) + GMT_CONV8_LIMIT) * inc[GMT_X];
			wesn[XHI] = ceil  ((GMT->common.R.wesn[XHI] / inc[GMT_X]) - GMT_CONV8_LIMIT) * inc[GMT_X];
			wesn[YLO] = floor ((GMT->common.R.wesn[YLO] / inc[GMT_Y]) + GMT_CONV8_LIMIT) * inc[GMT_Y];
			wesn[YHI] = ceil  ((GMT->common.R.wesn[YHI] / inc[GMT_Y]) - GMT_CONV8_LIMIT) * inc[GMT_Y];
			region = wesn;	/* Use this (possibly slightly adjusted) region */
		}
		/* Here, region is either pointing to specific entries or is NULL, meaning we default to given -R */
		if ((Grid = GMT_Create_Data (API, GMT_IS_GRID, GMT_IS_SURFACE, GMT_CONTAINER_ONLY, NULL, region, NULL,
			GMT_GRID_DEFAULT_REG, GMT_NOTSET, NULL)) == NULL)
				return (API->error);
		h_region = Grid->header;
		delayed = false;	/* Was able to create the grid from command line options */
	}

	status = merge2d_prepare_job(GMT, Ctrl->In.file, Ctrl->In.n,
	                             &h_region, &merge, delayed, Grid, Ctrl);
	if (status < 0) return (-status);


	if (Ctrl->In.n <= 1 && GMT_End_IO (API, GMT_IN, 0) != GMT_NOERROR) {	/* Disables further data input */
		return (API->error);
	}


	n_merge = status;
	if (!Ctrl->W.active && n_merge == 1 && !merge[0].secondary) {
		GMT_Report (API, GMT_MSG_INFORMATION, "Only 1 grid found; no merging will take place\n");
	}


	if (delayed) {	/* Now we can create the output grid */
		if ((Grid = GMT_Create_Data (API, GMT_IS_GRID, GMT_IS_SURFACE, GMT_CONTAINER_ONLY, NULL, h_region->wesn, h_region->inc,
			h_region->registration, GMT_NOTSET, NULL)) == NULL)
				return (API->error);
		gmt_free_header (API->GMT, &h_region);
	}


	if ((err = gmt_grd_get_format (GMT, Ctrl->G.file, Grid->header, false)) != GMT_NOERROR) {
		GMT_Report (API, GMT_MSG_ERROR, "%s [%s]\n", GMT_strerror(err), Ctrl->G.file);
		return (GMT_RUNTIME_ERROR);
	}
	HH = gmt_get_H_hidden (Grid->header);

	GMT_Report (API, GMT_MSG_INFORMATION, "Processing input grids\n");


	if (merge2d_validate_grid_format(GMT, Grid->header, Ctrl->G.file) != GMT_NOERROR)
		return (GMT_RUNTIME_ERROR);
	transform_output = Ctrl->Z.active;

	n_fill = n_tot = 0;


	/* Process merge parameters and populate merge structure and open input files and seek to first row inside the output grid */

	no_data_f = GMT->session.f_NaN;

	/* Initialize header structure for output merge grid */

	n_tot = gmt_M_get_nm (GMT, Grid->header->n_columns, Grid->header->n_rows);

	z = gmt_M_memory (GMT, NULL, Grid->header->n_columns, gmt_grdfloat);	/* Memory for one output row */


	if (GMT_Set_Comment (API, GMT_IS_GRID, GMT_COMMENT_IS_OPTION | GMT_COMMENT_IS_COMMAND, options, Grid)) {
		gmt_M_free (GMT, z);
		return (API->error);
	}

	if (gmt_M_file_is_memory (Ctrl->G.file)) {	/* GMT_merge2d is called by another module; must return as GMT_GRID */
		/* Allocate space for the entire output grid */
		if (GMT_Create_Data (API, GMT_IS_GRID, GMT_IS_GRID, GMT_DATA_ONLY, NULL, NULL, NULL, 0, 0, Grid) == NULL) {
			gmt_M_free (GMT, z);
			return (API->error);
		}
		write_all_at_once = true;
	}
	else {
		unsigned int w_mode;
		if (transform_output) {	/* Write a temporary grid before applying output transforms. */
			if (gmt_get_tempname (GMT->parent, "merge2d_temp", ".nc", outtemp))
				return GMT_RUNTIME_ERROR;
			outfile = outtemp;
		}
		else
			outfile = Ctrl->G.file;
		w_mode = GMT_CONTAINER_ONLY | GMT_GRID_ROW_BY_ROW;
		if ((error = GMT_Write_Data (API, GMT_IS_GRID, GMT_IS_FILE, GMT_IS_SURFACE, w_mode, NULL, outfile, Grid))) {
			gmt_M_free (GMT, z);
			return (error);
		}
	}

	Grid->header->z_min = DBL_MAX;	Grid->header->z_max = -DBL_MAX;	/* These will be updated in the loop below */
	if (gmt_M_is_geographic (GMT, GMT_IN)) gmt_set_geographic (GMT, GMT_OUT);	/* Inherit the flavor of the input */
	wrap_x = (gmt_M_x_is_lon (GMT, GMT_OUT));	/* Periodic geographic grid */
	if (wrap_x) nx_360 = urint (360.0 * HH->r_inc[GMT_X]);

	for (row = 0; row < Grid->header->n_rows; row++) {	/* For every output row */

		gmt_M_memset (z, Grid->header->n_columns, gmt_grdfloat);	/* Start from scratch */

		status = merge2d_sync_primary_rows (GMT, row, merge, n_merge);	/* Wind each primary input file to current record and read each of the overlapping rows */
		if (status) {
			gmt_M_free (GMT, z);
			return (status);
		}

		status = merge2d_sync_secondary_rows (GMT, row, merge, n_merge);	/* Wind each secondary input file to current record and read each of the overlapping rows */
		if (status) {
			gmt_M_free (GMT, z);
			return (status);
		}

		for (col = 0; col < Grid->header->n_columns; col++) {	/* For each output node on the current row */

			w = 0.0;	/* Reset weight */
			first_grid = true;	/* Since some grids do not contain this (row,col) we want to know when we are processing the first grid inside */
			not_nan = false;	/* Will be true once the first grid that has a non-NaN node is encountered for this (row,col); */
			if (Ctrl->A.active && !Ctrl->C.active) {
				unsigned int owner = n_merge;
				bool any_finite_primary = false;
				double sum_geometry = 0.0, sum_valid = 0.0, sum_values = 0.0;
				double secondary_weight = 0.0, secondary_values = 0.0;
				double background = NAN, fallback_primary = NAN;
				m = 0;

				for (k = 0; k < n_merge; k++) {
					int local_col;
					if (merge[k].ignore || merge[k].outside) continue;
					if (wrap_x) {
						local_col = col + nx_360;
						while (local_col > merge[k].out_i1) local_col -= nx_360;
						if (local_col < merge[k].out_i0) continue;
					}
					else if ((int)col < merge[k].out_i0 || (int)col > merge[k].out_i1)
						continue;
					owner = k;
					break;
				}
				if (owner == n_merge) {
					m = 0;
					goto merge2d_node_ready;
				}
				if (!merge[owner].secondary) {
					for (k = owner; k < n_merge; k++) {
						int local_col;
						if (merge[k].ignore || merge[k].outside) continue;
						if (wrap_x) {
							local_col = col + nx_360;
							while (local_col > merge[k].out_i1) local_col -= nx_360;
							if (local_col < merge[k].out_i0) continue;
						}
						else {
							local_col = col;
							if (local_col < merge[k].out_i0 || local_col > merge[k].out_i1)
								continue;
						}
						kk = local_col - merge[k].out_i0;
						if (!gmt_M_is_fnan(merge[k].z[kk])) {
							z[col] = merge[k].z[kk];
							m = 1;
							break;
						}
					}
					w = 0.0;
					not_nan = (m != 0);
					goto merge2d_node_ready;
				}
				for (k = 0; k < n_merge; k++) {
					double wk = 0.0, primary;
					int local_col;
					if (k < owner || !merge[k].secondary ||
					    strcmp(merge[k].s_source, merge[owner].s_source) ||
					    !strcmp(merge[k].source, merge[owner].s_source))
						continue;
					if (merge[k].ignore || merge[k].outside) continue;
					if (wrap_x) {
						local_col = col + nx_360;
						while (local_col > merge[k].out_i1) local_col -= nx_360;
						if (local_col < merge[k].out_i0) continue;
					}
					else {
						local_col = col;
						if (local_col < merge[k].out_i0 || local_col > merge[k].out_i1) continue;
					}
					kk = local_col - merge[k].out_i0;
					primary = merge[k].z[kk];
					if (!gmt_M_is_fnan(primary)) {
						any_finite_primary = true;
						if (gmt_M_is_dnan(fallback_primary)) fallback_primary = primary;
					}
					if (!merge[k].s_outside && local_col >= merge[k].s_out_i0 &&
					    local_col <= merge[k].s_out_i1) {
						s_kk = local_col - merge[k].s_out_i0;
						if (!gmt_M_is_fnan(merge[k].s_z[s_kk]) && gmt_M_is_dnan(background))
							background = merge[k].s_z[s_kk];
					}
					if (merge2d_support_weight_at(&merge[k], local_col, row, &wk) != GMT_NOERROR)
						return (GMT_RUNTIME_ERROR);
					sum_geometry += wk;
					if (!gmt_M_is_fnan(primary) && wk > 0.0) {
						sum_valid += wk;
						sum_values += wk * primary;
					}
					else if (Ctrl->P.active && !merge[k].s_outside &&
					         local_col >= merge[k].s_out_i0 && local_col <= merge[k].s_out_i1 &&
					         !gmt_M_is_fnan(merge[k].s_z[s_kk]) && wk > 0.0) {
						secondary_weight += wk;
						secondary_values += wk * merge[k].s_z[s_kk];
					}
				}
				w = MIN(1.0, sum_geometry);
				{
					double background_weight = !gmt_M_is_dnan(background)
					                         ? MAX(0.0, 1.0 - sum_geometry) : 0.0;
					double denominator = sum_valid + secondary_weight + background_weight;
					double numerator = sum_values + secondary_values +
					                   background_weight * (!gmt_M_is_dnan(background) ? background : 0.0);
					if (denominator > 0.0 && (any_finite_primary || Ctrl->P.active)) {
						z[col] = (gmt_grdfloat)(numerator / denominator);
						m = 1;
					}
					else if (any_finite_primary) {
						z[col] = (gmt_grdfloat)fallback_primary;
						m = 1;
					}
					else
						m = Ctrl->W.active ? 1 : 0;
				}
				not_nan = (m != 0);
				goto merge2d_node_ready;
			}
			for (k = m = 0; k < n_merge; k++) {	/* Loop over every primary input grid; m will be the number of contributing primary grids to this node  */
				if (merge[k].ignore) continue;					/* This grid is entirely outside the s/n range */
				if (merge[k].outside) continue;					/* This grid is currently outside the s/n range */
				if (wrap_x) {	/* Special testing for periodic x coordinates */
					pcol = col + nx_360;
					while (pcol > merge[k].out_i1) pcol -= nx_360;
					if (pcol < merge[k].out_i0) continue;	/* This grid is currently outside the w/e range */
				}
				else {	/* Not periodic */
					pcol = col;
					if (pcol < merge[k].out_i0 || pcol > merge[k].out_i1) continue;	/* This grid is currently outside the xmin/xmax range */
				}
				kk = pcol - merge[k].out_i0;					/* kk is the local column variable for this grid */
				if (merge[k].secondary && !merge[k].s_outside &&
				    pcol >= merge[k].s_out_i0 && pcol <= merge[k].s_out_i1) {
					s_kk = pcol - merge[k].s_out_i0; 
				}
				if (gmt_M_is_fnan (merge[k].z[kk])) {
					if (!Ctrl->C.active && merge[k].secondary) {
						if (merge2d_support_weight_at(&merge[k], pcol, row, &w) != GMT_NOERROR)
							return (GMT_RUNTIME_ERROR);
						if (Ctrl->P.active && !merge[k].s_outside &&
						    pcol >= merge[k].s_out_i0 && pcol <= merge[k].s_out_i1 &&
						    !gmt_M_is_fnan(merge[k].s_z[s_kk])) {
							z[col] = merge[k].s_z[s_kk];
							not_nan = true;
							m = 1;
						}
						else
							m = Ctrl->W.active ? 1 : 0;
						break;
					}
					continue;
				}
				not_nan = true;	/* At least one non-NaN grid contributing */
				if (Ctrl->C.active) {	/* Clobber; update z[col] according to selected mode - No merging takes place here */
					switch (Ctrl->C.mode) {
						case MERGE2D_FIRST: if (m) continue; break;	/* Already set */
						case MERGE2D_UPPER: if (m && merge[k].z[kk] <= z[col]) continue; break;	/* Already has a higher value; else set below */
						case MERGE2D_LOWER: if (m && merge[k].z[kk] >= z[col]) continue; break;	/* Already has a lower value; else set below */
						/* Last case MERGE2D_LAST is always true in that we always update z[col] */
					}
					switch (Ctrl->C.sign) {	/* Check if sign of input grid should be considered in decision */
						case -1: if (first_grid) {z[col] = merge[k].z[kk]; first_grid = false; continue;}	/* Must initialize with first grid in case nothing passes */
							 else if (merge[k].z[kk] > 0.0) continue;	/* Only pick grids value if negative or zero */
							 break;
						case +1: if (first_grid) { z[col] = merge[k].z[kk]; first_grid = false; continue;}	/* Must initialize with first grid in case nothing passes */
							 else if (merge[k].z[kk] < 0.0) continue;	/* Only pick grids value if positive or zero */
							 break;
						default: break;						/* Always use the grid value */

					}
					z[col] = merge[k].z[kk];			/* Just pick this grid's value */
					w = 1.0;							/* Set weights to 1 */
					m = 1;								/* Pretend only one grid came here */
				}
				else {	/* Do the weighted merging - only want one primary grid here */

					/* merging mode */
					if (merge[k].secondary) {
						if (merge2d_support_weight_at(&merge[k], pcol, row, &w) != GMT_NOERROR)
							return (GMT_RUNTIME_ERROR);

						/* get local grid coordinate of secondary grid - rows have been synced */
						/* if the secondary grid value at said location is nan, simply return the primary grid value, i.e., weight = 1 */
						if (merge[k].s_outside || pcol < merge[k].s_out_i0 ||
						    pcol > merge[k].s_out_i1 || gmt_M_is_fnan (merge[k].s_z[s_kk])) {
							z[col] = merge[k].z[kk];
							m = 1; break;
						}

						else { /* merge using the blend library */
							z[col] = (w * merge[k].z[kk]) + (1 - w) * merge[k].s_z[s_kk];
							m = 1; break;
						}
					}
					else { /* just tiling */
						z[col] = merge[k].z[kk];			/* Just pick this grid's value */
						w = 0.0;
						/* in both case we want to retain value only for the first file */
						m = 1; break;
					}
				
				}
			}

merge2d_node_ready:
			if (Ctrl->W.active && !Ctrl->C.active &&
			    merge2d_report_weight(Ctrl, merge, n_merge, (int)col, (int)row,
			                          wrap_x, nx_360, &w) != GMT_NOERROR)
				return GMT_RUNTIME_ERROR;
			if (Ctrl->C.sign && m == 0 && not_nan) m = 1, w = 1.0;	/* Since we started off with the first grid and never set m,w at that time. Default clobbering weight is 1 */

			if (m) {	/* OK, at least one grid contributed to an output value */
				if (Ctrl->W.active) z[col] = (gmt_grdfloat)w;
				n_fill++;						/* One more cell filled */
				if (z[col] < Grid->header->z_min) Grid->header->z_min = z[col];	/* Update the extrema for output grid */
				if (z[col] > Grid->header->z_max) Grid->header->z_max = z[col];
			}
			else {
				/* No grids covered this node, defaults to the no_data value */
				z[col] = no_data_f;
			}			
		}
		if (write_all_at_once) {	/* Must copy entire row to grid */
			ij = gmt_M_ijp (Grid->header, row, 0);
			gmt_M_memcpy (&(Grid->data[ij]), z, Grid->header->n_columns, gmt_grdfloat);
		}
		else
			GMT_Put_Row (API, row, Grid, z);

		if (row%10 == 0)  GMT_Report (API, GMT_MSG_INFORMATION, "Processed row %7ld of %d\r", row, Grid->header->n_rows);

	}
	GMT_Report (API, GMT_MSG_INFORMATION, "Processed row %7ld\n", row);
	nx_final = Grid->header->n_columns;	ny_final = Grid->header->n_rows;

	if (write_all_at_once) {	/* Must write entire grid */
		if (Ctrl->Z.active &&
		    merge2d_apply_transform(GMT, Grid, &Ctrl->Z.transform,
		                            !Ctrl->W.active) != GMT_NOERROR)
			return (GMT_RUNTIME_ERROR);
		if (GMT_Write_Data (API, GMT_IS_GRID, GMT_IS_FILE, GMT_IS_SURFACE, GMT_CONTAINER_AND_DATA, NULL, Ctrl->G.file, Grid) != GMT_NOERROR) {
			gmt_M_free (GMT, z);
			return (API->error);
		}
		if (Ctrl->Z.active &&
		    merge2d_finalize_transform_units(Ctrl->G.file, &Ctrl->Z.transform,
		                                     !Ctrl->W.active, true) != GMT_NOERROR)
			return (GMT_RUNTIME_ERROR);
	}
	else {	/* Finish the line-by-line writing */
		write_mode = GMT_CONTAINER_ONLY | GMT_GRID_ROW_BY_ROW;
		if ((error = GMT_Write_Data (API, GMT_IS_GRID, GMT_IS_FILE, GMT_IS_SURFACE, write_mode, NULL, outfile, Grid))) {
			return (error);
		}
		if ((error = GMT_Destroy_Data (API, &Grid)) != GMT_NOERROR) return (error);
	}
	gmt_M_free (GMT, z);

	/* Free up the list with grid information, closing files as necessary */

	for (k = 0; k < n_merge; k++) {
		if (merge[k].open || merge[k].memory) {
			gmt_M_free (GMT, merge[k].z);
			gmt_M_free (GMT, merge[k].RbR);
		}
		if (merge[k].open) {
			gmtlib_close_grd (GMT, merge[k].G);	/* Close the grid file so we don't have lots of them open */
			if (merge[k].delete && gmt_remove_file (GMT, merge[k].file))	/* Delete the temporary resampled file */
				GMT_Report (GMT->parent, GMT_MSG_ERROR, "Failed to delete file %s\n", merge[k].file);
			if ((error = GMT_Destroy_Data (API, &merge[k].G)) != GMT_NOERROR) return (error);
		}

		if (merge[k].secondary) {
			if (merge[k].s_open || merge[k].s_memory) {
				gmt_M_free (GMT, merge[k].s_z);
				gmt_M_free (GMT, merge[k].s_RbR);
			}
			if (merge[k].s_open) {
				gmtlib_close_grd (GMT, merge[k].s_G);	/* Close the grid file so we don't have lots of them open */
				if (merge[k].s_delete && gmt_remove_file (GMT, merge[k].s_file))	/* Delete the temporary resampled file */
					GMT_Report (GMT->parent, GMT_MSG_ERROR, "Failed to delete file %s\n", merge[k].s_file);
				if ((error = GMT_Destroy_Data (API, &merge[k].s_G)) != GMT_NOERROR) return (error);
			}
		}
		if (merge[k].v_data) {
			blend_window_boundary_clear(merge[k].v_data);
			free(merge[k].v_data);
		}
		blend_polygon_free(&merge[k].v_support);
	}

	if (gmt_M_is_verbose (GMT, GMT_MSG_INFORMATION)) {
		char empty[GMT_LEN64] = {""};
		GMT_Report (API, GMT_MSG_INFORMATION, "Merged grid size of %s is %d x %d\n", Ctrl->G.file, nx_final, ny_final);
		if (n_fill == n_tot)
			GMT_Report (API, GMT_MSG_INFORMATION, "All nodes assigned values\n");
		else {
			if (gmt_M_is_fnan (no_data_f))
				strcpy (empty, "NaN");
			else
				sprintf (empty, "%g", no_data_f);
			GMT_Report (API, GMT_MSG_INFORMATION, "%" PRIu64 " nodes assigned values, %" PRIu64 " set to %s\n", n_fill, n_tot - n_fill, empty);
		}
	}

	gmt_M_free (GMT, merge);

	if (transform_output && !write_all_at_once) {
		int status = merge2d_transform_file(GMT, outfile, Ctrl->G.file,
		                                    &Ctrl->Z.transform, !Ctrl->W.active);
		if (status) GMT_Report (API, GMT_MSG_ERROR, "Unable to transform output file %s.\n", outfile);
		if (gmt_remove_file(GMT, outfile))
			GMT_Report (GMT->parent, GMT_MSG_ERROR, "Failed to delete file %s\n", outfile);
		if (status) return (status);
	}

	return GMT_NOERROR;
}

#define bailout(code) { gmt_M_free_options(mode); return (code); }
#define Return(code) { Free_Ctrl(GMT, Ctrl); gmt_end_module(GMT, GMT_cpy); bailout(code); }

EXTERN_MSC int GMT_merge2d(void *V_API, int mode, void *args)
{
	struct GMTAPI_CTRL *API = gmt_get_api_ptr(V_API);
	struct GMT_CTRL *GMT = NULL, *GMT_cpy = NULL;
	struct GMT_OPTION *options = NULL;
	struct MERGE2D_CTRL *Ctrl = NULL;
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
	status = merge2d_run(GMT, Ctrl, options);
	Return(status);
}
