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
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include <dirent.h>
#include <netdb.h>
#include <inttypes.h>
#include <string.h>
#include <fcntl.h>
#include <libgen.h>
#include <errno.h>

#include <linux/limits.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/prctl.h>
#include <sys/statfs.h>

#include <mbedtls/sha256.h>

#include "updater.h"
#include "objects.h"
#include "storage.h"
#include "state.h"
#include "bootloader.h"
#include "init.h"
#include "utils.h"
#include "fops.h"
#include "addons.h"
#include "state.h"
#include "parser/parser.h"
#include "utils/json.h"
#include "utils/str.h"

#define MODULE_NAME             "storage"
#define pv_log(level, msg, ...)         vlog(MODULE_NAME, level, msg, ## __VA_ARGS__)
#include "log.h"

static int remove_at(char *path, char *filename)
{
	char full_path[PATH_MAX];

	sprintf(full_path, "%s/%s", path, filename);
	return remove(full_path);
}

static int remove_in(char *path, char *dirname)
{
	int n = 0;
	struct dirent **d;
	char full_path[512];

	sprintf(full_path, "%s/%s/", path, dirname);
	n = scandir(full_path, &d, NULL, alphasort);

	if (n < 0) {
		goto out;
	}

	while (n--) {
		// discard . and .. from scandir
		if (!strcmp(d[n]->d_name, ".") || !strcmp(d[n]->d_name, ".."))
			continue;
		// first try to remove it as a file
		if (remove_at(full_path, d[n]->d_name))
			// remove it as a dir if not a file
			remove_in(full_path, d[n]->d_name);
		free(d[n]);
	}
	free(d);

	if (remove(full_path))
		pv_log(WARN, "attempted to remove %s", full_path);

out:
	return n;
}

static int pv_storage_gc_objects(struct pantavisor *pv)
{
	int reclaimed = 0;
	struct stat st;
	struct pv_state *u;
	char path[PATH_MAX];
	char **obj, **obj_i;

	if (!pv->update)
		goto out;

	u = pv->update->pending;

	obj = pv_objects_get_all_ids(pv);
	for (obj_i = obj; *obj_i; obj_i++) {
		sprintf(path, "%s/objects/%s", pv_config_get_storage_mntpoint(), *obj_i);
		memset(&st, 0, sizeof(struct stat));
		if (stat(path, &st) < 0)
			continue;

		if (st.st_nlink > 1)
			continue;

		if (pv_objects_id_in_step(u, *obj_i))
			continue;

		// remove,unlink object and sync fs
		reclaimed += st.st_size;
		remove(path);
		sync();
		pv_log(DEBUG, "removed unused '%s', reclaimed %lu bytes", path, st.st_size);
	}

	if (obj) {
		obj_i = obj;
		while (*obj_i) {
			free(*obj_i);
			obj_i++;
		}
		free(obj);
	}

out:
	return reclaimed;
}

void pv_storage_rm_rev(struct pantavisor *pv, const char *rev)
{
	char path[PATH_MAX];
	char revision[PATH_MAX];

	pv_log(DEBUG, "removing revision %s from disk", rev);

	sprintf(revision, "%s", rev);

	sprintf(path, "%s/trails", pv_config_get_storage_mntpoint());
	remove_in(path, revision);

	sprintf(path, "%s/logs", pv_config_get_storage_mntpoint());
	remove_in(path, revision);

	sprintf(path, "%s/disks/rev", pv_config_get_storage_mntpoint());
	remove_in(path, revision);

	sync();
}

