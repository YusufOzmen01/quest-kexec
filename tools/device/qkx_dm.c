// SPDX-License-Identifier: GPL-2.0
/*
 * qkx_dm - build a dm-linear device from a pinned-file extent map.
 *
 * The kexec target cannot read Android's encrypted userdata filesystem, but the
 * blocks of a pinned file are plain: we stitch its physical extents back into a
 * contiguous block device, which then holds an image we can mount normally.
 *
 *   qkx_dm create <name> <backing-dev> <map-file>   map file lines: "<off> <phys> <len>"
 *   qkx_dm remove <name>
 *
 * create prints "<major> <minor> <bytes>" so the caller can mknod the node
 * itself; devtmpfs naming for dm devices is not something to rely on here.
 *
 * Build: aarch64-linux-gnu-gcc -static -O2 qkx_dm.c -o qkx_dm
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <linux/dm-ioctl.h>

#define DM_CONTROL "/dev/mapper/control"
#define BUF_SIZE (1 << 20)

static char buf[BUF_SIZE];

static int die(const char *what)
{
	fprintf(stderr, "qkx_dm: %s: %s\n", what, strerror(errno));
	return 1;
}

static struct dm_ioctl *init_hdr(const char *name)
{
	struct dm_ioctl *io = (struct dm_ioctl *)buf;

	memset(buf, 0, BUF_SIZE);
	io->version[0] = 4;
	io->version[1] = 0;
	io->version[2] = 0;
	io->data_size = BUF_SIZE;
	io->data_start = sizeof(*io);
	snprintf(io->name, sizeof(io->name), "%s", name);
	return io;
}

static int dm_open(void)
{
	int fd = open(DM_CONTROL, O_RDWR);

	if (fd < 0) {
		/* device-mapper's control node is not created by devtmpfs. */
		if (errno == ENOENT)
			fprintf(stderr, "qkx_dm: %s missing; create it with "
				"mknod (major 10, see /proc/misc)\n", DM_CONTROL);
		return -1;
	}
	return fd;
}

static int do_remove(int fd, const char *name)
{
	struct dm_ioctl *io = init_hdr(name);

	if (ioctl(fd, DM_DEV_REMOVE, io))
		return die("DM_DEV_REMOVE");
	return 0;
}

static int do_create(int fd, const char *name, const char *backing, const char *mapfile)
{
	unsigned long long off, phys, len, sectors = 0;
	struct dm_ioctl *io;
	struct stat st;
	unsigned targets = 0;
	char line[256];
	char *ptr;
	FILE *f;

	if (stat(backing, &st))
		return die(backing);

	f = fopen(mapfile, "r");
	if (!f)
		return die(mapfile);

	io = init_hdr(name);
	if (ioctl(fd, DM_DEV_CREATE, io))
		return die("DM_DEV_CREATE");

	io = init_hdr(name);
	ptr = buf + sizeof(*io);

	while (fgets(line, sizeof(line), f)) {
		struct dm_target_spec *spec;
		char params[128];
		size_t plen;

		if (line[0] == '#' || line[0] == '\n')
			continue;
		if (sscanf(line, "%llu %llu %llu", &off, &phys, &len) != 3) {
			fprintf(stderr, "qkx_dm: bad map line: %s", line);
			return 1;
		}
		if ((off | phys | len) & 511) {
			fprintf(stderr, "qkx_dm: extent not sector aligned: %s", line);
			return 1;
		}
		if (off / 512 != sectors) {
			fprintf(stderr, "qkx_dm: map is not contiguous at %llu\n", off);
			return 1;
		}

		spec = (struct dm_target_spec *)ptr;
		spec->sector_start = sectors;
		spec->length = len / 512;
		spec->status = 0;
		snprintf(spec->target_type, sizeof(spec->target_type), "linear");

		snprintf(params, sizeof(params), "%u:%u %llu",
			 major(st.st_rdev), minor(st.st_rdev), phys / 512);
		plen = strlen(params) + 1;
		/* dm requires each target's parameters to be 8-byte aligned. */
		plen = (plen + 7) & ~7UL;
		memcpy(ptr + sizeof(*spec), params, strlen(params) + 1);
		spec->next = sizeof(*spec) + plen;

		ptr += spec->next;
		sectors += len / 512;
		targets++;

		if ((size_t)(ptr - buf) > BUF_SIZE - 512) {
			fprintf(stderr, "qkx_dm: map too large\n");
			return 1;
		}
	}
	fclose(f);

	if (!targets) {
		fprintf(stderr, "qkx_dm: empty map\n");
		return 1;
	}

	io->target_count = targets;
	io->data_size = ptr - buf;
	if (ioctl(fd, DM_TABLE_LOAD, io)) {
		die("DM_TABLE_LOAD");
		goto fail;
	}

	io = init_hdr(name);
	if (ioctl(fd, DM_DEV_SUSPEND, io)) {
		die("DM_DEV_SUSPEND");
		goto fail;
	}

	printf("%u %u %llu\n", major(io->dev), minor(io->dev), sectors * 512);
	return 0;

fail:
	do_remove(fd, name);
	return 1;
}

int main(int argc, char **argv)
{
	int fd, ret;

	fd = dm_open();
	if (fd < 0)
		return 1;

	if (argc == 5 && !strcmp(argv[1], "create"))
		ret = do_create(fd, argv[2], argv[3], argv[4]);
	else if (argc == 3 && !strcmp(argv[1], "remove"))
		ret = do_remove(fd, argv[2]);
	else {
		fprintf(stderr, "usage: qkx_dm create <name> <backing-dev> <map-file>\n"
				"       qkx_dm remove <name>\n");
		ret = 1;
	}
	close(fd);
	return ret;
}
