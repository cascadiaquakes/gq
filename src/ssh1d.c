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
 * ssh1d generates one-dimensional small-scale heterogeneity fields and may
 * apply them as fractional perturbations to text tables or multiparameter
 * NetCDF series. Statistical parameters may be shared or field-specific,
 * and the generated fields may be tapered over interval supports.
 */

#include "gmt_dev.h"
#include "ssh1d_inc.h"
#include "ssh_module.h"

#define THIS_MODULE_CLASSIC_NAME "ssh1d"
#define THIS_MODULE_MODERN_NAME "ssh1d"
#define THIS_MODULE_LIB "gq"
#define THIS_MODULE_LIB_PURPOSE "The CRESCENT cyberinfrastructure team supplements to the Generic Mapping Tools"
#define THIS_MODULE_PURPOSE "Generate or apply one-dimensional small-scale heterogeneities in tables and multiparameter NetCDF series"
#define THIS_MODULE_KEYS "<D{"
#define THIS_MODULE_NEEDS ""
#define THIS_MODULE_OPTIONS "Vdf"

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
	          "usage: %s [<model.txt|model.nc>[+x<sx>][+X<unit>]"
	          "[+v<scales>][+V<units>][+n<missing>]] [-A] -G<output> "
	          "[-F<fields>] -D[<field>/]<sigma> "
	          "[-C[<field>/]<length>] [-U[<field>/]<hurst>] "
	          "[-L<lo>/<hi>] [-M<v|g|e|w>] "
	          "[-Q<seed>[+i][+n|+p<factor>]] [-T<min>/<max>/<inc>] "
	          "[-S<a|c|e|l|n|s<p>>[+g[<maxgap>]]] "
	          "[-W[<window>][+r<r1/r2>][+w]] "
	          "[-Z[+x<sx>][+X<unit>][+v<scales>][+V<units>]] [%s] [%s]\n",
	          name, GMT_V_OPT, GMT_di_OPT);
	if (level == GMT_SYNOPSIS) return GMT_MODULE_SYNOPSIS;
	GMT_Message(API, GMT_TIME_NONE, "  REQUIRED ARGUMENTS:\n");
	GMT_Usage(API, 1, "\n-G<output>");
	GMT_Usage(API, -2,
	          "Write the output. In synthetic mode, .nc, .nc4, and .cdf file "
	          "extensions select NetCDF. Any other name, including -, writes a "
	          "text table. Application mode preserves the input format: a text "
	          "model produces a text table and a NetCDF model produces NetCDF.");
	GMT_Usage(API, 1, "\n-D[<field>/]<sigma>");
	GMT_Usage(API, -2,
	          "Set the shared or field-specific fractional standard deviation. "
	          "Sigma must be greater than zero and is expressed as a fraction of "
	          "the model value rather than in percent. Every output field needs "
	          "either a shared value or a field-specific value. For example, "
	          "-D0.05 gives every field a standard deviation of 5 percent. Repeat "
	          "-D with a field name to override the shared value. -D0.05 "
	          "-Dvp/0.03 -Dvs/0.08 uses 3 percent for vp, 8 percent for vs, and "
	          "5 percent for every other selected field.");
	GMT_Message(API, GMT_TIME_NONE, "\n  OPTIONAL ARGUMENTS:\n");
	GMT_Usage(API, 1, "\n-A");
	GMT_Usage(API, -2,
	          "Apply the generated heterogeneity to one input model as "
	          "model*(1+epsilon). Missing model values remain missing. Without -A, "
	          "write the heterogeneity field itself. Application mode requires one "
	          "input model and -F.");
	GMT_Usage(API, 1, "\n<model.txt|model.nc>[+x<sx>][+X<unit>][+v<scales>]"
	                       "[+V<units>][+n<missing>]");
	GMT_Usage(API, -2,
	          "Supply one text or NetCDF model in application mode. A text table's "
	          "first column is the coordinate and all remaining columns are model "
	          "fields. NetCDF variables selected by -F must share one coordinate. "
	          "NetCDF packing and missing-value metadata are decoded first. Use +n "
	          "to identify an additional input missing-value sentinel, +x to scale "
	          "the coordinate, +X to set its working unit, +v to scale selected "
	          "fields, and +V to set their working units. One +v or +V entry is "
	          "broadcast. Otherwise entries follow -F order. Scaling does not reorder "
	          "coordinates or data. The transformed coordinate must be regular and "
	          "increasing. For example, model.nc+x0.001+Xkm+v0.001,0.001"
	          "+Vkm/s,km/s converts the coordinate from m to km and vp and vs from "
	          "m/s to km/s when used with -Fvp,vs.");
	GMT_Usage(API, 1, "\n-C[<field>/]<length>");
	GMT_Usage(API, -2,
	          "Set the shared or field-specific correlation length in transformed "
	          "coordinate units. Length must be greater than zero and is required "
	          "for von Karman, Gaussian, and exponential models. White noise does "
	          "not use a correlation length. For example, -C10 uses a length of 10 "
	          "for every field, while -C10 -Cvs/5 uses 5 for vs and 10 for the "
	          "remaining fields.");
	GMT_Usage(API, 1, "\n-F<field1,field2,...>");
	GMT_Usage(API, -2,
	          "Set field names and their order. In synthetic mode, omitting -F "
	          "creates one field named heterogeneity. -Fvp,vs creates NetCDF "
	          "variables vp_heterogeneity and vs_heterogeneity. In application "
	          "mode, -F is required. Its entries select exact NetCDF variable names, "
	          "or name every text data column in column order. For example, a text "
	          "table containing x, vp, vs, and rho uses -Fvp,vs,rho. Field-specific "
	          "-D, -C, and -U options and +v, +V, and output -Z value lists use this "
	          "same order and these same names.");
	GMT_Usage(API, 1, "\n-U[<field>/]<hurst>");
	GMT_Usage(API, -2,
	          "Set the shared or field-specific Hurst exponent for the von Karman "
	          "model. The value must be greater than zero and less than one. The "
	          "default exponent is 0.15. Gaussian, exponential, and white models do "
	          "not use this option. For example, -U0.3 applies 0.3 to every field, "
	          "while -U0.3 -Uvs/0.7 uses 0.7 for vs.");
	GMT_Usage(API, 1, "\n-L<lo>/<hi>");
	GMT_Usage(API, -2,
	          "Restrict the taper support to an interval in transformed coordinate "
	          "units. The interval must lie inside the working domain and lo must "
	          "be less than hi. Without -L, -W tapers over the complete domain. "
	          "For example, -L20/80 limits the heterogeneity to that interval.");
	GMT_Usage(API, 1, "\n-M<v|g|e|w>");
	GMT_Usage(API, -2,
	          "Select the statistical model: von Karman (v), Gaussian (g), "
	          "exponential (e), or white noise (w). Von Karman is the default. "
	          "White noise does not require -C, and only von Karman uses -U. For "
	          "example, -Mg selects the Gaussian model.");
	GMT_Usage(API, 1, "\n-Q<seed>[+i][+n|+p<factor>]");
	GMT_Usage(API, -2,
	          "Control random-number generation and spectral padding. The default "
	          "seed is 1, and multiple fields use the same underlying realization. "
	          "Append +i to derive an independent realization for each field. The "
	          "default padding is one maximum correlation length at each edge. +n "
	          "disables padding and +p<factor> changes that multiple. For example, "
	          "-Q42+i+p2 uses seed 42, independent fields, and padding of two "
	          "correlation lengths.");
	GMT_Usage(API, 1, "\n-T<min>/<max>/<inc>");
	GMT_Usage(API, -2,
	          "Set a regular increasing working axis with min < max and inc > 0. "
	          "Synthetic mode requires -T. In application mode, -T optionally "
	          "subsets or resamples within the transformed input domain. Otherwise "
	          "the transformed input axis is unchanged. Option -S controls the "
	          "interpolation. For example, -T0/100/0.1 creates 1001 nodes.");
	GMT_Usage(API, 1, "\n-S<a|c|e|l|n|s<p>>[+g[<maxgap>]]");
	GMT_Usage(API, -2,
	          "Choose Akima (a), cubic spline (c), step-up (e), linear (l), "
	          "nearest-neighbor (n), or smoothing spline (s<p>) interpolation with "
	          "a non-negative fit parameter p. Linear interpolation is the default. "
	          "Interpolation remains within contiguous non-missing runs. Append +g "
	          "to bridge internal missing runs, optionally only when the gap "
	          "does not exceed maxgap in transformed "
	          "coordinate units. Values are not extrapolated beyond data domain. "
	          "Use -Sl+g to fill every internal gap linearly, or -Sc+g5 to use a "
	          "cubic spline only across gaps whose brackets are at most 5 units "
	          "apart. In application mode, -S requires -T unless +g is used.");
	GMT_Usage(API, 1, "\n-W[<window>][+r<r1/r2>][+w]");
	GMT_Usage(API, -2,
	          "Taper the heterogeneity within the -L interval or, when -L is not "
	          "given, the complete working domain. The default window is cosine. "
	          "The default beginning and ending taper ratios are both 0.2. A taper "
	          "ratio is the fraction of the support length occupied by the "
	          "transition at that edge. It must be at least 0 and less than 0.5, "
	          "and 0 disables that edge taper. One +r value applies to both edges. "
	          "r1/r2 sets them independently. Append +w to add the taper weight as "
	          "a final text column or as the NetCDF variable weight. For example, "
	          "-Wcosine+r0/0.3 uses no beginning taper and a 0.3 ending taper.");
	GMT_Usage(API, 1, "\n-Z[+x<sx>][+X<unit>][+v<scales>][+V<units>]");
	GMT_Usage(API, -2,
	          "Transform output coordinates and values after generation or "
	          "application. +x scales the output coordinate, +X sets its unit, +v "
	          "supplies one broadcast value scale or one scale per -F field, and +V "
	          "sets their units. Scaling occurs in place and does not reorder samples. "
	          "A negative +x therefore produces a decreasing output axis. For "
	          "example, -Z+x-1000+Xm+v1000,1000+Vm/s,m/s converts an increasing "
	          "positive-down axis in km to a decreasing positive-up axis in m and "
	          "scales two selected velocity fields to m/s without reordering.");
	GMT_Usage(API, 1, "\nProcessing order:");
	GMT_Usage(API, -2,
	          "Input missing-value and NetCDF packing conversion. Input coordinate "
	          "and field scaling. Regular increasing-axis validation. -T/-S "
	          "resampling or gap bridging. Heterogeneity generation. BLEND tapering. "
	          "optional model application. Then -Z output transformation. "
	          "Unresolved missing values are written as NaN.");
	GMT_Option(API, "V,di,.");
	return GMT_MODULE_USAGE;
}

#define bailout(code) { gmt_M_free_options(mode); return (code); }
#define Return(code) { Free_Ctrl(GMT, Ctrl); gmt_end_module(GMT, GMT_cpy); bailout(code); }

EXTERN_MSC int GMT_ssh1d(void *V_API, int mode, void *args)
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
	if ((error = ssh_parse_options(GMT, Ctrl, options, 1)) != GMT_NOERROR)
		Return(error);
	status = ssh_execute(GMT, Ctrl, 1);
	Return(status);
}
