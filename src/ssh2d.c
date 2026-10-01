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
 * ssh2d generates two-dimensional small-scale heterogeneity fields and may
 * apply them as fractional perturbations to multiparameter NetCDF grids.
 * Statistical parameters may be shared or field-specific, and BLEND windows
 * may taper the generated fields within rectangular or polygonal supports.
 */

#include "gmt_dev.h"
#include "ssh2d_inc.h"
#include "ssh_module.h"

#define THIS_MODULE_CLASSIC_NAME "ssh2d"
#define THIS_MODULE_MODERN_NAME "ssh2d"
#define THIS_MODULE_LIB "gq"
#define THIS_MODULE_LIB_PURPOSE "The CRESCENT cyberinfrastructure team supplements to the Generic Mapping Tools"
#define THIS_MODULE_PURPOSE "Generate or apply two-dimensional small-scale heterogeneities in multiparameter NetCDF grids"
#define THIS_MODULE_KEYS "<G{,GG}"
#define THIS_MODULE_NEEDS ""
#define THIS_MODULE_OPTIONS "RVdfn"

static void *New_Ctrl(struct GMT_CTRL *GMT)
{
	struct SSH_CTRL *Ctrl = gmt_M_memory(GMT, NULL, 1, struct SSH_CTRL);
	ssh_ctrl_init(Ctrl);
	return Ctrl;
}
static void Free_Ctrl(struct GMT_CTRL *GMT, struct SSH_CTRL *Ctrl)
{
	if (!Ctrl) return;
	ssh_ctrl_free(Ctrl);
	gmt_M_free(GMT, Ctrl);
}

