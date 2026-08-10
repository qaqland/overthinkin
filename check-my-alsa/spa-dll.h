/*
 * spa-dll.h - simple DLL, ported from uos-pipewire
 * spa/include/spa/utils/dll.h (1.6.0-1deepin9).
 *
 * Verbatim copy of the pipewire header minus the SPA_API_DLL export
 * ceremony (kept static inline). The struct and the three functions
 * match pipewire 1:1 so this file diffs against the original.
 */

#ifndef SPA_DLL_H
#define SPA_DLL_H

#include <math.h>
#include <stddef.h>

#define SPA_DLL_BW_MAX 0.128
#define SPA_DLL_BW_MIN 0.016

struct spa_dll {
	double bw;
	double z1, z2, z3;
	double w0, w1, w2;
};

static inline void spa_dll_init(struct spa_dll *dll) {
	dll->bw = 0.0;
	dll->z1 = dll->z2 = dll->z3 = 0.0;
}

static inline void spa_dll_set_bw(struct spa_dll *dll, double bw,
				  unsigned period, unsigned rate) {
	double w = 2 * M_PI * bw * period / rate;
	dll->w0 = 1.0 - exp(-20.0 * w);
	dll->w1 = w * 1.5 / period;
	dll->w2 = w / 1.5;
	dll->bw = bw;
}

static inline double spa_dll_update(struct spa_dll *dll, double err) {
	dll->z1 += dll->w0 * (dll->w1 * err - dll->z1);
	dll->z2 += dll->w0 * (dll->z1 - dll->z2);
	dll->z3 += dll->w2 * dll->z2;
	return 1.0 - (dll->z2 + dll->z3);
}

#endif /* SPA_DLL_H */