int pv_storage_get_subdir(const char* path, const char* prefix, struct dl_list *subdirs)
{
	int n, len, ret = 0;
	char *basedir;
	struct dirent **dirs = NULL;
	struct pv_path *subdir;

	len = strlen(path) + strlen(prefix) + 1;
	basedir = calloc(1, len);
	sprintf(basedir, "%s%s", path, prefix);

	n = scandir(basedir, &dirs, NULL, alphasort);
	if (n < 0)
		goto out;

	while (n--) {
		char *tmp = dirs[n]->d_name;

		while (*tmp)
			tmp++;

		if (tmp[0] != '\0')
			continue;

		subdir = calloc(1, sizeof(struct pv_path));
		if (!subdir) {
			ret = -1;
			goto out;
		}

		len = strlen(prefix) + strlen(dirs[n]->d_name) + 1;
		subdir->path = calloc(1, len );
		snprintf(subdir->path, len, "%s%s", prefix, dirs[n]->d_name);
		dl_list_init(&subdir->list);
		dl_list_add(subdirs, &subdir->list);
	}

out:
	if (basedir)
		free(basedir);
	if (dirs)
		free(dirs);

	return ret;
}

void pv_storage_free_subdir(struct dl_list *subdirs)
{
	struct pv_path *p, *tmp;

	dl_list_for_each_safe(p, tmp, subdirs, struct pv_path, list) {
		free(p->path);
		dl_list_del(&p->list);
		free(p);
	}
}

static int pv_storage_get_revisions(struct dl_list *revisions)
{
	int ret = -1, len;
	char *basedir;

	len = strlen("%s/trails/") + strlen(pv_config_get_storage_mntpoint()) + 1;
	basedir = calloc(1, len);
	sprintf(basedir, "%s/trails/", pv_config_get_storage_mntpoint());

	if (pv_storage_get_subdir(basedir, "locals/", revisions) ||
		pv_storage_get_subdir(basedir, "", revisions))
		goto out;

	ret = 0;

out:
	if (basedir)
		free(basedir);

	return ret;
}

int pv_storage_gc_run(struct pantavisor *pv)
{
	int reclaimed = 0, len;
	struct pv_state *s = 0, *u = 0;
	struct dl_list revisions; // pv_path
	struct pv_path *r, *tmp;

	if (pv->state)
		s = pv->state;

	if (pv->update)
		u = pv->update->pending;

	dl_list_init(&revisions);

	if (pv_storage_get_revisions(&revisions)) {
		pv_log(ERROR, "error parsings revs on disk for GC");
		return -1;
	}

	// check all revisions in list
	dl_list_for_each_safe(r, tmp, &revisions, struct pv_path, list) {
		len = strlen(r->path) + 1;
		// dont reclaim current, locals, update, last booted up revisions or factory if configured
		if (!strncmp(r->path, "..", len) ||
			!strncmp(r->path, ".", len) ||
			!strncmp(r->path, "current", len) ||
			!strncmp(r->path, "locals", len) ||
			!strncmp(r->path, "locals/..", len) ||
			!strncmp(r->path, "locals/.", len) ||
			(s && !strncmp(r->path, s->rev, len)) ||
			(u && !strncmp(r->path, u->rev, len)) ||
			!strncmp(r->path, pv_bootloader_get_rev(), len) ||
			(pv_config_get_storage_gc_keep_factory() && !strncmp(r->path, "0", len)))
			continue;

		// unlink the given revision from local storage
		pv_storage_rm_rev(pv, r->path);
	}

	pv_storage_free_subdir(&revisions);

	// get rid of orphaned objects
	reclaimed = pv_storage_gc_objects(pv);

	if (reclaimed)
		pv_log(DEBUG, "total reclaimed: %d bytes", reclaimed);

	return reclaimed;
}

struct pv_storage {
	off_t total;
	off_t free;
	int free_percentage;
	off_t reserved;
	int reserved_percentage;
	off_t real_free;
	int real_free_percentage;
	int threshold;
};

static struct pv_storage* pv_storage_new(struct pantavisor *pv)
{
	struct statfs buf;
	struct pv_storage* this;

	if (!pv)
		return NULL;

	if (statfs("/storage/config/pantahub.config", &buf) < 0)
		return NULL;


