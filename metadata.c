/*
 * Copyright (c) 2018-2021 Pantacor Ltd.
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

#include <libgen.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <sys/stat.h>

#include <linux/limits.h>

#include "metadata.h"
#include "version.h"
#include "state.h"
#include "pantahub.h"
#include "init.h"
#include "utils.h"
#include "config_parser.h"

#define MODULE_NAME             "metadata"
#define pv_log(level, msg, ...)         vlog(MODULE_NAME, level, msg, ## __VA_ARGS__)
#include "log.h"

#define PV_USERMETA_ADD     (1<<0)
struct pv_meta {
	char *key;
	char *value;
	bool updated;
	struct dl_list list; // pv_meta
};

struct pv_devmeta_read{
	char *key;
	char *buf;
	int buflen;
	int (*reader)(struct pv_devmeta_read*);
};

static int pv_devmeta_buf_check(struct pv_devmeta_read *pv_devmeta_read)
{
	char *buf = pv_devmeta_read->buf;
	int buflen = pv_devmeta_read->buflen;

	if (!buf || buflen <= 0)
		return -1;
	return 0;
}

static int pv_devmeta_read_version(struct pv_devmeta_read
						*pv_devmeta_read)
{
	char *buf = pv_devmeta_read->buf;
	int buflen = pv_devmeta_read->buflen;

	if (pv_devmeta_buf_check(pv_devmeta_read))
		return -1;
	snprintf(buf, buflen,"%s",(char *) pv_build_version);
	return 0;
}

static int pv_devmeta_read_arch(struct pv_devmeta_read 
						*pv_devmeta_read)
{
	char *buf = pv_devmeta_read->buf;
	int buflen = pv_devmeta_read->buflen;

	if (pv_devmeta_buf_check(pv_devmeta_read))
		return -1;
	snprintf(buf, buflen, "%s/%s/%s", PV_ARCH, PV_BITS, get_endian() ? "EL" : "EB");
	return 0;
}

static int pv_devmeta_read_dtmodel(struct pv_devmeta_read 
						*pv_devmeta_read) 
{
	char *buf = pv_devmeta_read->buf;
	int buflen = pv_devmeta_read->buflen;
	int ret = -1;

	if (pv_devmeta_buf_check(pv_devmeta_read))
		return -1;

	ret = get_dt_model(buf, buflen);
	if (ret < 0)
		memset(buf, 0, buflen);
	return 0;
}

static int pv_devmeta_read_cpumodel(struct pv_devmeta_read
						*pv_devmeta_read)
{
	char *buf = pv_devmeta_read->buf;
	int buflen = pv_devmeta_read->buflen;
	int ret = -1;

	if (pv_devmeta_buf_check(pv_devmeta_read))
		return -1;

	ret = get_cpu_model(buf, buflen);
	if (ret < 0)
		memset(buf, 0, buflen);
	return 0;
}

static int pv_devmeta_read_revision(struct pv_devmeta_read
						*pv_devmeta_read)
{
	char *buf = pv_devmeta_read->buf;
	int buflen = pv_devmeta_read->buflen;
	struct pantavisor *pv = pv_get_instance();

	if (pv_devmeta_buf_check(pv_devmeta_read))
		return -1;

	snprintf(buf, buflen, "%s", pv->state->rev);
	return 0;
}

static struct pv_devmeta_read pv_devmeta_readkeys[] = {
	{
		.key = "pantavisor.arch",
		.reader = pv_devmeta_read_arch
	},
	{	.key = "pantavisor.version",
		.reader = pv_devmeta_read_version
	},
	{	.key = "pantavisor.dtmodel",
		.reader = pv_devmeta_read_dtmodel
	},
	{	.key = "pantavisor.cpumodel",
		.reader = pv_devmeta_read_cpumodel
	},
	{	.key = "pantavisor.revision",
		.reader = pv_devmeta_read_revision
	}
};

static void usermeta_add_hint(const char *key, const char *value)
{
	int fd;
	char *path_base;
	char path[PATH_MAX];

	if (!key || !value)
		return;

	sprintf(path, "/pv/user-meta/%s", key);
	path_base = strdup(path);

	dirname(path_base);
	if (strcmp("/pv/user-meta", path_base))
		mkdir_p(path_base, 0755);

	fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
	if (!fd)
		goto out;

	write(fd, value, strlen(value));
	close(fd);

out:
	free(path_base);

	return;
}

static void usermeta_remove_hint(struct pv_meta *m)
{
	char path[PATH_MAX];

	if (!m)
		return;

	sprintf(path, "/pv/user-meta/%s", m->key);
	remove(path);
}

static void pv_usermeta_free(struct pv_meta *usermeta)
{
	if (usermeta->key)
		free(usermeta->key);
	if (usermeta->value)
		free(usermeta->value);

	free(usermeta);
}

static void pv_usermeta_remove(struct pv_metadata *metadata)
{
	struct pv_meta *curr, *tmp;
	struct dl_list *head = &metadata->usermeta;

	pv_log(DEBUG, "removing user meta list");

	dl_list_for_each_safe(curr, tmp, head,
		struct pv_meta, list) {
		dl_list_del(&curr->list);
		pv_usermeta_free(curr);
	}
}

static void pv_devmeta_remove(struct pv_metadata *metadata)
{
	struct pv_meta *curr, *tmp;
	struct dl_list *head = &metadata->devmeta;

	pv_log(DEBUG, "removing devmeta list");

	dl_list_for_each_safe(curr, tmp, head,
		struct pv_meta, list) {
		dl_list_del(&curr->list);
		if (curr->key)
			free(curr->key);
		if (curr->value)
			free(curr->value);
		free(curr);
	}
}

static struct pv_meta* pv_metadata_get_by_key(struct dl_list *head, char *key)
{
	struct pv_meta *curr, *tmp;

	dl_list_for_each_safe(curr, tmp, head,
			struct pv_meta, list) {
		if (!strcmp(key, curr->key))
			return curr;
	}

	return NULL;
}

static int pv_metadata_add(struct dl_list *head, char *key, char *value)
{
	int ret = -1;
	struct pv_meta *curr;

	if (!head || !key || !value)
		goto out;

	// find and update value
	curr = pv_metadata_get_by_key(head, key);
	if (curr) {
		if (strcmp(curr->value, value) == 0) {
			curr->updated = true;
			ret = 0;
		} else {
			free(curr->value);
			curr->value = strdup(value);
			curr->updated = true;
			ret = 1;
		}
		goto out;
	}

	// add new key and value pair
	curr = calloc(1, sizeof(struct pv_meta));
	if (curr) {
		dl_list_init(&curr->list);
		curr->key = strdup(key);
		curr->value = strdup(value);
		if (curr->key && curr->value) {
			dl_list_add(head, &curr->list);
			curr->updated = true;
			ret = 1;
		} else {
			if (curr->key)
				free(curr->key);
			if (curr->value)
				free(curr->value);
			free(curr);
		}
	}

out:
	return ret;
}

static void pv_usermeta_add(struct pv_metadata *d, char *key, char *value)
{
	int ret = pv_metadata_add(&d->usermeta, key, value);

	if (ret > 0) {
		pv_log(DEBUG, "user metadata key %s added or updated", key);
		pv_config_override_value(key, value);
		usermeta_add_hint(key, value);
	}
}

static int pv_usermeta_parse(struct pantavisor *pv, char *buf)
{
	int ret = 0, tokc, n;
	jsmntok_t *tokv;
	jsmntok_t **keys, **key_i;
	char *um, *key, *value;

	// Parse full device json
	ret = jsmnutil_parse_json(buf, &tokv, &tokc);
	um = get_json_key_value(buf, "user-meta", tokv, tokc);

	if (!um) {
		ret = -1;
		goto out;
	}

	if (tokv)
		free(tokv);

	ret = jsmnutil_parse_json(um, &tokv, &tokc);
	keys = jsmnutil_get_object_keys(um, tokv);

	key_i = keys;
	while (*key_i) {
		n = (*key_i)->end - (*key_i)->start;

		// copy key
		key = malloc(n+1);
		if (!key)
			break;

		snprintf(key, n+1, "%s", um+(*key_i)->start);

		// copy value
		n = (*key_i+1)->end - (*key_i+1)->start;
		value = malloc(n+1);
		if (!value)
			break;

		snprintf(value, n+1, "%s", um+(*key_i+1)->start);

		// add or update metadata
		pv_usermeta_add(pv->metadata, key, value);

		// free intermediates
		if (key) {
			free(key);
			key = 0;
		}
		if (value) {
			free(value);
			value = 0;
		}

		key_i++;
	}

	jsmnutil_tokv_free(keys);

out:
	if (um)
		free(um);
	if (tokv)
		free(tokv);

	return ret;
}

static void usermeta_clear(struct pantavisor *pv)
{
	struct pv_meta *curr, *tmp;
	struct dl_list *head = NULL;

	if (!pv)
		return;
	if (!pv->metadata)
		return;

	head = &pv->metadata->usermeta;
	dl_list_for_each_safe(curr, tmp, head,
			struct pv_meta, list) {
		// clear the flag updated for next iteration
		if (curr->updated)
			curr->updated = false;
		// not updated means user meta is no longer in cloud
		else {
			dl_list_del(&curr->list);
			usermeta_remove_hint(curr);
			pv_usermeta_free(curr);
		}
	}
}

static void pv_devmeta_add(struct pv_metadata *metadata, char *key, char *value)
{
	int ret = pv_metadata_add(&metadata->devmeta, key, value);

	if (ret > 0)
		pv_log(INFO, "device metadata key %s added or updated with value %s", key, value);
}

int pv_metadata_parse_devmeta(struct pantavisor *pv)
{
	char *buf = NULL;
	struct log_buffer *log_buffer = NULL;
	int i = 0, bufsize = 0;
	/*
	 * we can use one of the large log_buffer. Since
	 * this information won't be very large, it's safe
	 * to assume even the complete json would
	 * be small enough to fit inside this log_buffer.
	 */
	log_buffer = pv_log_get_buffer(true);
	if (!log_buffer) {
		pv_log(INFO, "couldn't allocate buffer to upload device info");
		return -1;
	}

	buf = log_buffer->buf;
	bufsize = log_buffer->size;

	for (i = 0; i < ARRAY_LEN(pv_devmeta_readkeys); i++) {
		int ret = 0;

		pv_devmeta_readkeys[i].buf = buf;
		pv_devmeta_readkeys[i].buflen = bufsize;
		ret = pv_devmeta_readkeys[i].reader(&pv_devmeta_readkeys[i]);
		if (!ret)
			pv_devmeta_add(pv->metadata, pv_devmeta_readkeys[i].key, buf);
	}
	pv_log_put_buffer(log_buffer);
	pv->metadata->devmeta_uploaded = false;
	return 0;
}

