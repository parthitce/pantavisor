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
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>

#include <sys/param.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/sysmacros.h>
#include <stdbool.h>

#include <linux/reboot.h>

#define MODULE_NAME			"updater"
#define pv_log(level, msg, ...)		vlog(MODULE_NAME, level, msg, ## __VA_ARGS__)
#include "log.h"

#include "tsh.h"
#include "pantavisor.h"
#include "version.h"
#include "init.h"
#include "utils.h"
#include "utils/list.h"
#include "pvlogger.h"
#include "platforms.h"
#include "state.h"

#define MAX_PROC_STATUS (10)
pid_t pv_pid;
pid_t shell_pid;

static int early_mounts()
{
	int ret;
	struct stat st;

	ret = mount("none", "/proc", "proc", MS_NODEV | MS_NOSUID | MS_NOEXEC, NULL);
	if (ret < 0)
		exit_error(errno, "Could not mount /proc");

	ret = mount("none", "/dev", "devtmpfs", 0, "size=10240k,mode=0755");
	if (ret < 0)
		exit_error(errno, "Could not mount /dev");

	ret = mount("none", "/sys", "sysfs", 0, NULL);
	if (ret < 0)
		exit_error(errno, "Could not mount /sys");

	mkdir("/dev/pts", 0755);
	ret = mount("none", "/dev/pts", "devpts", 0, NULL);
	if (ret < 0)
		exit_error(errno, "Could not mount /dev/pts");

	remove("/dev/ptmx");
	mknod("/dev/ptmx", S_IFCHR | 0666, makedev(5, 2));

	ret = mount("none", "/sys/fs/cgroup", "cgroup", 0, NULL);
	if (ret < 0)
		exit_error(errno, "Could not mount /sys/fs/cgroup");

	mkdir("/sys/fs/cgroup/systemd", 0555);
	ret = mount("cgroup", "/sys/fs/cgroup/systemd", "cgroup", 0, "none,name=systemd");
	if (ret < 0)
		exit_error(errno, "Could not mount /sys/fs/cgroup/systemd");

	mkdir("/sys/fs/cgroup/devices", 0555);
	ret = mount("cgroup", "/sys/fs/cgroup/devices", "cgroup", 0, "none,name=devices");
	if (ret < 0)
		exit_error(errno, "Could not mount /sys/fs/cgroup/systemd");

	mkdir("/writable", 0755);
	if (!stat("/etc/fstab", &st))
		tsh_run("mount -a", 1, NULL);

	mkdir("/root", 0700);
	ret = mount("none", "/root", "tmpfs", 0, NULL);
	if (ret < 0)
		exit_error(errno, "Could not mount /root");

	mkdir("/run", 0755);
	ret = mount("none", "/run", "tmpfs", 0, NULL);
	if (ret < 0)
		exit_error(errno, "Could not mount /run");

	return 0;
}

#ifdef PANTAVISOR_DEBUG
static void debug_telnet()
{
	tsh_run("ifconfig lo up", 0, NULL);
	tsh_run("telnetd -b 127.0.0.1 -l /bin/sh", 0, NULL);
	tsh_run("dropbear -p 0.0.0.0:8222 -n /pv/user-meta/pvr-sdk.authorized_keys -R -c /usr/bin/fallbear-cmd", 0, NULL);
}
#else
static void debug_telnet()
{
	printf("Pantavisor debug telnet disabled in production builds.\n");
}
#endif

