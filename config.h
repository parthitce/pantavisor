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

#ifndef PV_CONFIG_H
#define PV_CONFIG_H

#include <stdbool.h>

#include "utils/list.h"

enum {
	BL_UBOOT_PLAIN = 0,
	BL_UBOOT_PVK,
	BL_GRUB
};

struct pantavisor_cache {
	char *metacachedir;
	char *dropbearcachedir;
};

struct pantavisor_factory {
	char *autotok;
};

struct pantavisor_tpm {
	char *key;
	char *cert;
};

struct pantavisor_creds {
	char *type;
	char *host;
	int port;
	char *host_proxy;
	int port_proxy;
	int noproxyconnect;
	char *id;
	char *prn;
	char *secret;
	char *token;
	struct pantavisor_tpm tpm;
};

struct pantavisor_gc {
	int reserved;
	bool keep_factory;
	int threshold;
	int threshold_defertime;
};

struct pantavisor_storage {
	char *path;
	char *fstype;
	char *opts;
	char *mntpoint;
	char *mnttype;
	char *logtempsize;
	int wait;
	struct pantavisor_gc gc;
};

struct pantavisor_updater {
	int interval;
	int network_timeout;
	bool use_tmp_objects;
	int revision_retries;
	int revision_retry_timeout;
	int commit_delay;
};

struct pantavisor_bootloader {
	int type;
	bool mtd_only;
	char *mtd_path;
};

struct pantavisor_watchdog {
	bool enabled;
	int timeout;
};

struct pantavisor_network {
	char *brdev;
	char *braddress4;
	char *brmask4;
};

struct pantavisor_log {
	char *logdir;
	int logmax;
	int loglevel;
	int logsize;
	bool push;
	bool capture;
	bool loggers;
};

struct pantavisor_lxc {
	int log_level;
};

struct pantavisor_control {
	bool remote;
};

struct pantavisor_libthttp {
	int loglevel;
};

typedef enum {
	SB_DISABLED,
	SB_LENIENT,
	SB_STRICT,
} secureboot_mode_t;

struct pantavisor_secureboot {
	secureboot_mode_t mode;
};

struct pantavisor_config {
	struct pantavisor_cache cache;
	struct pantavisor_bootloader bl;
	struct pantavisor_creds creds;
	struct pantavisor_factory factory;
	struct pantavisor_storage storage;
	struct pantavisor_updater updater;
	struct pantavisor_watchdog wdt;
	struct pantavisor_network net;
	struct pantavisor_log log;
	struct pantavisor_lxc lxc;
	struct pantavisor_control control;
	struct pantavisor_libthttp libthttp;
	struct pantavisor_secureboot secureboot;
};

int pv_config_load_creds(void);
int pv_config_save_creds(void);

void pv_config_override_value(const char* key, const char* value);

void pv_config_free(void);

void pv_config_set_creds_id(char *id);
void pv_config_set_creds_prn(char *prn);
void pv_config_set_creds_secret(char *secret);

char* pv_config_get_cache_metacachedir(void);
char* pv_config_get_cache_dropbearcachedir(void);

char* pv_config_get_creds_type(void);
char* pv_config_get_creds_host(void);
int pv_config_get_creds_port(void);
char* pv_config_get_creds_host_proxy(void);
int pv_config_get_creds_port_proxy(void);
int pv_config_get_creds_noproxyconnect(void);
char* pv_config_get_creds_id(void);
char* pv_config_get_creds_prn(void);
char* pv_config_get_creds_secret(void);
char* pv_config_get_creds_token(void);

char* pv_config_get_factory_autotok(void);

char* pv_config_get_storage_path(void);
char* pv_config_get_storage_fstype(void);
char* pv_config_get_storage_opts(void);
char* pv_config_get_storage_mntpoint(void);
char* pv_config_get_storage_mnttype(void);
char* pv_config_get_storage_logtempsize(void);
int pv_config_get_storage_wait(void);

int pv_config_get_storage_gc_reserved(void);
bool pv_config_get_storage_gc_keep_factory(void);
int pv_config_get_storage_gc_threshold(void);
int pv_config_get_storage_gc_threshold_defertime(void);

int pv_config_get_updater_interval(void);
int pv_config_get_updater_network_timeout(void);
bool pv_config_get_updater_network_use_tmp_objects(void);
int pv_config_get_updater_revision_retries(void);
int pv_config_get_updater_revision_retry_timeout(void);
int pv_config_get_updater_commit_delay(void);

int pv_config_get_bl_type(void);
bool pv_config_get_bl_mtd_only(void);
char* pv_config_get_bl_mtd_path(void);

bool pv_config_get_watchdog_enabled(void);
int pv_config_get_watchdog_timeout(void);

char* pv_config_get_network_brdev(void);
char* pv_config_get_network_braddress4(void);
char* pv_config_get_network_brmask4(void);

char* pv_config_get_log_logdir(void);
int pv_config_get_log_logmax(void);
int pv_config_get_log_loglevel(void);
int pv_config_get_log_logsize(void);
bool pv_config_get_log_push(void);
bool pv_config_get_log_capture(void);
bool pv_config_get_log_loggers(void);
int pv_config_get_libthttp_loglevel(void);

bool pv_config_get_control_remote(void);
secureboot_mode_t pv_config_get_secureboot_mode(void);

#endif