int pv_metadata_upload_devmeta(struct pantavisor *pv)
{
	unsigned int len = 0;
	char *json = NULL;
	struct pv_meta *info = NULL, *tmp = NULL;
	struct dl_list *head = NULL;
	int json_avail = 0;
	struct log_buffer *log_buffer = NULL;
	/*
	 * we can use one of the large log_buffer. Since
	 * this information won't be very large, it's safe
	 * to assume even the complete json would
	 * be small enough to fit inside this log_buffer.
	 */
	log_buffer = pv_log_get_buffer(true);
	if (!log_buffer) {
		pv_log(INFO, "couldn't allocate buffer to upload device info");
		return -1;
	}

	if (pv->metadata->devmeta_uploaded)
		goto out;
	json = log_buffer->buf;
	json_avail = log_buffer->size;
	json_avail -= sprintf(json, "{");
	len += 1;
	head = &pv->metadata->devmeta;
	dl_list_for_each_safe(info, tmp, head,
			struct pv_meta, list) {
		char *key = format_json(info->key, strlen(info->key));
		char *val = format_json(info->value, strlen(info->value));

		if (key && val) {
			int frag_len = strlen(key) + strlen(val) +
				/* 2 pairs of quotes*/
				2 * 2 +
				/* 1 colon and a ,*/
				1 + 1;
			if (json_avail > frag_len) {
				snprintf(json + len, json_avail,
						"\"%s\":\"%s\",",
						key, val);
				len += frag_len;
				json_avail -= frag_len;
			}
		}
		if (key)
			free(key);
		if (val)
			free(val);
	}
	/*
	 * replace , with closing brace.
	 */
	json[len - 1] = '}';
	pv_log(INFO, "device info json = %s", json);
	if(!pv_ph_upload_metadata(pv, json))
		pv->metadata->devmeta_uploaded = true;
out:
	pv_log_put_buffer(log_buffer);
	return 0;
}

