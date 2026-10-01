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
 * elygtl applies the Ely (2010) geotechnical layer to dry columns of a
 * multiparameter NetCDF model. Transformed input coordinates increase with
 * z positive down. Velocities are m/s, density is kg/m^3, and transition
 * thickness is metres while the Ely equations are evaluated.
 */

#include "gmt_dev.h"
#include "gq_remote.h"
#include "gq_transform.h"
#include "elygtl_inc.h"
#include <netcdf.h>

#define THIS_MODULE_CLASSIC_NAME "elygtl"
#define THIS_MODULE_MODERN_NAME "elygtl"
#define THIS_MODULE_LIB "gq"
#define THIS_MODULE_LIB_PURPOSE "The CRESCENT cyberinfrastructure team supplements to the Generic Mapping Tools"
#define THIS_MODULE_PURPOSE "Apply Ely geotechnical layering to three-dimensional multiparameter NetCDF cubes"
#define THIS_MODULE_KEYS "<G{,GG}"
#define THIS_MODULE_NEEDS ""
#define THIS_MODULE_OPTIONS "RVdfn"

enum ELYGTL_AXIS {
	ELYGTL_X = 0,
	ELYGTL_Y,
	ELYGTL_Z
};

enum ELYGTL_PROPERTY {
	ELYGTL_VP = 0,
	ELYGTL_VS,
	ELYGTL_RHO,
	ELYGTL_N_PROPERTIES
};

enum ELYGTL_CLASS_MODE {
	ELYGTL_CLASS_GMT = 0,
	ELYGTL_CLASS_MODEL,
	ELYGTL_CLASS_ALL_LAND,
	ELYGTL_CLASS_ALL_WET
};

enum ELYGTL_CLASS {
	ELYGTL_UNRESOLVED = 0,
	ELYGTL_LAND = 1,
	ELYGTL_WET = 2
};

enum ELYGTL_EVIDENCE {
	ELYGTL_EVIDENCE_VS30 = -1,
	ELYGTL_EVIDENCE_VP = ELYGTL_VP,
	ELYGTL_EVIDENCE_VS = ELYGTL_VS,
	ELYGTL_EVIDENCE_RHO = ELYGTL_RHO
};

struct ELYGTL_GAP {
	bool active;
	char method;
	double argument;
	unsigned int sectors;
	bool limited;
	unsigned int max_gap;
};

struct ELYGTL_MAPPING {
	char *name[ELYGTL_N_PROPERTIES];
	bool active[ELYGTL_N_PROPERTIES];
	bool used;
};

struct ELYGTL_CTRL {
	struct {
		char **file;
		size_t n;
	} In;
	struct {
		bool active;
		char selection[GMT_LEN256];
	} A;
	struct ELYGTL_MAPPING C;
	struct {
		bool active;
		char resolution;
	} D;
	struct {
		bool active;
		bool grid;
		double depth;
		char *file;
	} E;
	struct ELYGTL_MAPPING F;
	struct {
		bool active;
		char *file;
	} G;
	struct ELYGTL_GAP H;
	struct {
		bool active;
		double inc[2];
	} I;
	struct {
		bool active;
		char *file;
	} K;
	struct {
		bool active;
		enum ELYGTL_CLASS_MODE mode;
		enum ELYGTL_EVIDENCE evidence;
		bool evidence_set;
		bool water_set;
		double water;
		bool tolerance_set;
		double tolerance;
	} M;
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
		bool active;
		double velocity;
		double density;
	} U;
	struct {
		bool active;
		struct GQ_TRANSFORM transform;
	} Z;
};

struct ELYGTL_SOURCE {
	char *path;
	bool has_sentinel;
	double sentinel;
	struct GQ_TRANSFORM transform;
};

struct ELYGTL_FIELD {
	int varid;
	int dimids[3];
	int axis_position[3];
	char *name;
};

struct ELYGTL_CUBE {
	char *path;
	int ncid;
	bool has_sentinel;
	double sentinel;
	struct GQ_TRANSFORM transform;
	bool reverse[3];
	int axis_dimid[3];
	int coordinate_varid[3];
	char *coordinate_name[3];
	double *coordinate[3];
	size_t n[3];
	struct ELYGTL_FIELD field[ELYGTL_N_PROPERTIES];
	bool present[ELYGTL_N_PROPERTIES];
	bool horizontal_resampled;
	bool vertical_resampled;
};

struct ELYGTL_JOB {
	struct ELYGTL_CUBE cube;
	size_t plane;
	size_t total;
	double *value[ELYGTL_N_PROPERTIES];
	double *created[ELYGTL_N_PROPERTIES];
	double *vs30;
	double *transition;
	bool *wet;
	bool *classified;
	size_t wet_columns;
	size_t missing_vs30;
	size_t missing_transition;
	size_t missing_anchor;
	size_t modified_columns;
	size_t empirical_skips[ELYGTL_N_PROPERTIES];
};

struct ELYGTL_OUTPUT {
	int ncid;
	int *varid;
	int created_varid[ELYGTL_N_PROPERTIES];
	int nvars;
};

static const char *elygtl_key[ELYGTL_N_PROPERTIES] = {"vp", "vs", "rho"};

