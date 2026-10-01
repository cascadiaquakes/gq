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
 * ssh3d generates three-dimensional small-scale heterogeneity fields and may
 * apply them as fractional perturbations to multiparameter NetCDF cubes.
 * Statistical parameters may be shared or field-specific, and extruded BLEND
 * supports may taper the generated fields in all three dimensions.
 */

#include "gmt_dev.h"
#include "ssh3d_inc.h"
#include "ssh_module.h"

#define THIS_MODULE_CLASSIC_NAME "ssh3d"
#define THIS_MODULE_MODERN_NAME "ssh3d"
#define THIS_MODULE_LIB "gq"
#define THIS_MODULE_LIB_PURPOSE "The CRESCENT cyberinfrastructure team supplements to the Generic Mapping Tools"
#define THIS_MODULE_PURPOSE "Generate or apply three-dimensional small-scale heterogeneities in multiparameter NetCDF cubes"
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
	          "[+z<sz>][+Z<unit>][+v<scales>][+V<units>][+n<missing>]] [-A] "
	          "-G<output.nc> [-F<fields>] -D[<field>/]<sigma> "
	          "[-C[<field>/]<xlen>/<ylen>/<zlen>] "
	          "[-U[<field>/]<hurst>] [-H<n|l|a|s|m>[<args>][+m<maxgap>]] "
	          "[-S<a|c|e|l|n|s<p>>[+g[<maxgap>]]] [-M<v|g|e|w>] "
	          "[-P<polygon>] [-L<zlo>/<zhi>] [-EE|B[+w]] "
	          "[-Q<seed>[+i][+n|+p<factor>]] "
	          "[-W[<xwindow>/<ywindow>/<zwindow>][+r<ratios>][+w]] "
	          "[-T<zmin>/<zmax>/<dz>] "
	          "[-Z[+x<sx>][+X<unit>][+y<sy>][+Y<unit>][+z<sz>][+Z<unit>]"
	          "[+v<scales>][+V<units>]] [%s] [%s] [%s] [%s] [%s]\n",
	          name, GMT_Rgeo_OPT, GMT_I_OPT, GMT_n_OPT, GMT_V_OPT, GMT_di_OPT);
	if (level == GMT_SYNOPSIS) return GMT_MODULE_SYNOPSIS;
	GMT_Message(API, GMT_TIME_NONE, "  REQUIRED ARGUMENTS:\n");
	GMT_Usage(API, 1, "\n-G<output.nc>");
	GMT_Usage(API, -2,
	          "Write a multiparameter NetCDF cube. Synthetic output contains the "
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
	                       "[+z<sz>][+Z<unit>][+v<scales>][+V<units>]"
	                       "[+n<missing>]");
	GMT_Usage(API, -2,
	          "Supply one NetCDF model in application mode. Variables selected by "
	          "-F must be numeric three-dimensional fields that share x, y, and z "
	          "coordinates. NetCDF packing and missing-value metadata are decoded "
	          "first. Use +n to identify an additional input missing-value sentinel. "
	          "+x, +y, and +z to scale coordinates. +X, +Y, and +Z to set working "
	          "coordinate units. +v to scale selected fields. +V to set their "
	          "working units. One +v or +V entry is broadcast. Otherwise entries "
	          "follow -F order. Scaling does not reorder coordinates or data. All "
	          "transformed axes must be regular and increasing. A negative axis "
	          "scale may convert a descending convention without rearranging model "
	          "layers. For example, model.nc+x0.001+Xkm+y0.001+Ykm+z-0.001+Zkm"
	          "+v0.001,0.001+Vkm/s,km/s converts horizontal coordinates from m to "
	          "km, reverses the sign and converts the z coordinates from m to km, "
	          "and converts vp and vs from m/s to km/s when used with -Fvp,vs.");
	GMT_Usage(API, 1, "\n-C[<field>/]<xlen>[/<ylen>/<zlen>]");
	GMT_Usage(API, -2,
	          "Set shared or field-specific correlation lengths in transformed "
	          "coordinate units. Lengths must be greater than zero and are required "
	          "for von Karman, Gaussian, and exponential models. White noise does "
	          "not use them. Supply either one isotropic length or all three "
	          "axis-aligned lengths xlen/ylen/zlen. For example, -C20 uses 20 along "
	          "all axes, -C20/10/2 introduces axis-aligned anisotropy, and "
	          "-C20/10/2 -Cvs/8/4/1 overrides the lengths for vs.");
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
	          "Fill strictly internal horizontal missing-data holes independently "
	          "on every native z layer before -R/-I resampling and heterogeneity "
	          "application. Original non-missing nodes and boundary-connected "
	          "missing regions are preserved. Without -H, internal horizontal holes "
	          "are not filled. Available methods are:");
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
	          "Restrict the horizontal heterogeneity support to a polygon supplied "
	          "as x/y vertices in transformed working coordinates. The polygon is "
	          "extruded through the -L interval or the complete working z domain. "
	          "Without -P, -W uses the complete rectangular x-y domain. The polygon "
	          "must lie within that domain and have a strict xy-monotone boundary "
	          "unless -E converts it. GMT remote polygon files beginning with @ are "
	          "accepted.");
	GMT_Usage(API, 1, "\n-L<zlo>/<zhi>");
	GMT_Usage(API, -2,
	          "Restrict the vertical taper support to an interval in transformed z "
	          "units. The interval must lie inside the working z domain and zlo must "
	          "be less than zhi. Without -L, -W uses the complete working z domain. "
	          "For example, -L0/20 limits the heterogeneity to those z coordinates.");
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
	          "default padding is one maximum correlation length at every boundary. "
	          "+n disables padding and +p<factor> changes that multiple. For example, "
	          "-Q42+i+p2 uses seed 42, independent fields, and padding of two "
	          "correlation lengths.");
	GMT_Option(API, "R");
	GMT_Usage(API, -2,
	          "Set the working horizontal output region. Synthetic mode requires "
	          "-R. In application mode, -R optionally selects a region contained "
	          "within the transformed model domain. Otherwise the complete model "
	          "region is used. Input scaling and -H precede region selection.");
	GMT_Usage(API, 1, "\n-I<dx>[/<dy>]");
	GMT_Usage(API, -2,
	          "Set positive working horizontal increments. One value applies to x "
	          "and y. Two values set them independently. Synthetic mode requires "
	          "-I. In application mode, omitting -I retains the transformed model "
	          "increments. Common option -n controls horizontal interpolation when "
	          "-R or -I changes the lattice.");
	GMT_Usage(API, 1, "\n-T<zmin>/<zmax>/<dz>");
	GMT_Usage(API, -2,
	          "Set a regular increasing working z axis with zmin < zmax and dz > 0. "
	          "Synthetic mode requires -T. In application mode, -T optionally "
	          "subsets or resamples within the transformed input z domain. Otherwise "
	          "the transformed input z axis is unchanged. Option -S controls vertical "
	          "interpolation. For example, -T0/60/0.5 creates 121 z layers.");
	GMT_Usage(API, 1, "\n-S<a|c|e|l|n|s<p>>[+g[<maxgap>]]");
	GMT_Usage(API, -2,
	          "Choose Akima (a), cubic spline (c), step-up (e), linear (l), "
	          "nearest-neighbor (n), or smoothing spline (s<p>) vertical "
	          "interpolation with a non-negative fit parameter p. Linear "
	          "interpolation is the default. Interpolation remains within contiguous "
	          "non-missing runs. Append +g to bridge internal missing runs, "
	          "optionally only when the gap distance does "
	          "not exceed maxgap in transformed z units. Values are not extrapolated "
	          "beyond data domain. Use -Sl+g to fill every internal vertical gap "
	          "linearly, or -Sc+g5 to use a cubic spline only across gaps whose "
	          "brackets are at most 5 z units apart. In application mode, -S "
	          "requires -T unless +g is used. Option -H fills enclosed holes within "
	          "native x-y layers. Common -n controls horizontal resampling.");
	GMT_Usage(API, 1,
	          "\n-W[<xwindow>[/<ywindow>/<zwindow>]]"
	          "[+r<r>|<rx>/<ry>/<rz>|<rx1>/<rx2>/<ry1>/<ry2>/<rz1>/<rz2>]"
	          "[+w]");
	GMT_Usage(API, -2,
	          "Taper the heterogeneity inside the -P and -L supports or the complete "
	          "working cube. One window name applies to all axes. Three names set x, "
	          "y, and z independently. Cosine is the default on every axis. A taper "
	          "ratio is the fraction of a support dimension occupied by the "
	          "transition at one boundary. Each ratio must be at least 0 and less "
	          "than 0.5, and 0 disables that boundary taper. The default ratio is 0.2 "
	          "at all six boundaries. One ratio applies everywhere. Three ratios set "
	          "symmetric x, y, and z tapers. Six ratios set beginning and ending x, "
	          "y, and z tapers in that order. Append +w to include the NetCDF "
	          "variable weight. For example, -Wcosine+r0.2/0.2/0.2/0.2/0/0.2 "
	          "disables the beginning z taper while using a 0.2 ending z taper.");
	GMT_Usage(API, 1,
	          "\n-Z[+x<sx>][+X<unit>][+y<sy>][+Y<unit>][+z<sz>][+Z<unit>]"
	          "[+v<scales>][+V<units>]");
	GMT_Usage(API, -2,
	          "Transform output coordinates and values after generation or "
	          "application. +x, +y, and +z scale coordinates. +X, +Y, and +Z set "
	          "coordinate units. +v supplies one broadcast value scale or one scale "
	          "per -F field. +V sets field units. Scaling occurs in place and "
	          "does not reorder grid rows, columns, layers, or fields. A negative +z "
	          "therefore produces a decreasing output z axis. For example, "
	          "-Z+z-1000+Zm+v1000,1000+Vm/s,m/s restores a positive-up z convention "
	          "in m and converts two velocity fields from km/s to m/s without "
	          "reordering model layers.");
	GMT_Option(API, "n");
	if (gmt_M_showusage(API))
		GMT_Usage(API, -2,
		          "Control GMT horizontal interpolation onto a changed -R/-I "
		          "lattice. Common -n does not fill missing values and does not "
		          "conflict with -H, which fills strictly internal horizontal holes "
		          "first. Option -S independently controls vertical interpolation.");
	GMT_Usage(API, 1, "\nProcessing order:");
	GMT_Usage(API, -2,
	          "NetCDF missing-value and packing conversion. Input coordinate and "
	          "field scaling. Regular increasing-axis validation. -H horizontal "
	          "internal-hole filling. -R/-I and common -n horizontal resampling. "
	          "-T/-S vertical resampling or gap bridging. Heterogeneity generation. "
	          "BLEND tapering. Optional model application. Then -Z output "
	          "transformation. If the lattice changes, scalar ancillary variables "
	          "are unchanged and incompatible coordinate-dependent variables are "
	          "omitted. Unresolved missing values are written as NaN.");
	GMT_Option(API, "V,di,.");
	return GMT_MODULE_USAGE;
}

#define bailout(code) { gmt_M_free_options(mode); return (code); }
#define Return(code) { Free_Ctrl(GMT, Ctrl); gmt_end_module(GMT, GMT_cpy); bailout(code); }

EXTERN_MSC int GMT_ssh3d(void *V_API, int mode, void *args)
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
	if ((error = ssh_parse_options(GMT, Ctrl, options, 3)) != GMT_NOERROR)
		Return(error);
	status = ssh_execute(GMT, Ctrl, 3);
	Return(status);
}
