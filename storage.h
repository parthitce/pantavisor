/*
 * Copyright (c) 2017-2021 Pantacor Ltd.
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
#ifndef PV_STORAGE_H
#define PV_STORAGE_H

#define PATH_OBJECTS_TMP "%s/objects/%s.new"
#define PATH_OBJECTS "%s/objects/%s"
#define PATH_TRAILS_PVR_PARENT "%s/trails/%s/.pvr"
#define PATH_TRAILS_PV_PARENT "%s/trails/%s/.pv"
#define PATH_TRAILS "%s/trails/%s/.pvr/json"
#define PATH_TRAILS_PROGRESS "%s/trails/%s/.pv/progress"
#define PATH_TRAILS_COMMITMSG "%s/trails/%s/.pv/commitmsg"
#define PATH_USER_META "/pv/user-meta"
#define PATH_USERMETA_KEY "/pv/user-meta/%s"
#define PATH_USERMETA_PLAT "/pv/user-meta.%s"
#define PATH_USERMETA_PLAT_KEY "/pv/user-meta.%s/%s"

struct pv_path {
	char* path;
	struct dl_list list;
};

char* pv_storage_get_state_json(const char *rev);
void pv_storage_set_rev_done(struct pantavisor *pv, const char *rev);
void pv_storage_set_rev_progress(const char *rev, const char *progress);
void pv_storage_rm_rev(struct pantavisor *pv, const char *rev);
void pv_storage_set_active(struct pantavisor *pv);
int pv_storage_update_factory(const char* rev);
int pv_storage_make_config(struct pantavisor *pv);
bool pv_storage_is_revision_local(const char* rev);
char* pv_storage_get_revisions_string(void);

int pv_storage_get_subdir(const char* path, const char* prefix, struct dl_list *subdirs);
void pv_storage_free_subdir(struct dl_list *subdirs);

int pv_storage_validate_file_checksum(char* path, char* checksum);
bool pv_storage_validate_trails_object_checksum(const char *rev, const char *name, char *checksum);
bool pv_storage_validate_trails_json_value(const char *rev, const char *name, char *val);

int pv_storage_gc_run(struct pantavisor *pv);
off_t pv_storage_get_free(struct pantavisor *pv);
bool pv_storage_threshold_reached(struct pantavisor *pv);

void pv_storage_meta_set_objdir(struct pantavisor *pv);
int pv_storage_meta_expand_jsons(struct pantavisor *pv, struct pv_state *s);
int pv_storage_meta_link_boot(struct pantavisor *pv, struct pv_state *s);
void pv_storage_meta_set_tryonce(struct pantavisor *pv, int value);

void pv_storage_init_plat_usermeta(const char *name);
void pv_storage_save_usermeta(const char *key, const char *value);
void pv_storage_rm_usermeta(const char *key);

char *pv_storage_load_file(const char *path, const unsigned int max_size);
size_t pv_storage_get_file_size(const char *path);


#endif // PV_STORAGE_H