	this = calloc(1, sizeof(struct pv_storage));
	if (this) {
		this->total = (off_t) buf.f_bsize * (off_t) buf.f_blocks;
		this->free = (off_t) buf.f_bsize * (off_t) buf.f_bfree;
		if (this->total)
			this->free_percentage = (this->free * 100) / this->total;
		this->reserved_percentage = pv_config_get_storage_gc_reserved();
		this->reserved = (this->total * this->reserved_percentage) / 100;
		if (this->free > this->reserved)
			this->real_free = this->free - this->reserved;
		if (this->total)
			this->real_free_percentage = (this->real_free * 100) / this->total;
		this->threshold = pv_config_get_storage_gc_threshold();
		return this;
	}

	return NULL;
}

static void pv_storage_print(struct pv_storage* storage)
{
	pv_log(DEBUG, "total disk space: %"PRIu64" B", storage->total);
	pv_log(DEBUG, "free disk space: %"PRIu64" B (%d%% of total)", storage->free, storage->free_percentage);
	pv_log(DEBUG, "reserved disk space: %"PRIu64" B (%d%% of total)", storage->reserved, storage->reserved_percentage);
	pv_log(INFO, "real free disk space: %"PRIu64" B (%d%% of total)", storage->real_free, storage->real_free_percentage);
}

off_t pv_storage_get_free(struct pantavisor *pv)
{
	off_t real_free = 0;
	struct pv_storage* storage;

	storage = pv_storage_new(pv);
	if (storage) {
		pv_storage_print(storage);
		real_free = storage->real_free;
	}

	free(storage);

	return real_free;
}

bool pv_storage_threshold_reached(struct pantavisor *pv)
{
	bool threshold_reached = false;
	struct pv_storage* storage;

	storage = pv_storage_new(pv);
	if (storage &&
		(storage->real_free_percentage < storage->threshold)) {
		threshold_reached = true;
		pv_log(INFO, "free disk space is %d%%, which is under the %d%% threshold. Freeing up space", storage->real_free_percentage, storage->threshold);
	}

	free(storage);

	return threshold_reached;
}

int pv_storage_validate_file_checksum(char* path, char* checksum)
{
	int fd, ret = -1, bytes;
	mbedtls_sha256_context sha256_ctx;
	unsigned char buf[4096];
	unsigned char cloud_sha[32];
	unsigned char local_sha[32];
	char *tmp_sha;
	char byte[3];

	fd = open(path, O_RDONLY);
	if (fd < 0)
		goto out;

	mbedtls_sha256_init(&sha256_ctx);
	mbedtls_sha256_starts(&sha256_ctx, 0);

	while ((bytes = read(fd, buf, 4096)) > 0)
		mbedtls_sha256_update(&sha256_ctx, buf, bytes);

	mbedtls_sha256_finish(&sha256_ctx, local_sha);
	mbedtls_sha256_free(&sha256_ctx);

	// signed to unsigned
	tmp_sha = checksum;
	for (int i = 0, j = 0; j < 32; i=i+2, j++) {
		strncpy(byte, &tmp_sha[i], 2);
		byte[2] = 0;
		cloud_sha[j] = strtoul(byte, NULL, 16);
	}

	if(strncmp((char*)cloud_sha, (char*)local_sha, 32)) {
		pv_log(WARN, "sha256 mismatch in %s", path);
		goto out;
	}

	ret = 0;

out:
	close (fd);

	return ret;
}


bool pv_storage_validate_objects_object_checksum(char *checksum)
{
	int len;
	char path[PATH_MAX];

	len = strlen("%s/objects/%s") +
		strlen(pv_config_get_storage_mntpoint()) +
		strlen(checksum);
	snprintf(path, len, "%s/objects/%s",
		 pv_config_get_storage_mntpoint(),
		 checksum);

	pv_log(DEBUG, "validating checksum for object %s", path);
	return !pv_storage_validate_file_checksum(path, checksum);
}