/*
 * For iteration over config items.
 */
struct json_buf {
	char *buf;
	const char *factory_file;
	int len;
	int avail;
};
/*
 * opaque is the json buffer.
 */
static int on_factory_meta_iterate(char *key, char *value, void *opaque)
{
	struct json_buf *json_buf = (struct json_buf*) opaque;
	char abs_key[PATH_MAX + (PATH_MAX / 2)];
	char *formatted_key = NULL;
	char *formatted_val = NULL;
	int json_avail = json_buf->avail;
	int len = json_buf->len;
	bool written = false;
	char file[PATH_MAX];
	char *fname = NULL;

	strcpy(file, json_buf->factory_file);
	fname = basename(file);
	snprintf(abs_key, sizeof(abs_key), "factory/%s/%s", fname, key);
	formatted_key = format_json(abs_key, strlen(abs_key));
	formatted_val = format_json(value, strlen(value));

	if (formatted_key && formatted_val) {
		int frag_len = strlen(formatted_key) + strlen(formatted_val) +
			/* 2 pairs of quotes*/
			2 * 2 +
			/* 1 colon and a ,*/
			1 + 1;
		if (json_avail > frag_len) {
			snprintf(json_buf->buf + len, json_avail,
					"\"%s\":\"%s\",",
					formatted_key, formatted_val);
			len += frag_len;
			json_avail -= frag_len;
			json_buf->len = len;
			json_buf->avail = json_avail;
			written = true;
		}
	}
	if (formatted_key)
		free(formatted_key);
	if (formatted_val)
		free(formatted_val);
	return written ? 0 : -1;
}

