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

#ifndef MERGE3D_INC_H
#define MERGE3D_INC_H

static struct GMT_KEYWORD_DICTIONARY module_kw[] = {
	{ 0, 'A', "aggregate", "", "", "", "", GMT_TP_STANDARD },
	{ 0, 'C', "clobber",
	          "f,l,o,u", "first,low,last,high",
	          "n,p", "negative,positive",
	          GMT_TP_STANDARD },
	{ 0, 'F', "fields", "", "", "", "", GMT_TP_STANDARD },
	{ 0, 'G', "out|output", "", "", "", "", GMT_TP_STANDARD },
	{ 0, 'I', "increment", "", "", "", "", GMT_TP_STANDARD },
	{ 0, 'M', "monotone",
	          "E,B", "envelope,best",
	          "w", "write",
	          GMT_TP_STANDARD },
	{ 0, 'P', "pair-fill", "", "", "", "", GMT_TP_STANDARD },
	{ 0, 'S', "vertical-interpolation",
	          "a,c,e,l,n,s", "akima,cubic,step,linear,nearest,smooth",
	          "g", "gaps", GMT_TP_STANDARD },
	{ 0, 'T', "z-range", "", "", "", "", GMT_TP_STANDARD },
	{ 0, 'W', "weights", "", "", "o", "only", GMT_TP_STANDARD },
	{ 0, 'Z', "output-scale", "", "", "x,X,y,Y,z,Z,v,V", "x-scale,x-unit,y-scale,y-unit,z-scale,z-unit,value-scale,value-unit", GMT_TP_STANDARD },
	{ 0, '\0', "", "", "", "", "", 0 }
};

#endif