bool pv_storage_validate_trails_object_checksum(const char *rev, const char *name, char *checksum)
{
	int len;
	char path[PATH_MAX];

	len = strlen("%s/trails/%s/%s") +
		strlen(pv_config_get_storage_mntpoint()) +
		strlen(rev) +
		strlen(name);
	snprintf(path, len, "%s/trails/%s/%s",
		pv_config_get_storage_mntpoint(),
		rev,
		name);

	pv_log(DEBUG, "validating checksum for object %s", path);
	return !pv_storage_validate_file_checksum(path, checksum);
}

bool pv_storage_validate_trails_json_value(const char *rev, const char *name, char *val)
{
	int len;
	char path[PATH_MAX];
	char *buf;

	len = strlen("%s/trails/%s/%s") +
		strlen(pv_config_get_storage_mntpoint()) +
		strlen(rev) +
		strlen(name);
	snprintf(path, len, "%s/trails/%s/%s",
		pv_config_get_storage_mntpoint(),
		rev,
		name);

	buf = pv_storage_load_file(NULL, path, 0);
	if (!buf) {
		pv_log(ERROR, "could not find %s", path);
		return false;
	}

	pv_log(DEBUG, "validating value for json %s", path);
	return pv_str_matches(val, strlen(val), buf, strlen(buf));

}

void pv_storage_set_active(struct pantavisor *pv)
{
	char *path = NULL, *cur = NULL, *pdir = NULL;

	path = calloc(1, PATH_MAX);
	cur = calloc(1, PATH_MAX);
	pdir = calloc(1, PATH_MAX);
	if (!path || !cur || !pdir)
		goto out;

	// path to current revision - relative and dir for fd
	sprintf(pdir, "%s/trails/", pv_config_get_storage_mntpoint());
	sprintf(path, "%s%s", pdir, pv->state->rev);
	sprintf(cur, "%s" "current", pdir);
	unlink(cur);
	symlink(path + strlen(pdir), cur);

	// path to current logs - relative and fd for dir
	sprintf(pdir, "%s/logs/", pv_config_get_storage_mntpoint());
	sprintf(path, "%s%s", pdir, pv->state->rev);
	sprintf(cur, "%scurrent", pdir);
	unlink(cur);
	symlink(path + strlen(pdir), cur);

out:
	if (pdir)
		free(pdir);
	if (cur)
		free(cur);
	if (path)
		free(path);
}

int pv_storage_update_factory(const char* rev)
{
	int res = -1, fd_c = -1, fd_f = -1;
	struct pantavisor *pv = pv_get_instance();
	char revision[PATH_MAX], factory[PATH_MAX], factory_parent[PATH_MAX];

	// init paths
	sprintf(factory_parent, PATH_TRAILS_PVR_PARENT, pv_config_get_storage_mntpoint(), "0");
	sprintf(factory, PATH_TRAILS, pv_config_get_storage_mntpoint(), "0");
	sprintf(revision, PATH_TRAILS, pv_config_get_storage_mntpoint(), rev);

	// first, remove revision 0 that is going to be substituted
	pv_storage_rm_rev(pv, "0");

	// now, create revision 0
	mkdir_p(factory_parent, 0755);

	// finally, copy revision json to revision 0
	fd_f = open(factory, O_CREAT | O_WRONLY | O_TRUNC, 0644);
	if (fd_f < 0) {
		pv_log(ERROR, "cannot open %s revision json: %s", factory, strerror(errno));
		goto out;
	}

	fd_c = open(revision, O_RDONLY, 0);
	if (fd_c < 0) {
		pv_log(ERROR, "cannot open %s revision json: %s", revision, strerror(errno));
		goto out;
	}

	if (pv_fops_copy_and_close(fd_c, fd_f) < 0) {
		pv_log(ERROR, "cannot copy %s into %s: %s", revision, factory, strerror(errno));
		goto out;
	}

	res = 0;
out:
	close(fd_c);
	close(fd_f);

	return res;
}

