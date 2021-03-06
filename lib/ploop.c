/*
 *  Copyright (C) 2008-2012, Parallels, Inc. All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/sysmacros.h>
#include <limits.h>
#include <sys/file.h>
#include <fcntl.h>
#include <sys/vfs.h>
#include <sys/syscall.h>
#include <mntent.h>
#include <ext2fs/ext2_fs.h>

#include "ploop.h"
#include "cleanup.h"

static int ploop_mount_fs(struct ploop_mount_param *param);

static off_t round_bdsize(off_t size, __u32 blocksize, int version)
{
	if (version == PLOOP_FMT_V1 &&
			(size > 0xffffffff - blocksize))
		return (size / blocksize * blocksize);
	else if (version == PLOOP_FMT_V2 &&
			(size / blocksize > 0xffffffff - 1))
		return (size / blocksize * blocksize);

	return ROUNDUP(size, blocksize);
}

/* set cancel flag
 * Note: this function also clear the flag
 */
static int is_operation_cancelled(void)
{
	struct ploop_cancel_handle *cancel_data;

	cancel_data = ploop_get_cancel_handle();
	if (cancel_data->flags) {
		cancel_data->flags = 0;
		return 1;
	}
	return 0;
}

void free_mount_param(struct ploop_mount_param *param)
{
	free(param->target);
	free(param->guid);
}

static off_t bytes2sec(__u64 bytes)
{
	return (bytes >> PLOOP1_SECTOR_LOG) + ((bytes % SECTOR_SIZE) ? 1 : 0);
}

int sys_fallocate(int fd, int mode, off_t offset, off_t len)
{
	return syscall(__NR_fallocate, fd, mode, offset, len);
}

int sys_syncfs(int fd)
{
	return syscall(__NR_syncfs, fd);
}

int get_list_size(char **list)
{
	int i;
	for (i = 0; list[i] != NULL; i++);

	return i;
}

static char **make_images_list(struct ploop_disk_images_data *di, const char *guid, int reverse)
{
	int n;
	char **images;
	char *file;
	int done = 0;
	int snap_id;

	assert(guid);

	if (di->nimages == 0) {
		ploop_err(0, "No images");
		return NULL;
	}

	images = malloc(sizeof(char *) * (di->nimages + 1));
	if (images == NULL)
		return NULL;

	for (n = 0; n < di->nsnapshots; n++) {
		snap_id = find_snapshot_by_guid(di, guid);
		if (snap_id == -1) {
			ploop_err(0, "Can't find snapshot by uuid %s", guid);
			goto err;
		}
		file = find_image_by_guid(di, guid);
		if (file == NULL) {
			ploop_err(0, "Can't find image by guid %s", guid);
			goto err;
		}
		images[n] = strdup(file);
		if (images[n] == NULL)
			goto err;
		if (n == di->nimages) {
			ploop_err(0, "Inconsistency detected: snapshots > images");
			goto err;
		}
		guid = di->snapshots[snap_id]->parent_guid;
		if (!strcmp(guid, NONE_UUID)) {
			done = 1;
			break;
		}
	}
	if (!done) {
		ploop_err(0, "Inconsistency detected, base image not found");
		goto err;
	}
	images[++n] = NULL;

	if (!reverse) {
		int i;

		for (i = 0; i < n / 2; i++) {
			file = images[n-i-1];
			images[n-i-1] = images[i];
			images[i] = file;
		}
	}
	return images;

err:
	images[n] = NULL;
	free_images_list(images);
	return NULL;
}

static int get_snapshot_count(struct ploop_disk_images_data *di)
{
	int n;
	char **images;

	images = make_images_list(di, di->top_guid, 1);
	if (images == NULL)
		return -1;
	n = get_list_size(images);
	free_images_list(images);

	return n;
}

void free_images_list(char **images)
{
	int i;

	if (images == NULL)
		return;
	for (i = 0; images[i] != NULL; i++)
		free(images[i]);

	free(images);
}

static int WRITE(int fd, void * buf, unsigned int size)
{
	ssize_t res;

	res = write(fd, buf, size);
	if (res == size)
		return 0;

	if (res >= 0)
		errno = EIO;
	ploop_err(errno, "WRITE");

	return -1;
}

int PWRITE(struct delta * delta, void * buf, unsigned int size, off_t off)
{
	ssize_t res;

	res = delta->fops->pwrite(delta->fd, buf, size, off);
	if (res == size)
		return 0;
	if (res >= 0)
		errno = EIO;
	ploop_err(errno, "pwrite %d", size);

	return -1;
}

int PREAD(struct delta * delta, void *buf, unsigned int size, off_t off)
{
	ssize_t res;

	res = delta->fops->pread(delta->fd, buf, size, off);
	if (res == size)
		return 0;
	if (res >= 0)
		errno = EIO;
	ploop_err(errno, "pread %d", size);

	return -1;
}

static int get_temp_mountpoint(const char *file, int create, char *buf, int len)
{
	snprintf(buf, len, "%s.mnt", file);

	if (create) {
		if (access(buf, F_OK) == 0)
			return 0;
		if (mkdir(buf, 0700)) {
			ploop_err(errno, "mkdir %s", buf);
			return SYSEXIT_MKDIR;
		}
	}
	return 0;
}

int ploop_is_large_disk_supported(void)
{
	return (access("/sys/module/ploop/parameters/large_disk_support", R_OK) == 0 ? 1 : 0);
}

static int is_fmt_version_valid(int version)
{
	return version != PLOOP_FMT_V2 || ploop_is_large_disk_supported();
}

static int default_fmt_version(void)
{
	return ploop_is_large_disk_supported() ? PLOOP_FMT_V2 : PLOOP_FMT_V1;
}

static int check_size(unsigned long long sectors, __u32 blocksize, int version)
{
	__u64 max;

	switch(version) {
	case PLOOP_FMT_V1:
		max = (__u32)-1;
		break;
	case PLOOP_FMT_V2:
		max = 0xffffffffUL * blocksize;
		break;
	case PLOOP_FMT_UNDEFINED:
		/* RAW */
		return 0;
	default:
		ploop_err(0, "Unknown ploop image version: %d", version);
		return -1;
	}

	if (max > B2S(PLOOP_MAX_FS_SIZE))
		max = B2S(PLOOP_MAX_FS_SIZE);

	if (sectors > max) {
		ploop_err(0, "An incorrect block device size is specified: %llu sectors."
				" The maximum allowed size is %llu sectors",
				sectors, max);
		return -1;
	}
	return 0;
}

int check_blockdev_size(unsigned long long sectors, __u32 blocksize, int version)
{
	if (check_size(sectors, blocksize, version))
		return -1;

	if (sectors % blocksize) {
		ploop_err(0, "An incorrect block device size is specified: %llu sectors."
				" The block device size must be aligned to the cluster block size %d",
				sectors, blocksize);
		return -1;
	}

	return 0;
}

static int create_empty_delta(const char *path, __u32 blocksize, off_t bdsize,
		int version)
{
	int fd;
	void * buf = NULL;
	struct ploop_pvd_header *vh;
	__u32 SizeToFill;
	__u64 cluster = S2B(blocksize);

	assert(blocksize);

	if (!is_fmt_version_valid(version)) {
		ploop_err(0, "Unknown ploop image version: %d",
				version);
		return -1;
	}

	if (version == PLOOP_FMT_UNDEFINED)
		version = default_fmt_version();

	if (check_blockdev_size(bdsize, blocksize, version))
		return -1;

	if (p_memalign(&buf, 4096, cluster))
		return -1;

	ploop_log(0, "Creating delta %s bs=%d size=%ld sectors v%d",
			path, blocksize, (long)bdsize, version);
	fd = open(path, O_RDWR|O_CREAT|O_DIRECT|O_EXCL, 0600);
	if (fd < 0) {
		ploop_err(errno, "Can't open %s", path);
		free(buf);
		return -1;
	}

	memset(buf, 0, cluster);

	vh = buf;
	SizeToFill = generate_pvd_header(vh, bdsize, blocksize, version);
	vh->m_Flags = CIF_Empty;

	if (WRITE(fd, buf, cluster))
		goto out_close;

	if (SizeToFill > cluster) {
		int i;
		memset(buf, 0, cluster);
		for (i = 1; i < SizeToFill / cluster; i++)
			if (WRITE(fd, buf, cluster))
				goto out_close;
	}

	if (fsync(fd)) {
		ploop_err(errno, "fsync %s", path);
		goto out_close;
	}
	free(buf);

	return fd;

out_close:
	close(fd);
	unlink(path);
	free(buf);
	return -1;
}

static int create_empty_preallocated_delta(const char *path, __u32 blocksize,
		off_t bdsize, int version)
{
	struct delta odelta = {};
	int rc, clu, i;
	void * buf = NULL;
	struct ploop_pvd_header vh = {};
	__u32 SizeToFill;
	__u32 l2_slot = 0;
	off_t off;
	__u64 cluster = S2B(blocksize);
	__u64 sizeBytes;

	if (check_blockdev_size(bdsize, blocksize, version))
		return -1;

	if (p_memalign(&buf, 4096, cluster))
		return -1;

	ploop_log(0, "Creating preallocated delta %s bs=%d size=%ld sectors v%d",
			path, blocksize, (long)bdsize, version);
	rc = open_delta_simple(&odelta, path, O_RDWR|O_CREAT|O_EXCL, OD_OFFLINE);
	if (rc) {
		free(buf);
		return -1;
	}

	memset(buf, 0, cluster);
	SizeToFill = generate_pvd_header(&vh, bdsize, blocksize, version);
	vh.m_Flags = CIF_Empty;
	memcpy(buf, &vh, sizeof(struct ploop_pvd_header));

	sizeBytes = S2B(vh.m_FirstBlockOffset + get_SizeInSectors(&vh));
	rc = sys_fallocate(odelta.fd, 0, 0, sizeBytes);
	if (rc) {
		if (errno == ENOTSUP) {
			ploop_log(0, "Warning: fallocate is not supported, using truncate instead");
			rc = ftruncate(odelta.fd, sizeBytes);
		}
		if (rc) {
			ploop_err(errno, "Failed to create %s", path);
			goto out_close;
		}
	}

	for (clu = 0; clu < SizeToFill / cluster; clu++) {
		if (is_operation_cancelled())
			goto out_close;

		if (clu > 0)
			memset(buf, 0, cluster);
		for (i = (clu == 0 ? PLOOP_MAP_OFFSET : 0); i < (cluster / sizeof(__u32)) &&
				l2_slot < vh.m_Size;
				i++, l2_slot++)
		{
			off = (off_t)vh.m_FirstBlockOffset + (l2_slot * blocksize);
			((__u32*)buf)[i] = ploop_sec_to_ioff(off, blocksize, version);
		}
		if (WRITE(odelta.fd, buf, cluster))
			goto out_close;
	}

	if (fsync(odelta.fd)) {
		ploop_err(errno, "fsync %s", path);
		goto out_close;
	}
	free(buf);

	return odelta.fd;

out_close:
	close(odelta.fd);
	unlink(path);
	free(buf);
	return -1;
}

