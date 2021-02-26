/*
 * Copyright (c) 2018 Pantacor Ltd.
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
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <libgen.h>

#include <sys/utsname.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <linux/limits.h>
#include <dirent.h>

#define MODULE_NAME             "device"
#define pv_log(level, msg, ...)         vlog(MODULE_NAME, level, msg, ## __VA_ARGS__)
#include "log.h"

#include "loop.h"

#include "utils.h"
#include "pantavisor.h"
#include "pantahub.h"
#include "device.h"
#include "version.h"
#include "init.h"
#include "cmd.h"
#include "revision.h"
#include "state.h"

#define FW_PATH		"/lib/firmware"

#define PV_USERMETA_ADD     (1<<0)
struct pv_usermeta {
	char *key;
	char *value;
	long flags;
	struct dl_list list; // pv_usermeta
};

struct pv_devmeta_read{
	char *key;
	char *buf;
	int buflen;
	int (*reader)(struct pv_devmeta_read*);
};

struct pv_devmeta {
	char *key;
	char *value;
	struct dl_list list; // pv_devmeta
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
	struct pantavisor *pv = get_pv_instance();

	if (pv_devmeta_buf_check(pv_devmeta_read))
		return -1;

	snprintf(buf, buflen, "%d", pv->state->rev);
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

static void usermeta_add_hint(struct pv_usermeta *m)
{
	int fd;
	char *path_base;
	char path[PATH_MAX];

	if (!m)
		return;

	sprintf(path, "/pv/user-meta/%s", m->key);
	path_base = strdup(path);

	dirname(path_base);
	if (strcmp("/pv/user-meta", path_base))
		mkdir_p(path_base, 0755);

	fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
	if (!fd)
		goto out;

	write(fd, m->value, strlen(m->value));
	close(fd);

out:
	free(path_base);

	return;
}

static void usermeta_remove_hint(struct pv_usermeta *m)
{
	char path[PATH_MAX];

	if (!m)
		return;

	sprintf(path, "/pv/user-meta/%s", m->key);
	remove(path);
}

static void pv_usermeta_free(struct pv_usermeta *usermeta)
{
	if (usermeta->key)
		free(usermeta->key);
	if (usermeta->value)
		free(usermeta->value);

	free(usermeta);
}

static void pv_usermeta_remove(struct pv_device *dev)
{
	struct pv_usermeta *curr, *tmp;
	struct dl_list *head = &dev->usermeta_list;

	pv_log(DEBUG, "removing user meta list");

	dl_list_for_each_safe(curr, tmp, head,
		struct pv_usermeta, list) {
		dl_list_del(&curr->list);
		pv_usermeta_free(curr);
	}
}

static void pv_devmeta_remove(struct pv_device *dev)
{
	struct pv_devmeta *curr, *tmp;
	struct dl_list *head = &dev->devmeta_list;

	pv_log(DEBUG, "removing devmeta list");

	dl_list_for_each_safe(curr, tmp, head,
		struct pv_devmeta, list) {
		dl_list_del(&curr->list);
		if (curr->key)
			free(curr->key);
		if (curr->value)
			free(curr->value);
		free(curr);
	}
}

static struct pv_usermeta* pv_usermeta_get_by_key(struct pv_device *d, char *key)
{
	struct pv_usermeta *curr, *tmp;
	struct dl_list *head = &d->usermeta_list;

	dl_list_for_each_safe(curr, tmp, head,
			struct pv_usermeta, list) {
		if (!strcmp(key, curr->key))
			return curr;
	}

	return NULL;
}

static struct pv_usermeta* pv_usermeta_add(struct pv_device *d, char *key, char *value)
{
	int changed = 1;
	struct pv_usermeta *curr;

	if (!d || !key)
		return NULL;

	curr = pv_usermeta_get_by_key(d, key);
	if (curr) {
		if (strcmp(curr->value, value) == 0)
			changed = 0;
		if (changed) {
			free(curr->value);
			curr->value = strdup(value);
		}
		goto out;
	}

	// not found? add
	curr = calloc(1, sizeof(struct pv_usermeta));
	if (curr) {
		dl_list_init(&curr->list);
		curr->key = strdup(key);
		curr->value = strdup(value);
		if (curr->key && curr->value)
			dl_list_add(&d->usermeta_list, &curr->list);
		else {
			if (curr->key)
				free(curr->key);
			if (curr->value)
				free(curr->value);
			free(curr);
			curr = NULL;
		}
	}
out:
	if (curr)
		curr->flags |= PV_USERMETA_ADD;

	if (changed && curr)
		usermeta_add_hint(curr);
	return curr;
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
		pv_usermeta_add(pv->dev, key, value);

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
	if (tokv)
		free(tokv);

	return ret;
}

static void usermeta_clear(struct pantavisor *pv)
{
	struct pv_usermeta *curr, *tmp;
	struct dl_list *head = NULL;

	if (!pv)
		return;
	if (!pv->dev)
		return;

	head = &pv->dev->usermeta_list;
	dl_list_for_each_safe(curr, tmp, head,
			struct pv_usermeta, list) {
		/*
		 * If ADD flag is set then clear it
		 * for the next check cycle.
		 */
		if (curr->flags & PV_USERMETA_ADD)
			curr->flags &= ~PV_USERMETA_ADD;
		else {
			dl_list_del(&curr->list);
			usermeta_remove_hint(curr);
			pv_usermeta_free(curr);
		}
	}
}

