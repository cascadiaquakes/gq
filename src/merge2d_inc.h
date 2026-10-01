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

#ifndef MERGE2D_INC_H
#define MERGE2D_INC_H

/* Translation table from long to short module options, directives and modifiers */

static struct GMT_KEYWORD_DICTIONARY module_kw[] = {
	/* separator, short_option, long_option,
		  short_directives,    long_directives,
		  short_modifiers,     long_modifiers,
		  transproc_mask */
	{ 0, 'A', "aggregate",           "", "", "", "", GMT_TP_STANDARD },
	{ 0, 'C', "clobber",
	          "f,l,o,u",           "first,low,last,high",
	          "n,p",               "negative,positive",
		  GMT_TP_STANDARD },
	{ 0, 'F', "fields",              "", "", "", "", GMT_TP_STANDARD },
	GMT_G_OUTGRID_KW,
	GMT_I_INCREMENT_KW,
	{ 0, 'M', "monotone",
	          "E,B",               "envelope,best",
	          "w",                 "write",
		  GMT_TP_STANDARD },
	{ 0, 'P', "pair-fill",            "", "", "", "", GMT_TP_STANDARD },
	{ 0, 'W', "weights",
	          "",                  "",
	          "o",                 "only",
		  GMT_TP_STANDARD },
	{ 0, 'Z', "output-scale",      "", "", "x,X,y,Y,v,V", "x-scale,x-unit,y-scale,y-unit,value-scale,value-unit", GMT_TP_STANDARD },
	{ 0, '\0', "", "", "", "", "", 0 }  /* End of list marked with empty option and strings */
};
#endif  /* !MERGE2D_INC_H */