static int create_raw_delta(const char * path, off_t bdsize)
{
	int fd;
	void * buf = NULL;
	off_t pos;

	ploop_log(0, "Creating raw delta %s size=%ld sectors",
			path, (long)bdsize);

	if (p_memalign(&buf, 4096, DEF_CLUSTER))
		return -1;

	fd = open(path, O_RDWR|O_CREAT|O_EXCL, 0600);
	if (fd < 0) {
		ploop_err(errno, "Can't open %s", path);
		free(buf);
		return -1;
	}

	memset(buf, 0, DEF_CLUSTER);

	pos = 0;
	while (pos < bdsize) {
		if (is_operation_cancelled())
			goto out_close;
		off_t copy = bdsize - pos;
		if (copy > DEF_CLUSTER/512)
			copy = DEF_CLUSTER/512;
		if (WRITE(fd, buf, copy*512))
			goto out_close;
		pos += copy;
	}

	if (fsync(fd)) {
		ploop_err(errno, "fsync");
		goto out_close;
	}

	free(buf);
	close(fd);

	return fd;

out_close:
	close(fd);
	unlink(path);
	free(buf);
	return -1;
}

static void get_disk_descriptor_fname_by_image(const char *image,
		char *buf, int size)
{
	get_basedir(image, buf, size - sizeof(DISKDESCRIPTOR_XML));
	strcat(buf, DISKDESCRIPTOR_XML);
}

void get_disk_descriptor_fname(struct ploop_disk_images_data *di, char *buf, int size)
{
	if (di->runtime->xml_fname == NULL) {
		// Use default DiskDescriptor.xml
		get_disk_descriptor_fname_by_image(di->images[0]->file,
				buf, size);
	} else {
		// Use custom
		snprintf(buf, size, "%s", di->runtime->xml_fname);
	}
}

static void fill_diskdescriptor(struct ploop_pvd_header *vh, struct ploop_disk_images_data *di)
{
	di->size = get_SizeInSectors(vh);
	di->heads = vh->m_Heads;
	di->cylinders = vh->m_Cylinders;
	di->sectors = vh->m_Sectors;
}

static int create_image(struct ploop_disk_images_data *di,
		const char *file, __u32 blocksize, off_t size_sec, int mode,
		int version)
{
	int fd = -1;
	int ret;
	struct ploop_pvd_header vh = {};
	char fname[PATH_MAX];
	char ddxml[PATH_MAX];

	if (size_sec == 0) {
		ploop_err(0, "Incorrect block device size specified: "
				"%lu sectors", (long)size_sec);
		return SYSEXIT_PARAM;
	}
	if (file == NULL) {
		ploop_err(0, "Image file name not specified");
		return SYSEXIT_PARAM;
	}

	if (access(file, F_OK) == 0) {
		ploop_err(EEXIST, "Can't create %s", file);
		return SYSEXIT_PARAM;
	}

	get_disk_descriptor_fname_by_image(file, ddxml, sizeof(ddxml));
	if (access(ddxml, F_OK) == 0) {
		ploop_err(EEXIST, "Can't create %s", ddxml);
		return SYSEXIT_PARAM;
	}

	di->size = size_sec;
	di->mode = mode;

	ret = SYSEXIT_CREAT;
	if (mode == PLOOP_RAW_MODE)
		fd = create_raw_delta(file, size_sec);
	else if (mode == PLOOP_EXPANDED_MODE)
		fd = create_empty_delta(file, blocksize, size_sec, version);
	else if (mode == PLOOP_EXPANDED_PREALLOCATED_MODE)
		fd = create_empty_preallocated_delta(file, blocksize, size_sec, version);
	if (fd < 0)
		goto err;
	close(fd);

	generate_pvd_header(&vh, size_sec, blocksize, version);
	fill_diskdescriptor(&vh, di);

	if (realpath(file, fname) == NULL) {
		ploop_err(errno, "failed realpath(%s)", file);
		goto err;
	}

	if (ploop_di_add_image(di, fname, TOPDELTA_UUID, NONE_UUID)) {
		ret = SYSEXIT_MALLOC;
		goto err;
	}

	ret = ploop_store_diskdescriptor(ddxml, di);
err:
	if (ret)
		unlink(file);

	return ret;
}

static int create_balloon_file(struct ploop_disk_images_data *di,
		const char *device)
{
	int fd, ret;
	char mnt[PATH_MAX];
	char fname[PATH_MAX + sizeof(BALLOON_FNAME)];
	struct ploop_mount_param mount_param = {};

	if (device == NULL)
		return -1;
	ploop_log(0, "Creating balloon file " BALLOON_FNAME);
	ret = get_temp_mountpoint(di->images[0]->file, 1, mnt, sizeof(mnt));
	if (ret)
		return ret;
	strcpy(mount_param.device, device);
	mount_param.target = mnt;
	ret = ploop_mount_fs(&mount_param);
	if (ret)
		goto out;
	snprintf(fname, sizeof(fname), "%s/"BALLOON_FNAME, mnt);

	fd = open(fname, O_CREAT|O_RDONLY|O_TRUNC, 0600);
	if (fd == -1) {
		ploop_err(errno, "Can't create balloon file %s", fname);
		ret = SYSEXIT_CREAT;
		goto out;
	}
	close(fd);
	ret = 0;
out:
	umount(mnt);
	rmdir(mnt);

	return ret;
}

static int ploop_init_image(struct ploop_disk_images_data *di, struct ploop_create_param *param)
{
	int ret;
	struct ploop_mount_param mount_param = {};

	if (param->fstype == NULL)
		return SYSEXIT_PARAM;

	if (di->nimages == 0) {
		ploop_err(0, "No images specified");
		return SYSEXIT_PARAM;
	}
	ret = ploop_mount_image(di, &mount_param);
	if (ret)
		return ret;
	if (!param->without_partition) {
		off_t size;

		ret = ploop_get_size(mount_param.device, &size);
		if (ret)
			goto err;

		ret = create_gpt_partition(mount_param.device, size, di->blocksize);
		if (ret)
			goto err;
	}
	ret = make_fs(mount_param.device, param->fstype, param->fsblocksize);
	if (ret)
		goto err;
	ret = create_balloon_file(di, mount_param.device);
	if (ret)
		goto err;

err:
	if (ploop_umount_image(di)) {
		if (ret == 0)
			ret = SYSEXIT_UMOUNT;
	}

	return ret;
}

static int ploop_drop_image(struct ploop_disk_images_data *di)
{
	int i;
	char fname[PATH_MAX];

	if (di->nimages == 0)
		return SYSEXIT_PARAM;

	get_disk_descriptor_fname(di, fname, sizeof(fname));
	unlink(fname);

	get_disk_descriptor_lock_fname(di, fname, sizeof(fname));
	unlink(fname);

	for (i = 0; i < di->nimages; i++) {
		ploop_log(1, "Dropping image %s", di->images[i]->file);
		unlink(di->images[i]->file);
	}

	get_temp_mountpoint(di->images[0]->file, 0, fname, sizeof(fname));
	unlink(fname);

	return 0;
}

int ploop_create_image(struct ploop_create_param *param)
{
	struct ploop_disk_images_data *di;
	int ret;
	__u32 blocksize;
	int version = param->fmt_version;

	if (!is_fmt_version_valid(version)) {
		ploop_err(0, "Unknown ploop image version: %d",
				version);
		return SYSEXIT_PARAM;
	}
	if (version == PLOOP_FMT_UNDEFINED)
		version = default_fmt_version();

	blocksize = param->blocksize ?
			param->blocksize : (1 << PLOOP1_DEF_CLUSTER_LOG);

	if (check_size(param->size, blocksize, version))
		return SYSEXIT_PARAM;

	if (!is_valid_blocksize(blocksize)) {
		ploop_err(0, "Incorrect blocksize specified: %d",
				blocksize);
		return SYSEXIT_PARAM;
	}

	di = alloc_diskdescriptor();
	if (di == NULL)
		return SYSEXIT_MALLOC;
	di->blocksize = blocksize;
	ret = create_image(di, param->image, di->blocksize,
			round_bdsize(param->size, di->blocksize, version),
			param->mode, version);
	if (ret)
		goto out;

	if (param->fstype != NULL) {
		ret = ploop_init_image(di, param);
		if (ret)
			ploop_drop_image(di);
	}

out:
	ploop_free_diskdescriptor(di);

	return ret;
}

#define PROC_PLOOP_MINOR	"/proc/vz/ploop_minor"

int ploop_getdevice(int *minor)
{
	int fd, ret;
	char buf[64];

	fd = open(PROC_PLOOP_MINOR, O_RDONLY);
	if (fd < 0) {
		ploop_err(errno, "Can't open " PROC_PLOOP_MINOR);
		return -1;
	}
	ret = read(fd, buf, sizeof(buf));
	if (ret == -1) {
		ploop_err(errno, "Can't read from " PROC_PLOOP_MINOR);
		close(fd);
		return -1;
	}
	if (sscanf(buf, "%d", minor) != 1) {
		ploop_err(0, "Can't get ploop minor '%s'", buf);
		close(fd);
		return -1;
	}

	return fd;
}

/* Workaround for bug #PCLIN-30116 */
static int do_ioctl(int fd, int req)
{
	int i, ret;

	for (i = 0; i < 60; i++) {
		ret = ioctl(fd, req, 0);
		if (ret == 0 || (ret == -1 && errno != EBUSY))
			return ret;
		sleep(1);
	}
	return ret;
}

static int print_output(int level, const char *cmd, const char *arg)
{
	FILE *fp;
	char command[PATH_MAX];
	char buffer[LOG_BUF_SIZE/2];
	int ret = -1;
	int eno = errno;
	int i;

	snprintf(command, sizeof(command), DEF_PATH_ENV " %s %s 2>&1",
			cmd, arg);
	if ((fp = popen(command, "r")) == NULL) {
		ploop_err(errno, "Can't exec %s %s", cmd, arg);
		goto out;
	}

	ploop_log(level, "--- %s %s output ---", cmd, arg);
	while (fgets(buffer, sizeof(buffer), fp)) {
		char *p = strrchr(buffer, '\n');
		if (p != NULL)
			*p = '\0';
		ploop_log(level, "%s", buffer);
	}

	i = pclose(fp);
	if (i == -1) {
		ploop_err(errno, "Error in pclose() for %s", cmd);
		goto out;
	} else if (WIFEXITED(i)) {
		ret = WEXITSTATUS(i);
		switch (ret) {
			case 0:
				ploop_log(level, "--- %s finished ---", cmd);
				break;
			case 127: /* "command not found" from shell */
				/* error is printed by shell*/
				break;
			default:
				ploop_err(0, "Command %s exited with "
						"status %d", cmd, ret);
		}
	} else if (WIFSIGNALED(i)) {
		ploop_err(0, "Command %s received signal %d",
				cmd, WTERMSIG(i));
	} else
		ploop_err(0, "Command %s died", cmd);

out:
	errno = eno;
	return ret;
}

static int do_umount(const char *mnt)
{
	int i = 0;
	int ret = 0;

retry:
	if (umount(mnt) == 0)
		return 0;

	if (errno != EBUSY)
		goto err;

	if (i++ < 6) {
		if (ploop_get_log_level() >= 3 && ret != 127)
			ret = print_output(3, "lsof", mnt);

		sleep(1);
		ploop_log(3, "Retrying umount %s", mnt);
		goto retry;
	}

	if (ret != 127)
		print_output(-1, "lsof", mnt);

err:
	ploop_err(errno, "Failed to umount %s", mnt);

	return SYSEXIT_UMOUNT;
}

static int delete_deltas(int devfd, const char *devname)
{
	int top;

	if (ploop_get_top_level(devfd, devname, &top))
		return errno;

	while (top >= 0) {
		if (ioctl(devfd, PLOOP_IOC_DEL_DELTA, &top) < 0) {
			ploop_err(errno, "PLOOP_IOC_DEL_DELTA dev=%s lvl=%d",
					devname, top);
			return errno;
		}
		top--;
	}

	return 0;
}

