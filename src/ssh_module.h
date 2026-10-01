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

/* Shared module controls for ssh1d, ssh2d, and ssh3d. */

#ifndef SSH_MODULE_H
#define SSH_MODULE_H

#include "gq_transform.h"
#include "ssh_common.h"

struct SSH_RANGE {
	bool active;
	double min;
	double max;
	double increment;
	size_t n;
};

struct SSH_GAP {
	bool active;
	char method;
	double argument;
	unsigned int sectors;
	bool limited;
	unsigned int max_gap;
};

struct SSH_INTERPOLATION {
	bool active;
	unsigned int mode;
	double fit;
	bool bridge;
	double max_gap;
};

struct SSH_CTRL {
	struct {
		char **file;
		size_t n;
	} In;
	struct {
		bool active;
	} A;
	struct {
		bool active;
		char **name;
		size_t n;
	} F;
	struct {
		bool active;
		char *file;
	} G;
	struct SSH_GAP H;
	struct {
		bool active;
		double value[2];
	} I;
	struct {
		bool active;
	} M;
	enum SSH_MODEL model;
	struct {
		bool active;
	} Q;
	struct SSH_RANDOM random;
	struct SSH_INTERPOLATION S;
	struct SSH_RANGE T;
	struct SSH_STAT_CONFIG statistic;
	struct SSH_TAPER taper;
	struct {
		bool active;
		struct GQ_TRANSFORM transform;
	} Z;
};

void ssh_ctrl_init(struct SSH_CTRL *Ctrl);
void ssh_ctrl_free(struct SSH_CTRL *Ctrl);
int ssh_parse_options(struct GMT_CTRL *GMT, struct SSH_CTRL *Ctrl,
                      struct GMT_OPTION *options, unsigned int dim);
int ssh_execute(struct GMT_CTRL *GMT, const struct SSH_CTRL *Ctrl,
                unsigned int dim);

#endif
