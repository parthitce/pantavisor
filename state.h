/*
 * Copyright (c) 2020 Pantacor Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef PV_STATE_H
#define PV_STATE_H

#include "pantavisor.h"

struct pv_state {
	int rev;
	char *spec;
	char *kernel;
	char *fdt;
	char *firmware;
	char *modules;
	char *initrd;
	struct dl_list platforms; // pv_platform
	struct dl_list volumes; // pv_volume
	struct dl_list addons; // pv_addon
	struct dl_list objects; //pv_object
	int retries;
	char *json;
	int tryonce;
};

struct pv_state* pv_state_new(int rev, char *spec);
void pv_state_free(struct pv_state *s);

void pv_state_print(struct pv_state *s);
void pv_state_validate(struct pv_state *s);

#endif