static int ploop_stop(int fd, const char *devname)
{
	if (do_ioctl(fd, PLOOP_IOC_STOP) < 0) {
		if (errno != EINVAL) {
			ploop_err(errno, "PLOOP_IOC_STOP");
			return SYSEXIT_DEVIOC;
		}
		if (delete_deltas(fd, devname))
			return SYSEXIT_DEVIOC;
	}

	if (ioctl(fd, PLOOP_IOC_CLEAR, 0) < 0) {
		ploop_err(errno, "PLOOP_IOC_CLEAR");
		return SYSEXIT_DEVIOC;
	}
	return 0;
}

static int get_mount_dir(const char *device, char *out, int size)
{
	FILE *fp;
	int ret = 1;
	int n;
	char buf[PATH_MAX];
	char target[4097];
	unsigned _major, _minor, minor, u;
	dev_t dev;

	if (get_dev_by_name(device, &dev))
		return -1;
	minor = gnu_dev_minor(dev);

	fp = fopen("/proc/self/mountinfo", "r");
	if (fp == NULL) {
		ploop_err(errno, "Can't open /proc/self/mountinfo");
		return -1;
	}

	while (fgets(buf, sizeof(buf), fp)) {
		n = sscanf(buf, "%u %u %u:%u %*s %4096s", &u, &u, &_major, &_minor, target);
		if (n != 5)
			continue;
		// check for /dev/ploopN or /dev/ploopNp1
		if (_major == PLOOP_DEV_MAJOR &&
				(_minor == minor || _minor == minor + 1))
		{
			strncpy(out, target, size - 1);
			out[size - 1] = 0;
			ret = 0;
			break;
		}
	}
	fclose(fp);
	return ret;
}

int ploop_get_mnt_by_dev(const char *dev, char *buf, int size)
{
	return get_mount_dir(dev, buf, size);
}

int ploop_fname_cmp(const char *p1, const char *p2)
{
	struct stat st1, st2;

	if (stat(p1, &st1)) {
		ploop_err(errno, "stat %s", p1);
		return -1;
	}
	if (stat(p2, &st2)) {
		ploop_err(errno, "stat %s", p2);
		return -1;
	}
	if (st1.st_dev == st2.st_dev &&
	    st1.st_ino == st2.st_ino)
		return 0;
	return 1;
}

static int get_dev_by_mnt(const char *path, int dev, char *buf, int size)
{
	FILE *fp;
	struct mntent *ent;
	int len;

	fp = fopen("/proc/mounts", "r");
	if (fp == NULL) {
		ploop_err(errno, "Can't open /proc/mounts");
		return -1;
	}
	while ((ent = getmntent(fp))) {
		if (strncmp(ent->mnt_fsname, "/dev/ploop", 10) != 0)
			continue;
		if (ploop_fname_cmp(path, ent->mnt_dir) == 0 ) {
			fclose(fp);
			len = strlen(ent->mnt_fsname);
			if (dev) {
				// return device in case partition used ploop1p1 -> ploop1
				if (strcmp(ent->mnt_fsname + len - 2, "p1") == 0 &&
						isdigit(ent->mnt_fsname[len - 3]))
					len -= 2; // strip p1
			}
			if (len + 1 > size) {
				ploop_err(0, "Buffer is too short");
				return -1;
			}

			snprintf(buf, len + 1, "%s", ent->mnt_fsname);
			return 0;
		}
	}
	fclose(fp);
	return 1;
}

int ploop_get_partition_by_mnt(const char *path, char *buf, int size)
{
	return get_dev_by_mnt(path, 0, buf, size);
}

int ploop_get_dev_by_mnt(const char *path, char *buf, int size)
{
	return get_dev_by_mnt(path, 1, buf, size);
}

char *ploop_get_base_delta_uuid(struct ploop_disk_images_data *di)
{
	int i;

	for (i = 0; i < di->nsnapshots; i++)
		if (strcmp(di->snapshots[i]->parent_guid, NONE_UUID) == 0)
			return di->snapshots[i]->guid;

	return NULL;
}

static const char *get_top_delta_guid(struct ploop_disk_images_data *di)
{
	return di->top_guid;
}

int ploop_get_top_delta_fname(struct ploop_disk_images_data *di, char *out, int len)
{
	const char *fname;

	fname = find_image_by_guid(di, get_top_delta_guid(di));
	if (fname == NULL){
		ploop_err(0, "Can't find image by uuid %s", di->top_guid);
		return -1;
	}
	if (snprintf(out, len, "%s", fname) > len -1) {
		ploop_err(0, "Not enough space to store data");
		return -1;
	}
	return 0;
}

int ploop_find_dev_by_uuid(struct ploop_disk_images_data *di,
		int check_state, char *out, int len)
{
	int ret;
	int running = 0;

	if (di->nimages <= 0) {
		ploop_err(0, "No images found in " DISKDESCRIPTOR_XML);
		return -1;
	}
	ret = ploop_find_dev(di->runtime->component_name, di->images[0]->file,
			out, len);
	if (ret == 0 && check_state) {
		if (ploop_get_attr(out, "running", &running)) {
			ploop_err(0, "Can't get running attr for %s",
					out);
			return -1;
		}
		if (!running) {
			ploop_err(0, "Unexpectedly found stopped ploop device %s",
					out);
			return -1;
		}
	}

	return ret;
}

int ploop_get_dev(struct ploop_disk_images_data *di, char *out, int len)
{
	int ret;

	if (ploop_lock_di(di))
		return SYSEXIT_LOCK;

	ret = ploop_find_dev(di->runtime->component_name, di->images[0]->file, out, len);

	ploop_unlock_di(di);

	return ret;
}

int ploop_get_devs(struct ploop_disk_images_data *di, char ***out)
{
	return ploop_get_dev_by_delta(di->images[0]->file, NULL, out);
}

static int reread_part(const char *device)
{
	int fd;

	fd = open(device, O_RDONLY);
	if (fd == -1) {
		ploop_err(errno, "Can't open %s", device);
		return -1;
	}
	if (ioctl(fd, BLKRRPART, 0) < 0)
		ploop_err(errno, "BLKRRPART %s", device);
	close(fd);

	return 0;
}

static int ploop_mount_fs(struct ploop_mount_param *param)
{
	unsigned long flags =
		(param->flags & MS_NOATIME) |
		(param->ro ? MS_RDONLY : 0);
	char buf[PATH_MAX + sizeof(BALLOON_FNAME)];
	struct stat st;
	char *fstype = param->fstype == NULL ? DEFAULT_FSTYPE : param->fstype;
	char data[1024];
	char balloon_ino[64] = "";
	char part_device[64];

	if (reread_part(param->device))
		return SYSEXIT_MOUNT;

	if (get_partition_device_name(param->device, part_device, sizeof(part_device)))
		return SYSEXIT_MOUNT;

	if (param->fsck && (strncmp(fstype, "ext", 3) == 0))
		if (e2fsck(part_device, E2FSCK_PREEN))
			return SYSEXIT_FSCK;

	/* Two step mount
	 * 1 mount ro and read balloon inode
	 * 2 remount with balloon_ino=ino
	 */
	if (mount(part_device, param->target, fstype, MS_RDONLY, param->mount_data)) {
		ploop_err(errno, "Can't mount file system dev=%s target=%s data='%s'",
				part_device, param->target, param->mount_data);
		return SYSEXIT_MOUNT;
	}
	snprintf(buf, sizeof(buf), "%s/" BALLOON_FNAME, param->target);
	if (stat(buf, &st) == 0)
		sprintf(balloon_ino, "balloon_ino=%llu,",
				(unsigned long long) st.st_ino);

	snprintf(data, sizeof(data), "%s%s%s",
			balloon_ino,
			param->quota ? "usrjquota=aquota.user,grpjquota=aquota.group,jqfmt=vfsv0," : "",
			param->mount_data ? param->mount_data : "");

	ploop_log(0, "Mounting %s at %s fstype=%s data='%s' %s",
			part_device, param->target, fstype,
			data, param->ro  ? "ro":"");

	if (mount(part_device, param->target, fstype, flags | MS_REMOUNT, data)) {
		ploop_err(errno, "Can't mount file system dev=%s target=%s",
				part_device, param->target);
		umount(param->target);
		return SYSEXIT_MOUNT;
	}

	return 0;
}

static int add_delta(int lfd, const char *image, struct ploop_ctl_delta *req)
{
	int fd;
	int ro = (req->c.pctl_flags & PLOOP_FMT_RDONLY);
	int ret;

	fd = open(image, O_DIRECT | (ro ? O_RDONLY : O_RDWR));
	if (fd < 0) {
		ploop_err(errno, "Can't open file %s", image);
		return SYSEXIT_OPEN;
	}

	req->f.pctl_fd = fd;

	if (ioctl(lfd, PLOOP_IOC_ADD_DELTA, req) < 0) {
		if (errno == EBUSY)
			print_output(-1, "find",
					"/sys/block/ploop[0-9]*/ -type f "
					"-not -name '*event' "
					"-exec echo {} \\; -exec cat {} \\;");

		ploop_err(0, "Can't add image %s: %s", image,
				(errno == ENOTSUP) ?
					"unsupported underlying filesystem"
					: strerror(errno));
		ret = SYSEXIT_DEVIOC;
		goto out;
	}
	ret = 0;
out:
	close(fd);

	return ret;
}

static int create_ploop_dev(int minor)
{
	char device[64];
	char devicep1[64];

	strcpy(device, "/dev/");
	make_sysfs_dev_name(minor, device + 5, sizeof(device) - 5);
	/* Create pair /dev/ploopN & /dev/ploopNp1 */
	if (access(device, F_OK)) {
		if (mknod(device, S_IFBLK, gnu_dev_makedev(PLOOP_DEV_MAJOR, minor))) {
			ploop_err(errno, "mknod %s", device);
			return SYSEXIT_MKNOD;
		}
		if (chmod(device, 0600)) {
			ploop_err(errno, "chmod %s", device);
			return SYSEXIT_SYS;
		}
	}
	snprintf(devicep1, sizeof(devicep1), "%sp1", device);
	if (access(devicep1, F_OK)) {
		if (mknod(devicep1, S_IFBLK, gnu_dev_makedev(PLOOP_DEV_MAJOR, minor+1))) {
			ploop_err(errno, "mknod %s", devicep1);
			return SYSEXIT_MKNOD;
		}
		if (chmod(devicep1, 0600)) {
			ploop_err(errno, "chmod %s", devicep1);
			return SYSEXIT_SYS;
		}
	}
	return 0;
}