int pv_storage_make_config(struct pantavisor *pv)
{
	struct stat st;
	char targetpath[128];
	char srcpath[128];
	char cmd[PATH_MAX];
	int rv;

	sprintf(srcpath, "%s/trails/%s/_config/", pv_config_get_storage_mntpoint(), pv->state->rev);
	sprintf(targetpath, "/configs/");

	if (stat(targetpath, &st))
		mkdir_p(targetpath, 0755);

	memset(&st, '\0', sizeof(st));

	// we allow overloading behaviour via plugin from initrd addon
	if (!stat("/usr/local/bin/pvext_sysconfig", &st) &&
			st.st_mode & S_IXUSR ) {
		sprintf(cmd, "/usr/local/bin/pvext_sysconfig %s %s", srcpath, targetpath);
	} else {
		sprintf(cmd, "/bin/cp -a %s/* %s/", srcpath, targetpath);
	}
	pv_log(INFO, "processing trail _config: %s", cmd);

	/*
	 * [PKS]
	 * Should we do a tsh_run and wait
	 * for command to finish?
	 */
	rv = system(cmd);
	return rv;
}

bool pv_storage_is_revision_local(const char* rev)
{
	char *first = strchr(rev, '/');
	char *last = strrchr(rev, '/');

	if (strncmp(rev, "locals/", strlen("locals/")))
		return false;

	if (first && (first == last))
		return true;

	pv_log(WARN, "revision name %s not valid", rev);
	return false;
}

char* pv_storage_get_revisions_string()
{
	int len = 1, line_len;
	char *json = calloc(1, len), *basedir = NULL, *progress = NULL, *commitmsg = NULL, *esc_commitmsg = NULL;
	struct dl_list revisions; // pv_path
	struct pv_path *r, *tmp;

	dl_list_init(&revisions);
	if (pv_storage_get_revisions(&revisions)) {
		pv_log(ERROR, "error parsings revs on disk for ctrl");
		goto out;
	}

	// open json
	json[0]='[';

	if (dl_list_empty(&revisions)) {
		len++;
		goto out;
	}

	// fill up revision list in json
	dl_list_for_each_safe(r, tmp, &revisions, struct pv_path, list) {
		// dont list current or locals dir
		if (!strncmp(r->path, "..", strlen("..") + 1) ||
			!strncmp(r->path, ".", strlen(".") + 1) ||
			!strncmp(r->path, "current", strlen("current") + 1) ||
			!strncmp(r->path, "locals", strlen("locals") + 1) ||
			!strncmp(r->path, "locals/..", strlen("locals/..") + 1) ||
			!strncmp(r->path, "locals/.", strlen("locals/.") + 1))
			continue;

		// get revision base path
		line_len = strlen("%s/trails/%s") + strlen(pv_config_get_storage_mntpoint()) + strlen(r->path) + 1;
		basedir = calloc(1, line_len);
		sprintf(basedir, "%s/trails/%s", pv_config_get_storage_mntpoint(), r->path);

		// get revision progress
		progress = pv_storage_load_file(basedir, ".pv/progress", 512);
		if (!progress || !strlen(progress)) {
			progress = calloc(1, 3);
			sprintf (progress, "{}");
		}

		// get revision commit message
		commitmsg = pv_storage_load_file(basedir, ".pv/commitmsg", 512);
		if (commitmsg)
			esc_commitmsg = pv_json_format(commitmsg, strlen(commitmsg));

		if (!commitmsg || !esc_commitmsg) {
			esc_commitmsg = calloc(1, 1);
			esc_commitmsg[0] = '\0';
		}
		if (commitmsg) {
			free (commitmsg);
			commitmsg = NULL;
		}

		// add new revision line to json
		line_len = strlen(r->path) + strlen(esc_commitmsg) + strlen(progress) + 41;
		json = realloc(json, len + line_len + 1);
		snprintf(&json[len], line_len + 1, "{\"name\":\"%s\", \"commitmsg\":\"%s\", \"progress\":%s},", r->path, esc_commitmsg, progress);
		len += line_len;

		if (basedir) {
			free (basedir);
			basedir = NULL;
		}
		if (progress) {
			free (progress);
			progress = NULL;
		}
		if (esc_commitmsg) {
			free (esc_commitmsg);
			esc_commitmsg = NULL;
		}
	}

out:
	len += 1;
	json = realloc(json, len);
	// close json
	json[len-2] = ']';
	json[len-1] = '\0';

	// free temporary revision list
	dl_list_for_each_safe(r, tmp, &revisions, struct pv_path, list) {
		free(r->path);
		dl_list_del(&r->list);
		free(r);
	}

	if (basedir)
		free(basedir);
	if (progress)
		free(progress);
	if (commitmsg)
		free(commitmsg);

	return json;
}

