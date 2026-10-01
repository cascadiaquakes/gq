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

#ifndef TOPOBATH_INC_H
#define TOPOBATH_INC_H

static struct GMT_KEYWORD_DICTIONARY module_kw[] = {
	{ 0, 'A', "shore-features|min-area",
	          "", "",
	          "a,l,p,r", "antarctica,regular-lakes,min-polygon,river-lakes",
	          GMT_TP_STANDARD },
	{ 0, 'C', "classification",
	          "g,m,l,w", "gmt,model,land,wet",
	          "", "", GMT_TP_STANDARD },
	{ 0, 'D', "shore-resolution",
	          "a,f,h,i,l,c,n", "auto,full,high,intermediate,low,crude,none",
	          "", "", GMT_TP_STANDARD },
	{ 0, 'E', "old-surface|old-topography", "", "", "", "", GMT_TP_STANDARD },
	{ 0, 'F', "air", "", "", "", "", GMT_TP_STANDARD },
	{ 0, 'G', "out|output", "", "", "", "", GMT_TP_STANDARD },
	{ 0, 'I', "increment|spacing", "", "", "", "", GMT_TP_STANDARD },
	{ 0, 'K', "landmask", "", "", "", "", GMT_TP_STANDARD },
	{ 0, 'L', "minimum", "", "", "", "", GMT_TP_STANDARD },
	{ 0, 'M', "method",
	          "p,e,l", "pull,extend,linear",
	          "", "", GMT_TP_STANDARD },
	{ 0, 'O', "operation",
	          "a,r,x", "add,remove,replace",
	          "t,b", "topography,bathymetry", GMT_TP_STANDARD },
	{ 0, 'Q', "surface-grid", "", "", "c", "classification", GMT_TP_STANDARD },
	{ 0, 'S', "vertical-interpolation",
	          "a,c,e,l,n,s", "akima,cubic,step,linear,nearest,smooth",
	          "", "", GMT_TP_STANDARD },
	{ 0, 'T', "vertical-range", "", "", "", "", GMT_TP_STANDARD },
	{ 0, 'W', "water", "", "", "t", "tolerance", GMT_TP_STANDARD },
	{ 0, 'Z', "z-scale", "", "", "u", "unit", GMT_TP_STANDARD },
	{ 0, '\0', "", "", "", "", "", 0 }
};

#endif