static int elygtl_parse_number(const char *text, double *value)
{
	char copy[GMT_LEN128], *slash = NULL, *end = NULL;
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

static int elygtl_property(const char *name)
{
	int k;
	for (k = 0; k < ELYGTL_N_PROPERTIES; k++)
		if (!strcmp(name, elygtl_key[k])) return k;
	return -1;
}

static void elygtl_mapping_free(struct ELYGTL_MAPPING *mapping)
{
	int k;
	for (k = 0; k < ELYGTL_N_PROPERTIES; k++) free(mapping->name[k]);
	memset(mapping, 0, sizeof(*mapping));
}

static int elygtl_parse_mapping(struct GMTAPI_CTRL *API, const char *text,
                                struct ELYGTL_MAPPING *mapping,
                                const char *option)
{
	char *copy = NULL, *token = NULL, *save = NULL;
	int status = GMT_PARSE_ERROR;

	if (text == NULL || !text[0]) goto bad;
	copy = strdup(text);
	if (copy == NULL) return GMT_MEMORY_ERROR;
	for (token = strtok_r(copy, ",", &save); token;
	     token = strtok_r(NULL, ",", &save)) {
		char *equal = strchr(token, '=');
		int property;
		if (!equal || equal == token || !equal[1] || strchr(equal + 1, '='))
			goto bad;
		*equal++ = '\0';
		property = elygtl_property(token);
		if (property < 0 || mapping->active[property]) goto bad;
		mapping->name[property] = strdup(equal);
		if (mapping->name[property] == NULL) {
			status = GMT_MEMORY_ERROR;
			goto cleanup;
		}
		mapping->active[property] = true;
	}
	status = GMT_NOERROR;
	goto cleanup;
bad:
	GMT_Report(API, GMT_MSG_ERROR,
	           "Option -%s must be a comma-separated list of "
	           "vp=<name>,vs=<name>,rho=<name>\n", option);
cleanup:
	free(copy);
	if (status != GMT_NOERROR) elygtl_mapping_free(mapping);
	return status;
}

static char *elygtl_modifier_start(char *text)
{
	char *p;
	for (p = text; *p; p++) {
		if (*p != '+') continue;
		if (p > text && (p[-1] == 'e' || p[-1] == 'E')) continue;
		if (strchr("nxyzXYZvV", p[1])) return p;
	}
	return p;
}

static int elygtl_parse_source(struct GMTAPI_CTRL *API, const char *text,
                               struct ELYGTL_SOURCE *source)
{
	char *copy = NULL, *modifier;
	char message[GMT_LEN256];

	memset(source, 0, sizeof(*source));
	gq_transform_init(&source->transform);
	copy = strdup(text);
	if (copy == NULL) return GMT_MEMORY_ERROR;
	modifier = elygtl_modifier_start(copy);
	if (*modifier) *modifier++ = '\0';
	if (modifier &&
	    gq_transform_parse(modifier,
	                       GQ_TRANSFORM_X_MASK | GQ_TRANSFORM_Y_MASK |
	                       GQ_TRANSFORM_Z_MASK,
	                       true, &source->transform,
	                       &source->has_sentinel, &source->sentinel,
	                       message, sizeof(message)))
		goto bad;
	if (!copy[0]) goto bad;
	{
		int status = gq_resolve_remote_path(API, GMT_IS_GRID,
		                                    copy, &source->path);
		if (status != GMT_NOERROR) {
			free(copy);
			memset(source, 0, sizeof(*source));
			return status;
		}
	}
	free(copy);
	return GMT_NOERROR;
bad:
	GMT_Report(API, GMT_MSG_ERROR,
	           "Invalid model source %s; use +n, coordinate, value, and "
	           "target-unit modifiers\n", text);
	free(copy);
	free(source->path);
	gq_transform_free(&source->transform);
	return GMT_PARSE_ERROR;
}

static void *New_Ctrl(struct GMT_CTRL *GMT)
{
	struct ELYGTL_CTRL *Ctrl =
	    gmt_M_memory(GMT, NULL, 1, struct ELYGTL_CTRL);
	strcpy(Ctrl->A.selection, "0/0/1");
	Ctrl->D.resolution = 'l';
	Ctrl->M.mode = ELYGTL_CLASS_GMT;
	Ctrl->M.evidence = ELYGTL_EVIDENCE_VS30;
	Ctrl->S.mode = GMT_SPLINE_LINEAR;
	Ctrl->E.depth = 350.0;
	Ctrl->U.velocity = 1.0;
	Ctrl->U.density = 1.0;
	gq_transform_init(&Ctrl->Z.transform);
	return Ctrl;
}

static void Free_Ctrl(struct GMT_CTRL *GMT, struct ELYGTL_CTRL *Ctrl)
{
	size_t k;
	if (Ctrl == NULL) return;
	for (k = 0; k < Ctrl->In.n; k++) free(Ctrl->In.file[k]);
	free(Ctrl->In.file);
	elygtl_mapping_free(&Ctrl->C);
	elygtl_mapping_free(&Ctrl->F);
	free(Ctrl->G.file);
	free(Ctrl->K.file);
	free(Ctrl->E.file);
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
	          "usage: %s <model.nc>[+n<missing>][+x<sx>][+X<unit>]"
	          "[+y<sy>][+Y<unit>][+z<sz>][+Z<unit>]"
	          "[+v<scales>][+V<units>] "
	          "<vs30_grid>[?field][+x<sx>][+X<unit>][+y<sy>][+Y<unit>]"
	          "[+v<scale>][+V<unit>] -G<output.nc> "
	          "-Fvp=<name>,vs=<name>[,rho=<name>] "
	          "[-Cvp=<name>,vs=<name>,rho=<name>] "
	          "[-H[n|l|a|s|m[<arg>]][+m<maxgap>]] "
	          "[-E<depth>|<grid>[?field][+x<sx>][+X<unit>]"
	          "[+y<sy>][+Y<unit>][+v<scale>][+V<unit>]] "
	          "[-Mg|m|l|w[+e<vs30|vp|vs|rho>][+w<value>]"
	          "[+t<tolerance>]] "
	          "[-Sa|c|e|l|n|s<p>[+g[<maxgap>]]] "
	          "[-T<zmin>/<zmax>/<dz>] "
	          "[-U<velocity_scale>[/<density_scale>]] "
	          "[-Z+x<sx>+X<unit>+y<sy>+Y<unit>+z<sz>+Z<unit>"
	          "+v<scales>+V<units>] "
	          "[-A<min_area>[/<min_level>/<max_level>]] "
	          "[-D<a|f|h|i|l|c|n>] [-K<landmask>] [%s] "
	          "[-I<dx>[/<dy>]] [%s] [%s] [%s]\n",
	          name, GMT_Rgeo_OPT, GMT_V_OPT, GMT_di_OPT, GMT_n_OPT);
	if (level == GMT_SYNOPSIS) return GMT_MODULE_SYNOPSIS;

	GMT_Message(API, GMT_TIME_NONE, "  REQUIRED ARGUMENTS:\n");
	GMT_Usage(API, 1, "\n<model.nc>[+n<missing>][+x<sx>][+X<unit>]"
	                       "[+y<sy>][+Y<unit>][+z<sz>][+Z<unit>]"
	                       "[+v<scales>][+V<units>]");
	GMT_Usage(API, -2,
	          "Read the 3-D NetCDF model. NetCDF scale_factor and add_offset are "
	          "applied first, followed by the lowercase modifiers. All transformed "
	          "x, y, and z coordinates must be strictly increasing. The working z "
	          "axis must be positive down and expressed in metres for the Ely "
	          "calculation. Scaling changes coordinate values in place and does not "
	          "reorder model layers or data. For example, +z-1 converts a stored "
	          "positive-up axis of 4000 ... -16000 m to an increasing positive-down "
	          "axis of -4000 ... 16000 m. For an axis stored in kilometres, use "
	          "+z-1000 to obtain the same working axis in metres. +v/+V "
	          "follow active -F properties in vp,vs,rho order. Use +n for an "
	          "additional missing-value sentinel.");
	GMT_Usage(API, 1, "\n<vs30_grid>[?field][+x<sx>][+X<unit>]"
	                       "[+y<sy>][+Y<unit>][+v<scale>][+V<unit>]");
	GMT_Usage(API, -2,
	          "Supply Vs30. Coordinate and value transforms are applied before "
	          "sampling it onto the working model lattice. Vs30 must be in m/s "
	          "when the Ely equations are evaluated. Legacy +s<scale> remains an "
	          "alias for +v<scale>.");
	GMT_Usage(API, 1, "\n-G<output.nc>");
	GMT_Usage(API, -2,
	          "Write the transformed model. Unselected variables are preserved "
	          "when their dimensions remain compatible. If -R, -I, or -T changes "
	          "a coordinate lattice, unselected variables that depend on a changed "
	          "coordinate are omitted because elygtl does not resample them.");
	GMT_Usage(API, 1, "\n-Fvp=<name>,vs=<name>[,rho=<name>]");
	GMT_Usage(API, -2,
	          "Map existing model variables. At least vp or vs is required. The "
	          "three names are semantic roles, not required NetCDF variable names. "
	          "For example, -Fvp=Vp,vs=Vs,rho=Density maps the NetCDF variables "
	          "Vp, Vs, and Density to the P-wave velocity, S-wave velocity, and "
	          "density roles used by elygtl.");

	GMT_Message(API, GMT_TIME_NONE, "\n  OPTIONAL ARGUMENTS:\n");
	GMT_Option(API, "R");
	GMT_Usage(API, -2,
	          "Set an output horizontal region contained within the transformed "
	          "model domain. The complete transformed model region is used when "
	          "-R is omitted. Input scaling precedes region selection.");
	GMT_Usage(API, 1, "\n-I<dx>[/<dy>]");
	GMT_Usage(API, -2,
	          "Set output horizontal increments. The model increments are retained "
	          "when -I is omitted. "
	          "Mapped model properties, Vs30, transition thickness, and the wet mask "
	          "are sampled onto this lattice. Common option -n selects the GMT "
	          "horizontal interpolation used for mapped model layers and ancillary "
	          "grids. Input scaling and -H precede -R/-I.");
	GMT_Usage(API, 1, "\n-Cvp=<name>,vs=<name>,rho=<name>");
	GMT_Usage(API, -2,
	          "Create entirely missing properties under the requested names. "
	          "Vp and Vs use Brocher relations outside the GTL. Density uses "
	          "the Nafe-Drake relation from final Vp. Empirical creation occurs "
	          "after input interpolation and before output -Z scaling. For example, "
	          "-Fvs=Vs -Cvp=Vp,rho=Density creates Vp and Density from an existing "
	          "Vs variable, while -Fvp=Vp -Cvs=Vs creates Vs from an existing Vp "
	          "variable.");
	GMT_Usage(API, 1,
	          "\n-A<min_area>[/<min_level>/<max_level>][+a<antarctica>]"
	          "[+l|r][+p<percent>]");
	GMT_Usage(API, -2,
	          "Select GSHHG features used by automatic wet/land classification. "
	          "The default is 0/0/1, which treats oceans as wet and land as dry "
	          "while ignoring lakes and smaller water bodies. Levels are 0 ocean, "
	          "1 land, 2 lake, 3 island in lake, and 4 pond. GMT's +a, +l, +r, "
	          "and +p modifiers follow grdlandmask.");
	GMT_Usage(API, 1, "\n-D<a|f|h|i|l|c|n>");
	GMT_Usage(API, -2,
	          "Set the GMT shoreline resolution used by -Mg or as the prior for "
	          "-Mm. The default is low resolution (l). Use n to disable shoreline "
	          "classification. This option has no effect for Cartesian models, "
	          "-K, -Ml, or -Mw.");
	GMT_Usage(API, 1, "\n-E<depth>|<grid>[?field][+x<sx>][+X<unit>]"
	                       "[+y<sy>][+Y<unit>][+v<scale>][+V<unit>]");
	GMT_Usage(API, -2,
	          "Set the positive transition thickness below the local model "
	          "surface, either as a constant or spatially variable grid. The default "
	          "is 350 m. The local surface is the shallowest finite layer among the "
	          "mapped properties, and the transition base is surface + thickness on "
	          "the positive-down axis. Grid transforms precede sampling. Legacy +s "
	          "is an alias for +v on a transition grid. For example, -E500 uses a "
	          "500 m thickness everywhere, while -Etransition.nc?depth+v1000 reads "
	          "a thickness grid stored in kilometres.");
	GMT_Usage(API, 1,
	          "\n-H[n|l|a|s|m[<arg>]][+m<maxgap>]");
	GMT_Usage(API, -2,
	          "Fill strictly internal horizontal missing-data holes in every native "
	          "x-y model layer before horizontal resampling and Ely GTL. Original "
	          "finite nodes and boundary-connected missing regions are preserved. "
	          "Without -H, native horizontal holes are not filled. Use linear "
	          "Delaunay interpolation when -H is given without a method. Available "
	          "methods are:");
	GMT_Usage(API, 3,
	          "Nearest neighbor (n). Optionally append a search radius in grid nodes.");
	GMT_Usage(API, 3, "Linear Delaunay interpolation (l). This is the default.");
	GMT_Usage(API, 3,
	          "Local weighted average (a). Optionally append radius[/sectors] in "
	          "grid nodes. The default is 3/4.");
	GMT_Usage(API, 3,
	          "Spline interpolation (s). Optionally append tension from 0 through 1. "
	          "The default is 0.");
	GMT_Usage(API, 3,
	          "Minimum-curvature interpolation (m). Optionally append tension from "
	          "0 through 1. The default is 0.");
	GMT_Usage(API, 3,
	          "+m fills only holes whose x and y spans are no larger than <maxgap> "
	          "grid nodes. By default, every strictly internal hole is eligible.");
	GMT_Usage(API, 1, "\n-K<landmask>[?field][+x<sx>][+X<unit>]"
	                       "[+y<sy>][+Y<unit>]");
	GMT_Usage(API, -2,
	          "Supply an authoritative mask with wet=0 and land=1. It replaces "
	          "GMT and model classification. Use ?field for a multi-variable file. "
	          "+x and +y scale mask coordinates. +X and +Y set their units. Mask "
	          "values cannot be scaled. -K cannot be combined with -Mm, -Ml, or -Mw.");
	GMT_Usage(API, 1,
	          "\n-Mg|m|l|w[+e<vs30|vp|vs|rho>][+w<value>]"
	          "[+t<tolerance>]");
	GMT_Usage(API, -2,
	          "Choose wet/land classification precedence. Use g for GMT shoreline "
	          "priority (the default), m to let the selected evidence override GMT "
	          "where it resolves a class, l to classify the entire domain as land, "
	          "or w to classify it as wet. A user -K mask is authoritative. Append "
	          "+e to select the evidence source. vs30 is the default. Finite positive "
	          "Vs30 indicates land, a finite non-positive value indicates wet, and "
	          "missing Vs30 is unresolved. For vp, vs, or rho evidence, the selected "
	          "role must be mapped by -F. The shallowest finite model sample above "
	          "sea level indicates land and one below sea level indicates wet. At "
	          "sea level, +w gives the water value and +t gives its non-negative "
	          "matching tolerance. Vs defaults to +w0. Vp and density require +w. "
	          "Water values and tolerances use transformed model units after input "
	          "+v scaling but before -U conversion. For example, -Mm+evs gives "
	          "model Vs priority, -Mm+evp+w1500+t50 recognizes Vp from 1450 through "
	          "1550 as water, and -Mg+erho+w1000+t25 retains GMT priority while "
	          "using model density where GMT is unresolved.");
	GMT_Usage(API, 1, "\n-Sa|c|e|l|n|s<p>[+g[<maxgap>]]");
	GMT_Usage(API, -2,
	          "Choose vertical interpolation: Akima (a), cubic (c), step-up (e), "
	          "linear (l), nearest (n), or smoothing spline (s<p>) with a "
	          "non-negative fit parameter p. Linear is the default. Append +g to "
	          "bridge internal missing layers, optionally only when the bracketing "
	          "z-coordinate distance does not exceed maxgap. -S applies when -T "
	          "resamples z. +g also fills internal gaps on an unchanged z lattice. "
	          "Option -H fills enclosed holes within x-y layers, whereas common -n "
	          "controls horizontal sampling onto the -R/-I lattice.");
	GMT_Usage(API, 1, "\n-T<zmin>/<zmax>/<dz>");
	GMT_Usage(API, -2,
	          "Set the increasing positive-down working z lattice in metres, with "
	          "zmin at the top, zmax at the bottom, zmin < zmax, and dz > 0. The "
	          "requested range must remain within the transformed model axis. The "
	          "native z lattice is retained when -T is omitted. Option -S controls "
	          "vertical interpolation onto this lattice. For example, -T0/2000/50 "
	          "resamples from 0 to 2000 m depth at 50 m intervals.");
	GMT_Usage(API, 1, "\n-U<velocity_scale>[/<density_scale>]");
	GMT_Usage(API, -2,
	          "Multiply mapped velocities and density by these factors after input "
	          "+v scaling to obtain m/s and kg/m^3 for the empirical equations, then "
	          "apply the inverse factors before -Z output scaling. Both factors "
	          "default to 1. This option does not scale z, Vs30, or transition "
	          "thickness. "
	          "use their input modifiers to express those quantities in metres and "
	          "m/s before Ely GTL.");
	GMT_Usage(API, 1,
	          "\n-Z[+x<sx>][+X<xunit>][+y<sy>][+Y<yunit>]"
	          "[+z<sz>][+Z<zunit>][+v<scales>][+V<units>]");
	GMT_Usage(API, -2,
	          "Transform output coordinates and properties after interpolation, "
	          "classification, and Ely GTL. +x, +y, and +z scale coordinates. "
	          "+X, +Y, and +Z set coordinate units. +v supplies one broadcast "
	          "scale or one scale per output property. +V sets property units. "
	          "Property lists follow vp,vs,rho order for mapped and created outputs. "
	          "Scaling occurs in place and does not reorder coordinates, layers, or "
	          "data. For example, -Z+z-0.001+Zkm+v0.001+Vkm/s restores a positive-up "
	          "z axis in kilometres and writes velocity in km/s.");
	GMT_Usage(API, 1, "\nProcessing order:");
	GMT_Usage(API, -2,
	          "NetCDF unpacking and input modifiers. Increasing positive-down axis "
	          "validation. -H horizontal gap filling. -R/-I and common -n horizontal "
	          "resampling. -T/-S vertical resampling and gap bridging. Vs30, "
	          "transition-grid, and mask sampling. Wet/land classification. -U SI "
	          "conversion and Ely GTL. Empirical property creation. Then -Z output "
	          "scaling. Interpolation does not extrapolate beyond  "
	          "the model footprint.");
	GMT_Option(API, "V,di,n,.");
	return GMT_MODULE_USAGE;
}