void pv_storage_set_rev_done(struct pantavisor *pv, const char *rev)
{
	// DEPRECATED: this done files are not used anymore for rollback and bootloader env
	// are used insted. We keep it here to serve old versions in case a device needs to
	// be downgraded

	int fd;
	char path[256];

	sprintf(path, "%s/trails/%s/.pv/done", pv_config_get_storage_mntpoint(), rev);

	fd = open(path, O_CREAT | O_WRONLY, 0644);
	if (fd < 0) {
		pv_log(WARN, "unable to set current(done) flag for revision %s", rev);
		return;
	}

	// commit to disk
	fsync(fd);
	close(fd);
}

void pv_storage_set_rev_progress(const char *rev, const char *progress)
{
	int fd;
	char path[256];

	sprintf(path, "%s/trails/%s/.pv/progress", pv_config_get_storage_mntpoint(), rev);

	fd = open(path, O_CREAT | O_WRONLY | O_TRUNC , 0644);
	if (fd < 0) {
		pv_log(DEBUG, "unable to open progress file %s for revision %s"
				" (err=%s)", path, rev, strerror(errno));
		return;
	}

	if (write(fd, progress, strlen(progress)) < 0) {
		pv_log(DEBUG, "unable to write progress  (%s) to file (%s) for revision %s (err=%s)", progress, path, rev, strerror(errno));
		return;
	}

	// commit to disk
	fsync(fd);
	close(fd);
}

void pv_storage_meta_set_objdir(struct pantavisor *pv)
{
	int fd = 0;
	char path[PATH_MAX];
	struct stat st;

	if (!pv)
		return;

	sprintf(path, "%s/trails/%s/.pvr/config", pv_config_get_storage_mntpoint(), pv->state->rev);
	if (stat(path, &st) == 0)
		return;

	fd = open(path, O_CREAT | O_WRONLY, 0644);
	/*
	 * [PKS]
	 * check for
	 * fd < 0
	 */
	if (fd < 0)
		goto err;

	sprintf(path, "{\"ObjectsDir\": \"%s/objects\"}", pv_config_get_storage_mntpoint());
	/*
	 * [PKS]
	 * Use pv_fops_write_nointr
	 */
	if (write(fd, path, strlen(path)) < 0)
		goto err;

	close(fd);
	pv_log(DEBUG, "wrote '%s' to .pvr/config @rev=%s", path, pv->state->rev);

	return;
err:
	pv_log(WARN, "unable to set ObjectsDir pvr config key");
	if (fd)
		close(fd);
}