static int usage(struct GMTAPI_CTRL *API, int level)
{
	const char *name = gmt_show_name_and_purpose(API, THIS_MODULE_LIB,
	                                             THIS_MODULE_CLASSIC_NAME,
	                                             THIS_MODULE_PURPOSE);
	if (level == GMT_MODULE_PURPOSE) return GMT_NOERROR;
	GMT_Usage(API, 0,
	          "usage: %s [<model.nc>[+x<sx>][+X<unit>][+y<sy>][+Y<unit>]"
	          "[+v<scales>][+V<units>][+n<missing>]] [-A] "
	          "-G<output.nc> [-F<fields>] "
	          "-D[<field>/]<sigma> [-C[<field>/]<xlen>/<ylen>] "
	          "[-U[<field>/]<hurst>] [-H<n|l|a|s|m>[<args>][+m<maxgap>]] "
	          "[-M<v|g|e|w>] "
	          "[-P<polygon>] [-EE|B[+w]] "
	          "[-Q<seed>[+i][+n|+p<factor>]] "
	          "[-W[<xwindow>/<ywindow>][+r<ratios>][+w]] "
	          "[-Z[+x<sx>][+X<unit>][+y<sy>][+Y<unit>]"
	          "[+v<scales>][+V<units>]] "
	          "[%s] [%s] [%s] [%s] [%s]\n",
	          name, GMT_Rgeo_OPT, GMT_I_OPT, GMT_n_OPT, GMT_V_OPT, GMT_di_OPT);
	if (level == GMT_SYNOPSIS) return GMT_MODULE_SYNOPSIS;
	GMT_Message(API, GMT_TIME_NONE, "  REQUIRED ARGUMENTS:\n");
	GMT_Usage(API, 1, "\n-G<output.nc>");
	GMT_Usage(API, -2,
	          "Write a multiparameter NetCDF grid. Synthetic output contains the "
	          "generated heterogeneity fields. Application output contains the "
	          "perturbed selected variables and preserves compatible unselected "
	          "variables and metadata from the input model.");
	GMT_Usage(API, 1, "\n-D[<field>/]<sigma>");
	GMT_Usage(API, -2,
	          "Set the shared or field-specific fractional standard deviation. "
	          "Sigma must be greater than zero and is expressed as a fraction of "
	          "the model value rather than in percent. Every output field needs "
	          "either a shared value or a field-specific value. For example, "
	          "-D0.05 gives every field a standard deviation of 5 percent, while "
	          "-D0.05 -Dvp/0.03 -Dvs/0.08 uses 3 percent for vp, 8 percent for vs, "
	          "and 5 percent for every other selected field.");
	GMT_Message(API, GMT_TIME_NONE, "\n  OPTIONAL ARGUMENTS:\n");
	GMT_Usage(API, 1, "\n-A");
	GMT_Usage(API, -2,
	          "Apply each generated heterogeneity field to its selected model "
	          "variable as model*(1+epsilon). Missing model values remain missing. "
	          "Without -A, write the heterogeneity fields themselves. Application "
	          "mode requires one input model and -F.");
	GMT_Usage(API, 1, "\n<model.nc>[+x<sx>][+X<unit>][+y<sy>][+Y<unit>]"
	                       "[+v<scales>][+V<units>][+n<missing>]");
	GMT_Usage(API, -2,
	          "Supply one NetCDF model in application mode. Variables selected by "
	          "-F must be numeric two-dimensional fields that share x and y "
	          "coordinates. NetCDF packing and missing-value metadata are decoded "
	          "first. Use +n to identify an additional input missing-value sentinel. "
	          "+x and +y to scale coordinates. +X and +Y to set working coordinate "
	          "units. +v to scale selected fields. +V to set their working "
	          "units. One +v or +V entry is broadcast. Otherwise entries follow -F "
	          "order. Scaling does not reorder coordinates or data. Both transformed "
	          "axes must be regular and increasing. For example, model.nc+x0.001"
	          "+Xkm+y0.001+Ykm+v0.001,0.001+Vkm/s,km/s converts coordinates from "
	          "m to km and vp and vs from m/s to km/s when used with -Fvp,vs.");
	GMT_Usage(API, 1, "\n-C[<field>/]<xlen>[/<ylen>]");
	GMT_Usage(API, -2,
	          "Set shared or field-specific correlation lengths in transformed "
	          "coordinate units. Lengths must be greater than zero and are required "
	          "for von Karman, Gaussian, and exponential models. White noise does "
	          "not use them. One length is isotropic. xlen/ylen specifies "
	          "axis-aligned anisotropy. For example, -C20 uses 20 along both axes, "
	          "-C20/10 uses x and y lengths of 20 and 10, and -C20/10 -Cvs/8/4 "
	          "overrides those lengths for vs.");
	GMT_Usage(API, 1, "\n-F<field1,field2,...>");
	GMT_Usage(API, -2,
	          "Set field names and their order. In synthetic mode, omitting -F "
	          "creates one variable named heterogeneity. -Fvp,vs creates variables "
	          "vp_heterogeneity and vs_heterogeneity. In application mode, -F is "
	          "required and selects exact NetCDF variable names, such as "
	          "-Fvp,vs,rho. Selected variables must share the same coordinate "
	          "dimensions. Field-specific -D, -C, and -U options and +v, +V, and "
	          "output -Z value lists use this order and these names.");
	GMT_Usage(API, 1, "\n-U[<field>/]<hurst>");
	GMT_Usage(API, -2,
	          "Set the shared or field-specific Hurst exponent for the von Karman "
	          "model. The value must be greater than zero and less than one. The "
	          "default exponent is 0.15. Gaussian, exponential, and white models do "
	          "not use this option. For example, -U0.3 applies 0.3 to every field, "
	          "while -U0.3 -Uvs/0.7 uses 0.7 for vs.");
	GMT_Usage(API, 1, "\n-H<n|l|a|s|m>[<args>][+m<maxgap>]");
	GMT_Usage(API, -2,
	          "Fill strictly internal horizontal missing-data holes before -R/-I "
	          "resampling and heterogeneity application. Original non-missing nodes "
	          "and boundary-connected missing regions are preserved. Without -H, "
	          "internal holes are not filled. Available methods are:");
	GMT_Usage(API, 3,
	          "Nearest neighbor (n). Optionally append a search radius in grid nodes.");
	GMT_Usage(API, 3,
	          "Linear Delaunay interpolation (l). This is the default method.");
	GMT_Usage(API, 3,
	          "Local weighted average (a). Optionally append radius/sectors in grid "
	          "nodes. The default radius and sector count are 3 and 4.");
	GMT_Usage(API, 3,
	          "Spline interpolation (s). Optionally append tension from 0 through 1. "
	          "The default tension is 0.");
	GMT_Usage(API, 3,
	          "Minimum-curvature interpolation (m). Optionally append tension from "
	          "0 through 1. The default tension is 0.");
	GMT_Usage(API, 3,
	          "+m fills only holes whose x and y spans are each no larger than "
	          "maxgap grid nodes. Without +m, every strictly internal hole is "
	          "eligible. For example, -Ha4/8+m20 uses local weighted averaging "
	          "with radius 4 and 8 sectors for holes spanning at most 20 nodes.");
	GMT_Usage(API, 1, "\n-M<v|g|e|w>");
	GMT_Usage(API, -2,
	          "Select the statistical model: von Karman (v), Gaussian (g), "
	          "exponential (e), or white noise (w). Von Karman is the default. "
	          "White noise does not require -C, and only von Karman uses -U. For "
	          "example, -Mg selects the Gaussian model.");
	GMT_Usage(API, 1, "\n-P<polygon>");
	GMT_Usage(API, -2,
	          "Restrict the heterogeneity to a polygonal support supplied as x/y "
	          "vertices in transformed working coordinates. Without -P, -W uses "
	          "the complete rectangular working domain. The polygon must lie within "
	          "that domain and must have a strict xy-monotone boundary unless -E "
	          "converts it. GMT remote polygon files beginning with @ are accepted.");
	GMT_Usage(API, 1, "\n-EE|B[+w]");
	GMT_Usage(API, -2,
	          "Convert a non-monotone -P polygon using the strict envelope (-EE) "
	          "or best piecewise envelope (-EB). Append +w to write the converted "
	          "polygon beside the input with a _monotone suffix. Without -E, a "
	          "non-monotone polygon is rejected. For example, -Pstar.txt -EB+w "
	          "uses the best conversion and writes star_monotone.txt.");
	GMT_Usage(API, 1, "\n-Q<seed>[+i][+n|+p<factor>]");
	GMT_Usage(API, -2,
	          "Control random-number generation and spectral padding. The default "
	          "seed is 1, and multiple fields use the same underlying realization. "
	          "Append +i to derive an independent realization for each field. The "
	          "default padding is one maximum correlation length at every edge. +n "
	          "disables padding and +p<factor> changes that multiple. For example, "
	          "-Q42+i+p2 uses seed 42, independent fields, and padding of two "
	          "correlation lengths.");
	GMT_Option(API, "R");
	GMT_Usage(API, -2,
	          "Set the working output region. Synthetic mode requires -R. In "
	          "application mode, -R optionally selects a region contained within "
	          "the transformed model domain. Otherwise the complete model region "
	          "is used. Input scaling and -H precede region selection.");
	GMT_Usage(API, 1, "\n-I<dx>[/<dy>]");
	GMT_Usage(API, -2,
	          "Set positive working output increments. One value applies to x and y. "
	          "two values set them independently. Synthetic mode requires -I. In "
	          "application mode, omitting -I retains the transformed model "
	          "increments. Common option -n controls horizontal interpolation when "
	          "-R or -I changes the lattice.");
	GMT_Usage(API, 1,
	          "\n-W[<xwindow>[/<ywindow>]][+r<r>|<rx>/<ry>|<rx1>/<rx2>/<ry1>/<ry2>][+w]");
	GMT_Usage(API, -2,
	          "Taper the heterogeneity inside the -P support or the complete "
	          "rectangular domain. One window name applies to both axes. Two names "
	          "set x and y independently. Cosine is the default on both axes. A "
	          "taper ratio is the fraction of a support dimension occupied by the "
	          "transition at one edge. Each ratio must be at least 0 and less than "
	          "0.5, and 0 disables that edge taper. The default ratio is 0.2 at all "
	          "four edges. One ratio applies everywhere, rx/ry sets symmetric ratios "
	          "by axis, and rx1/rx2/ry1/ry2 sets every edge independently. Append "
	          "+w to include the NetCDF variable weight. For example, "
	          "-Wcosine/tukey+r0.1/0.3 uses cosine in x, Tukey in y, and symmetric "
	          "x and y ratios of 0.1 and 0.3.");
	GMT_Usage(API, 1,
	          "\n-Z[+x<sx>][+X<unit>][+y<sy>][+Y<unit>]"
	          "[+v<scales>][+V<units>]");
	GMT_Usage(API, -2,
	          "Transform output coordinates and values after generation or "
	          "application. +x and +y scale coordinates. +X and +Y set coordinate "
	          "units. +v supplies one broadcast value scale or one scale per -F "
	          "field. +V sets field units. Scaling occurs in place and does not "
	          "reorder grid rows, columns, or fields. For example, -Z+x1000+Xm"
	          "+y1000+Ym+v1000,1000+Vm/s,m/s converts coordinates from km to m and "
	          "two selected velocity fields from km/s to m/s.");
	GMT_Option(API, "n");
	if (gmt_M_showusage(API))
		GMT_Usage(API, -2,
		          "Control GMT horizontal interpolation onto a changed -R/-I "
		          "lattice. Common -n does not fill missing values and does not "
		          "conflict with -H, which fills strictly internal holes first.");
	GMT_Usage(API, 1, "\nProcessing order:");
	GMT_Usage(API, -2,
	          "NetCDF missing-value and packing conversion. Input coordinate and "
	          "field scaling. Regular increasing-axis validation. -H internal-hole "
	          "filling. -R/-I and common -n horizontal resampling. Heterogeneity "
	          "generation. BLEND tapering. Optional model application. Then -Z "
	          "output transformation. If the lattice changes, scalar ancillary "
	          "variables are unchanged and incompatible coordinate-dependent "
	          "variables are omitted. Unresolved missing values are written as NaN.");
	GMT_Option(API, "V,di,.");
	return GMT_MODULE_USAGE;
}