static int __pv_metadata_factory_meta(struct pantavisor *pv, const char *factory_file)
{
	int ret = -1;
	DEFINE_DL_LIST(factory_kv_list);
	struct log_buffer *log_buffer = NULL;
	char *json_holder = NULL;
	int json_len = 0;
	int json_avail = 0;
	struct json_buf json_buf;

	if (!factory_file)
		goto out;
	ret = load_key_value_file(factory_file, &factory_kv_list);
	if (ret < 0)
		goto out;
	log_buffer = pv_log_get_buffer(true);
	if (!log_buffer)
		goto out;

	json_holder = log_buffer->buf;
	json_avail = log_buffer->size;
	json_avail -= sprintf(json_holder, "{");
	json_len += 1;

	json_buf.buf = json_holder;
	json_buf.len = json_len;
	json_buf.avail = json_avail;
	json_buf.factory_file = factory_file;
	config_iterate_items(&factory_kv_list,
			on_factory_meta_iterate, &json_buf);
	json_len = json_buf.len;
	/*
	 * replace last ,.
	 */
	json_holder[json_len - 1] = '}';
	
	ret = pv_ph_upload_metadata(pv, json_holder);
	pv_log_put_buffer(log_buffer);
	pv_log(INFO, "metadata_json : %s", json_holder);
	config_clear_items(&factory_kv_list);
out:
	return ret;
}

int pv_metadata_factory_meta(struct pantavisor *pv)
{
	struct dirent **dirlist = NULL;
	int n = 0;
	char abs_path[PATH_MAX];
	char factory_dir[128];
	bool upload_failed = false;

	snprintf(factory_dir, sizeof(factory_dir), "%s/%s",
			pv_config_get_storage_mntpoint(), "factory/meta");
	n = scandir(factory_dir, &dirlist, NULL, alphasort);
	if (n < 0)
		pv_log(WARN, "%s: %s", factory_dir, strerror(errno));
	while (n > 0) {
		struct stat st;
		n--;
		if (!upload_failed) {
			snprintf(abs_path, sizeof(abs_path),
				"%s/%s", factory_dir, dirlist[n]->d_name);
			if (!stat(abs_path, &st)) {
				if ((st.st_mode & S_IFMT) == S_IFREG) {
					int ret = -1;

					ret = __pv_metadata_factory_meta(pv,
							(const char*)abs_path);
					if (ret)
						upload_failed = true;
				}
			}
		}
		free(dirlist[n]);
	}
	if (dirlist)
		free(dirlist);
	if (!upload_failed) {
		int fd;
		/*
		 * reusing abs_path
		 */
		snprintf(abs_path, sizeof(abs_path), "%s/trails/0/.pv/factory-meta.done", pv_config_get_storage_mntpoint());
		fd = open(abs_path, O_CREAT | O_SYNC);
		if (fd < 0)
			pv_log(ERROR, "Unable to open file %s", abs_path);
		close(fd);
	}
	return upload_failed ? -1 : 0;
}