/* NB: caller will take care about *lfd_p even if we fail */
static int add_deltas(struct ploop_disk_images_data *di,
		char **images, struct ploop_mount_param *param,
		int raw, __u32 blocksize, int *lfd_p)
{
	int lckfd = -1;
	char *device = param->device;
	int i;
	int ret = 0;
	struct ploop_ctl_delta req = {};

	if (device[0] == '\0') {
		char buf[64];
		int minor;

		lckfd = ploop_getdevice(&minor);
		if (lckfd == -1)
			return SYSEXIT_DEVICE;

		snprintf(device, sizeof(param->device), "/dev/%s",
				make_sysfs_dev_name(minor, buf, sizeof(buf)));
		ret = create_ploop_dev(minor);
		if (ret)
			goto err;
	}

	*lfd_p = open(device, O_RDONLY);
	if (*lfd_p < 0) {
		ploop_err(errno, "Can't open device %s", device);
		ret = SYSEXIT_DEVICE;
		goto err;
	}

	if (di != NULL && di->runtime->component_name != NULL) {
		req.c.pctl_flags |= PLOOP_FLAG_COOKIE;
		strncpy(req.cookie, di->runtime->component_name,
				PLOOP_COOKIE_SIZE);
	}
	req.c.pctl_cluster_log = ffs(blocksize) - 1;
	req.c.pctl_size = 0;
	req.c.pctl_chunks = 1;

	req.f.pctl_fd = -1;
	req.f.pctl_type = PLOOP_IO_AUTO;

	for (i = 0; images[i] != NULL; i++) {
		int ro = (images[i+1] != NULL || param->ro) ? 1: 0;
		char *image = images[i];

		req.c.pctl_format = PLOOP_FMT_PLOOP1;
		if (raw && i == 0)
			req.c.pctl_format = PLOOP_FMT_RAW;
		if (ro)
			req.c.pctl_flags |= PLOOP_FMT_RDONLY;
		else
			req.c.pctl_flags &= ~PLOOP_FMT_RDONLY;

		ploop_log(0, "Adding delta dev=%s img=%s (%s)",
				device, image, ro ? "ro" : "rw");
		ret = add_delta(*lfd_p, image, &req);
		if (ret)
			goto err1;
	}
	if (ioctl(*lfd_p, PLOOP_IOC_START, 0) < 0) {
		ploop_err(errno, "PLOOP_IOC_START");
		ret = SYSEXIT_DEVIOC;
		goto err1;
	}

err1:
	if (ret) {
		int err = 0;
		int empty = !i;

		for (i = i - 1; i >= 0; i--) {
			err = ioctl(*lfd_p, PLOOP_IOC_DEL_DELTA, &i);
			if (err < 0) {
				ploop_err(errno, "PLOOP_IOC_DEL_DELTA level=%d", i);
				break;
			}
		}
		if (!empty && err == 0 && ioctl(*lfd_p, PLOOP_IOC_CLEAR, 0) < 0)
			ploop_err(errno, "PLOOP_IOC_CLEAR");
	}
err:
	if (lckfd != -1)
		close(lckfd);
	return ret;
}

#ifndef FS_IOC_GETFLAGS
#define FS_IOC_GETFLAGS	_IOR('f', 1, long)
#endif
#ifndef EXT4_EXTENTS_FL
#define EXT4_EXTENTS_FL		0x00080000 /* Inode uses extents */
#endif

static int check_mount_restrictions(struct ploop_mount_param *param, const char *fname)
{
	struct statfs st;
	int fd;
	long flags;

	/* FIXME: */
	if (getenv("PLOOP_SKIP_EXT4_EXTENTS_CHECK") != NULL)
		return 0;
	if (statfs(fname, &st) < 0) {
		ploop_err(errno, "Unable to statfs %s", fname);
		return -1;
	}
	if (st.f_type == EXT4_SUPER_MAGIC) {
		fd = open(fname, O_RDONLY);
		if (fd < 0) {
			ploop_err(errno, "Can't open %s", fname);
			return -1;
		}
		if (ioctl(fd, FS_IOC_GETFLAGS, &flags) < 0) {
			ploop_err(errno, "FS_IOC_GETFLAGS %s", fname);
			close(fd);
			return -1;
		}
		close(fd);

		if (!(flags & EXT4_EXTENTS_FL)) {
			ploop_err(0, "The ploop image can not be used on ext3 or ext4 file"
					" system without extents");
			return 1;
		}
	}

	return 0;
}

int ploop_mount(struct ploop_disk_images_data *di, char **images,
		struct ploop_mount_param *param, int raw)
{
	int lfd = -1;
	struct stat st;
	int ret = 0;
	__u32 blocksize = 0;

	if (images == NULL || images[0] == NULL) {
		ploop_err(0, "ploop_mount: no deltas to mount");
		return SYSEXIT_PARAM;
	}

	if (param->target != NULL) {
		if (stat(param->target, &st)) {
			ploop_err(errno, "Failed to stat mount point %s", param->target);
			return SYSEXIT_PARAM;
		}
		if (!S_ISDIR(st.st_mode)) {
			ploop_err(0, "Mount point %s not a directory", param->target);
			return SYSEXIT_PARAM;
		}
	}

	if (raw) {
		if (param->blocksize)
			blocksize = param->blocksize;
		else if (di)
			blocksize = di->blocksize;
		else {
			ploop_err(0, "Blocksize is not specified");
			return SYSEXIT_PARAM;
		}
	} else if (di)
		blocksize = di->blocksize;

	if (check_mount_restrictions(param, images[0]))
		return SYSEXIT_MOUNT;

	if (di && (ret = check_and_restore_fmt_version(di)))
		goto err;

	ret = check_deltas(di, images, param, raw, &blocksize);
	if (ret)
		goto err;

	ret = add_deltas(di, images, param, raw, blocksize, &lfd);
	if (ret)
		goto err;

	if (param->target != NULL) {
		ret = ploop_mount_fs(param);
		if (ret)
			ploop_stop(lfd, param->device);
	} else {
		/* Dummy call to recreate devices */
		reread_part(param->device);
	}

err:
	if (lfd >= 0)
		close(lfd);

	if (ret == 0 && di != NULL &&
			di->runtime->component_name == NULL &&
			param->target != NULL)
		drop_statfs_info(di->images[0]->file);

	return ret;
}

static int mount_image(struct ploop_disk_images_data *di, struct ploop_mount_param *param, int flags)
{
	int ret;
	char **images;
	char *guid;

	if (param->guid != NULL) {
		if (find_image_by_guid(di, param->guid) == NULL) {
			ploop_err(0, "Uuid %s not found", param->guid);
			return SYSEXIT_PARAM;
		}
		guid = param->guid;
	} else
		guid = di->top_guid;

	if (!param->ro) {
		int nr_ch = ploop_get_child_count_by_uuid(di, guid);
		if (nr_ch != 0) {
			ploop_err(0, "Unable to mount (rw) snapshot %s: "
				"it has %d child%s", guid,
				nr_ch, (nr_ch == 1) ? "" : "ren");
			return SYSEXIT_PARAM;
		}
	}

	images = make_images_list(di, guid, 0);
	if (images == NULL)
		return SYSEXIT_MALLOC;

	ret = ploop_mount(di, images, param, (di->mode == PLOOP_RAW_MODE));

	free_images_list(images);

	return ret;
}

int auto_mount_image(struct ploop_disk_images_data *di,
		struct ploop_mount_param *param)
{
	char mnt[PATH_MAX];
	int ret;

	ret = get_temp_mountpoint(di->images[0]->file, 1, mnt, sizeof(mnt));
	if (ret)
		return ret;
	param->target = strdup(mnt);

	return mount_image(di, param, 0);
}

int ploop_mount_image(struct ploop_disk_images_data *di, struct ploop_mount_param *param)
{
	int ret;
	char dev[64];

	if (ploop_lock_di(di))
		return SYSEXIT_LOCK;

	ret = ploop_find_dev_by_uuid(di, 1, dev, sizeof(dev));
	if (ret == -1) {
		ploop_unlock_di(di);
		return SYSEXIT_SYS;
	}
	if (ret == 0) {
		ploop_err(0, "Image %s already used by device %s",
				di->images[0]->file, dev);

		ret = SYSEXIT_MOUNT;
		goto err;
	}

	ret = mount_image(di, param, 0);
err:
	ploop_unlock_di(di);

	return ret;
}

int ploop_mount_snapshot(struct ploop_disk_images_data *di, struct ploop_mount_param *param)
{
	if (param->guid == NULL) {
		ploop_err(0, "Snapshot guid is not specified");
		return SYSEXIT_PARAM;
	}
	return ploop_mount_image(di, param);
}

static int ploop_stop_device(const char *device)
{
	int lfd, ret;

	ploop_log(0, "Unmounting device %s", device);
	lfd = open(device, O_RDONLY);
	if (lfd < 0) {
		ploop_err(errno, "Can't open dev %s", device);
		return SYSEXIT_DEVICE;
	}

	ret = ploop_stop(lfd, device);
	close(lfd);

	return ret;
}

int ploop_umount(const char *device, struct ploop_disk_images_data *di)
{
	int ret;
	char mnt[PATH_MAX] = "";

	if (!device) {
		ploop_err(0, "ploop_umount: device is not specified");
		return -1;
	}

	if (get_mount_dir(device, mnt, sizeof(mnt)) == 0) {
		/* The component_name feature allows multiple image mount.
		 * Skip store statfs in custom case.
		 */
		if (di != NULL && di->runtime->component_name == NULL)
			store_statfs_info(mnt, di->images[0]->file);
		ploop_log(0, "Unmounting file system at %s", mnt);
		ret = do_umount(mnt);
		if (ret)
			return ret;
	}

	ret = ploop_stop_device(device);

	return ret;
}

int ploop_umount_image(struct ploop_disk_images_data *di)
{
	int ret;
	char dev[PATH_MAX];

	if (di->nimages == 0) {
		ploop_err(0, "No images specified");
		return SYSEXIT_PARAM;
	}

	if (ploop_lock_di(di))
		return SYSEXIT_LOCK;

	ret = ploop_find_dev_by_uuid(di, 0, dev, sizeof(dev));
	if (ret == -1) {
		ploop_unlock_di(di);
		return SYSEXIT_SYS;
	}
	if (ret != 0) {
		ploop_unlock_di(di);
		ploop_err(0, "Image %s is not mounted", di->images[0]->file);
		return SYSEXIT_DEV_NOT_MOUNTED;
	}

	ret = ploop_complete_running_operation(dev);
	if (ret) {
		ploop_unlock_di(di);
		return ret;
	}

	ret = ploop_umount(dev, di);

	ploop_unlock_di(di);

	return ret;
}

static int get_image_param_online(const char *device, off_t *size,
		__u32 *blocksize, int *version)
{
	if (ploop_get_attr(device, "block_size",  (int *)blocksize))
		return SYSEXIT_SYSFS;

	*version = PLOOP_FMT_V1;
	if (ploop_is_large_disk_supported() &&
			ploop_get_attr(device, "fmt_version", version))
		return SYSEXIT_SYSFS;

	return ploop_get_size(device, size);
}

static int get_image_param_offline(struct ploop_disk_images_data *di, const char *guid,
		off_t *size, __u32 *blocksize, int *version)
{
	int ret;
	struct delta delta;
	const char *image;
	int raw = 0;

	image = find_image_by_guid(di, guid);
	if (image == NULL) {
		ploop_err(0, "Can't find image by top guid %s",
				guid);
		return SYSEXIT_PARAM;
	}
	if (di->mode == PLOOP_RAW_MODE) {
		int i;

		i = find_snapshot_by_guid(di, guid);
		if (i == -1) {
			ploop_err(0, "Can't find snapshot by guid %s",
					guid);
			return SYSEXIT_PARAM;
		}
		if (strcmp(di->snapshots[i]->parent_guid, NONE_UUID) == 0)
			raw = 1;
	}
	if (raw) {
		struct stat st;

		if (stat(image, &st)) {
			ploop_err(errno, "Failed to stat %s",
					image);
			return SYSEXIT_FSTAT;
		}
		*size = st.st_size / SECTOR_SIZE;
		*version = PLOOP_FMT_UNDEFINED;
		*blocksize = di->blocksize;
	} else {
		ret = open_delta(&delta, image, O_RDONLY, OD_OFFLINE);
		if (ret)
			return ret;
		*size = delta.l2_size * delta.blocksize;
		*version = delta.version;
		*blocksize = delta.blocksize;
		close_delta(&delta);
	}

	return 0;
}