static void signal_handler(int signal)
{
	pid_t pid = 0;
	int wstatus;
	struct pantavisor *pv = get_pv_instance();

	if (signal != SIGCHLD)
		return;

	while (	(pid = waitpid(-1, &wstatus, WNOHANG | WUNTRACED)) > 0) {
		struct pv_platform *p, *tmp_p;
		struct pv_log_info *l, *tmp_l;
		struct dl_list *head_platforms, *head_logger;
		bool found = false;

		/*
		 * See if the pid is one of the loggers
		 * */
		if (pv && pv->state) {
			head_platforms = &pv->state->platforms;
			dl_list_for_each_safe(p, tmp_p, head_platforms,
					struct pv_platform, list) {
				head_logger = &p->logger_list;
				dl_list_for_each_safe(l, tmp_l, head_logger,
						struct pv_log_info, next) {
					if (l->logger_pid == pid) {
						dl_list_del(&l->next);
						if (l->on_logger_closed) {
							l->on_logger_closed(l);
						}
						free(l);
						found = true;
					}
				}
				if (found)
					break;
			}
		}
		// Check for pantavisor
		if (pid != pv_pid)
			continue;

		pv_teardown(pv);

		if (WIFSIGNALED(wstatus) || WIFEXITED(wstatus)) {
			sleep(10);
			sync();
			reboot(LINUX_REBOOT_CMD_RESTART);
		}
	}
}

#ifdef PANTAVISOR_DEBUG
static void debug_shell()
{
	char c[64] = { 0 };
	int t = 5;
	int con_fd;

	con_fd = open("/dev/console", O_RDWR);
	if (!con_fd) {
		printf("Unable to open /dev/console\n");
		return;
	}

	dprintf(con_fd, "Press [d] for debug ash shell... ");
	fcntl(con_fd, F_SETFL, fcntl(con_fd, F_GETFL) | O_NONBLOCK);
	while (t && (read(con_fd, &c, sizeof(c)) < 0)) {
		dprintf(con_fd, "%d ", t);
		fflush(NULL);
		sleep(1);
		t--;
	}
	dprintf(con_fd, "\n");

	if (c[0] == 'd')
		shell_pid = tsh_run("sh", 0, NULL);
}
#else
static void debug_shell()
{
	printf("Pantavisor debug shell disabled in production builds\n");
}

#endif

#define PV_STANDALONE	(1 << 0)
#define	PV_DEBUG	(1 << 1)

static int is_arg(int argc, char *argv[], char *arg)
{
	if (argc < 2)
		return 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], arg) == 0)
			return 1;
	}

	return 0;
}

static void parse_args(int argc, char *argv[], unsigned short *args)
{
	if (is_arg(argc, argv, "pv_standalone"))
		*args |= PV_STANDALONE;

	if (is_arg(argc, argv, "debug"))
		*args |= PV_DEBUG;

	// For now
	*args |= PV_DEBUG;
}

int main(int argc, char *argv[])
{
	unsigned short args = 0;
	parse_args(argc, argv, &args);

	if (getpid() != 1) {
		if (is_arg(argc, argv, "--version")) {
			printf("version: %s\n", pv_build_version);
			return 0;
		}
		if (is_arg(argc, argv, "--manifest")) {
			printf("manifest: \n%s\n", pv_build_manifest);
			return 0;
		}
		pantavisor_init(false);
		return 0;
	}

	early_mounts();
	signal(SIGCHLD, signal_handler);

	if (args & PV_DEBUG) {
		debug_shell();
		debug_telnet();
	}

	// Run PV main loop
	if (!(args & PV_STANDALONE))
		pv_pid = pantavisor_init(true);

	// loop init
	for (;;)
		pause();

	return 0;
}
/*
 * The order of appearence is important here.
 * Make sure to list the initializer in the correct
 * order.
 */
struct pv_init *pv_init_tbl [] = {
	&pv_init_config,
	&pv_init_mount,
	&ph_init_config,
	&ph_init_mount,
	&pv_init_revision,
	&pv_init_log,
	&pv_init_device,
	&pv_init_network,
	&pv_init_platform,
	&pv_init_bl,
	&pv_init_state,
	&pv_init_update
};

int pv_do_execute_init()
{
	int i = 0;

	for ( i = 0; i < ARRAY_LEN(pv_init_tbl); i++) {
		struct pv_init *init = pv_init_tbl[i];
		int ret = 0;

		ret = init->init_fn(init);
		if (ret) {
			if (!(init->flags & PV_INIT_FLAG_CANFAIL))
				return -1;
		}
	}
	return 0;
}
