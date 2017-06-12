/*
 * Copyright (c) 2017 Pantacor Ltd.
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
#ifndef PV_OBJECTS_H
#define PV_OBJECTS_H

#define OBJPATH_FMT	"%s/objects/%s"
#define RELPATH_FMT	"%s/trails/%d/data/%s"

#include "pantavisor.h"

char** pv_objects_get_all_ids(struct pantavisor *pv);
int pv_objects_id_in_step(struct pantavisor *pv, struct pv_state *s, char *id);
struct pv_object* pv_objects_add(struct pv_state *s, char *filename, char *id, char *c);
struct pv_object* pv_objects_get_by_name(struct pv_state *s, char *name);
struct pv_object* pv_objects_get_by_id(struct pv_state *s, char *id);
void pv_objects_remove_all(struct pv_state *s);

#endif // PV_OBJECTS_H