static int get_image_param(struct ploop_disk_images_data *di, const char *guid,
		off_t *size, __u32 *blocksize, int *version)
{
	int ret;
	char dev[64];

	/* The 'size' parameter is delta specific so
	 * get offline for non top delta.
	 */
	if (strcmp(di->top_guid, guid) == 0) {
		ret = ploop_find_dev_by_uuid(di, 1, dev, sizeof(dev));
		if (ret == -1)
			return SYSEXIT_SYS;
		if (ret == 0)
			return get_image_param_online(dev, size, blocksize, version);
	}
	return get_image_param_offline(di, guid, size, blocksize, version);
}

int ploop_grow_device(const char *device, off_t new_size)
{
	int fd, ret;
	struct ploop_ctl ctl;
	off_t size;
	__u32 blocksize = 0;
	int version = PLOOP_FMT_V1;

	ret = ploop_get_size(device, &size);
	if (ret)
		return ret;

	if (ploop_get_attr(device, "block_size", (int*) &blocksize))
		return SYSEXIT_SYSFS;

	if (ploop_is_large_disk_supported() &&
			ploop_get_attr(device, "fmt_version", &version))
		return SYSEXIT_SYSFS;

	if (new_size == size)
		return 0;

	if (new_size < size) {
		ploop_err(0, "Incorrect new size specified %ld current size %ld",
				(long)new_size, (long)size);
		return SYSEXIT_PARAM;
	}

	if (check_size(new_size, blocksize, version))
		return SYSEXIT_PARAM;

	ploop_log(0, "Growing dev=%s size=%llu sectors (new size=%llu)",
				device, (unsigned long long)size,
				(unsigned long long)new_size);

	fd = open(device, O_RDONLY);
	if (fd < 0) {
		ploop_err(errno, "Can't open device %s", device);
		return SYSEXIT_DEVICE;
	}

	memset(&ctl, 0, sizeof(ctl));

	ctl.pctl_cluster_log = ffs(blocksize) - 1;
	if (ploop_is_large_disk_supported()) {
		/* the new size is aligned to cluster block */
		ctl.pctl_flags |= PLOOP_FLAG_CLUBLKS;
		ctl.pctl_size = new_size >> ctl.pctl_cluster_log;
	} else
		ctl.pctl_size = new_size;

	if (ioctl(fd, PLOOP_IOC_GROW, &ctl) < 0) {
		ploop_err(errno, "PLOOP_IOC_GROW");
		close(fd);
		return SYSEXIT_DEVIOC;
	}
	close(fd);

	return 0;
}

int ploop_grow_image(struct ploop_disk_images_data *di, off_t size)
{
	int ret;
	char device[64];

	if (ploop_lock_di(di))
		return SYSEXIT_LOCK;

	ret = ploop_find_dev_by_uuid(di, 1, device, sizeof(device));
	if (ret == -1) {
		ret = SYSEXIT_SYS;
		goto err;
	}
	if (ret == 0) {
		ret = ploop_grow_device(device, size);
	} else {
		int i;
		const char *fname;

		i = find_snapshot_by_guid(di, di->top_guid);
		if (i == -1) {
			ploop_err(0, "Unable to find top delta file name");
			ret = SYSEXIT_PARAM;
			goto err;
		}

		fname = find_image_by_guid(di, di->top_guid);
		if (!fname) {
			ploop_err(0, "Unable to find top delta file name");
			ret = SYSEXIT_PARAM;
			goto err;
		}

		if (strcmp(di->snapshots[i]->parent_guid, NONE_UUID) == 0 &&
				di->mode == PLOOP_RAW_MODE)
			ret = ploop_grow_raw_delta_offline(fname, size);
		else
			ret = ploop_grow_delta_offline(fname, size);
	}

err:
	ploop_unlock_di(di);

	return ret;
}

static int ploop_raw_discard(struct ploop_disk_images_data *di, const char *device,
		__u32 blocksize, off_t start, off_t end)
{
	int ret;
	char conf[PATH_MAX];
	off_t new_end;

	new_end = start + GPT_DATA_SIZE;
	new_end = ROUNDUP(new_end, blocksize);

	if (new_end >= end)
		return 0;

	ret = resize_gpt_partition(device, new_end);
	if (ret)
		return ret;

	ret = ploop_stop_device(device);
	if (ret)
		return ret;

	ploop_log(0, "Truncate %s %lu",
			di->images[0]->file, (long)S2B(new_end));
	if (truncate(di->images[0]->file, S2B(new_end))) {
		ploop_err(errno, "Failed to truncate %s",
				di->images[0]->file);
		return SYSEXIT_FTRUNCATE;
	}

	di->size = new_end;
	get_disk_descriptor_fname(di, conf, sizeof(conf));
	ret = ploop_store_diskdescriptor(conf, di);
	if (ret)
		return ret;

	return 0;
}

/* The code below works correctly only if
 *	device=/dev/ploopN
 *	part_dev_size=/dev/ploopNp1
 */
static int shrink_device(struct ploop_disk_images_data *di,
		const char *device, const char *part_device,
		off_t part_dev_size, off_t new_size, __u32 blocksize)
{
	struct dump2fs_data data;
	char buf[PATH_MAX];
	__u32 part_start;
	int ret;
	int top, raw;
	off_t start, end;

	snprintf(buf, sizeof(buf), "/sys/block/%s/%s/start",
			basename(device), basename(part_device));
	if (get_dev_start(buf, &part_start)) {
		ploop_err(0, "Can't find out offset from start of ploop device (%s)",
				part_device);
		return SYSEXIT_SYSFS;
	}
	ret = ploop_get_attr(device, "top", &top);
	if (ret)
		return SYSEXIT_SYSFS;

	raw = (di->mode == PLOOP_RAW_MODE && top == 0);
	ploop_log(0, "Offline shrink %s dev=%s size=%lu new_size=%lu, start=%u",
			(raw) ? "raw" : "",
			part_device, (long)part_dev_size, (long)new_size, part_start);
	ret = e2fsck(part_device, E2FSCK_FORCE | E2FSCK_PREEN);
	if (ret)
		return ret;

	/* offline resize */
	ret = resize_fs(part_device, new_size);
	if (ret)
		return ret;

	ret = dumpe2fs(part_device, &data);
	if (ret)
		return ret;

	start = part_start + B2S(data.block_count * data.block_size);
	end = part_start + part_dev_size;
	if (raw)
		ret = ploop_raw_discard(di, device, blocksize, start, end);
	else
		ret = ploop_blk_discard(device, blocksize, start, end);

	if (ret)
		return ret;

	return 0;
}

int ploop_resize_image(struct ploop_disk_images_data *di, struct ploop_resize_param *param)
{
	int ret;
	struct ploop_mount_param mount_param = {};
	char buf[PATH_MAX];
	char part_device[64];
	int mounted = -1;
	int balloonfd = -1;
	struct stat st;
	off_t part_dev_size = 0;
	off_t dev_size = 0;
	__u64 balloon_size = 0;
	__u64 new_balloon_size = 0;
	struct statfs fs;
	unsigned long long new_size;
	__u32 blocksize = 0;
	int version;
	off_t new_fs_size = 0;

	if (di->nimages == 0) {
		ploop_err(0, "No images in DiskDescriptor");
		return SYSEXIT_DISKDESCR;
	}

	if (ploop_lock_di(di))
		return SYSEXIT_LOCK;

	ret = ploop_find_dev_by_uuid(di, 1, buf, sizeof(buf));
	if (ret == -1) {
		ret = SYSEXIT_SYS;
		goto err;
	}
	if (ret != 0) {
		ret = auto_mount_image(di, &mount_param);
		if (ret)
			goto err;
		mounted = 0;
	} else {
		ret = ploop_complete_running_operation(buf);
		if (ret)
			goto err;

		strncpy(mount_param.device, buf, sizeof(mount_param.device));
		if (get_mount_dir(mount_param.device, buf, sizeof(buf))) {
			ploop_err(0, "Can't find mount point for %s", buf);
			ret = SYSEXIT_PARAM;
			goto err;
		}
		mount_param.target = strdup(buf);
		mounted = 1;
	}

	//FIXME: Deny resize image if there are childs
	ret = get_image_param_online(mount_param.device, &dev_size,
			&blocksize, &version);
	if (ret)
		goto err;

	if (check_size(param->size, blocksize, version)) {
		ret = SYSEXIT_PARAM;
		goto err;
	}

	new_size = round_bdsize(param->size, blocksize, version);

	ret = get_partition_device_name(mount_param.device, part_device, sizeof(part_device));
	if (ret) {
		ret = SYSEXIT_SYS;
		goto err;
	}

	ret = ploop_get_size(part_device, &part_dev_size);
	if (ret)
		goto err;

	if (new_size != 0) {
		/* use (4 * blocksize) as reserved space for alignment */
		if (new_size <= (4 * blocksize)) {
			ploop_err(0, "Unable to change image size to %llu sectors",
					new_size);
			ret = SYSEXIT_PARAM;
			goto err;
		}
		new_fs_size = new_size - (4 * blocksize);
	}

	ret = get_balloon(mount_param.target, &st, &balloonfd);
	if (ret)
		goto err;
	balloon_size = bytes2sec(st.st_size);

	if (param->size == 0) {
		int delta = 1024 * 1024;

		/* Inflate balloon up to max free space */
		if (statfs(mount_param.target, &fs) != 0) {
			ploop_err(errno, "statfs(%s)", mount_param.target);
			ret = SYSEXIT_FSTAT;
			goto err;
		}
		if (fs.f_bfree <= delta / fs.f_bsize) {
			ret = 0; // no free space
			goto err;
		}

		new_balloon_size = balloon_size + B2S(fs.f_bfree * fs.f_bsize);
		new_balloon_size -= B2S(delta);
		ret = ploop_balloon_change_size(mount_param.device,
				balloonfd, new_balloon_size);
	} else if (new_size > dev_size) {
		char conf[PATH_MAX];
		char conf_tmp[PATH_MAX];

		/* GROW */
		if (balloon_size != 0) {
			ret = ploop_balloon_change_size(mount_param.device,
					balloonfd, 0);
			if (ret)
				goto err;
		}
		close(balloonfd);
		balloonfd = -1;
		if (!mounted && param->offline_resize) {
			/* offline */
			ret = do_umount(mount_param.target);
			if (ret)
				goto err;
			ret = e2fsck(part_device, E2FSCK_FORCE | E2FSCK_PREEN);
			if (ret)
				goto err;
		}

		// Update size in the DiskDescriptor.xml
		di->size = new_size;
		get_disk_descriptor_fname(di, conf, sizeof(conf));
		snprintf(conf_tmp, sizeof(conf_tmp), "%s.tmp", conf);
		ret = ploop_store_diskdescriptor(conf_tmp, di);
		if (ret)
			goto err;

		ret = ploop_grow_device(mount_param.device, new_size);
		if (ret) {
			unlink(conf_tmp);
			goto err;
		}

		if (rename(conf_tmp, conf)) {
			ploop_err(errno, "Can't rename %s to %s",
					conf_tmp, conf);
			ret = SYSEXIT_RENAME;
			goto err;
		}

		ret = resize_gpt_partition(mount_param.device, 0);
		if (ret)
			goto err;

		/* resize up to the end of device */
		ret = resize_fs(part_device, new_fs_size);
		if (ret)
			goto err;
	} else {
		/* Grow or shrink fs but do not change block device size */
		if (!mounted && param->offline_resize) {
			/* Offline */
			if (balloon_size != 0) {
				/* FIXME: restore balloon size on failure */
				ret = ploop_balloon_change_size(mount_param.device, balloonfd, 0);
				if (ret)
					goto err;
			}
			close(balloonfd); /* close to make umount possible */
			balloonfd = -1;

			ret = do_umount(mount_param.target);
			if (ret)
				goto err;

			ret = shrink_device(di, mount_param.device, part_device, part_dev_size,
					new_fs_size, blocksize);
			if (ret)
				goto err;
		} else {
			/* Online */
			struct dump2fs_data data = {};
			__u64 available_balloon_size;
			__u64 blocks;

			ret = dumpe2fs(part_device, &data);
			if (ret)
				goto err;

			blocks = data.block_count * B2S(data.block_size);
			if (new_fs_size < blocks) {
				/* shrink fs */
				if (statfs(mount_param.target, &fs) != 0) {
					ploop_err(errno, "statfs(%s)", mount_param.target);
					ret = SYSEXIT_FSTAT;
					goto err;
				}

				new_balloon_size = blocks - new_fs_size;
				available_balloon_size = balloon_size + (fs.f_bfree * B2S(fs.f_bsize));
				if (available_balloon_size < new_balloon_size) {
					ploop_err(0, "Unable to change image size to %lu "
							"sectors, minimal size is %llu",
							(long)new_fs_size,
							(blocks - available_balloon_size));
					ret = SYSEXIT_PARAM;
					goto err;
				}
			} else {
				/* grow fs */
				new_balloon_size = 0;
			}

			if (new_balloon_size != balloon_size) {
				ret = ploop_balloon_change_size(mount_param.device,
						balloonfd, new_balloon_size);
				if (ret)
					goto err;
				tune_fs(mount_param.target, part_device, new_fs_size);
			}

			if (new_balloon_size == 0) {
				ret = resize_fs(part_device, new_fs_size);
				if (ret)
					goto err;
			}
		}
	}

err:
	if (balloonfd != -1)
		close(balloonfd);
	if (mounted == 0)
		ploop_umount(mount_param.device, di);
	ploop_unlock_di(di);
	free_mount_param(&mount_param);

	return ret;
}