static struct pv_devmeta* pv_devmeta_add(struct pv_device *dev, char *key, char *value)
{
	struct pv_devmeta *this = NULL;

	if (!key || !value)
		goto out;

	this = calloc(1, sizeof(struct pv_devmeta));
	if (!this)
		goto out;

	dl_list_init(&this->list);
	this->key = strdup(key);
	if (!this->key) {
		free(this);
		this = NULL;
		goto out;
	}

	this->value = strdup(value);
	if (!this->value) {
		free(this->key);
		free(this);
		this = NULL;
		goto out;
	}
	dl_list_add(&dev->devmeta_list, &this->list);
out:
	if (!this) {
		pv_log(WARN, "Skipping device meta information [%s : %s]",
				(key ? key : "nil"),
				(value ? value : "nil"));
	}
	return this;
}

int pv_device_parse_devmeta(struct pantavisor *pv)
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

	dl_list_init(&pv->dev->devmeta_list);

	buf = log_buffer->buf;
	bufsize = log_buffer->size;

	for (i = 0; i < ARRAY_LEN(pv_devmeta_readkeys); i++) {
		int ret = 0;

		pv_devmeta_readkeys[i].buf = buf;
		pv_devmeta_readkeys[i].buflen = bufsize;
		ret = pv_devmeta_readkeys[i].reader(&pv_devmeta_readkeys[i]);
		if (!ret) {
			/*
			 * we managed to add at least one item in the list.
			 */
			pv_devmeta_add(pv->dev, pv_devmeta_readkeys[i].key, buf);
		}
	}
	pv_log_put_buffer(log_buffer);
	return 0;
}