static int elygtl_parse_increment(struct GMTAPI_CTRL *API, const char *text,
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
	if (elygtl_parse_number(copy, &inc[0]) ||
	    (slash && elygtl_parse_number(slash, &inc[1])))
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

static int elygtl_parse_range(struct GMTAPI_CTRL *API, const char *text,
                              struct ELYGTL_CTRL *Ctrl)
{
	char copy[GMT_LEN256], *token = NULL, *save = NULL;
	double value[3], intervals, adjusted, tolerance;
	size_t n = 0;

	if (text == NULL || strlen(text) >= sizeof(copy)) return GMT_PARSE_ERROR;
	strcpy(copy, text);
	for (token = strtok_r(copy, "/", &save); token && n < 3;
	     token = strtok_r(NULL, "/", &save)) {
		if (elygtl_parse_number(token, &value[n])) break;
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

static int elygtl_parse_interpolation(struct GMTAPI_CTRL *API,
                                      const char *text,
                                      struct ELYGTL_CTRL *Ctrl)
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

static int elygtl_parse_gap_option(struct GMTAPI_CTRL *API, const char *text,
                                   struct ELYGTL_GAP *H)
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

static int elygtl_parse_output_scale(struct GMTAPI_CTRL *API,
                                     const char *text,
                                     struct ELYGTL_CTRL *Ctrl)
{
	char message[GMT_LEN256] = {""};

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

static int elygtl_parse_transition(struct GMTAPI_CTRL *API, const char *text,
                                   struct ELYGTL_CTRL *Ctrl)
{
	double value;
	if (elygtl_parse_number(text, &value) == GMT_NOERROR) {
		if (value <= 0.0) {
			GMT_Report(API, GMT_MSG_ERROR,
			           "Option -E transition thickness must be positive\n");
			return GMT_PARSE_ERROR;
		}
		Ctrl->E.depth = value;
		return GMT_NOERROR;
	}
	if (text == NULL || !text[0]) return GMT_PARSE_ERROR;
	Ctrl->E.file = strdup(text);
	if (Ctrl->E.file == NULL) return GMT_MEMORY_ERROR;
	Ctrl->E.grid = true;
	return GMT_NOERROR;
}

static int elygtl_parse_classification(struct GMTAPI_CTRL *API,
	                                   const char *text,
	                                   struct ELYGTL_CTRL *Ctrl)
{
	char copy[GMT_LEN256], *modifier = NULL;

	if (text == NULL || !text[0] || strlen(text) >= sizeof(copy)) goto bad;
	strcpy(copy, text);
	modifier = strchr(copy, '+');
	if (modifier) *modifier++ = '\0';
	if (!copy[0] || copy[1]) goto bad;
	switch (copy[0]) {
		case 'g': Ctrl->M.mode = ELYGTL_CLASS_GMT; break;
		case 'm': Ctrl->M.mode = ELYGTL_CLASS_MODEL; break;
		case 'l': Ctrl->M.mode = ELYGTL_CLASS_ALL_LAND; break;
		case 'w': Ctrl->M.mode = ELYGTL_CLASS_ALL_WET; break;
		default: goto bad;
	}
	while (modifier && *modifier) {
		char code = *modifier++, value[GMT_LEN128], *next = modifier;
		size_t length;
		while (*next) {
			if (*next == '+' &&
			    !(next > modifier && (next[-1] == 'e' || next[-1] == 'E')))
				break;
			next++;
		}
		length = (size_t)(next - modifier);
		if (!length || length >= sizeof(value)) goto bad;
		memcpy(value, modifier, length);
		value[length] = '\0';
		switch (code) {
			case 'e':
				if (Ctrl->M.evidence_set) goto bad;
				if (!strcmp(value, "vs30"))
					Ctrl->M.evidence = ELYGTL_EVIDENCE_VS30;
				else if (!strcmp(value, "vp"))
					Ctrl->M.evidence = ELYGTL_EVIDENCE_VP;
				else if (!strcmp(value, "vs"))
					Ctrl->M.evidence = ELYGTL_EVIDENCE_VS;
				else if (!strcmp(value, "rho"))
					Ctrl->M.evidence = ELYGTL_EVIDENCE_RHO;
				else
					goto bad;
				Ctrl->M.evidence_set = true;
				break;
			case 'w':
				if (Ctrl->M.water_set ||
				    elygtl_parse_number(value, &Ctrl->M.water))
					goto bad;
				Ctrl->M.water_set = true;
				break;
			case 't':
				if (Ctrl->M.tolerance_set ||
				    elygtl_parse_number(value, &Ctrl->M.tolerance) ||
				    Ctrl->M.tolerance < 0.0)
					goto bad;
				Ctrl->M.tolerance_set = true;
				break;
			default: goto bad;
		}
		modifier = *next ? next + 1 : NULL;
	}
	return GMT_NOERROR;
bad:
	GMT_Report(API, GMT_MSG_ERROR,
	           "Option -M must be g, m, l, or w, optionally followed by "
	           "+e<vs30|vp|vs|rho>, +w<water>, and +t<tolerance>\n");
	return GMT_PARSE_ERROR;
}

static int elygtl_parse_units(struct GMTAPI_CTRL *API, const char *text,
                              struct ELYGTL_CTRL *Ctrl)
{
	char copy[GMT_LEN128], *slash;
	if (text == NULL || strlen(text) >= sizeof(copy)) goto bad;
	strcpy(copy, text);
	slash = strchr(copy, '/');
	if (slash) *slash++ = '\0';
	if (elygtl_parse_number(copy, &Ctrl->U.velocity) ||
	    Ctrl->U.velocity <= 0.0)
		goto bad;
	if (slash) {
		if (strchr(slash, '/') ||
		    elygtl_parse_number(slash, &Ctrl->U.density) ||
		    Ctrl->U.density <= 0.0)
			goto bad;
	}
	return GMT_NOERROR;
bad:
	GMT_Report(API, GMT_MSG_ERROR,
	           "Option -U must be positive velocity[/density] scales\n");
	return GMT_PARSE_ERROR;
}

static int parse(struct GMT_CTRL *GMT, struct ELYGTL_CTRL *Ctrl,
                 struct GMT_OPTION *options)
{
	struct GMT_OPTION *opt;
	struct GMTAPI_CTRL *API = GMT->parent;
	unsigned int n_errors = 0;
	int p;

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
				n_errors += gmt_M_repeated_module_option(API, Ctrl->C.used);
				n_errors += elygtl_parse_mapping(API, opt->arg, &Ctrl->C, "C");
				break;
			case 'D':
				n_errors += gmt_M_repeated_module_option(API, Ctrl->D.active);
				if (!opt->arg[0] || opt->arg[1] ||
				    strchr("afhilcn", opt->arg[0]) == NULL)
					n_errors++;
				else
					Ctrl->D.resolution = opt->arg[0];
				break;
			case 'F':
				n_errors += gmt_M_repeated_module_option(API, Ctrl->F.used);
				n_errors += elygtl_parse_mapping(API, opt->arg, &Ctrl->F, "F");
				break;
			case 'G':
				n_errors += gmt_M_repeated_module_option(API, Ctrl->G.active);
				if (!opt->arg[0]) n_errors++;
				else Ctrl->G.file = strdup(opt->arg);
				break;
			case 'H':
				n_errors += gmt_M_repeated_module_option(API, Ctrl->H.active);
				n_errors += elygtl_parse_gap_option(API, opt->arg, &Ctrl->H);
				break;
			case 'I':
				n_errors += gmt_M_repeated_module_option(API, Ctrl->I.active);
				n_errors += elygtl_parse_increment(API, opt->arg, Ctrl->I.inc);
				break;
			case 'K':
				n_errors += gmt_M_repeated_module_option(API, Ctrl->K.active);
				if (!opt->arg[0]) n_errors++;
				else Ctrl->K.file = strdup(opt->arg);
				break;
			case 'E':
				n_errors += gmt_M_repeated_module_option(API, Ctrl->E.active);
				n_errors += elygtl_parse_transition(API, opt->arg, Ctrl);
				break;
			case 'M':
				n_errors += gmt_M_repeated_module_option(API, Ctrl->M.active);
				n_errors += elygtl_parse_classification(API, opt->arg, Ctrl);
				break;
			case 'S':
				n_errors += gmt_M_repeated_module_option(API, Ctrl->S.active);
				n_errors += elygtl_parse_interpolation(API, opt->arg, Ctrl);
				break;
			case 'T':
				n_errors += gmt_M_repeated_module_option(API, Ctrl->T.active);
				n_errors += elygtl_parse_range(API, opt->arg, Ctrl);
				break;
			case 'U':
				n_errors += gmt_M_repeated_module_option(API, Ctrl->U.active);
				n_errors += elygtl_parse_units(API, opt->arg, Ctrl);
				break;
			case 'Z':
				n_errors += gmt_M_repeated_module_option(API, Ctrl->Z.active);
				n_errors += elygtl_parse_output_scale(API, opt->arg, Ctrl);
				break;
			default:
				n_errors += gmt_default_option_error(GMT, opt);
				break;
		}
	}
	n_errors += gmt_M_check_condition(GMT, Ctrl->In.n != 2,
	                                  "Specify one model and one Vs30 grid\n");
	n_errors += gmt_M_check_condition(GMT, !Ctrl->G.active,
	                                  "Option -G is required\n");
	n_errors += gmt_M_check_condition(
	    GMT, !Ctrl->F.active[ELYGTL_VP] && !Ctrl->F.active[ELYGTL_VS],
	    "Option -F must map at least vp or vs\n");
	for (p = 0; p < ELYGTL_N_PROPERTIES; p++) {
		n_errors += gmt_M_check_condition(
		    GMT, Ctrl->F.active[p] && Ctrl->C.active[p],
		    "A property cannot appear in both -F and -C\n");
	}
	n_errors += gmt_M_check_condition(
	    GMT, Ctrl->K.active && Ctrl->M.active && Ctrl->M.mode != ELYGTL_CLASS_GMT,
	    "Option -K is authoritative and cannot be combined with -Mm, -Ml, or -Mw\n");
	n_errors += gmt_M_check_condition(
	    GMT, Ctrl->K.active && Ctrl->M.active &&
	         (Ctrl->M.evidence_set || Ctrl->M.water_set || Ctrl->M.tolerance_set),
	    "Option -K is authoritative and cannot be combined with -M evidence modifiers\n");
	n_errors += gmt_M_check_condition(
	    GMT, (Ctrl->M.mode == ELYGTL_CLASS_ALL_LAND ||
	          Ctrl->M.mode == ELYGTL_CLASS_ALL_WET) &&
	         (Ctrl->M.evidence_set || Ctrl->M.water_set || Ctrl->M.tolerance_set),
	    "Options -Ml and -Mw do not accept evidence modifiers\n");
	n_errors += gmt_M_check_condition(
	    GMT, Ctrl->M.evidence == ELYGTL_EVIDENCE_VS30 &&
	         (Ctrl->M.water_set || Ctrl->M.tolerance_set),
	    "Vs30 evidence does not accept +w or +t\n");
	if (Ctrl->M.evidence != ELYGTL_EVIDENCE_VS30) {
		int evidence = (int)Ctrl->M.evidence;
		n_errors += gmt_M_check_condition(
		    GMT, !Ctrl->F.active[evidence],
		    "The model property selected by -M+e must be mapped by -F\n");
		if (Ctrl->M.evidence == ELYGTL_EVIDENCE_VS && !Ctrl->M.water_set)
			Ctrl->M.water = 0.0;
		n_errors += gmt_M_check_condition(
		    GMT, Ctrl->M.evidence != ELYGTL_EVIDENCE_VS && !Ctrl->M.water_set,
		    "Vp and density model evidence require +w<water>\n");
	}
	{
		size_t n_output = 0;
		char message[GMT_LEN256] = {""};
		for (p = 0; p < ELYGTL_N_PROPERTIES; p++)
			if (Ctrl->F.active[p] || Ctrl->C.active[p]) n_output++;
		if (gq_transform_validate_values(&Ctrl->Z.transform, n_output,
		                                  message, sizeof(message))) {
			GMT_Report(API, GMT_MSG_ERROR, "Option -Z: %s\n", message);
			n_errors++;
		}
	}
	if (n_errors)
		GMT_Report(API, GMT_MSG_ERROR,
		           "Invalid elygtl options; use elygtl -? for usage\n");
	return n_errors ? GMT_PARSE_ERROR : GMT_NOERROR;
}

static char *elygtl_text_attribute(int ncid, int varid, const char *name)
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

static bool elygtl_numeric_type(nc_type type)
{
	return type == NC_BYTE || type == NC_UBYTE || type == NC_SHORT ||
	       type == NC_USHORT || type == NC_INT || type == NC_UINT ||
	       type == NC_INT64 || type == NC_UINT64 || type == NC_FLOAT ||
	       type == NC_DOUBLE;
}

static int elygtl_axis_name(const char *text)
{
	char lower[NC_MAX_NAME + 1];
	size_t k, length;
	if (text == NULL || (length = strlen(text)) > NC_MAX_NAME) return -1;
	for (k = 0; k <= length; k++)
		lower[k] = (char)tolower((unsigned char)text[k]);
	if (!strcmp(lower, "x") || !strcmp(lower, "lon") ||
	    !strcmp(lower, "longitude") || !strcmp(lower, "easting"))
		return ELYGTL_X;
	if (!strcmp(lower, "y") || !strcmp(lower, "lat") ||
	    !strcmp(lower, "latitude") || !strcmp(lower, "northing"))
		return ELYGTL_Y;
	if (!strcmp(lower, "z") || !strcmp(lower, "depth") ||
	    !strcmp(lower, "elevation") || !strcmp(lower, "altitude") ||
	    !strcmp(lower, "level"))
		return ELYGTL_Z;
	return -1;
}

static int elygtl_coordinate_axis(int ncid, int varid, const char *name)
{
	char *attribute;
	int axis = -1;
	attribute = elygtl_text_attribute(ncid, varid, "axis");
	if (attribute) {
		if ((attribute[0] == 'X' || attribute[0] == 'x') && !attribute[1])
			axis = ELYGTL_X;
		else if ((attribute[0] == 'Y' || attribute[0] == 'y') && !attribute[1])
			axis = ELYGTL_Y;
		else if ((attribute[0] == 'Z' || attribute[0] == 'z') && !attribute[1])
			axis = ELYGTL_Z;
		free(attribute);
		if (axis >= 0) return axis;
	}
	attribute = elygtl_text_attribute(ncid, varid, "standard_name");
	if (attribute) {
		if (strstr(attribute, "longitude") ||
		    strstr(attribute, "projection_x_coordinate"))
			axis = ELYGTL_X;
		else if (strstr(attribute, "latitude") ||
		         strstr(attribute, "projection_y_coordinate"))
			axis = ELYGTL_Y;
		else if (strstr(attribute, "depth") || strstr(attribute, "height") ||
		         strstr(attribute, "altitude"))
			axis = ELYGTL_Z;
		free(attribute);
		if (axis >= 0) return axis;
	}
	return elygtl_axis_name(name);
}

static bool elygtl_same_dimensions(const int a[3], const int b[3])
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

static void elygtl_cube_free(struct ELYGTL_CUBE *cube)
{
	int p, axis;
	if (cube == NULL) return;
	if (cube->ncid >= 0) nc_close(cube->ncid);
	free(cube->path);
	for (p = 0; p < ELYGTL_N_PROPERTIES; p++) free(cube->field[p].name);
	for (axis = 0; axis < 3; axis++) {
		free(cube->coordinate_name[axis]);
		free(cube->coordinate[axis]);
	}
	gq_transform_free(&cube->transform);
	memset(cube, 0, sizeof(*cube));
	gq_transform_init(&cube->transform);
	cube->ncid = -1;
}

static int elygtl_open_cube(struct GMTAPI_CTRL *API,
                            const struct ELYGTL_CTRL *Ctrl,
                            struct ELYGTL_CUBE *cube)
{
	struct ELYGTL_SOURCE source;
	int reference = Ctrl->F.active[ELYGTL_VP] ? ELYGTL_VP : ELYGTL_VS;
	int p, axis, status = GMT_DATA_READ_ERROR;
	int reference_dims[3];

	memset(cube, 0, sizeof(*cube));
	cube->ncid = -1;
	if (elygtl_parse_source(API, Ctrl->In.file[0], &source))
		return GMT_PARSE_ERROR;
	cube->path = strdup(source.path);
	cube->transform = source.transform;
	gq_transform_init(&source.transform);
	cube->has_sentinel = source.has_sentinel;
	cube->sentinel = source.sentinel;
	if (!cube->path || nc_open(cube->path, NC_NOWRITE, &cube->ncid) != NC_NOERR)
		goto cleanup;
	for (p = 0; p < ELYGTL_N_PROPERTIES; p++) {
		struct ELYGTL_FIELD *field = &cube->field[p];
		nc_type type;
		int ndims, position;
		if (!Ctrl->F.active[p]) continue;
		if (nc_inq_varid(cube->ncid, Ctrl->F.name[p], &field->varid) != NC_NOERR ||
		    nc_inq_var(cube->ncid, field->varid, NULL, &type, &ndims,
		               field->dimids, NULL) != NC_NOERR ||
		    !elygtl_numeric_type(type) || ndims != 3) {
			GMT_Report(API, GMT_MSG_ERROR,
			           "%s is not a numeric 3-D variable in %s\n",
			           Ctrl->F.name[p], cube->path);
			goto cleanup;
		}
		field->name = strdup(Ctrl->F.name[p]);
		if (field->name == NULL) goto cleanup;
		cube->present[p] = true;
		if (p == reference)
			memcpy(reference_dims, field->dimids, sizeof(reference_dims));
		else if (!elygtl_same_dimensions(reference_dims, field->dimids)) {
			GMT_Report(API, GMT_MSG_ERROR,
			           "Mapped variables do not share the same dimensions\n");
			goto cleanup;
		}
		for (position = 0; position < 3; position++)
			field->axis_position[position] = -1;
	}
	{
		size_t n_fields = 0;
		char message[GMT_LEN256];
		for (p = 0; p < ELYGTL_N_PROPERTIES; p++)
			if (cube->present[p]) n_fields++;
		if (gq_transform_validate_values(&cube->transform, n_fields,
		                                  message, sizeof(message))) {
			GMT_Report(API, GMT_MSG_ERROR, "%s: %s\n", Ctrl->In.file[0], message);
			goto cleanup;
		}
	}
	for (p = 0; p < 3; p++) {
		char dim_name[NC_MAX_NAME + 1];
		int coordinate_varid, identified;
		if (nc_inq_dimname(cube->ncid, reference_dims[p], dim_name) != NC_NOERR ||
		    nc_inq_varid(cube->ncid, dim_name, &coordinate_varid) != NC_NOERR)
			goto cleanup;
		identified = elygtl_coordinate_axis(cube->ncid, coordinate_varid,
		                                    dim_name);
		if (identified < 0 || cube->coordinate_name[identified]) {
			GMT_Report(API, GMT_MSG_ERROR,
			           "Cannot uniquely identify x, y, and z axes in %s\n",
			           cube->path);
			goto cleanup;
		}
		cube->axis_dimid[identified] = reference_dims[p];
		cube->coordinate_varid[identified] = coordinate_varid;
		cube->coordinate_name[identified] = strdup(dim_name);
	}
	for (p = 0; p < ELYGTL_N_PROPERTIES; p++) {
		int position;
		if (!cube->present[p]) continue;
		for (axis = 0; axis < 3; axis++)
			for (position = 0; position < 3; position++)
				if (cube->field[p].dimids[position] == cube->axis_dimid[axis])
					cube->field[p].axis_position[axis] = position;
		for (axis = 0; axis < 3; axis++)
			if (cube->field[p].axis_position[axis] < 0) goto cleanup;
	}
	for (axis = 0; axis < 3; axis++) {
		int ndims, dimid;
		nc_type type;
		if (!cube->coordinate_name[axis] ||
		    nc_inq_dimlen(cube->ncid, cube->axis_dimid[axis],
		                  &cube->n[axis]) != NC_NOERR ||
		    nc_inq_vartype(cube->ncid, cube->coordinate_varid[axis],
		                   &type) != NC_NOERR ||
		    !elygtl_numeric_type(type) ||
		    nc_inq_varndims(cube->ncid, cube->coordinate_varid[axis],
		                    &ndims) != NC_NOERR || ndims != 1 ||
		    nc_inq_vardimid(cube->ncid, cube->coordinate_varid[axis],
		                    &dimid) != NC_NOERR ||
		    dimid != cube->axis_dimid[axis])
			goto cleanup;
		cube->coordinate[axis] = calloc(cube->n[axis], sizeof(double));
		if (!cube->coordinate[axis] ||
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
		for (k = 0; k < cube->n[axis]; k++) {
			if (!isfinite(cube->coordinate[axis][k]) ||
			    (k && cube->coordinate[axis][k] <=
			          cube->coordinate[axis][k - 1])) {
				GMT_Report(API, GMT_MSG_ERROR,
				           "Scaled %c coordinates must be strictly increasing; "
				           "use an input axis scale to adopt the required convention "
				           "without reordering data\n", "xyz"[axis]);
				goto cleanup;
			}
		}
	}
	status = GMT_NOERROR;
cleanup:
	free(source.path);
	gq_transform_free(&source.transform);
	if (status != GMT_NOERROR) {
		GMT_Report(API, GMT_MSG_ERROR,
		           "Unable to read model metadata from %s\n", Ctrl->In.file[0]);
		elygtl_cube_free(cube);
	}
	return status;
}

static size_t elygtl_transform_index(const struct ELYGTL_CUBE *cube,
                                     int property)
{
	int p;
	size_t index = 0;
	for (p = 0; p < property; p++)
		if (cube->present[p]) index++;
	return index;
}

static size_t elygtl_output_transform_index(const struct ELYGTL_CTRL *Ctrl,
	                                        const struct ELYGTL_CUBE *cube,
	                                        int property)
{
	size_t index = 0;
	int p;
	for (p = 0; p < property; p++)
		if (cube->present[p] || Ctrl->C.active[p]) index++;
	return index;
}

static const char *elygtl_output_axis_unit(const struct ELYGTL_CTRL *Ctrl,
	                                       const struct ELYGTL_CUBE *cube,
	                                       int axis)
{
	if (Ctrl->Z.transform.axis_unit[axis])
		return Ctrl->Z.transform.axis_unit[axis];
	if (Ctrl->Z.transform.axis_scale[axis] != 1.0) return NULL;
	return cube->transform.axis_unit[axis];
}

static const char *elygtl_output_value_unit(const struct ELYGTL_CTRL *Ctrl,
	                                        const struct ELYGTL_CUBE *cube,
	                                        int property)
{
	size_t output = elygtl_output_transform_index(Ctrl, cube, property);
	const char *unit = gq_transform_value_unit(&Ctrl->Z.transform, output);
	if (unit) return unit;
	if (gq_transform_value_scale(&Ctrl->Z.transform, output) != 1.0)
		return NULL;
	if (cube->present[property])
		return gq_transform_value_unit(
		    &cube->transform, elygtl_transform_index(cube, property));
	return NULL;
}

static size_t elygtl_field_index(const struct ELYGTL_CUBE *cube, int property,
                                 size_t ix, size_t iy, size_t iz,
                                 bool input_order)
{
	const struct ELYGTL_FIELD *field = &cube->field[property];
	size_t index[3], length[3], stride = 1, offset = 0;
	int position, axis;
	if (input_order && cube->reverse[ELYGTL_X])
		ix = cube->n[ELYGTL_X] - 1 - ix;
	if (input_order && cube->reverse[ELYGTL_Y])
		iy = cube->n[ELYGTL_Y] - 1 - iy;
	if (input_order && cube->reverse[ELYGTL_Z])
		iz = cube->n[ELYGTL_Z] - 1 - iz;
	index[field->axis_position[ELYGTL_X]] = ix;
	index[field->axis_position[ELYGTL_Y]] = iy;
	index[field->axis_position[ELYGTL_Z]] = iz;
	for (position = 0; position < 3; position++) {
		length[position] = 0;
		for (axis = 0; axis < 3; axis++)
			if (field->axis_position[axis] == position)
				length[position] = cube->n[axis];
	}
	for (position = 2; position >= 0; position--) {
		offset += index[position] * stride;
		stride *= length[position];
	}
	return offset;
}

static bool elygtl_missing(const struct ELYGTL_CUBE *cube, int varid,
                           double value, struct GMTAPI_CTRL *API)
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

static int elygtl_read_property(struct GMTAPI_CTRL *API,
                                const struct ELYGTL_CTRL *Ctrl,
                                struct ELYGTL_JOB *job, int property)
{
	const struct ELYGTL_CUBE *cube = &job->cube;
	int varid = cube->field[property].varid;
	double *raw = calloc(job->total, sizeof(*raw));
	double scale = 1.0, offset = 0.0;
	double user_scale = gq_transform_value_scale(
	    &cube->transform, elygtl_transform_index(cube, property));
	size_t ix, iy, iz;
	if (raw == NULL) return GMT_MEMORY_ERROR;
	job->value[property] = calloc(job->total, sizeof(double));
	if (!job->value[property]) {
		free(raw);
		return GMT_MEMORY_ERROR;
	}
	if (nc_get_var_double(cube->ncid, varid, raw) != NC_NOERR) {
		free(raw);
		return GMT_DATA_READ_ERROR;
	}
	nc_get_att_double(cube->ncid, varid, "scale_factor", &scale);
	nc_get_att_double(cube->ncid, varid, "add_offset", &offset);
	for (iz = 0; iz < cube->n[ELYGTL_Z]; iz++)
		for (iy = 0; iy < cube->n[ELYGTL_Y]; iy++)
			for (ix = 0; ix < cube->n[ELYGTL_X]; ix++) {
				size_t logical = iz * job->plane +
				                 iy * cube->n[ELYGTL_X] + ix;
				double value = raw[elygtl_field_index(cube, property,
				                                      ix, iy, iz, true)];
				if (elygtl_missing(cube, varid, value, API))
					job->value[property][logical] = NAN;
				else {
					value = (value * scale + offset) * user_scale;
					job->value[property][logical] =
					    value * (property == ELYGTL_RHO
					             ? Ctrl->U.density : Ctrl->U.velocity);
				}
			}
	free(raw);
	return GMT_NOERROR;
}

static char *elygtl_grid_modifier(char *text)
{
	char *p;
	for (p = text; *p; p++) {
		if (*p != '+') continue;
		if (p > text && (p[-1] == 'e' || p[-1] == 'E')) continue;
		if (strchr("sxyXYvV", p[1])) return p;
	}
	return p;
}

static int elygtl_grid_source(struct GMTAPI_CTRL *API, const char *source,
                              bool allow_values, char **clean,
                              struct GQ_TRANSFORM *transform)
{
	char *copy, *modifier, *p, message[GMT_LEN256];
	*clean = NULL;
	gq_transform_init(transform);
	copy = strdup(source);
	if (copy == NULL) return GMT_MEMORY_ERROR;
	modifier = elygtl_grid_modifier(copy);
	if (*modifier) *modifier++ = '\0';
	for (p = modifier; p && *p; p = strchr(p, '+')) {
		if (*p == '+') p++;
		if (*p == 's') *p = 'v';
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

static int elygtl_sample_grid(struct GMT_CTRL *GMT, const char *source,
                              const struct ELYGTL_CUBE *cube,
                              bool allow_values, double *output)
{
	struct GMT_GRID *Grid = NULL;
	struct GQ_TRANSFORM transform;
	char *clean = NULL;
	size_t row, col;
	int status = elygtl_grid_source(GMT->parent, source, allow_values,
	                                &clean, &transform);
	if (status != GMT_NOERROR) return status;
	Grid = GMT_Read_Data(GMT->parent, GMT_IS_GRID, GMT_IS_FILE,
	                     GMT_IS_SURFACE, GMT_CONTAINER_AND_DATA,
	                     NULL, clean, NULL);
	free(clean);
	if (Grid == NULL) {
		gq_transform_free(&transform);
		return GMT->parent->error;
	}
	if (gmt_grd_BC_set(GMT, Grid, GMT_IN) != GMT_NOERROR) {
		status = GMT_RUNTIME_ERROR;
		goto cleanup;
	}
	for (row = 0; row < cube->n[ELYGTL_Y]; row++)
		for (col = 0; col < cube->n[ELYGTL_X]; col++) {
			double value = gmt_bcr_get_z(
			    GMT, Grid,
			    cube->coordinate[ELYGTL_X][col] /
			        transform.axis_scale[GQ_TRANSFORM_X],
			    cube->coordinate[ELYGTL_Y][row] /
			        transform.axis_scale[GQ_TRANSFORM_Y]);
			output[row * cube->n[ELYGTL_X] + col] =
			    isfinite(value)
			        ? value * gq_transform_value_scale(&transform, 0) : NAN;
		}
	status = GMT_NOERROR;
cleanup:
	if (GMT_Destroy_Data(GMT->parent, &Grid) != GMT_NOERROR &&
	    status == GMT_NOERROR)
		status = GMT_RUNTIME_ERROR;
	gq_transform_free(&transform);
	return status;
}

static bool elygtl_regular_coordinate(const double *coordinate, size_t n,
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

static int elygtl_grid_size(struct GMTAPI_CTRL *API, const char *axis,
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

static int elygtl_horizontal_bcr(struct GMT_CTRL *GMT,
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

	if (!elygtl_regular_coordinate(source_x, source_nx, &inc[0]) ||
	    !elygtl_regular_coordinate(source_y, source_ny, &inc[1])) {
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

static int elygtl_resample_horizontal(struct GMT_CTRL *GMT,
                                      const struct ELYGTL_CTRL *Ctrl,
                                      struct ELYGTL_JOB *job)
{
	struct ELYGTL_CUBE *cube = &job->cube;
	bool use_region = GMT->common.R.active[RSET];
	bool use_increment = Ctrl->I.active;
	double wesn[4], inc[2], source_inc[2], adjusted, tolerance_x, tolerance_y;
	double *x = NULL, *y = NULL, *native = NULL, *sampled = NULL;
	double *resampled[ELYGTL_N_PROPERTIES] = {NULL, NULL, NULL};
	size_t source_nx = cube->n[ELYGTL_X], source_ny = cube->n[ELYGTL_Y];
	size_t source_plane, target_plane, nx, ny, ix, iy, iz, k;
	int p, status = GMT_MEMORY_ERROR;

	if (!use_region && !use_increment) return GMT_NOERROR;
	if (!elygtl_regular_coordinate(cube->coordinate[ELYGTL_X], source_nx,
	                               &source_inc[0]) ||
	    !elygtl_regular_coordinate(cube->coordinate[ELYGTL_Y], source_ny,
	                               &source_inc[1])) {
		GMT_Report(GMT->parent, GMT_MSG_ERROR,
		           "Options -R and -I require regular model x and y coordinates\n");
		return GMT_RUNTIME_ERROR;
	}
	wesn[XLO] = use_region ? GMT->common.R.wesn[XLO]
	                       : cube->coordinate[ELYGTL_X][0];
	wesn[XHI] = use_region ? GMT->common.R.wesn[XHI]
	                       : cube->coordinate[ELYGTL_X][source_nx - 1];
	wesn[YLO] = use_region ? GMT->common.R.wesn[YLO]
	                       : cube->coordinate[ELYGTL_Y][0];
	wesn[YHI] = use_region ? GMT->common.R.wesn[YHI]
	                       : cube->coordinate[ELYGTL_Y][source_ny - 1];
	inc[0] = use_increment ? Ctrl->I.inc[0] : source_inc[0];
	inc[1] = use_increment ? Ctrl->I.inc[1] : source_inc[1];
	tolerance_x = 64.0 * DBL_EPSILON *
	              MAX(1.0, MAX(fabs(cube->coordinate[ELYGTL_X][0]),
	                           fabs(cube->coordinate[ELYGTL_X][source_nx - 1])));
	if (wesn[XLO] < cube->coordinate[ELYGTL_X][0] - tolerance_x ||
	    wesn[XHI] > cube->coordinate[ELYGTL_X][source_nx - 1] + tolerance_x) {
		GMT_Report(GMT->parent, GMT_MSG_ERROR,
		           "Option -R x range must remain within %.12g/%.12g\n",
		           cube->coordinate[ELYGTL_X][0],
		           cube->coordinate[ELYGTL_X][source_nx - 1]);
		return GMT_RUNTIME_ERROR;
	}
	tolerance_y = 64.0 * DBL_EPSILON *
	              MAX(1.0, MAX(fabs(cube->coordinate[ELYGTL_Y][0]),
	                           fabs(cube->coordinate[ELYGTL_Y][source_ny - 1])));
	if (wesn[YLO] < cube->coordinate[ELYGTL_Y][0] - tolerance_y ||
	    wesn[YHI] > cube->coordinate[ELYGTL_Y][source_ny - 1] + tolerance_y) {
		GMT_Report(GMT->parent, GMT_MSG_ERROR,
		           "Option -R y range must remain within %.12g/%.12g\n",
		           cube->coordinate[ELYGTL_Y][0],
		           cube->coordinate[ELYGTL_Y][source_ny - 1]);
		return GMT_RUNTIME_ERROR;
	}
	if (elygtl_grid_size(GMT->parent, "x", wesn[XLO], wesn[XHI], inc[0],
	                    &nx, &adjusted))
		return GMT_RUNTIME_ERROR;
	wesn[XHI] = adjusted;
	if (elygtl_grid_size(GMT->parent, "y", wesn[YLO], wesn[YHI], inc[1],
	                    &ny, &adjusted))
		return GMT_RUNTIME_ERROR;
	wesn[YHI] = adjusted;
	if (wesn[XHI] > cube->coordinate[ELYGTL_X][source_nx - 1] + tolerance_x ||
	    wesn[YHI] > cube->coordinate[ELYGTL_Y][source_ny - 1] + tolerance_y) {
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
	    !memcmp(x, cube->coordinate[ELYGTL_X], nx * sizeof(*x)) &&
	    !memcmp(y, cube->coordinate[ELYGTL_Y], ny * sizeof(*y))) {
		status = GMT_NOERROR;
		goto cleanup;
	}
	source_plane = source_nx * source_ny;
	target_plane = nx * ny;
	native = calloc(source_plane, sizeof(*native));
	sampled = calloc(target_plane, sizeof(*sampled));
	if (!native || !sampled) goto cleanup;
	for (p = 0; p < ELYGTL_N_PROPERTIES; p++) {
		if (!cube->present[p]) continue;
		resampled[p] = calloc(cube->n[ELYGTL_Z] * target_plane,
		                      sizeof(*resampled[p]));
		if (!resampled[p]) goto cleanup;
		for (iz = 0; iz < cube->n[ELYGTL_Z]; iz++) {
			for (iy = 0; iy < source_ny; iy++)
				for (ix = 0; ix < source_nx; ix++)
					native[iy * source_nx + ix] =
					    job->value[p][iz * source_plane + iy * source_nx + ix];
			status = elygtl_horizontal_bcr(
			    GMT, cube->coordinate[ELYGTL_X], source_nx,
			    cube->coordinate[ELYGTL_Y], source_ny, native,
			    x, nx, y, ny, sampled);
			if (status != GMT_NOERROR) goto cleanup;
			memcpy(&resampled[p][iz * target_plane], sampled,
			       target_plane * sizeof(*sampled));
		}
	}
	for (p = 0; p < ELYGTL_N_PROPERTIES; p++) {
		if (!cube->present[p]) continue;
		free(job->value[p]);
		job->value[p] = resampled[p];
		resampled[p] = NULL;
	}
	free(cube->coordinate[ELYGTL_X]);
	free(cube->coordinate[ELYGTL_Y]);
	cube->coordinate[ELYGTL_X] = x;
	cube->coordinate[ELYGTL_Y] = y;
	x = y = NULL;
	cube->n[ELYGTL_X] = nx;
	cube->n[ELYGTL_Y] = ny;
	cube->reverse[ELYGTL_X] = cube->reverse[ELYGTL_Y] = false;
	cube->horizontal_resampled = true;
	job->plane = target_plane;
	job->total = target_plane * cube->n[ELYGTL_Z];
	GMT_Report(GMT->parent, GMT_MSG_INFORMATION,
	           "Resampled model horizontally to %zu by %zu nodes\n", nx, ny);
	status = GMT_NOERROR;

cleanup:
	for (p = 0; p < ELYGTL_N_PROPERTIES; p++) free(resampled[p]);
	free(x);
	free(y);
	free(native);
	free(sampled);
	return status;
}

static int elygtl_interpolate_run(struct GMT_CTRL *GMT,
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

static int elygtl_interpolate_finite(struct GMT_CTRL *GMT,
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
			status = elygtl_interpolate_run(
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
				status = elygtl_interpolate_run(
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
			return elygtl_interpolate_run(
			    GMT, bridge_x, bridge_value, run_count,
			    target, n_target, output, fit, mode);
	}
	return GMT_NOERROR;
}

static int elygtl_resample_vertical(struct GMT_CTRL *GMT,
	                                const struct ELYGTL_CTRL *Ctrl,
	                                struct ELYGTL_JOB *job)
{
	struct ELYGTL_CUBE *cube = &job->cube;
	const double *source_z = cube->coordinate[ELYGTL_Z];
	size_t source_nz = cube->n[ELYGTL_Z], target_nz, column, iz;
	double *target_z = NULL, *trace = NULL, *sampled = NULL;
	double *bridge_x = NULL, *bridge_value = NULL;
	double *resampled[ELYGTL_N_PROPERTIES] = {NULL, NULL, NULL};
	double tolerance;
	bool lattice_changed = false;
	int property, status = GMT_MEMORY_ERROR;

	if (!Ctrl->T.active && !Ctrl->S.bridge) return GMT_NOERROR;
	target_nz = Ctrl->T.active ? Ctrl->T.n : source_nz;
	target_z = calloc(target_nz, sizeof(*target_z));
	trace = calloc(source_nz, sizeof(*trace));
	sampled = calloc(target_nz, sizeof(*sampled));
	if (Ctrl->S.bridge) {
		bridge_x = calloc(source_nz, sizeof(*bridge_x));
		bridge_value = calloc(source_nz, sizeof(*bridge_value));
	}
	if (!target_z || !trace || !sampled ||
	    (Ctrl->S.bridge && (!bridge_x || !bridge_value)))
		goto cleanup;
	if (Ctrl->T.active) {
		for (iz = 0; iz < target_nz; iz++)
			target_z[iz] = Ctrl->T.min + (double)iz * Ctrl->T.inc;
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
	else
		memcpy(target_z, source_z, source_nz * sizeof(*target_z));
	if (target_nz != source_nz)
		lattice_changed = true;
	else {
		for (iz = 0; iz < source_nz; iz++)
			if (target_z[iz] != source_z[iz]) {
				lattice_changed = true;
				break;
			}
	}
	for (property = 0; property < ELYGTL_N_PROPERTIES; property++) {
		if (!cube->present[property]) continue;
		resampled[property] = calloc(target_nz * job->plane,
		                              sizeof(*resampled[property]));
		if (!resampled[property]) goto cleanup;
		for (column = 0; column < job->plane; column++) {
			for (iz = 0; iz < source_nz; iz++)
				trace[iz] = job->value[property][iz * job->plane + column];
			status = elygtl_interpolate_finite(
			    GMT, source_z, trace, source_nz, target_z, target_nz, sampled,
			    Ctrl->S.fit, Ctrl->S.mode, Ctrl->S.bridge, Ctrl->S.max_gap,
			    bridge_x, bridge_value);
			if (status != GMT_NOERROR) goto cleanup;
			for (iz = 0; iz < target_nz; iz++)
				resampled[property][iz * job->plane + column] = sampled[iz];
		}
	}
	for (property = 0; property < ELYGTL_N_PROPERTIES; property++) {
		if (!cube->present[property]) continue;
		free(job->value[property]);
		job->value[property] = resampled[property];
		resampled[property] = NULL;
	}
	free(cube->coordinate[ELYGTL_Z]);
	cube->coordinate[ELYGTL_Z] = target_z;
	target_z = NULL;
	cube->n[ELYGTL_Z] = target_nz;
	cube->vertical_resampled = lattice_changed;
	job->total = job->plane * target_nz;
	GMT_Report(GMT->parent, GMT_MSG_INFORMATION,
	           "%s model vertically on %zu levels from %.12g to %.12g\n",
	           lattice_changed ? "Resampled" : "Processed",
	           target_nz, cube->coordinate[ELYGTL_Z][0],
	           cube->coordinate[ELYGTL_Z][target_nz - 1]);
	status = GMT_NOERROR;

cleanup:
	for (property = 0; property < ELYGTL_N_PROPERTIES; property++)
		free(resampled[property]);
	free(target_z);
	free(trace);
	free(sampled);
	free(bridge_x);
	free(bridge_value);
	return status;
}

static bool elygtl_is_geographic(const struct ELYGTL_CUBE *cube)
{
	char *x_units = cube->transform.axis_unit[ELYGTL_X]
	              ? strdup(cube->transform.axis_unit[ELYGTL_X])
	              : elygtl_text_attribute(
	                    cube->ncid, cube->coordinate_varid[ELYGTL_X], "units");
	char *y_units = cube->transform.axis_unit[ELYGTL_Y]
	              ? strdup(cube->transform.axis_unit[ELYGTL_Y])
	              : elygtl_text_attribute(
	                    cube->ncid, cube->coordinate_varid[ELYGTL_Y], "units");
	bool geographic =
	    ((!strcasecmp(cube->coordinate_name[ELYGTL_X], "lon") ||
	      !strcasecmp(cube->coordinate_name[ELYGTL_X], "longitude")) &&
	     (!strcasecmp(cube->coordinate_name[ELYGTL_Y], "lat") ||
	      !strcasecmp(cube->coordinate_name[ELYGTL_Y], "latitude")));
	if (x_units && y_units &&
	    strstr(x_units, "degree") && strstr(y_units, "degree"))
		geographic = true;
	free(x_units);
	free(y_units);
	return geographic;
}

static const char *elygtl_gap_method_name(char method)
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

static int elygtl_write_xyz(struct GMT_CTRL *GMT, struct GMT_GRID *Grid,
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

static int elygtl_fill_horizontal_layer(struct GMT_CTRL *GMT,
	                                    const struct ELYGTL_CTRL *Ctrl,
	                                    const struct ELYGTL_CUBE *cube,
	                                    size_t iz, double *values,
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
	size_t nx = cube->n[ELYGTL_X], ny = cube->n[ELYGTL_Y];
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
			if (visited[node] || isfinite(values[iz * nxy + node])) continue;
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
						if (visited[next] || isfinite(values[iz * nxy + next]))
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
	wesn[XLO] = cube->coordinate[ELYGTL_X][0];
	wesn[XHI] = cube->coordinate[ELYGTL_X][nx - 1];
	wesn[YLO] = cube->coordinate[ELYGTL_Y][0];
	wesn[YHI] = cube->coordinate[ELYGTL_Y][ny - 1];
	if (!elygtl_regular_coordinate(cube->coordinate[ELYGTL_X], nx, &inc[0]) ||
	    !elygtl_regular_coordinate(cube->coordinate[ELYGTL_Y], ny, &inc[1])) {
		GMT_Report(GMT->parent, GMT_MSG_ERROR,
		           "Option -H requires regular x and y coordinates in %s\n",
		           cube->path);
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
			Grid->data[gmt_M_ijp(Grid->header, row, ix)] =
			    (gmt_grdfloat)values[iz * nxy + iy * nx + ix];
	}
	if (gmt_get_tempname(GMT->parent, "elygtl_gap_input", ".nc", input) ||
	    gmt_get_tempname(GMT->parent, "elygtl_gap_candidate", ".nc", candidate) ||
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
		const char *geographic = elygtl_is_geographic(cube) ? "-fg" : "";
		double radius = Ctrl->H.argument * MAX(inc[0], inc[1]);
		unsigned int minimum_sectors = MAX(1U, (Ctrl->H.sectors + 1U) / 2U);
		if (gmt_get_tempname(GMT->parent, "elygtl_gap_points", ".txt", xyz) ||
		    elygtl_write_xyz(GMT, Grid, xyz) != GMT_NOERROR) {
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

static int elygtl_fill_horizontal_gaps(struct GMT_CTRL *GMT,
	                                   const struct ELYGTL_CTRL *Ctrl,
	                                   struct ELYGTL_JOB *job)
{
	int property;
	for (property = 0; property < ELYGTL_N_PROPERTIES; property++) {
		size_t iz, holes = 0, nodes = 0;
		int status;
		if (!job->cube.present[property]) continue;
		for (iz = 0; iz < job->cube.n[ELYGTL_Z]; iz++) {
			status = elygtl_fill_horizontal_layer(
			    GMT, Ctrl, &job->cube, iz, job->value[property], &holes, &nodes);
			if (status != GMT_NOERROR) {
				GMT_Report(GMT->parent, GMT_MSG_ERROR,
				           "Unable to fill horizontal gaps in %s?%s at z=%.12g "
				           "using %s interpolation\n",
				           job->cube.path, job->cube.field[property].name,
				           job->cube.coordinate[ELYGTL_Z][iz],
				           elygtl_gap_method_name(Ctrl->H.method));
				return status;
			}
		}
		GMT_Report(GMT->parent, GMT_MSG_INFORMATION,
		           "Filled %zu nodes in %zu internal horizontal gap%s in %s?%s "
		           "using %s interpolation\n",
		           nodes, holes, holes == 1 ? "" : "s", job->cube.path,
		           job->cube.field[property].name,
		           elygtl_gap_method_name(Ctrl->H.method));
	}
	return GMT_NOERROR;
}

static int elygtl_gshhg_mask(struct GMT_CTRL *GMT,
                             const struct ELYGTL_CTRL *Ctrl,
                             const struct ELYGTL_CUBE *cube,
                             signed char *classification)
{
	struct GMT_GRID *Grid = NULL;
	char virtual_file[GMT_VF_LEN] = {0}, command[GMT_LEN512];
	double dx, dy;
	size_t row, col;
	int status;
	if (Ctrl->D.resolution == 'n' || !elygtl_is_geographic(cube) ||
	    !elygtl_regular_coordinate(cube->coordinate[ELYGTL_X],
	                               cube->n[ELYGTL_X], &dx) ||
	    !elygtl_regular_coordinate(cube->coordinate[ELYGTL_Y],
	                               cube->n[ELYGTL_Y], &dy))
		return GMT_NOTSET;
	if (GMT_Open_VirtualFile(GMT->parent, GMT_IS_GRID, GMT_IS_SURFACE,
	                         GMT_OUT, NULL, virtual_file))
		return GMT_NOTSET;
	snprintf(command, sizeof(command),
	         "-G%s -R%.16g/%.16g/%.16g/%.16g -I%.16g/%.16g -D%c "
	         "-A%s -N0/1/2/3/4 --GMT_HISTORY=false",
	         virtual_file,
	         cube->coordinate[ELYGTL_X][0],
	         cube->coordinate[ELYGTL_X][cube->n[ELYGTL_X] - 1],
	         cube->coordinate[ELYGTL_Y][0],
	         cube->coordinate[ELYGTL_Y][cube->n[ELYGTL_Y] - 1],
	         dx, dy, Ctrl->D.resolution, Ctrl->A.selection);
	status = GMT_Call_Module(GMT->parent, "grdlandmask", GMT_MODULE_CMD, command);
	if (status != GMT_NOERROR) {
		GMT_Close_VirtualFile(GMT->parent, virtual_file);
		GMT_Report(GMT->parent, GMT_MSG_WARNING,
		           "GMT shoreline classification was unavailable; "
		           "treating finite Vs30 columns as dry\n");
		return GMT_NOTSET;
	}
	Grid = GMT_Read_VirtualFile(GMT->parent, virtual_file);
	if (Grid == NULL || gmt_grd_BC_set(GMT, Grid, GMT_IN) != GMT_NOERROR) {
		GMT_Close_VirtualFile(GMT->parent, virtual_file);
		return GMT_NOTSET;
	}
	for (row = 0; row < cube->n[ELYGTL_Y]; row++)
		for (col = 0; col < cube->n[ELYGTL_X]; col++) {
			double value = gmt_bcr_get_z(
			    GMT, Grid, cube->coordinate[ELYGTL_X][col],
			    cube->coordinate[ELYGTL_Y][row]);
			size_t k = row * cube->n[ELYGTL_X] + col;
			if (!isfinite(value))
				classification[k] = ELYGTL_UNRESOLVED;
			else {
				int level = (int)lrint(value);
				classification[k] = level == 0 || level == 2 || level == 4
				                  ? ELYGTL_WET : ELYGTL_LAND;
			}
		}
	GMT_Close_VirtualFile(GMT->parent, virtual_file);
	GMT_Report(GMT->parent, GMT_MSG_INFORMATION,
	           "Used GMT shoreline data as a wet/land classification prior\n");
	return GMT_NOERROR;
}

static const char *elygtl_evidence_name(enum ELYGTL_EVIDENCE evidence)
{
	switch (evidence) {
		case ELYGTL_EVIDENCE_VP: return "model Vp";
		case ELYGTL_EVIDENCE_VS: return "model Vs";
		case ELYGTL_EVIDENCE_RHO: return "model density";
		default: return "Vs30";
	}
}

static signed char elygtl_column_evidence(const struct ELYGTL_CTRL *Ctrl,
	                                      const struct ELYGTL_JOB *job,
	                                      size_t column)
{
	const struct ELYGTL_CUBE *cube = &job->cube;
	size_t iz;
	double z_tolerance, water, tolerance, value, surface;
	int property;

	if (Ctrl->M.evidence == ELYGTL_EVIDENCE_VS30)
		return !isfinite(job->vs30[column]) ? ELYGTL_UNRESOLVED
		     : job->vs30[column] > 0.0 ? ELYGTL_LAND : ELYGTL_WET;
	property = (int)Ctrl->M.evidence;
	for (iz = 0; iz < cube->n[ELYGTL_Z]; iz++)
		if (isfinite(job->value[property][iz * job->plane + column])) break;
	if (iz == cube->n[ELYGTL_Z]) return ELYGTL_UNRESOLVED;
	surface = cube->coordinate[ELYGTL_Z][iz];
	z_tolerance = 64.0 * DBL_EPSILON *
	              MAX(1.0, MAX(fabs(cube->coordinate[ELYGTL_Z][0]),
	                           fabs(cube->coordinate[ELYGTL_Z]
	                                [cube->n[ELYGTL_Z] - 1])));
	if (surface < -z_tolerance) return ELYGTL_LAND;
	if (surface > z_tolerance) return ELYGTL_WET;
	value = job->value[property][iz * job->plane + column];
	water = Ctrl->M.water * (property == ELYGTL_RHO
	                         ? Ctrl->U.density : Ctrl->U.velocity);
	tolerance = Ctrl->M.tolerance * (property == ELYGTL_RHO
	                                 ? Ctrl->U.density : Ctrl->U.velocity);
	return fabs(value - water) <= tolerance ? ELYGTL_WET : ELYGTL_LAND;
}

static int elygtl_prepare_horizontal(struct GMT_CTRL *GMT,
                                     const struct ELYGTL_CTRL *Ctrl,
                                     struct ELYGTL_JOB *job)
{
	signed char *prior = NULL;
	size_t k, conflicts = 0, unresolved = 0;
	int status;
	job->vs30 = calloc(job->plane, sizeof(*job->vs30));
	job->transition = calloc(job->plane, sizeof(*job->transition));
	job->wet = calloc(job->plane, sizeof(*job->wet));
	job->classified = calloc(job->plane, sizeof(*job->classified));
	if (!job->vs30 || !job->transition || !job->wet || !job->classified)
		return GMT_MEMORY_ERROR;
	status = elygtl_sample_grid(GMT, Ctrl->In.file[1], &job->cube, true,
	                            job->vs30);
	if (status != GMT_NOERROR) return status;
	if (Ctrl->E.grid) {
		status = elygtl_sample_grid(GMT, Ctrl->E.file, &job->cube, true,
		                            job->transition);
		if (status != GMT_NOERROR) return status;
	}
	else
		for (k = 0; k < job->plane; k++) job->transition[k] = Ctrl->E.depth;
	prior = calloc(job->plane, sizeof(*prior));
	if (!prior) return GMT_MEMORY_ERROR;
	if (Ctrl->M.mode == ELYGTL_CLASS_ALL_LAND ||
	    Ctrl->M.mode == ELYGTL_CLASS_ALL_WET) {
		signed char value = Ctrl->M.mode == ELYGTL_CLASS_ALL_LAND
		                  ? ELYGTL_LAND : ELYGTL_WET;
		for (k = 0; k < job->plane; k++) prior[k] = value;
	}
	else if (Ctrl->K.active) {
		double *mask = calloc(job->plane, sizeof(*mask));
		if (!mask) {
			free(prior);
			return GMT_MEMORY_ERROR;
		}
		status = elygtl_sample_grid(GMT, Ctrl->K.file, &job->cube, false, mask);
		if (status == GMT_NOERROR)
			for (k = 0; k < job->plane; k++) {
				if (!isfinite(mask[k]))
					prior[k] = ELYGTL_UNRESOLVED;
				else
					prior[k] = mask[k] < 0.5 ? ELYGTL_WET : ELYGTL_LAND;
			}
		free(mask);
		if (status != GMT_NOERROR) {
			free(prior);
			return status;
		}
	}
	else if (elygtl_gshhg_mask(GMT, Ctrl, &job->cube, prior) != GMT_NOERROR)
		GMT_Report(GMT->parent, GMT_MSG_WARNING,
		           "GMT shoreline classification is unavailable; %s evidence "
		           "will be used where it resolves a class\n",
		           elygtl_evidence_name(Ctrl->M.evidence));
	for (k = 0; k < job->plane; k++) {
		signed char model = elygtl_column_evidence(Ctrl, job, k);
		signed char selected;
		bool authoritative = Ctrl->K.active ||
		    Ctrl->M.mode == ELYGTL_CLASS_ALL_LAND ||
		    Ctrl->M.mode == ELYGTL_CLASS_ALL_WET;
		if (prior[k] != ELYGTL_UNRESOLVED && model != ELYGTL_UNRESOLVED &&
		    prior[k] != model)
			conflicts++;
		if (authoritative)
			selected = prior[k];
		else if (Ctrl->M.mode == ELYGTL_CLASS_MODEL &&
		         model != ELYGTL_UNRESOLVED)
			selected = model;
		else if (prior[k] != ELYGTL_UNRESOLVED)
			selected = prior[k];
		else
			selected = model;
		if (selected == ELYGTL_UNRESOLVED) unresolved++;
		job->classified[k] = selected != ELYGTL_UNRESOLVED;
		job->wet[k] = selected == ELYGTL_WET;
		if (job->wet[k]) job->wet_columns++;
	}
	if (conflicts)
		GMT_Report(GMT->parent, GMT_MSG_WARNING,
		           "%s evidence disagrees with the classification prior in %zu "
		           "columns; %s classification takes precedence\n",
		           elygtl_evidence_name(Ctrl->M.evidence), conflicts,
		           Ctrl->M.mode == ELYGTL_CLASS_MODEL && !Ctrl->K.active
		               ? "model" : "prior");
	if (unresolved)
		GMT_Report(GMT->parent, GMT_MSG_WARNING,
		           "Wet/land classification remains unresolved in %zu columns; "
		           "those columns are left unchanged\n", unresolved);
	free(prior);
	return GMT_NOERROR;
}

static double elygtl_brocher_vp(double vs)
{
	double s, vp;
	if (!isfinite(vs) || vs <= 0.0 || vs > 4500.0) return NAN;
	s = vs * 0.001;
	vp = 0.9409 + s * (2.0947 - s * (0.8206 - s * (0.2683 - s * 0.0251)));
	return vp > 0.0 && isfinite(vp) ? vp * 1000.0 : NAN;
}

static double elygtl_brocher_vs(double vp)
{
	double p, vs;
	if (!isfinite(vp) || vp < 1500.0 || vp > 8500.0) return NAN;
	p = vp * 0.001;
	vs = 0.7858 - 1.2344 * p + 0.7949 * p * p -
	     0.1238 * p * p * p + 0.0064 * p * p * p * p;
	return vs > 0.0 && isfinite(vs) ? vs * 1000.0 : NAN;
}

static double elygtl_nafe_drake(double vp)
{
	double p, rho;
	if (!isfinite(vp) || vp <= 0.0) return NAN;
	p = vp * 0.001;
	rho = p * (1.6612 - p * (0.4721 - p *
	      (0.0671 - p * (0.0043 - p * 0.000106))));
	if (!isfinite(rho) || rho <= 0.0) return NAN;
	return MAX(1000.0, rho * 1000.0);
}

static double elygtl_anchor(struct GMT_CTRL *GMT,
	                        const struct ELYGTL_CTRL *Ctrl,
	                        const struct ELYGTL_JOB *job, int property,
	                        size_t column, double target,
	                        double *trace, double *bridge_x,
	                        double *bridge_value)
{
	const double *z = job->cube.coordinate[ELYGTL_Z];
	const double *value = job->value[property];
	size_t nz = job->cube.n[ELYGTL_Z], iz;
	double output = NAN;
	double tolerance = 64.0 * DBL_EPSILON *
	                   MAX(1.0, MAX(fabs(target), fabs(z[nz - 1])));
	if (!value || target < z[0] - tolerance || target > z[nz - 1] + tolerance)
		return NAN;
	for (iz = 0; iz < nz; iz++)
		trace[iz] = value[iz * job->plane + column];
	if (elygtl_interpolate_finite(
	        GMT, z, trace, nz, &target, 1, &output,
	        Ctrl->S.fit, Ctrl->S.mode, Ctrl->S.bridge, Ctrl->S.max_gap,
	        bridge_x, bridge_value) != GMT_NOERROR)
		return NAN;
	return output;
}

static double elygtl_profile(double q, double base, double surface)
{
	const double a = 0.5, b = 2.0 / 3.0, c = 1.5;
	double f = q + b * (q - q * q);
	double g = a - a * q + c * (q * q + 2.0 * sqrt(q) - 3.0 * q);
	return f * base + g * surface;
}

static int elygtl_find_surface(const struct ELYGTL_JOB *job, size_t column,
                               int authority, double *surface)
{
	size_t iz;
	for (iz = 0; iz < job->cube.n[ELYGTL_Z]; iz++)
		if (isfinite(job->value[authority][iz * job->plane + column])) {
			*surface = job->cube.coordinate[ELYGTL_Z][iz];
			return GMT_NOERROR;
		}
	return GMT_NOTSET;
}

static void elygtl_fill_created(struct ELYGTL_JOB *job,
                                const struct ELYGTL_CTRL *Ctrl)
{
	size_t column, iz;
	for (column = 0; column < job->plane; column++) {
		if (!job->classified[column] || job->wet[column]) continue;
		for (iz = 0; iz < job->cube.n[ELYGTL_Z]; iz++) {
			size_t k = iz * job->plane + column;
			double vp = NAN, vs = NAN;
			if (job->cube.present[ELYGTL_VP]) vp = job->value[ELYGTL_VP][k];
			if (job->cube.present[ELYGTL_VS]) vs = job->value[ELYGTL_VS][k];
			if (!isfinite(vp) && isfinite(vs)) vp = elygtl_brocher_vp(vs);
			if (!isfinite(vs) && isfinite(vp)) vs = elygtl_brocher_vs(vp);
			if (Ctrl->C.active[ELYGTL_VP])
				job->created[ELYGTL_VP][k] = vp;
			if (Ctrl->C.active[ELYGTL_VS])
				job->created[ELYGTL_VS][k] = vs;
			if (Ctrl->C.active[ELYGTL_RHO])
				job->created[ELYGTL_RHO][k] = elygtl_nafe_drake(vp);
		}
	}
}

static int elygtl_apply(struct GMT_CTRL *GMT, const struct ELYGTL_CTRL *Ctrl,
                        struct ELYGTL_JOB *job)
{
	int authority = job->cube.present[ELYGTL_VS] ? ELYGTL_VS : ELYGTL_VP;
	const double *z = job->cube.coordinate[ELYGTL_Z];
	size_t column, iz;
	double *surface_at = calloc(job->plane, sizeof(*surface_at));
	double *vp_at = calloc(job->plane, sizeof(*vp_at));
	double *vs_at = calloc(job->plane, sizeof(*vs_at));
	double *trace = calloc(job->cube.n[ELYGTL_Z], sizeof(*trace));
	double *bridge_x = Ctrl->S.bridge
	                 ? calloc(job->cube.n[ELYGTL_Z], sizeof(*bridge_x)) : NULL;
	double *bridge_value = Ctrl->S.bridge
	                     ? calloc(job->cube.n[ELYGTL_Z], sizeof(*bridge_value))
	                     : NULL;
	bool *applied = calloc(job->plane, sizeof(*applied));
	int status = GMT_NOERROR;
	if (!surface_at || !vp_at || !vs_at || !trace || !applied ||
	    (Ctrl->S.bridge && (!bridge_x || !bridge_value))) {
		status = GMT_MEMORY_ERROR;
		goto cleanup;
	}
	for (column = 0; column < job->plane; column++) {
		surface_at[column] = NAN;
		vp_at[column] = NAN;
		vs_at[column] = NAN;
	}
	for (column = 0; column < job->plane; column++) {
		double surface, depth, transition_z;
		double vp_anchor = NAN, vs_anchor = NAN, vp30, vs30;
		bool usable_vp, usable_vs;
		if (!job->classified[column] || job->wet[column]) continue;
		if (!isfinite(job->vs30[column]) || job->vs30[column] <= 0.0) {
			job->missing_vs30++;
			continue;
		}
		if (!isfinite(job->transition[column])) {
			job->missing_transition++;
			continue;
		}
		if (job->transition[column] <= 0.0) {
			GMT_Report(GMT->parent, GMT_MSG_ERROR,
			           "Transition depth must be positive at column %zu\n",
			           column);
			status = GMT_RUNTIME_ERROR;
			goto cleanup;
		}
		if (elygtl_find_surface(job, column, authority, &surface))
			continue;
		depth = job->transition[column];
		transition_z = surface + depth;
		if (transition_z > z[job->cube.n[ELYGTL_Z] - 1]) {
			GMT_Report(GMT->parent, GMT_MSG_ERROR,
			           "Transition depth extends below the model at column %zu\n",
			           column);
			status = GMT_RUNTIME_ERROR;
			goto cleanup;
		}
		if (job->cube.present[ELYGTL_VP])
			vp_anchor = elygtl_anchor(GMT, Ctrl, job, ELYGTL_VP, column,
			                          transition_z, trace, bridge_x, bridge_value);
		if (job->cube.present[ELYGTL_VS])
			vs_anchor = elygtl_anchor(GMT, Ctrl, job, ELYGTL_VS, column,
			                          transition_z, trace, bridge_x, bridge_value);
		if (!isfinite(vp_anchor) && isfinite(vs_anchor))
			vp_anchor = elygtl_brocher_vp(vs_anchor);
		if (!isfinite(vs_anchor) && isfinite(vp_anchor))
			vs_anchor = elygtl_brocher_vs(vp_anchor);
		usable_vp = isfinite(vp_anchor);
		usable_vs = isfinite(vs_anchor);
		if (!usable_vp && !usable_vs) {
			job->missing_anchor++;
			continue;
		}
		surface_at[column] = surface;
		vp_at[column] = vp_anchor;
		vs_at[column] = vs_anchor;
		applied[column] = true;
		vs30 = job->vs30[column];
		vp30 = elygtl_brocher_vp(vs30);
		for (iz = 0; iz < job->cube.n[ELYGTL_Z]; iz++) {
			size_t k = iz * job->plane + column;
			double d = z[iz] - surface, q, vp = NAN, vs = NAN;
			if (d < 0.0 || d > depth) continue;
			q = MIN(1.0, MAX(0.0, d / depth));
			if (usable_vs) vs = elygtl_profile(q, vs_anchor, vs30);
			if (usable_vp && isfinite(vp30))
				vp = elygtl_profile(q, vp_anchor, vp30);
			if (job->cube.present[ELYGTL_VS] && isfinite(vs))
				job->value[ELYGTL_VS][k] = vs;
			if (job->cube.present[ELYGTL_VP] && isfinite(vp))
				job->value[ELYGTL_VP][k] = vp;
			if (job->cube.present[ELYGTL_RHO] && isfinite(vp))
				job->value[ELYGTL_RHO][k] = elygtl_nafe_drake(vp);
		}
		job->modified_columns++;
	}
	elygtl_fill_created(job, Ctrl);
	/* Replace empirical values inside the GTL with the independent Ely profiles. */
	for (column = 0; column < job->plane; column++) {
		double surface, depth, vp_anchor, vs_anchor;
		double vp30;
		if (!applied[column]) continue;
		surface = surface_at[column];
		depth = job->transition[column];
		vp_anchor = vp_at[column];
		vs_anchor = vs_at[column];
		vp30 = elygtl_brocher_vp(job->vs30[column]);
		for (iz = 0; iz < job->cube.n[ELYGTL_Z]; iz++) {
			size_t k = iz * job->plane + column;
			double d = z[iz] - surface, q, vp = NAN;
			if (d < 0.0 || d > depth) continue;
			q = MIN(1.0, MAX(0.0, d / depth));
			if (Ctrl->C.active[ELYGTL_VP] && isfinite(vp_anchor) && isfinite(vp30))
				job->created[ELYGTL_VP][k] =
				    elygtl_profile(q, vp_anchor, vp30);
			if (Ctrl->C.active[ELYGTL_VS] && isfinite(vs_anchor))
				job->created[ELYGTL_VS][k] =
				    elygtl_profile(q, vs_anchor, job->vs30[column]);
			if (job->cube.present[ELYGTL_VP])
				vp = job->value[ELYGTL_VP][k];
			else if (Ctrl->C.active[ELYGTL_VP])
				vp = job->created[ELYGTL_VP][k];
			else if (isfinite(vp_anchor) && isfinite(vp30))
				vp = elygtl_profile(q, vp_anchor, vp30);
			if (Ctrl->C.active[ELYGTL_RHO])
				job->created[ELYGTL_RHO][k] = elygtl_nafe_drake(vp);
		}
	}
	for (column = 0; column < job->plane; column++) {
		if (!job->classified[column] || job->wet[column]) continue;
		for (iz = 0; iz < job->cube.n[ELYGTL_Z]; iz++) {
			size_t k = iz * job->plane + column;
			if (!isfinite(job->value[authority][k])) continue;
			if (Ctrl->C.active[ELYGTL_VP] &&
			    !isfinite(job->created[ELYGTL_VP][k]))
				job->empirical_skips[ELYGTL_VP]++;
			if (Ctrl->C.active[ELYGTL_VS] &&
			    !isfinite(job->created[ELYGTL_VS][k]))
				job->empirical_skips[ELYGTL_VS]++;
			if (Ctrl->C.active[ELYGTL_RHO] &&
			    !isfinite(job->created[ELYGTL_RHO][k]))
				job->empirical_skips[ELYGTL_RHO]++;
		}
	}
cleanup:
	free(surface_at);
	free(vp_at);
	free(vs_at);
	free(trace);
	free(bridge_x);
	free(bridge_value);
	free(applied);
	return status;
}

static bool elygtl_skip_attribute(const char *name)
{
	return !strcmp(name, "_FillValue") || !strcmp(name, "missing_value") ||
	       !strcmp(name, "scale_factor") || !strcmp(name, "add_offset");
}

static int elygtl_copy_attributes(int input, int input_var,
                                  int output, int output_var, bool unpacked)
{
	int natts, k;
	if (nc_inq_varnatts(input, input_var, &natts) != NC_NOERR)
		return NC_EINVAL;
	for (k = 0; k < natts; k++) {
		char name[NC_MAX_NAME + 1];
		if (nc_inq_attname(input, input_var, k, name) != NC_NOERR)
			return NC_EINVAL;
		if (unpacked && elygtl_skip_attribute(name)) continue;
		if (nc_copy_att(input, input_var, name, output, output_var) != NC_NOERR)
			return NC_EINVAL;
	}
	return NC_NOERR;
}

static int elygtl_mapped_property(const struct ELYGTL_CUBE *cube, int varid)
{
	int p;
	for (p = 0; p < ELYGTL_N_PROPERTIES; p++)
		if (cube->present[p] && cube->field[p].varid == varid) return p;
	return -1;
}

static int elygtl_coordinate(const struct ELYGTL_CUBE *cube, int varid)
{
	int axis;
	for (axis = 0; axis < 3; axis++)
		if (cube->coordinate_varid[axis] == varid) return axis;
	return -1;
}

static int elygtl_replace_units(int ncid, int varid, const char *units,
                                bool invalidate)
{
	if (units) {
		nc_del_att(ncid, varid, "units");
		return nc_put_att_text(ncid, varid, "units", strlen(units), units);
	}
	if (invalidate) nc_del_att(ncid, varid, "units");
	return NC_NOERR;
}

static int elygtl_copy_variable(const struct ELYGTL_CUBE *cube,
                                int input_varid, int output_ncid,
                                int output_varid, nc_type type, size_t count)
{
	int ndims, dimids[NC_MAX_VAR_DIMS], position;
	size_t type_size, length[NC_MAX_VAR_DIMS], target;
	bool reverse[NC_MAX_VAR_DIMS] = {false}, reordered = false;
	void *data, *ordered = NULL;
	int status;
	if (nc_inq_type(cube->ncid, type, NULL, &type_size) != NC_NOERR)
		return NC_EINVAL;
	if (nc_inq_var(cube->ncid, input_varid, NULL, NULL, &ndims,
	               dimids, NULL) != NC_NOERR)
		return NC_EINVAL;
	for (position = 0; position < ndims; position++) {
		int axis;
		if (nc_inq_dimlen(cube->ncid, dimids[position], &length[position])
		    != NC_NOERR)
			return NC_EINVAL;
		for (axis = 0; axis < 3; axis++)
			if (dimids[position] == cube->axis_dimid[axis] && cube->reverse[axis]) {
				reverse[position] = true;
				reordered = true;
			}
	}
	data = calloc(count, type_size);
	if (!data) return NC_ENOMEM;
	status = nc_get_var(cube->ncid, input_varid, data);
	if (status == NC_NOERR && reordered) {
		ordered = calloc(count, type_size);
		if (!ordered) status = NC_ENOMEM;
		for (target = 0; status == NC_NOERR && target < count; target++) {
			size_t remainder = target, source = 0, stride = 1;
			for (position = ndims - 1; position >= 0; position--) {
				size_t index = remainder % length[position];
				remainder /= length[position];
				if (reverse[position]) index = length[position] - 1 - index;
				source += index * stride;
				stride *= length[position];
			}
			memcpy((char *)ordered + target * type_size,
			       (char *)data + source * type_size, type_size);
		}
	}
	if (status == NC_NOERR)
		status = nc_put_var(output_ncid, output_varid,
		                    reordered ? ordered : data);
	if (type == NC_STRING && status == NC_NOERR)
		nc_free_string(count, (char **)data);
	free(ordered);
	free(data);
	return status;
}

static int elygtl_create_output(struct GMT_CTRL *GMT,
                                const struct ELYGTL_CTRL *Ctrl,
                                const struct ELYGTL_JOB *job,
                                struct ELYGTL_OUTPUT *output)
{
	const struct ELYGTL_CUBE *cube = &job->cube;
	int ndims, nvars, ngatts, nunlim = 0, *unlim = NULL;
	int *dimid = NULL, varid, k;
	float fill = NAN;
	memset(output, 0, sizeof(*output));
	output->ncid = -1;
	for (k = 0; k < ELYGTL_N_PROPERTIES; k++) output->created_varid[k] = -1;
	if (nc_inq(cube->ncid, &ndims, &nvars, &ngatts, NULL) != NC_NOERR)
		return GMT_DATA_READ_ERROR;
	output->nvars = nvars;
	output->varid = calloc(nvars, sizeof(*output->varid));
	dimid = calloc(ndims, sizeof(*dimid));
	if (!output->varid || !dimid) goto memory;
	for (k = 0; k < nvars; k++) output->varid[k] = -1;
	nc_inq_unlimdims(cube->ncid, &nunlim, NULL);
	if (nunlim) {
		unlim = calloc(nunlim, sizeof(*unlim));
		if (!unlim || nc_inq_unlimdims(cube->ncid, &nunlim, unlim) != NC_NOERR)
			goto memory;
	}
	if (nc_create(Ctrl->G.file, NC_CLOBBER | NC_NETCDF4, &output->ncid) != NC_NOERR)
		goto netcdf;
	for (k = 0; k < ndims; k++) {
		char name[NC_MAX_NAME + 1];
		size_t length;
		int u;
		bool unlimited = false;
		if (nc_inq_dim(cube->ncid, k, name, &length) != NC_NOERR) goto netcdf;
		if (k == cube->axis_dimid[ELYGTL_X]) length = cube->n[ELYGTL_X];
		if (k == cube->axis_dimid[ELYGTL_Y]) length = cube->n[ELYGTL_Y];
		if (k == cube->axis_dimid[ELYGTL_Z]) length = cube->n[ELYGTL_Z];
		for (u = 0; u < nunlim; u++) if (unlim[u] == k) unlimited = true;
		if (nc_def_dim(output->ncid, name, unlimited ? NC_UNLIMITED : length,
		               &dimid[k]) != NC_NOERR)
			goto netcdf;
	}
	for (varid = 0; varid < nvars; varid++) {
		char name[NC_MAX_NAME + 1];
		nc_type type;
		int vndims, input_dims[NC_MAX_VAR_DIMS], output_dims[NC_MAX_VAR_DIMS];
		int p, coordinate, position;
		if (nc_inq_var(cube->ncid, varid, name, &type, &vndims,
		               input_dims, NULL) != NC_NOERR)
			goto netcdf;
		for (position = 0; position < vndims; position++)
			output_dims[position] = dimid[input_dims[position]];
		p = elygtl_mapped_property(cube, varid);
		coordinate = elygtl_coordinate(cube, varid);
		if ((cube->horizontal_resampled || cube->vertical_resampled) &&
		    p < 0 && coordinate < 0) {
			bool incompatible = false;
			for (position = 0; position < vndims; position++)
				if ((cube->horizontal_resampled &&
				     (input_dims[position] == cube->axis_dimid[ELYGTL_X] ||
				      input_dims[position] == cube->axis_dimid[ELYGTL_Y])) ||
				    (cube->vertical_resampled &&
				     input_dims[position] == cube->axis_dimid[ELYGTL_Z]))
					incompatible = true;
			if (incompatible) continue;
		}
		if (nc_def_var(output->ncid, name,
		               p >= 0 ? NC_FLOAT : (coordinate >= 0 ? NC_DOUBLE : type),
		               vndims, output_dims, &output->varid[varid]) != NC_NOERR)
			goto netcdf;
		if (p >= 0 &&
		    nc_put_att_float(output->ncid, output->varid[varid],
		                     "_FillValue", NC_FLOAT, 1, &fill) != NC_NOERR)
			goto netcdf;
		if (elygtl_copy_attributes(cube->ncid, varid, output->ncid,
		                           output->varid[varid],
		                           p >= 0 || coordinate >= 0) != NC_NOERR)
			goto netcdf;
		if (coordinate >= 0 &&
		    elygtl_replace_units(
		        output->ncid, output->varid[varid],
		        elygtl_output_axis_unit(Ctrl, cube, coordinate),
		        (cube->transform.axis_set[coordinate] &&
		         cube->transform.axis_scale[coordinate] != 1.0) ||
		        Ctrl->Z.transform.axis_scale[coordinate] != 1.0) != NC_NOERR)
			goto netcdf;
		if (coordinate == ELYGTL_Z) {
			const char *positive = Ctrl->Z.transform.axis_scale[ELYGTL_Z] < 0.0
			                     ? "up" : "down";
			nc_del_att(output->ncid, output->varid[varid], "positive");
			if (nc_put_att_text(output->ncid, output->varid[varid], "positive",
			                    strlen(positive), positive) != NC_NOERR)
				goto netcdf;
		}
		if (p >= 0) {
			if (elygtl_replace_units(
			        output->ncid, output->varid[varid],
			        elygtl_output_value_unit(Ctrl, cube, p),
			        gq_transform_value_scale(
			            &cube->transform, elygtl_transform_index(cube, p)) != 1.0 ||
			        gq_transform_value_scale(
			            &Ctrl->Z.transform,
			            elygtl_output_transform_index(Ctrl, cube, p)) != 1.0)
			    != NC_NOERR)
				goto netcdf;
		}
		if (p >= 0)
			nc_def_var_deflate(output->ncid, output->varid[varid], 1, 1, 2);
	}
	for (k = 0; k < ELYGTL_N_PROPERTIES; k++) {
		int dimensions[3], position;
		const struct ELYGTL_FIELD *reference =
		    &cube->field[cube->present[ELYGTL_VS] ? ELYGTL_VS : ELYGTL_VP];
		const char *long_name[3] =
		    {"P-wave velocity", "S-wave velocity", "density"};
		const char *units = elygtl_output_value_unit(Ctrl, cube, k);
		if (!Ctrl->C.active[k]) continue;
		if (nc_inq_varid(cube->ncid, Ctrl->C.name[k], &position) == NC_NOERR) {
			GMT_Report(GMT->parent, GMT_MSG_ERROR,
			           "Created variable %s already exists in the input\n",
			           Ctrl->C.name[k]);
			goto netcdf;
		}
		for (position = 0; position < 3; position++)
			dimensions[position] = dimid[reference->dimids[position]];
		if (nc_def_var(output->ncid, Ctrl->C.name[k], NC_FLOAT, 3,
		               dimensions, &output->created_varid[k]) != NC_NOERR ||
		    nc_put_att_float(output->ncid, output->created_varid[k],
		                     "_FillValue", NC_FLOAT, 1, &fill) != NC_NOERR)
			goto netcdf;
		nc_put_att_text(output->ncid, output->created_varid[k], "long_name",
		                strlen(long_name[k]), long_name[k]);
		if (!units && gq_transform_value_scale(
		        &Ctrl->Z.transform,
		        elygtl_output_transform_index(Ctrl, cube, k)) == 1.0 &&
		    k != ELYGTL_RHO) {
			int source = cube->present[ELYGTL_VS] ? ELYGTL_VS : ELYGTL_VP;
			if (nc_copy_att(cube->ncid, cube->field[source].varid, "units",
			                output->ncid,
			                output->created_varid[k]) != NC_NOERR)
				units = fabs(Ctrl->U.velocity - 1000.0) < 1.0e-12
				      ? "km s-1" : (fabs(Ctrl->U.velocity - 1.0) < 1.0e-12
				                       ? "m s-1" : NULL);
		}
		else if (!units && gq_transform_value_scale(
		             &Ctrl->Z.transform,
		             elygtl_output_transform_index(Ctrl, cube, k)) == 1.0)
			units = fabs(Ctrl->U.density - 1000.0) < 1.0e-12
			      ? "g cm-3" : (fabs(Ctrl->U.density - 1.0) < 1.0e-12
			                       ? "kg m-3" : NULL);
		if (units)
			nc_put_att_text(output->ncid, output->created_varid[k], "units",
			                strlen(units), units);
		nc_def_var_deflate(output->ncid, output->created_varid[k], 1, 1, 2);
	}
	for (k = 0; k < ngatts; k++) {
		char name[NC_MAX_NAME + 1];
		if (nc_inq_attname(cube->ncid, NC_GLOBAL, k, name) == NC_NOERR &&
		    strcmp(name, "_NCProperties") && strcmp(name, "_SuperblockVersion"))
			nc_copy_att(cube->ncid, NC_GLOBAL, name,
			            output->ncid, NC_GLOBAL);
	}
	nc_put_att_text(output->ncid, NC_GLOBAL, "source",
	                strlen("Created by GMT elygtl"), "Created by GMT elygtl");
	if (nc_enddef(output->ncid) != NC_NOERR) goto netcdf;
	for (varid = 0; varid < nvars; varid++) {
		nc_type type;
		int vndims, dims[NC_MAX_VAR_DIMS], position;
		size_t count = 1, length;
		if (output->varid[varid] < 0 ||
		    elygtl_mapped_property(cube, varid) >= 0 ||
		    elygtl_coordinate(cube, varid) >= 0) continue;
		if (nc_inq_var(cube->ncid, varid, NULL, &type, &vndims,
		               dims, NULL) != NC_NOERR)
			goto netcdf;
		for (position = 0; position < vndims; position++) {
			if (nc_inq_dimlen(cube->ncid, dims[position], &length) != NC_NOERR)
				goto netcdf;
			count *= length;
		}
		if (elygtl_copy_variable(cube, varid, output->ncid,
		                         output->varid[varid], type, count) != NC_NOERR)
			goto netcdf;
	}
	for (k = 0; k < 3; k++) {
		double *coordinate = calloc(cube->n[k], sizeof(*coordinate));
		size_t index;
		if (!coordinate) goto netcdf;
		for (index = 0; index < cube->n[k]; index++)
			coordinate[index] = cube->coordinate[k][index] *
			                    Ctrl->Z.transform.axis_scale[k];
		if (nc_put_var_double(output->ncid,
		                      output->varid[cube->coordinate_varid[k]],
		                      coordinate) != NC_NOERR) {
			free(coordinate);
			goto netcdf;
		}
		free(coordinate);
	}
	free(dimid);
	free(unlim);
	return GMT_NOERROR;
memory:
	free(dimid);
	free(unlim);
	return GMT_MEMORY_ERROR;
netcdf:
	GMT_Report(GMT->parent, GMT_MSG_ERROR,
	           "NetCDF error while creating %s\n", Ctrl->G.file);
	free(dimid);
	free(unlim);
	if (output->ncid >= 0) nc_close(output->ncid);
	output->ncid = -1;
	free(output->varid);
	output->varid = NULL;
	return GMT_RUNTIME_ERROR;
}

static int elygtl_write_array(const struct ELYGTL_CTRL *Ctrl,
                              const struct ELYGTL_JOB *job,
                              struct ELYGTL_OUTPUT *output, int property,
                              const double *logical, int output_varid)
{
	const struct ELYGTL_CUBE *cube = &job->cube;
	int reference_property = cube->present[property]
	                       ? property
	                       : (cube->present[ELYGTL_VS] ? ELYGTL_VS : ELYGTL_VP);
	double inverse = 1.0 / (property == ELYGTL_RHO
	                       ? Ctrl->U.density : Ctrl->U.velocity);
	double output_scale = gq_transform_value_scale(
	    &Ctrl->Z.transform,
	    elygtl_output_transform_index(Ctrl, cube, property));
	float *raw = calloc(job->total, sizeof(*raw));
	size_t ix, iy, iz;
	int status;
	if (!raw) return GMT_MEMORY_ERROR;
	for (iz = 0; iz < cube->n[ELYGTL_Z]; iz++)
		for (iy = 0; iy < cube->n[ELYGTL_Y]; iy++)
			for (ix = 0; ix < cube->n[ELYGTL_X]; ix++) {
				size_t k = iz * job->plane + iy * cube->n[ELYGTL_X] + ix;
				size_t raw_k = elygtl_field_index(cube, reference_property,
				                                  ix, iy, iz, false);
				raw[raw_k] = isfinite(logical[k])
				           ? (float)(logical[k] * inverse * output_scale) : NAN;
			}
	status = nc_put_var_float(output->ncid, output_varid, raw);
	free(raw);
	return status == NC_NOERR ? GMT_NOERROR : GMT_RUNTIME_ERROR;
}

static int elygtl_write_output(struct GMT_CTRL *GMT,
                               const struct ELYGTL_CTRL *Ctrl,
                               const struct ELYGTL_JOB *job,
                               struct ELYGTL_OUTPUT *output)
{
	int p, status;
	for (p = 0; p < ELYGTL_N_PROPERTIES; p++) {
		if (job->cube.present[p]) {
			status = elygtl_write_array(
			    Ctrl, job, output, p, job->value[p],
			    output->varid[job->cube.field[p].varid]);
			if (status != GMT_NOERROR) return status;
		}
		if (Ctrl->C.active[p]) {
			status = elygtl_write_array(
			    Ctrl, job, output, p, job->created[p],
			    output->created_varid[p]);
			if (status != GMT_NOERROR) return status;
		}
	}
	if (nc_close(output->ncid) != NC_NOERR) return GMT_RUNTIME_ERROR;
	output->ncid = -1;
	GMT_Report(GMT->parent, GMT_MSG_INFORMATION,
	           "Modified %zu dry columns; skipped %zu wet, %zu without Vs30, "
	           "%zu without transition thickness, and %zu without a finite anchor\n",
	           job->modified_columns, job->wet_columns, job->missing_vs30,
	           job->missing_transition, job->missing_anchor);
	for (p = 0; p < ELYGTL_N_PROPERTIES; p++)
		if (job->empirical_skips[p])
			GMT_Report(GMT->parent, GMT_MSG_WARNING,
			           "Left %zu requested %s values undefined because the "
			           "source was missing or outside the empirical range\n",
			           job->empirical_skips[p], elygtl_key[p]);
	return GMT_NOERROR;
}

static void elygtl_job_free(struct ELYGTL_JOB *job)
{
	int p;
	if (job == NULL) return;
	elygtl_cube_free(&job->cube);
	for (p = 0; p < ELYGTL_N_PROPERTIES; p++) {
		free(job->value[p]);
		free(job->created[p]);
	}
	free(job->vs30);
	free(job->transition);
	free(job->wet);
	free(job->classified);
	memset(job, 0, sizeof(*job));
}

static int elygtl_run(struct GMT_CTRL *GMT, const struct ELYGTL_CTRL *Ctrl)
{
	struct ELYGTL_JOB job;
	struct ELYGTL_OUTPUT output;
	int p, status;
	memset(&job, 0, sizeof(job));
	job.cube.ncid = -1;
	memset(&output, 0, sizeof(output));
	output.ncid = -1;
	status = elygtl_open_cube(GMT->parent, Ctrl, &job.cube);
	if (status != GMT_NOERROR) goto cleanup;
	job.plane = job.cube.n[ELYGTL_X] * job.cube.n[ELYGTL_Y];
	job.total = job.plane * job.cube.n[ELYGTL_Z];
	for (p = 0; p < ELYGTL_N_PROPERTIES; p++) {
		if (job.cube.present[p]) {
			status = elygtl_read_property(GMT->parent, Ctrl, &job, p);
			if (status != GMT_NOERROR) goto cleanup;
		}
	}
	if (Ctrl->H.active) {
		status = elygtl_fill_horizontal_gaps(GMT, Ctrl, &job);
		if (status != GMT_NOERROR) goto cleanup;
	}
	status = elygtl_resample_horizontal(GMT, Ctrl, &job);
	if (status != GMT_NOERROR) goto cleanup;
	status = elygtl_resample_vertical(GMT, Ctrl, &job);
	if (status != GMT_NOERROR) goto cleanup;
	for (p = 0; p < ELYGTL_N_PROPERTIES; p++) {
		if (Ctrl->C.active[p]) {
			size_t k;
			job.created[p] = calloc(job.total, sizeof(*job.created[p]));
			if (!job.created[p]) {
				status = GMT_MEMORY_ERROR;
				goto cleanup;
			}
			for (k = 0; k < job.total; k++) job.created[p][k] = NAN;
		}
	}
	status = elygtl_prepare_horizontal(GMT, Ctrl, &job);
	if (status != GMT_NOERROR) goto cleanup;
	status = elygtl_apply(GMT, Ctrl, &job);
	if (status != GMT_NOERROR) goto cleanup;
	status = elygtl_create_output(GMT, Ctrl, &job, &output);
	if (status != GMT_NOERROR) goto cleanup;
	status = elygtl_write_output(GMT, Ctrl, &job, &output);
cleanup:
	if (output.ncid >= 0) nc_close(output.ncid);
	free(output.varid);
	elygtl_job_free(&job);
	return status;
}

#define bailout(code) { gmt_M_free_options(mode); return (code); }
#define Return(code) { Free_Ctrl(GMT, Ctrl); gmt_end_module(GMT, GMT_cpy); bailout(code); }

EXTERN_MSC int GMT_elygtl(void *V_API, int mode, void *args)
{
	struct GMTAPI_CTRL *API = gmt_get_api_ptr(V_API);
	struct GMT_CTRL *GMT = NULL, *GMT_cpy = NULL;
	struct GMT_OPTION *options = NULL;
	struct ELYGTL_CTRL *Ctrl = NULL;
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
	status = elygtl_run(GMT, Ctrl);
	Return(status);
}