int pv_metadata_update_usermeta(struct pantavisor *pv, char *buf)
{
	int ret;
	char *body, *esc;

	body = strdup(buf);
	esc = unescape_str_to_ascii(body, "\\n", '\n');
	ret = pv_usermeta_parse(pv, esc);
	if (body)
		free(body);
	if (esc)
		free(esc);
	// clear old
	usermeta_clear(pv);
	return ret;
}

static struct pv_meta* pv_metadata_get_usermeta(struct pantavisor *pv, char *key)
{
	if (!pv || !pv->metadata)
		return NULL;

	struct pv_meta *curr, *tmp;
	struct dl_list *head = &pv->metadata->usermeta;

	dl_list_for_each_safe(curr, tmp, head,
			struct pv_meta, list) {
		if (!strcmp(curr->key, key))
			return curr;
	}
	return NULL;
}

static int pv_metadata_init(struct pv_init *this)
{
	struct pantavisor *pv = pv_get_instance();

	pv->metadata = calloc(1, sizeof(struct pv_metadata));
	if (!pv->metadata)
		return -1;

	dl_list_init(&pv->metadata->usermeta);
	dl_list_init(&pv->metadata->devmeta);

	pv->metadata->devmeta_uploaded = false;

	return 0;
}

bool pv_metadata_factory_meta_done(struct pantavisor *pv)
{
	char path[PATH_MAX];
	struct stat st;

	/*
	 * Don't check for meta done for non-factory
	 * boot revision. It's possible that trails/0
	 * may not exist and the device would then be
	 * stuck getting any updates.
	 */
	if (strncmp(pv->state->rev, "0", strlen(pv->state->rev) + 1))
		return true;
	snprintf(path, sizeof(path), "%s/trails/0/.pv/factory-meta.done", pv_config_get_storage_mntpoint());

	if (stat(path, &st))
		return false;
	return true;
}

static void pv_metadata_free(struct pv_metadata *metadata)
{
	if (!metadata)
		return;

	pv_log(DEBUG, "removing metadata");

	pv_usermeta_remove(metadata);
	pv_devmeta_remove(metadata);

	free(metadata);
}

static char* pv_metadata_get_meta_string(struct dl_list *meta_list)
{
	struct pv_meta *curr, *tmp;
	int len = 1, line_len;
	char *out = calloc(1, len * sizeof(char*));

	// open json
	out[0]='{';
	// add value,key pair to json
	dl_list_for_each_safe(curr, tmp, meta_list,
		struct pv_meta, list) {
		line_len = strlen(curr->key) + strlen(curr->value) + 7;
		out = realloc(out, (len + line_len) * sizeof(char*));
		snprintf(&out[len], line_len + 1, "\"%s\": \"%s\",", curr->key, curr->value);
		len += line_len;
	}

	len += 2;
	out = realloc(out, len * sizeof(char*));
	// close json
	out[len-2] = '}';
	out[len-1] = '\0';

	return out;
}

char* pv_metadata_get_user_meta_string()
{
	return pv_metadata_get_meta_string(&pv_get_instance()->metadata->usermeta);
}

char* pv_metadata_get_device_meta_string()
{
	return pv_metadata_get_meta_string(&pv_get_instance()->metadata->devmeta);
}

void pv_metadata_remove(struct pantavisor *pv)
{
	pv_metadata_free(pv->metadata);
	pv->metadata = NULL;
}

struct pv_init pv_init_metadata = {
	.init_fn = pv_metadata_init,
	.flags = 0,
};