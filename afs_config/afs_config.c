// SPDX-License-Identifier: GPL-2.0
/*
 * oplus_afs_config - Oplus AFS (FScene) config proc nodes for peridot.
 *
 * The ColorOS userspace lib (system_ext/lib64/afsConfig.so, loaded by
 * system_server / SystemUI / launcher) opens
 * /proc/oplus_afs_config/afs_config and parses it as an
 * oplus.fscene.fcore.afsConfig protobuf. On a kernel without this node it
 * retries forever and logs:
 *
 *   E [OPLUS_AFS_CONFIG]: Failed to open AFS config file:
 *       /proc/oplus_afs_config/afs_config
 *   E AFS_CONFIG: Failed to get scene config
 *   E OplusAfsManager: afsConfigSceneConfigGet sceneType is null
 *
 * This module only serves that blob. It performs no scheduling, no cpufreq
 * and no policy work, and has no dependency on sched_assist / frame_boost.
 *
 * ColorOS opens the afs_config node with write access (O_RDWR) even though it
 * only ever reads it, so the files must be world-writable or every poll attempt
 * logs "Failed to open AFS config file" forever:
 *
 *   E [OPLUS_AFS_CONFIG]: Failed to open AFS config file:
 *       /proc/oplus_afs_config/afs_config
 *
 * afs_config also accepts a pushed payload and serves it back on read, and
 * afs_enable stores "1"/"0".
 *
 * Payload resolution order at load:
 *   1. /system_ext/etc/afsConfig.pb   (the real file, if present)
 *   2. built-in default captured from a working Oplus device
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>

#define AFS_PROC_DIR		"oplus_afs_config"
#define AFS_PROC_CONFIG		"afs_config"
#define AFS_PROC_ENABLE		"afs_enable"
#define AFS_PB_PATH		"/system_ext/etc/afsConfig.pb"
#define AFS_BUF_SIZE		4096

/*
 * oplus.fscene.fcore.afsConfig:
 *   field 1 (string)  = "20250515"   version / config date
 *   field 2 (varint)  = 2            schema revision
 *   field 3 (string)  = "true"       feature gate
 *   field 4 (message, repeated) = { 1,1,1,0,1 } and { 2,1,1,0,1 }
 */
static const u8 afs_default_payload[] = {
	0x0a, 0x08, 0x32, 0x30, 0x32, 0x35, 0x30, 0x35,
	0x31, 0x35, 0x10, 0x02, 0x1a, 0x04, 0x74, 0x72,
	0x75, 0x65, 0x22, 0x0a, 0x08, 0x01, 0x10, 0x01,
	0x18, 0x01, 0x20, 0x00, 0x28, 0x01, 0x22, 0x0a,
	0x08, 0x02, 0x10, 0x01, 0x18, 0x01, 0x20, 0x00,
	0x28, 0x01,
};

static u8 afs_buf[AFS_BUF_SIZE];
static size_t afs_len;
static bool afs_from_file;
static bool afs_enabled = true;
static DEFINE_MUTEX(afs_lock);
static struct proc_dir_entry *afs_dir;

static void afs_load_payload(void)
{
	struct file *f;
	loff_t pos = 0;
	ssize_t got;

	memset(afs_buf, 0, sizeof(afs_buf));
	afs_from_file = false;

	f = filp_open(AFS_PB_PATH, O_RDONLY, 0);
	if (!IS_ERR(f)) {
		got = kernel_read(f, afs_buf, sizeof(afs_buf) - 1, &pos);
		if (got > 0) {
			afs_len = (size_t)got;
			afs_from_file = true;
		}
		filp_close(f, NULL);
	}

	if (!afs_len) {
		memcpy(afs_buf, afs_default_payload, sizeof(afs_default_payload));
		afs_len = sizeof(afs_default_payload);
	}

	pr_info("oplus_afs_config: %zu bytes (from %s)\n", afs_len,
		afs_from_file ? AFS_PB_PATH : "builtin default");
}

static int afs_config_show(struct seq_file *m, void *v)
{
	mutex_lock(&afs_lock);
	if (afs_len)
		seq_write(m, afs_buf, afs_len);
	mutex_unlock(&afs_lock);
	return 0;
}

static int afs_config_open(struct inode *inode, struct file *file)
{
	return single_open(file, afs_config_show, NULL);
}

static ssize_t afs_config_write(struct file *file, const char __user *buf,
				size_t len, loff_t *ppos)
{
	if (len >= AFS_BUF_SIZE)
		return -EINVAL;

	mutex_lock(&afs_lock);
	if (copy_from_user(afs_buf, buf, len)) {
		mutex_unlock(&afs_lock);
		return -EFAULT;
	}
	afs_len = len;
	afs_from_file = false;
	mutex_unlock(&afs_lock);

	return len;
}

static const struct proc_ops afs_config_proc_ops = {
	.proc_open	= afs_config_open,
	.proc_read	= seq_read,
	.proc_write	= afs_config_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int afs_enable_show(struct seq_file *m, void *v)
{
	mutex_lock(&afs_lock);
	seq_printf(m, "%d\n", afs_enabled ? 1 : 0);
	mutex_unlock(&afs_lock);
	return 0;
}

static int afs_enable_open(struct inode *inode, struct file *file)
{
	return single_open(file, afs_enable_show, NULL);
}

static ssize_t afs_enable_write(struct file *file, const char __user *buf,
				size_t len, loff_t *ppos)
{
	char c;
	bool en;

	if (len == 0)
		return 0;

	if (copy_from_user(&c, buf, 1))
		return -EFAULT;

	switch (c) {
	case '0':
		en = false;
		break;
	case '1':
		en = true;
		break;
	default:
		return -EINVAL;
	}

	mutex_lock(&afs_lock);
	afs_enabled = en;
	mutex_unlock(&afs_lock);

	return len;
}

static const struct proc_ops afs_enable_proc_ops = {
	.proc_open	= afs_enable_open,
	.proc_read	= seq_read,
	.proc_write	= afs_enable_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int __init oplus_afs_config_init(void)
{
	afs_load_payload();

	afs_dir = proc_mkdir(AFS_PROC_DIR, NULL);
	if (!afs_dir) {
		pr_err("oplus_afs_config: proc_mkdir failed\n");
		return -ENOMEM;
	}

	if (!proc_create(AFS_PROC_CONFIG, 0666, afs_dir, &afs_config_proc_ops)) {
		pr_err("oplus_afs_config: create %s failed\n", AFS_PROC_CONFIG);
		proc_remove(afs_dir);
		return -ENOMEM;
	}

	if (!proc_create(AFS_PROC_ENABLE, 0666, afs_dir, &afs_enable_proc_ops)) {
		pr_err("oplus_afs_config: create %s failed\n", AFS_PROC_ENABLE);
		proc_remove(afs_dir);
		return -ENOMEM;
	}

	return 0;
}

static void __exit oplus_afs_config_exit(void)
{
	if (!afs_dir)
		return;
	remove_proc_entry(AFS_PROC_CONFIG, afs_dir);
	remove_proc_entry(AFS_PROC_ENABLE, afs_dir);
	remove_proc_entry(AFS_PROC_DIR, NULL);
	afs_dir = NULL;
}

module_init(oplus_afs_config_init);
module_exit(oplus_afs_config_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("OPLUS AFS config proc nodes");
MODULE_AUTHOR("hoshikv");
MODULE_VERSION("2.0");