static int expanded2raw(struct ploop_disk_images_data *di)
{
	struct delta delta = {};
	struct delta odelta = {};
	__u32 clu;
	void *buf = NULL;
	char tmp[PATH_MAX] = "";
	int ret = -1;
	__u64 cluster;

	ploop_log(0, "Converting image to raw...");
	// FIXME: deny snapshots
	if (open_delta(&delta, di->images[0]->file, O_RDONLY, OD_OFFLINE))
		return SYSEXIT_OPEN;
	cluster = S2B(delta.blocksize);

	if (p_memalign(&buf, 4096, cluster))
		goto err;

	snprintf(tmp, sizeof(tmp), "%s.tmp",
			di->images[0]->file);
	if (open_delta_simple(&odelta, tmp, O_RDWR|O_CREAT|O_EXCL|O_TRUNC, OD_OFFLINE))
		goto err;

	for (clu = 0; clu < delta.l2_size; clu++) {
		int l2_cluster = (clu + PLOOP_MAP_OFFSET) / (cluster / sizeof(__u32));
		__u32 l2_slot  = (clu + PLOOP_MAP_OFFSET) % (cluster / sizeof(__u32));

		if (l2_cluster >= delta.l1_size) {
			ploop_err(0, "abort: l2_cluster >= delta.l1_size");

			goto err;
		}

		if (delta.l2_cache != l2_cluster) {
			if (PREAD(&delta, delta.l2, cluster, (off_t)l2_cluster * cluster))
				goto err;
			delta.l2_cache = l2_cluster;
		}
		if (delta.version == PLOOP_FMT_V1 &&
				(delta.l2[l2_slot] % delta.blocksize) != 0) {
			ploop_err(0, "Image corrupted: delta.l2[%d]=%d",
					l2_slot, delta.l2[l2_slot]);
			goto err;
		}
		if (delta.l2[l2_slot] != 0) {
			if (PREAD(&delta, buf, cluster, S2B(ploop_ioff_to_sec(delta.l2[l2_slot],
								delta.blocksize, delta.version))))
				goto err;
		} else {
			bzero(buf, cluster);
		}

		if (PWRITE(&odelta, buf, cluster, clu * cluster))
			goto err;
	}

	if (fsync(odelta.fd))
		ploop_err(errno, "fsync");

	if (rename(tmp, di->images[0]->file)) {
		ploop_err(errno, "rename %s %s",
			tmp, di->images[0]->file);
		goto err;
	}
	ret = 0;
err:
	close(odelta.fd);
	if (ret && tmp[0])
		unlink(tmp);
	close_delta(&delta);
	free(buf);

	return ret;
}

static int expanded2preallocated(struct ploop_disk_images_data *di)
{
	struct delta delta = {};
	__u32 clu;
	off_t data_off;
	int ret = -1;
	__u64 cluster;
	void *buf = NULL;

	ploop_log(0, "Converting image to preallocated...");
	// FIXME: deny on snapshots
	if (open_delta(&delta, di->images[0]->file, O_RDWR, OD_OFFLINE))
		return SYSEXIT_OPEN;

	cluster = S2B(delta.blocksize);
	data_off = delta.alloc_head;

	// Second stage: update index
	for (clu = 0; clu < delta.l2_size; clu++) {
		int l2_cluster = (clu + PLOOP_MAP_OFFSET) / (cluster / sizeof(__u32));
		__u32 l2_slot  = (clu + PLOOP_MAP_OFFSET) % (cluster / sizeof(__u32));

		if (l2_cluster >= delta.l1_size) {
			ploop_err(0, "abort: l2_cluster >= delta.l1_size");
			goto err;
		}

		if (delta.l2_cache != l2_cluster) {
			if (PREAD(&delta, delta.l2, cluster, (off_t)l2_cluster * cluster))
				goto err;
			delta.l2_cache = l2_cluster;
		}
		if (delta.l2[l2_slot] == 0) {
			off_t idx_off = (off_t)l2_cluster * cluster + (l2_slot*sizeof(__u32));
			int rc;

			delta.l2[l2_slot] = ploop_sec_to_ioff(data_off * delta.blocksize,
					delta.blocksize, delta.version);

			rc = sys_fallocate(delta.fd, 0, data_off * cluster, cluster);
			if (rc) {
				if (errno == ENOTSUP) {
					if (buf == NULL) {
						ploop_log(0, "Warning: fallocate is not supported,"
								" using write instead");
						buf = calloc(1, cluster);
						if (buf == NULL) {
							ploop_err(errno, "malloc");
							goto err;
						}
					}
					rc = PWRITE(&delta, buf, cluster, data_off * cluster);
				}
				if (rc) {
					ploop_err(errno, "Failed to expand %s", di->images[0]->file);
					goto err;
				}
			}

			if (PWRITE(&delta, &delta.l2[l2_slot], sizeof(__u32), idx_off))
				goto err;
			data_off++;
		}
	}

	if (fsync(delta.fd)) {
		ploop_err(errno, "fsync");
		goto err;
	}
	ret = 0;
err:
	close_delta(&delta);
	free(buf);
	return ret;
}

int ploop_convert_image(struct ploop_disk_images_data *di, int mode, int flags)
{
	char conf_tmp[PATH_MAX];
	char conf[PATH_MAX];
	int ret = -1;

	if (di->mode == PLOOP_RAW_MODE) {
		ploop_err(0, "Converting raw image is not supported");
		return SYSEXIT_PARAM;
	}
	if (di->nimages == 0) {
		ploop_err(0, "No images specified");
		return SYSEXIT_PARAM;
	}
	if (ploop_lock_di(di))
		return SYSEXIT_LOCK;

	di->mode = mode;
	get_disk_descriptor_fname(di, conf, sizeof(conf));
	snprintf(conf_tmp, sizeof(conf_tmp), "%s.tmp", conf);
	ret = ploop_store_diskdescriptor(conf_tmp, di);
	if (ret)
		goto err;

	if (mode == PLOOP_EXPANDED_PREALLOCATED_MODE)
		ret = expanded2preallocated(di);
	else if (mode == PLOOP_RAW_MODE)
		ret = expanded2raw(di);
	/* else if (mode == PLOOP_EXPANDED)
	 * do nothing because di->mode = mode and store DiskDescriptot.xml (see above) is enough;
	 */
	if (ret) {
		unlink(conf_tmp);
		goto err;
	}

	if (rename(conf_tmp, conf)) {
		ploop_err(errno, "Can't rename %s %s",
				conf_tmp, conf);
		ret = SYSEXIT_RENAME;
	}

err:
	ploop_unlock_di(di);

	return ret;
}

#define BACKUP_IDX_FNAME(fname, image)	snprintf(fname, sizeof(fname), "%s.idx", image)
static int backup_idx_table(struct delta *d, const char *image)
{
	char fname[PATH_MAX];
	int fd, ret;
	__u32 clu, cluster;

	BACKUP_IDX_FNAME(fname, image);

	ploop_log(0, "Backing up index table %s", fname);
	fd = open(fname, O_CREAT | O_WRONLY | O_TRUNC, 0600);
	if (fd < 0 ) {
		ploop_err(errno, "Failed to create %s", fname);
		return SYSEXIT_OPEN;
	}

	cluster = S2B(d->blocksize);
	for (clu = 0; clu < d->l1_size; clu++) {
		if (PREAD(d, d->l2, cluster, (off_t)clu * cluster)) {
			ret = SYSEXIT_WRITE;
			goto err;
		}

		if (WRITE(fd, d->l2, cluster)) {
			ret = SYSEXIT_READ;
			goto err;
		}
	}
	if (fsync(fd)) {
		ploop_err(errno, "Failed to sync %s", fname);
		ret = SYSEXIT_FSYNC;
		goto err;
	}
	ret = 0;
err:
	close(fd);
	return ret;
}

/* Write index table */
static int writeback_idx(struct delta *delta)
{
	__u32 l1_cluster = delta->l2_cache;
	__u8 *buf = (__u8 *)delta->l2;
	int skip = l1_cluster == 0 ? sizeof(struct ploop_pvd_header) : 0;

	if (PWRITE(delta, (__u8 *)buf + skip, S2B(delta->blocksize) - skip,
				(off_t)l1_cluster * S2B(delta->blocksize) + skip))
		return SYSEXIT_WRITE;

	delta->dirtied = 0;
	return 0;
}

static int change_fmt_version(struct delta *d, int new_version)
{
	__u32 clu, l2_cluster, l2_slot;
	int ret, n;
	off_t off;
	__u32 cluster = S2B(d->blocksize);

	n = cluster / sizeof(__u32);
	d->dirtied = 0;
	for (clu = 0; clu < d->l1_size * n - PLOOP_MAP_OFFSET; clu++) {
		l2_cluster = (clu + PLOOP_MAP_OFFSET) / n;
		l2_slot    = (clu + PLOOP_MAP_OFFSET) % n;

		if (d->l2_cache != l2_cluster) {
			if (d->dirtied && (ret = writeback_idx(d)))
				goto err;

			if (PREAD(d, d->l2, cluster, (off_t)l2_cluster * cluster)) {
				ret = SYSEXIT_READ;
				goto err;
			}

			d->l2_cache = l2_cluster;
		}
		if (d->l2[l2_slot] == 0)
			continue;

		off = ploop_ioff_to_sec(d->l2[l2_slot], d->blocksize, d->version);
		if (new_version == PLOOP_FMT_V1 && check_size(off, d->blocksize, new_version)) {
			ret = SYSEXIT_PARAM;
			goto err;
		}
		d->l2[l2_slot] = ploop_sec_to_ioff(off, d->blocksize, new_version);
		d->dirtied = 1;
	}

	if (d->dirtied && (ret = writeback_idx(d)))
		goto err;

	/* update header and sync */
	ret = change_delta_version(d, new_version);
	if (ret)
		goto err;
err:
	return ret;
}