#define bailout(code) { gmt_M_free_options(mode); return (code); }
#define Return(code) { Free_Ctrl(GMT, Ctrl); gmt_end_module(GMT, GMT_cpy); bailout(code); }

EXTERN_MSC int GMT_ssh2d(void *V_API, int mode, void *args)
{
	struct GMTAPI_CTRL *API = gmt_get_api_ptr(V_API);
	struct GMT_CTRL *GMT = NULL, *GMT_cpy = NULL;
	struct GMT_OPTION *options = NULL;
	struct SSH_CTRL *Ctrl = NULL;
	int error, status;
	if (!API) return GMT_NOT_A_SESSION;
	if (mode == GMT_MODULE_PURPOSE) return usage(API, GMT_MODULE_PURPOSE);
	options = GMT_Create_Options(API, mode, args);
	if (API->error) return API->error;
	if ((error = gmt_report_usage(API, options, 0, usage)) != GMT_NOERROR)
		bailout(error);
	if ((GMT = gmt_init_module(API, THIS_MODULE_LIB, THIS_MODULE_CLASSIC_NAME,
	                           THIS_MODULE_KEYS, THIS_MODULE_NEEDS, module_kw,
	                           &options, &GMT_cpy)) == NULL)
		bailout(API->error);
	if (GMT_Parse_Common(API, THIS_MODULE_OPTIONS, options)) Return(API->error);
	Ctrl = New_Ctrl(GMT);
	if ((error = ssh_parse_options(GMT, Ctrl, options, 2)) != GMT_NOERROR)
		Return(error);
	status = ssh_execute(GMT, Ctrl, 2);
	Return(status);
}