int pv_storage_meta_expand_jsons(struct pantavisor *pv, struct pv_state *s)
{
	int fd = -1, n, bytes, tokc;
	int ret = 0;
	char *buf = 0, *key = 0, *ext = 0;
	char *value = 0, *file = 0, *dir = 0;
	char path[PATH_MAX];
	struct stat st;
	jsmntok_t *tokv = 0;
	jsmntok_t **k, **keys;

	if (!pv || !s)
		goto out;

	buf = strdup(s->json);
	ret = jsmnutil_parse_json(buf, &tokv, &tokc);
	if (ret < 0)
		goto out;

	keys = jsmnutil_get_object_keys(buf, tokv);
	k = keys;

	while (*k) {
		n = (*k)->end - (*k)->start + 1;

		// copy key
		key = malloc(n+1);
		key[n] = 0;
		snprintf(key, n, "%s", buf+(*k)->start);
		ext = strrchr(key, '.');
		if (!ext || strcmp(ext, ".json")) {
			free(key);
			k++;
			continue;
		}

		// copy value
		n = (*k+1)->end - (*k+1)->start + 1;
		value = malloc(n+1);
		value[n] = 0;
		snprintf(value, n, "%s", buf+(*k+1)->start);

		sprintf(path, "%s/trails/%s/%s",
			pv_config_get_storage_mntpoint(), s->rev, key);

		if (stat(path, &st) == 0)
			goto out;

		file = strdup(path);
		dir = dirname(file);
		if (stat(dir, &st))
			mkdir_p(dir, 0755);
		free(file);

		fd = open(path, O_CREAT | O_SYNC | O_WRONLY, 0644);
		if (fd < 0)
			goto out;

		bytes = write(fd, value, strlen(value));
		if (bytes)
			pv_log(DEBUG, "%s: written %d bytes", path, bytes);

		close(fd);
		k++;
	}
	jsmnutil_tokv_free(keys);

	ret = 1;

out:
	if (buf)
		free(buf);
	if (tokv)
		free(tokv);
	if (fd > 0)
		close(fd);

	return ret;
}

void pv_storage_meta_set_tryonce(struct pantavisor *pv, int value)
{
	int fd;
	char path[PATH_MAX];

	sprintf(path, "%s/trails/%s/.pv/.tryonce", pv_config_get_storage_mntpoint(), pv->state->rev);

	if (value) {
		fd = open(path, O_WRONLY | O_CREAT | O_SYNC, 0444);
		if (fd >= 0)
			close(fd);
	} else {
		remove(path);
		sync();
	}
}

int pv_storage_meta_link_boot(struct pantavisor *pv, struct pv_state *s)
{
	int i;
	char src[PATH_MAX], dst[PATH_MAX], fname[PATH_MAX], prefix[32];
	struct pv_addon *a, *tmp;
	struct dl_list *addons = NULL;

	if (!s)
		s = pv->state;

	/*
	 * Toggle directory depth with null prefix
	 */
	switch (pv_state_spec(s)) {
	case SPEC_SYSTEM1:
		sprintf(prefix, "bsp/");
		break;
	case SPEC_MULTI1:
	default:
		prefix[0] = '\0';
		break;
	}

	// initrd
	sprintf(dst, "%s/trails/%s/.pv/", pv_config_get_storage_mntpoint(), s->rev);
	sprintf(src, "%s/trails/%s/%s%s", pv_config_get_storage_mntpoint(), s->rev, prefix, s->bsp.initrd);

	mkdir_p(dst, 0755);
	strcat(dst, "pv-initrd.img");

	remove(dst);
	if (link(src, dst) < 0)
		goto err;

	// addons
	i = 0;
	addons = &s->addons;
	dl_list_for_each_safe(a, tmp, addons,
			struct pv_addon, list) {
		sprintf(dst, "%s/trails/%s/.pv/", pv_config_get_storage_mntpoint(), s->rev);
		sprintf(src, "%s/trails/%s/%s%s", pv_config_get_storage_mntpoint(), s->rev, prefix, a->name);
		sprintf(fname, "pv-initrd.img.%d", i++);
		strcat(dst, fname);
		remove(dst);
		if (link(src, dst) < 0)
			goto err;
	}

	// kernel
	sprintf(dst, "%s/trails/%s/.pv/pv-kernel.img", pv_config_get_storage_mntpoint(), s->rev);
	sprintf(src, "%s/trails/%s/%s%s", pv_config_get_storage_mntpoint(), s->rev, prefix, s->bsp.kernel);

	remove(dst);
	if (link(src, dst) < 0)
		goto err;

	// fdt
	if (s->bsp.fdt) {
		sprintf(dst, "%s/trails/%s/.pv/pv-fdt.dtb", pv_config_get_storage_mntpoint(), s->rev);
		sprintf(src, "%s/trails/%s/%s%s", pv_config_get_storage_mntpoint(), s->rev, prefix, s->bsp.fdt);

		remove(dst);
		if (link(src, dst) < 0)
			goto err;
	}


	pv_log(DEBUG, "linked boot assets for rev=%s", s->rev);

	return 0;
err:
	pv_log(ERROR, "unable to link '%s' to '%s', errno %d", src, dst, errno);
	return -1;
}