int ploop_change_fmt_version(struct ploop_disk_images_data *di, int new_version, int flags)
{
	char fname[PATH_MAX];
	struct delta_array da = {};
	int ret = 0, rc, i;
	struct ploop_pvd_header *vh;

	init_delta_array(&da);
	if (new_version != PLOOP_FMT_V1 && new_version != PLOOP_FMT_V2) {
		ploop_err(0, "Incorrect version is specified");
		return SYSEXIT_PARAM;
	}

	if (new_version == PLOOP_FMT_V2 && !ploop_is_large_disk_supported()) {
		ploop_err(0, "The PLOOP_FMT_V2 is not supported by kernel");
		return SYSEXIT_PARAM;
	}

	if (ploop_lock_di(di))
		return SYSEXIT_LOCK;

	if (di->mode == PLOOP_RAW_MODE) {
		ploop_err(0, "Changing image version format"
				" on raw image is not supported");
		goto err;
	}

	rc = ploop_find_dev_by_uuid(di, 1, fname, sizeof(fname));
	if (rc == -1) {
		ret = SYSEXIT_SYS;
		goto err;
	} else if (rc == 0) {
		ret = SYSEXIT_PARAM;
		ploop_err(0, "Image is mounted: changing image version "
				" online is not supported");
		goto err;
	}
	/* 0. Validate */
	for (i = 0; i < di->nimages; i++) {
		if (extend_delta_array(&da, di->images[i]->file,
					O_RDWR, OD_OFFLINE)) {
			ret = SYSEXIT_OPEN;
			goto err;
		}
		if (new_version == PLOOP_FMT_V1 &&
		    ((off_t)da.delta_arr[i].l2_size * da.delta_arr[i].blocksize) > 0xffffffff)
		{
			ret = SYSEXIT_PARAM;
			ploop_err(0, "Unable to convert image to PLOOP_FMT_V1:"
					" the image size is not compatible");
			goto err;
		}
	}
	/* 1. Backup index table */
	for (i = 0; i < di->nimages; i++) {
		ret = backup_idx_table(&da.delta_arr[i], di->images[i]->file);
		if (ret)
			goto err_rm;
	}
	/* 2. Lock deltas */
	for (i = 0; i < di->nimages; i++) {
		if (dirty_delta(&da.delta_arr[i])) {
			ret = SYSEXIT_WRITE;
			goto err;
		}
		vh = (struct ploop_pvd_header *) da.delta_arr[i].hdr0;
		ret = change_delta_flags(&da.delta_arr[i],
				(vh->m_Flags | CIF_FmtVersionConvert));
		if (ret)
			goto err;
	}

	/* Recheck ploop state after locking */
	rc = ploop_find_dev_by_uuid(di, 1, fname, sizeof(fname));
	if (rc == -1) {
		ret = SYSEXIT_SYS;
		goto err;
	} else if (rc == 0) {
		ret = SYSEXIT_PARAM;
		ploop_err(0, "Image is mounted: changing image version "
				" online is not supported");
		goto err;
	}

	/* 3. Convert */
	for (i = 0; i < di->nimages; i++) {
		ploop_log(0, "Converting %s to version %d",
				di->images[i]->file, new_version);
		ret = change_fmt_version(&da.delta_arr[i], new_version);
		if (ret)
			goto err;
	}

	/* 4. Unlock */
	for (i = 0; i < di->nimages; i++) {
		vh = (struct ploop_pvd_header *) da.delta_arr[i].hdr0;
		ret = change_delta_flags(&da.delta_arr[i],
				(vh->m_Flags & ~CIF_FmtVersionConvert));
		if (ret)
			goto err;

		if (clear_delta(&da.delta_arr[i])) {
			ret = SYSEXIT_WRITE;
			goto err;
		}
	}

err_rm:
	/* 5. Drop index table backup */
	for (i = 0; i < di->nimages; i++) {
		BACKUP_IDX_FNAME(fname, di->images[i]->file);
		if (unlink(fname) && errno != ENOENT)
			ploop_err(errno, "Failed to unlink %s", fname);
	}

err:
	deinit_delta_array(&da);
	ploop_unlock_di(di);

	if (ret == 0)
		ploop_log(0, "ploop image has been successfully converted");

	return ret;
}

static int do_restore_fmt_version(struct delta *d, struct delta *idelta)
{
	int ret = 1;
	__u32 cluster;
	__u32 clu;
	void *buf = NULL;

	if (d->l1_size != idelta->l1_size ||
			d->l2_size != idelta->l2_size ||
			d->blocksize != idelta->blocksize)
	{
		ret = SYSEXIT_PARAM;
		ploop_err(0, "Unable to restore: header mismatch");
		goto err;
	}

	cluster = S2B(idelta->blocksize);
	if (p_memalign(&buf, 4096, cluster)) {
		ret = SYSEXIT_MALLOC;
		goto err;
	}

	for (clu = 0; clu < idelta->l1_size; clu++) {
		off_t off = clu * cluster;

		if (PREAD(idelta, buf, cluster, off)) {
			ret = SYSEXIT_READ;
			goto err;
		}

		if (clu == 0) {
			struct ploop_pvd_header *vh = (struct ploop_pvd_header *)buf;
			vh->m_DiskInUse = 1;
			vh->m_Flags |= CIF_FmtVersionConvert;
		}

		if (PWRITE(d, buf, cluster, off)) {
			ret = SYSEXIT_WRITE;
			goto err;
		}
	}
	if (fsync(d->fd)) {
		ploop_err(errno, "Failed to sync");
		ret = SYSEXIT_FSYNC;
		goto err;
	}
	ret = 0;
err:
	free(buf);
	return ret;
}

static int restore_fmt_version(const char *file)
{
	char fname[PATH_MAX];
	int ret;
	struct ploop_pvd_header *vh;
	struct delta d = {};
	struct delta idelta = {};

	ret = open_delta(&d, file, O_RDWR, OD_ALLOW_DIRTY | OD_OFFLINE);
	if (ret)
		return ret;

	vh = (struct ploop_pvd_header *) d.hdr0;
	if (!(vh->m_Flags & CIF_FmtVersionConvert)) {
		close_delta(&d);
		return 0;
	}

	BACKUP_IDX_FNAME(fname, file);
	ret = open_delta(&idelta, fname, O_RDONLY, OD_ALLOW_DIRTY | OD_OFFLINE);
	if (ret)
		goto err;

	ploop_log(0, "Restore index table %s", file);
	ret = do_restore_fmt_version(&d, &idelta);
	if (ret)
		goto err;

	ret = change_delta_flags(&d,(vh->m_Flags & ~CIF_FmtVersionConvert));
	if (ret)
		goto err;

	if (clear_delta(&d)) {
		ret = SYSEXIT_WRITE;
		goto err;
	}

	if (unlink(fname) && errno != ENOENT)
		ploop_err(errno, "Failed to unlink %s", fname);

err:
	close_delta(&d);
	close_delta(&idelta);

	return ret;
}

int check_and_restore_fmt_version(struct ploop_disk_images_data *di)
{
	int i, base_id, ret;
	struct ploop_pvd_header *vh;
	struct delta d = {};
	const char *guid;

	if (di->mode == PLOOP_RAW_MODE)
		return 0;

	if ((guid = ploop_get_base_delta_uuid(di)) == NULL ||
		 (base_id = find_image_idx_by_guid(di, guid)) == -1)
	{
		ploop_log(-1, "Unable to find base image");
		return SYSEXIT_PARAM;
	}

	/* Check CIF_FmtVersionConvert mark on root image */
	ret = open_delta(&d, di->images[base_id]->file, O_RDONLY, OD_ALLOW_DIRTY | OD_OFFLINE);
	if (ret)
		return ret;

	vh = (struct ploop_pvd_header *) d.hdr0;
	if (!(vh->m_Flags & CIF_FmtVersionConvert)) {
		close_delta(&d);
		return 0;
	}

	close_delta(&d);

	ploop_log(0, "Image remains in converting fmt version state, restoring...");
	for (i = 0; i < di->nimages; i++) {
		if (i == base_id)
			continue;
		ret = restore_fmt_version(di->images[i]->file);
		if (ret)
			goto err;
	}
	/* Do restore base image at the end */
	ret = restore_fmt_version(di->images[base_id]->file);

err:
	return ret;
}

static int ploop_get_info(struct ploop_disk_images_data *di, struct ploop_info *info)
{
	char mnt[PATH_MAX];
	char dev[64];
	int ret = -1;

	if (ploop_lock_di(di))
		return SYSEXIT_LOCK;

	ret = ploop_find_dev_by_uuid(di, 1, dev, sizeof(dev));
	if (ret == -1) {
		ret = SYSEXIT_SYS;
		goto err;
	}
	if (ret == 0) {
		ret = get_mount_dir(dev, mnt, sizeof(mnt));
		if (ret)
			goto err;
		ret = get_statfs_info(mnt, info);
	} else {
		/* reinit .statfs */
		struct ploop_mount_param param = {};

		ret = auto_mount_image(di, &param);
		if (ret == 0)
			ploop_umount(param.device, di);
		free_mount_param(&param);
		ret = read_statfs_info(di->images[0]->file, info);
		if (ret)
			goto err;
	}

err:
	ploop_unlock_di(di);

	return ret;
}

int ploop_get_info_by_descr(const char *descr, struct ploop_info *info)
{
	struct ploop_disk_images_data *di;
	int ret;

	/* Try the fast path first, for stopped ploop */
	if (read_statfs_info(descr, info) == 0)
		return 0;

	ret = ploop_read_disk_descr(&di, descr);
	if (ret)
		return ret;

	ret = ploop_get_info(di, info);

	ploop_free_diskdescriptor(di);

	return ret;
}

static int do_snapshot(int lfd, int fd, struct ploop_ctl_delta *req)
{
	req->f.pctl_fd = fd;

	if (ioctl(lfd, PLOOP_IOC_SNAPSHOT, req) < 0) {
		ploop_err(errno, "PLOOP_IOC_SNAPSHOT");
		return SYSEXIT_DEVIOC;
	}

	return 0;
}

int create_snapshot(const char *device, const char *delta, int syncfs)
{
	int ret;
	int lfd = -1;
	int fd = -1;
	off_t bdsize;
	struct ploop_ctl_delta req;
	__u32 blocksize;
	int version;

	ret = ploop_complete_running_operation(device);
	if (ret)
		return ret;

	ret = get_image_param_online(device, &bdsize,
			&blocksize, &version);
	if (ret)
		return ret;

	lfd = open(device, O_RDONLY);
	if (lfd < 0) {
		ploop_err(errno, "Can't open device %s", device);
		return SYSEXIT_DEVICE;
	}

	fd = create_empty_delta(delta, blocksize, bdsize, version);
	if (fd < 0) {
		ret = SYSEXIT_OPEN;
		goto err;
	}

	memset(&req, 0, sizeof(req));

	req.c.pctl_format = PLOOP_FMT_PLOOP1;
	req.c.pctl_flags = syncfs ? PLOOP_FLAG_FS_SYNC : 0;
	req.c.pctl_cluster_log = ffs(blocksize) - 1;
	req.c.pctl_size = 0;
	req.c.pctl_chunks = 1;
	req.f.pctl_type = PLOOP_IO_AUTO;

	ploop_log(0, "Creating snapshot dev=%s img=%s", device, delta);
	ret = do_snapshot(lfd, fd, &req);
	if (ret)
		unlink(delta);
err:
	if (lfd >= 0)
		close(lfd);
	if (fd >= 0)
		close(fd);

	return ret;
}