int pv_device_upload_devmeta(struct pantavisor *pv)
{
	unsigned int len = 0;
	char *json = NULL;
	struct pv_devmeta *info = NULL, *tmp = NULL;
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

	if (dl_list_empty(&pv->dev->devmeta_list))
		goto out;
	json = log_buffer->buf;
	json_avail = log_buffer->size;
	json_avail -= sprintf(json, "{");
	len += 1;
	head = &pv->dev->devmeta_list;
	dl_list_for_each_safe(info, tmp, head,
			struct pv_devmeta, list) {
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
		pv_devmeta_remove(pv->dev);
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

static int __pv_device_factory_meta(struct pantavisor *pv, const char *factory_file)
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

int pv_device_factory_meta(struct pantavisor *pv)
{
	struct dirent **dirlist = NULL;
	int n = 0;
	char abs_path[PATH_MAX];
	char factory_dir[128];
	bool upload_failed = false;

	snprintf(factory_dir, sizeof(factory_dir), "%s/%s",
			pv->config->storage.mntpoint, "factory/meta");
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

					ret = __pv_device_factory_meta(pv,
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
		snprintf(abs_path, sizeof(abs_path), "%s/trails/0/.pv/factory-meta.done", pv->config->storage.mntpoint);
		fd = open(abs_path, O_CREAT | O_SYNC);
		if (fd < 0)
			pv_log(ERROR, "Unable to open file %s", abs_path);
		close(fd);
	}
	return upload_failed ? -1 : 0;
}

int pv_device_update_usermeta(struct pantavisor *pv, char *buf)
{
	int ret;
	char *body, *esc;

	body = strdup(buf);
	esc = unescape_str_to_ascii(body, "\\n", '\n');
	ret = pv_usermeta_parse(pv, esc);
	free(esc);
	// clear old
	usermeta_clear(pv);
	return ret;
}

static struct pv_usermeta* pv_device_get_usermeta(struct pantavisor *pv, char *key)
{
	if (!pv || !pv->dev)
		return NULL;

	struct pv_usermeta *curr, *tmp;
	struct dl_list *head = &pv->dev->usermeta_list;

	dl_list_for_each_safe(curr, tmp, head,
			struct pv_usermeta, list) {
		if (!strcmp(curr->key, key))
			return curr;
	}
	return NULL;
}

bool pv_device_push_logs_activated(struct pantavisor *pv)
{
	struct pv_usermeta* usermeta = NULL;

	// check metadata first
    usermeta = pv_device_get_usermeta(pv, "pantahub.log.push");
	if (usermeta)
	{
		if (!strcmp(usermeta->value, "0"))
			return false;

		return true;
	}

	// then, check config
	if (pv && pv->config)
		return pv->config->log.push;

	// default
	return true;
}

int pv_device_capture_logs_activated(struct pantavisor *pv)
{
	if (pv && pv->config)
		return pv->config->log.capture;

	// default
	return true;
}

bool pv_device_use_updater_tmp_objects(struct pantavisor *pv)
{
	if (pv && pv->config)
		return pv->config->updater.use_tmp_objects;

	// default
	return true;
}

int pv_device_get_gc_reserved(struct pantavisor *pv)
{
	if (pv && pv->config)
		return pv->config->storage.gc.reserved;

	return 0;
}

int pv_device_get_gc_threshold(struct pantavisor *pv)
{
	if (pv && pv->config)
		return pv->config->storage.gc.threshold;

	return 0;
}

static int pv_device_init(struct pv_init *this)
{
	struct pantavisor *pv = NULL;
	struct pantavisor_config *config = NULL;
	char tmp[256];
	int fd = -1;

	pv = get_pv_instance();
	if (!pv || !pv->config)
		return -1;
	config = pv->config;
	// create hints
	fd = open("/pv/challenge", O_CREAT | O_SYNC | O_WRONLY, 0444);
	close(fd);
	fd = open("/pv/device-id", O_CREAT | O_SYNC | O_WRONLY, 0444);
	if (strcmp(config->creds.prn, "") == 0) {
		pv->flags |= DEVICE_UNCLAIMED;
	} else {
		sprintf(tmp, "%s\n", config->creds.id);
		write(fd, tmp, strlen(tmp));
	}
	close(fd);
	fd = open("/pv/pantahub-host", O_CREAT | O_SYNC | O_WRONLY, 0444);
	sprintf(tmp, "https://%s:%d\n", config->creds.host, config->creds.port);
	write(fd, tmp, strlen(tmp));
	close(fd);

	pv->dev = calloc(1, sizeof(struct pv_device));
	if (pv->dev) {
		dl_list_init(&pv->dev->usermeta_list);
		pv->dev->id = strdup(pv->config->creds.id);
		if (!pv->dev->id) {
			free(pv->dev);
			pv->dev = NULL;
		}
	}
	if (pv_cmd_socket_open(pv, "/pv/pv-ctrl") < 0)
		pv_log(DEBUG, "control socket initialized fd=%d", pv->ctrl_fd);

	return 0;
}

bool pv_device_factory_meta_done(struct pantavisor *pv)
{
	char path[PATH_MAX];
	struct stat st;

	/*
	 * Don't check for meta done for non-factory
	 * boot revision. It's possible that trails/0
	 * may not exist and the device would then be
	 * stuck getting any updates.
	 */
	if (pv->state->rev != 0)
		return true;
	snprintf(path, sizeof(path), "%s/trails/0/.pv/factory-meta.done", pv->config->storage.mntpoint);

	if (stat(path, &st))
		return false;
	return true;
}

static void pv_device_free(struct pv_device *dev)
{
	if (!dev)
		return;

	pv_log(DEBUG, "removing device");

	if (dev->id)
		free(dev->id);
	if (dev->nick)
		free(dev->nick);
	if (dev->owner)
		free(dev->owner);
	if (dev->prn)
		free(dev->prn);

	pv_usermeta_remove(dev);
	pv_devmeta_remove(dev);

	free(dev);
}

void pv_device_remove(struct pantavisor *pv)
{
	pv_device_free(pv->dev);
	pv->dev = NULL;
}

struct pv_init pv_init_device = {
	.init_fn = pv_device_init,
	.flags = 0,
};