char* pv_storage_get_state_json(const char *rev)
{
	char path[256];

	sprintf(path, "%s/trails/%s/.pvr/json", pv_config_get_storage_mntpoint(), rev);

	pv_log(DEBUG, "reading state from: '%s'", path);

	return pv_storage_load_file(NULL, path, 0);
}

char *pv_storage_load_file(const char *prefix, const char *path, const unsigned int max_size)
{
	struct stat st;
	unsigned int size;
	int fd, res;
	char abs_path[PATH_MAX];
	char *content = NULL;

	if (!prefix) {
		sprintf(abs_path, "%s", path);
	} else {
		sprintf(abs_path, "%s/%s", prefix, path);
	}

	stat(abs_path, &st);
	size = st.st_size;

	if (max_size && (size > max_size)) {
		pv_log(ERROR, "file size is too big %d %d", size, max_size);
		goto out;
	}

	content = calloc(1, size+1);
	if (!content) {
		pv_log(ERROR, "cannot alloc file buffer");
		goto out;
	}

	fd = open(abs_path, O_RDONLY, 0644);
	if (fd < 0) {
		pv_log(ERROR, "cannot open file: %s", strerror(errno));
		goto out;
	}

	res = read(fd, content, size);
	if (res < 0) {
		pv_log(ERROR, "cannot read file: %s", strerror(errno));
		goto out;
	}

	close(fd);
out:
	return content;
}

void pv_storage_save_file(const char *path_base, const char *name, const char *content)
{
	int fd;
	char path[PATH_MAX];

	if (!name || !content)
		return;

	sprintf(path, "%s/%s", path_base, name);

	fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
	if (fd < 0)
		goto out;

	write(fd, content, strlen(content));
	close(fd);

out:
	return;
}

void pv_storage_rm_file(const char *path_base, const char *name)
{
	char path[PATH_MAX];

	sprintf(path, "%s/%s", path_base, name);
	remove(path);
}

size_t pv_storage_get_file_size(const char *path)
{
	struct stat st;

	stat(path, &st);
	return st.st_size;
}

static int pv_storage_init(struct pv_init *this)
{
	struct pantavisor *pv = pv_get_instance();
	char tmp[256];
	int fd = -1;

	// create hints
	fd = open("/pv/challenge", O_CREAT | O_SYNC | O_WRONLY, 0444);
	close(fd);
	fd = open("/pv/device-id", O_CREAT | O_SYNC | O_WRONLY, 0444);
	if (!pv_config_get_creds_prn() ||
		(!strcmp(pv_config_get_creds_prn(), ""))) {
		pv->unclaimed = true;
	} else {
		pv->unclaimed = false;
		sprintf(tmp, "%s\n", pv_config_get_creds_id());
		write(fd, tmp, strlen(tmp));
	}
	close(fd);
	fd = open("/pv/pantahub-host", O_CREAT | O_SYNC | O_WRONLY, 0444);
	sprintf(tmp, "https://%s:%d\n", pv_config_get_creds_host(), pv_config_get_creds_port());
	write(fd, tmp, strlen(tmp));
	close(fd);

	return 0;
}

struct pv_init pv_init_storage = {
	.init_fn = pv_storage_init,
	.flags = 0,
};
