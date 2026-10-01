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
#ifndef SSH1D_INC_H
#define SSH1D_INC_H

static struct GMT_KEYWORD_DICTIONARY module_kw[] = {
	{ 0, 'A', "apply", "", "", "", "", GMT_TP_STANDARD },
	{ 0, 'C', "correlation", "", "", "", "", GMT_TP_STANDARD },
	{ 0, 'D', "standard-deviation", "", "", "", "", GMT_TP_STANDARD },
	{ 0, 'F', "fields", "", "", "", "", GMT_TP_STANDARD },
	{ 0, 'G', "out|output", "", "", "", "", GMT_TP_STANDARD },
	{ 0, 'L', "support", "", "", "", "", GMT_TP_STANDARD },
	{ 0, 'M', "model",
	          "v,g,e,w", "von-karman,gaussian,exponential,white",
	          "", "", GMT_TP_STANDARD },
	{ 0, 'Q', "random-seed", "", "", "i,n,p", "independent,no-padding,padding",
	          GMT_TP_STANDARD },
	{ 0, 'S', "interpolation",
	          "a,c,e,l,n,s", "akima,cubic,step,linear,nearest,smooth",
	          "g", "bridge-gaps", GMT_TP_STANDARD },
	{ 0, 'T', "range", "", "", "", "", GMT_TP_STANDARD },
	{ 0, 'U', "hurst", "", "", "", "", GMT_TP_STANDARD },
	{ 0, 'W', "taper", "", "", "r,w", "ratios,write-weight",
	          GMT_TP_STANDARD },
	{ 0, 'Z', "output-transform", "", "", "x,X,v,V",
	          "x-scale,x-unit,value-scale,value-unit", GMT_TP_STANDARD },
	{ 0, '\0', "", "", "", "", "", 0 }
};
#endif
