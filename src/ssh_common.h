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

/* Shared small-scale heterogeneity generation and BLEND taper support. */

#ifndef SSH_COMMON_H
#define SSH_COMMON_H

#include <blend/blend.h>

#define SSH_MAX_DIM 3

enum SSH_MODEL {
	SSH_MODEL_VON_KARMAN = 0,
	SSH_MODEL_GAUSSIAN,
	SSH_MODEL_EXPONENTIAL,
	SSH_MODEL_WHITE
};

enum SSH_STAT_KIND {
	SSH_STAT_SIGMA = 0,
	SSH_STAT_CORRELATION,
	SSH_STAT_HURST
};

struct SSH_STAT {
	double sigma;
	double correlation[SSH_MAX_DIM];
	double hurst;
	bool have_sigma;
	bool have_correlation;
	bool have_hurst;
};

struct SSH_OVERRIDE {
	char *field;
	struct SSH_STAT value;
};

struct SSH_STAT_CONFIG {
	struct SSH_STAT global;
	struct SSH_OVERRIDE *override;
	size_t n_overrides;
};

struct SSH_RANDOM {
	uint64_t seed;
	bool independent;
	double padding;
};

struct SSH_TAPER {
	bool active;
	bool write_weight;
	blend_window_function function[SSH_MAX_DIM];
	double ratio[2 * SSH_MAX_DIM];
	char *polygon;
	bool have_interval;
	double interval[2];
	char monotone;
	bool write_polygon;
};

struct SSH_SUPPORT {
	window blend;
	polygon real_polygon;
	int lo[SSH_MAX_DIM];
	int hi[SSH_MAX_DIM];
	bool ready;
};

int ssh_parse_number(const char *text, double *value);
int ssh_parse_field_list(struct GMTAPI_CTRL *API, const char *text,
                         char ***fields, size_t *count);
void ssh_free_field_list(char **fields, size_t count);
int ssh_parse_stat_option(struct GMTAPI_CTRL *API, const char *text,
                          enum SSH_STAT_KIND kind, unsigned int dim,
                          struct SSH_STAT_CONFIG *config);
int ssh_resolve_stat(struct GMTAPI_CTRL *API,
                     const struct SSH_STAT_CONFIG *config,
                     const char *field, unsigned int dim,
                     enum SSH_MODEL model, struct SSH_STAT *result);
void ssh_free_stat_config(struct SSH_STAT_CONFIG *config);
int ssh_parse_model(struct GMTAPI_CTRL *API, const char *text,
                    enum SSH_MODEL *model);
const char *ssh_model_name(enum SSH_MODEL model);
int ssh_parse_random(struct GMTAPI_CTRL *API, const char *text,
                     struct SSH_RANDOM *random);
uint64_t ssh_field_seed(uint64_t seed, const char *field, bool independent);

void ssh_taper_defaults(struct SSH_TAPER *taper);
void ssh_taper_free(struct SSH_TAPER *taper);
int ssh_parse_taper(struct GMTAPI_CTRL *API, const char *text,
                    unsigned int dim, struct SSH_TAPER *taper);
int ssh_parse_interval(struct GMTAPI_CTRL *API, const char *text,
                       double interval[2]);
int ssh_parse_monotone(struct GMTAPI_CTRL *API, const char *text,
                       struct SSH_TAPER *taper);
int ssh_prepare_support(struct GMT_CTRL *GMT, unsigned int dim,
                        const size_t n[SSH_MAX_DIM],
                        const double origin[SSH_MAX_DIM],
                        const double increment[SSH_MAX_DIM],
                        const struct SSH_TAPER *taper,
                        struct SSH_SUPPORT *support);
int ssh_support_weight(struct SSH_SUPPORT *support, unsigned int dim,
                       size_t i, size_t j, size_t k, double *weight);
void ssh_support_free(struct SSH_SUPPORT *support);

int ssh_generate(void *API, unsigned int dim,
                 const size_t n[SSH_MAX_DIM],
                 const double increment[SSH_MAX_DIM],
                 const double max_correlation[SSH_MAX_DIM],
                 const struct SSH_STAT *stat, enum SSH_MODEL model,
                 const struct SSH_RANDOM *random, uint64_t seed,
                 double *output);

int ssh_apply_taper(struct GMTAPI_CTRL *API, unsigned int dim,
                    const size_t n[SSH_MAX_DIM],
                    struct SSH_SUPPORT *support, double *field,
                    double *weight);

#endif