int ploop_get_spec(struct ploop_disk_images_data *di, struct ploop_spec *spec)
{
	int ret;

	if (ploop_lock_di(di))
		return SYSEXIT_LOCK;

	ret = get_image_param(di, di->top_guid, &spec->size, &spec->blocksize,
			&spec->fmt_version);

	ploop_unlock_di(di);

	return ret;
}

int ploop_create_snapshot(struct ploop_disk_images_data *di, struct ploop_snapshot_param *param)
{
	int ret;
	int fd;
	char dev[64];
	char snap_guid[61];
	char file_guid[61];
	char fname[PATH_MAX];
	char conf[PATH_MAX];
	char conf_tmp[PATH_MAX];
	int online = 0;
	int n;
	off_t size;
	__u32 blocksize;
	int version;

	if (di->nimages == 0) {
		ploop_err(0, "No images");
		return SYSEXIT_PARAM;
	}
	if (param->guid != NULL && !is_valid_guid(param->guid)) {
		ploop_err(0, "Incorrect guid %s", param->guid);
		return SYSEXIT_PARAM;
	}

	if (ploop_lock_di(di))
		return SYSEXIT_LOCK;

	ret = gen_uuid_pair(snap_guid, sizeof(snap_guid),
			file_guid, sizeof(file_guid));
	if (ret) {
		ploop_err(errno, "Can't generate uuid");
		goto err_cleanup1;
	}

	if (param->guid != NULL) {
		if (find_snapshot_by_guid(di, param->guid) != -1) {
			ploop_err(0, "The snapshot %s already exist",
				param->guid);
			ret = SYSEXIT_PARAM;
			goto err_cleanup1;
		}
		strcpy(snap_guid, param->guid);
	}
	n = get_snapshot_count(di);
	if (n == -1) {
		ret = SYSEXIT_PARAM;
		goto err_cleanup1;
	} else if (n > 128-2) {
		/* The number of images limited by 128
		   so the snapshot limit 128 - base_image - one_reserverd
		 */
		ret = SYSEXIT_PARAM;
		ploop_err(errno, "Unable to create a snapshot."
			" The maximum number of snapshots (%d) has been reached",
			n-1);
		goto err_cleanup1;
	}

	snprintf(fname, sizeof(fname), "%s.%s",
			di->images[0]->file, file_guid);
	ploop_di_change_guid(di, di->top_guid, snap_guid);
	ret = ploop_di_add_image(di, fname, TOPDELTA_UUID, snap_guid);
	if (ret)
		goto err_cleanup1;

	get_disk_descriptor_fname(di, conf, sizeof(conf));
	snprintf(conf_tmp, sizeof(conf_tmp), "%s.tmp", conf);
	ret = ploop_store_diskdescriptor(conf_tmp, di);
	if (ret)
		goto err_cleanup1;

	ret = ploop_find_dev_by_uuid(di, 1, dev, sizeof(dev));
	if (ret == -1) {
		ret = SYSEXIT_SYS;
		goto err_cleanup2;
	}
	else if (ret == 0)
		online = 1;

	if (!online) {
		// offline snapshot
		ret = get_image_param_offline(di, snap_guid, &size, &blocksize, &version);
		if (ret)
			goto err_cleanup2;

		fd = create_empty_delta(fname, blocksize, size, version);
		if (fd < 0) {
			ret = SYSEXIT_CREAT;
			goto err_cleanup2;
		}
		close(fd);
	} else {
		// Always sync fs
		ret = create_snapshot(dev, fname, 1);
		if (ret)
			goto err_cleanup2;
	}

	if (rename(conf_tmp, conf)) {
		ploop_err(errno, "Can't rename %s %s",
				conf_tmp, conf);
		ret = SYSEXIT_RENAME;
	}

	if (ret && !online && unlink(fname))
		ploop_err(errno, "Can't unlink %s",
				fname);

	ploop_log(0, "ploop snapshot %s has been successfully created",
			snap_guid);
err_cleanup2:
	if (ret && !online && unlink(conf_tmp))
		ploop_err(errno, "Can't unlink %s", conf_tmp);

err_cleanup1:
	ploop_unlock_di(di);

	return ret;
}

int ploop_switch_snapshot_ex(struct ploop_disk_images_data *di,
		struct ploop_snapshot_switch_param *param)
{
	int ret;
	int fd;
	char dev[64];
	char uuid[61];
	char file_uuid[61];
	char new_top_delta_fname[PATH_MAX] = "";
	char *old_top_delta_fname = NULL;
	char conf[PATH_MAX];
	char conf_tmp[PATH_MAX];
	off_t size;
	const char *guid = param->guid;
	int flags = param->flags;
	__u32 blocksize;
	int version;

	if (!is_valid_guid(guid)) {
		ploop_err(0, "Incorrect guid %s", guid);
		return SYSEXIT_PARAM;
	}

	if (ploop_lock_di(di))
		return SYSEXIT_LOCK;

	ret = SYSEXIT_PARAM;
	if (strcmp(di->top_guid, guid) == 0) {
		ploop_err(errno, "Nothing to do, already on %s snapshot",
				guid);
		goto err_cleanup1;
	}

	if (find_snapshot_by_guid(di, guid) == -1) {
		ploop_err(0, "Can't find snapshot by uuid %s",
				guid);
		goto err_cleanup1;
	}

	// Read image param from snapshot we going to switch on
	ret = get_image_param(di, guid, &size, &blocksize, &version);
	if (ret)
		goto err_cleanup1;

	ret = gen_uuid_pair(uuid, sizeof(uuid), file_uuid, sizeof(file_uuid));
	if (ret) {
		ploop_err(errno, "Can't generate uuid");
		goto err_cleanup1;
	}

	if (!(flags & PLOOP_SNAP_SKIP_TOPDELTA_DESTROY)) {
		// device should be stopped
		ret = ploop_find_dev_by_uuid(di, 1, dev, sizeof(dev));
		if (ret == -1) {
			ret = SYSEXIT_SYS;
			goto err_cleanup1;
		} else if (ret == 0) {
			ret = SYSEXIT_PARAM;
			ploop_err(0, "Unable to perform switch to snapshot operation"
					" on running device (%s)",
					dev);
			goto err_cleanup1;
		}
		ret = ploop_di_remove_image(di, di->top_guid, 0, &old_top_delta_fname);
		if (ret)
			goto err_cleanup1;
	} else if (param->guid_old != NULL) {
		if (!is_valid_guid(param->guid_old)) {
			ploop_err(0, "Incorrect guid %s", param->guid_old);
			goto err_cleanup1;
		}

		if (find_snapshot_by_guid(di, param->guid_old) != -1) {
			ploop_err(0, "Incorrect guid_old %s: already exists",
					param->guid_old);
			goto err_cleanup1;
		}

		ploop_di_change_guid(di, di->top_guid, param->guid_old);
	}

	if (flags & PLOOP_SNAP_SKIP_TOPDELTA_CREATE) {
		ploop_di_change_guid(di, guid, TOPDELTA_UUID);
	} else {
		snprintf(new_top_delta_fname, sizeof(new_top_delta_fname), "%s.%s",
			di->images[0]->file, file_uuid);
		ret = ploop_di_add_image(di, new_top_delta_fname, TOPDELTA_UUID, guid);
		if (ret)
			goto err_cleanup1;
	}

	get_disk_descriptor_fname(di, conf, sizeof(conf));
	snprintf(conf_tmp, sizeof(conf_tmp), "%s.tmp", conf);
	ret = ploop_store_diskdescriptor(conf_tmp, di);
	if (ret)
		goto err_cleanup1;

	// offline snapshot
	if (!(flags & PLOOP_SNAP_SKIP_TOPDELTA_CREATE)) {
		fd = create_empty_delta(new_top_delta_fname, blocksize, size, version);
		if (fd == -1) {
			ret = SYSEXIT_CREAT;
			goto err_cleanup2;
		}
		close(fd);
	}

	if (rename(conf_tmp, conf)) {
		ploop_err(errno, "Can't rename %s %s",
				conf_tmp, conf);
		ret = SYSEXIT_RENAME;
		goto err_cleanup3;
	}

	/* destroy precached info */
	drop_statfs_info(di->images[0]->file);

	if (old_top_delta_fname != NULL) {
		ploop_log(0, "Removing %s", old_top_delta_fname);
		if (unlink(old_top_delta_fname))
			ploop_err(errno, "Can't unlink %s",
					old_top_delta_fname);
	}

	ploop_log(0, "ploop snapshot has been successfully switched");
err_cleanup3:
	if (ret && unlink(new_top_delta_fname))
		ploop_err(errno, "Can't unlink %s",
				conf_tmp);
err_cleanup2:
	if (ret && unlink(conf_tmp))
		ploop_err(errno, "Can't unlink %s",
				conf_tmp);
err_cleanup1:
	ploop_unlock_di(di);
	free(old_top_delta_fname);

	return ret;
}

int ploop_switch_snapshot(struct ploop_disk_images_data *di, const char *guid, int flags)
{
	struct ploop_snapshot_switch_param param = {};

	param.guid = (char *) guid;
	param.flags = flags;

	return ploop_switch_snapshot_ex(di, &param);
}

int ploop_delete_top_delta(struct ploop_disk_images_data *di)
{
	return ploop_delete_snapshot(di, di->top_guid);
}

/* delete snapshot by guid
 * 1) if guid is not active and last -> delete guid
 * 2) if guid is not last merge with child -> delete child
 */
int ploop_delete_snapshot(struct ploop_disk_images_data *di, const char *guid)
{
	int ret;
	char conf[PATH_MAX];
	char *fname = NULL;
	int nelem = 0;
	char dev[64];
	int snap_id;

	if (ploop_lock_di(di))
		return SYSEXIT_LOCK;

	ret = SYSEXIT_PARAM;
	snap_id = find_snapshot_by_guid(di, guid);
	if (snap_id == -1) {
		ploop_err(0, "Can't find snapshot by uuid %s",
				guid);
		goto err;
	}
	ret = ploop_find_dev_by_uuid(di, 1, dev, sizeof(dev));
	if (ret == -1) {
		ret = SYSEXIT_SYS;
		goto err;
	}
	else if (ret == 0 && strcmp(di->top_guid, guid) == 0) {
		ret = SYSEXIT_PARAM;
		ploop_err(0, "Unable to delete active snapshot %s",
				guid);
		goto err;
	}

	nelem = ploop_get_child_count_by_uuid(di, guid);
	if (nelem == 0) {
		if (strcmp(di->snapshots[snap_id]->parent_guid, NONE_UUID) == 0) {
			ret = SYSEXIT_PARAM;
			ploop_err(0, "Unable to delete base image");
			goto err;
		}
		/* snapshot is not active and last -> delete */
		ret = ploop_di_remove_image(di, guid, 1, &fname);
		if (ret)
			goto err;
		get_disk_descriptor_fname(di, conf, sizeof(conf));
		ret = ploop_store_diskdescriptor(conf, di);
		if (ret)
			goto err;
		ploop_log(0, "Removing %s", fname);
		if (fname != NULL && unlink(fname)) {
			ploop_err(errno, "unlink %s", fname);
			ret = SYSEXIT_UNLINK;
		}
		if (ret == 0)
			ploop_log(0, "ploop snapshot %s has been successfully deleted",
				guid);
	} else if (nelem == 1) {
		ret = ploop_merge_snapshot_by_guid(di, guid, PLOOP_MERGE_WITH_CHILD);
	} else {
		/* There no functionality to merge snapshot with >1 child */
		ret = SYSEXIT_PARAM;
		ploop_err(0, "There are %d references on %s snapshot: operation not supported",
				nelem, guid);
	}

err:
	free(fname);
	ploop_unlock_di(di);

	return ret;
}
