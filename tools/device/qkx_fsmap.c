// SPDX-License-Identifier: GPL-2.0
/*
 * qkx_fsmap - allocate GC-immune (pinned) files on f2fs and dump their extents.
 *
 * Android's userdata is metadata-encrypted by dm-default-key and file-encrypted
 * by FBE, so a kexec'd kernel cannot read the filesystem at all. Instead we let
 * f2fs allocate and pin a file, then address its raw blocks on the underlying
 * partition directly. Pinning keeps f2fs garbage collection from relocating the
 * blocks, so the physical map stays valid across reboots.
 *
 *   alloc <path> <bytes>   create, pin and fully allocate a file
 *   map   <path>           print "<file_offset> <phys_offset> <length>" in bytes
 *   mapdd <path> <bs>      same map in <bs> units, for dd's skip/seek/count
 *
 * mapdd exists because Android's shell evaluates $(( )) in 32 bits, so byte
 * offsets into a 230 GiB partition silently overflow if divided in script.
 *
 * Build: aarch64-linux-gnu-gcc -static -O2 qkx_fsmap.c -o qkx_fsmap
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>
#include <linux/fiemap.h>
#include <linux/fs.h>

#define F2FS_IOC_SET_PIN_FILE _IOW(0xf5, 13, uint32_t)
#define F2FS_IOC_GET_PIN_FILE _IOR(0xf5, 14, uint32_t)
#define F2FS_SUPER_MAGIC 0xf2f52010

#define MAX_EXTENTS 4096

static int die(const char *what)
{
	fprintf(stderr, "qkx_fsmap: %s: %s\n", what, strerror(errno));
	return 1;
}

static int do_alloc(const char *path, unsigned long long size)
{
	struct statfs sfs;
	uint32_t pin = 1;
	int fd;

	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		return die("open");

	if (fstatfs(fd, &sfs))
		return die("fstatfs");
	if (sfs.f_type != F2FS_SUPER_MAGIC) {
		fprintf(stderr, "qkx_fsmap: %s is not on f2fs (magic %#lx); "
			"pinning is unavailable\n", path, (unsigned long)sfs.f_type);
		return 1;
	}

	/* Pin before allocating: f2fs only guarantees GC immunity for blocks
	 * that were allocated while the inode was already pinned. */
	if (ioctl(fd, F2FS_IOC_SET_PIN_FILE, &pin))
		return die("F2FS_IOC_SET_PIN_FILE");

	if (fallocate(fd, 0, 0, (off_t)size))
		return die("fallocate");

	if (fsync(fd))
		return die("fsync");

	/* GET_PIN_FILE reports the GC failure count for the pinned inode, so a
	 * nonzero value means f2fs could not keep the blocks in place. */
	pin = 0;
	if (!ioctl(fd, F2FS_IOC_GET_PIN_FILE, &pin) && pin) {
		fprintf(stderr, "qkx_fsmap: %s reports %u pin/GC failures\n",
			path, pin);
		return 1;
	}
	close(fd);
	return 0;
}

static int do_map(const char *path, unsigned long long unit)
{
	char buf[sizeof(struct fiemap) + MAX_EXTENTS * sizeof(struct fiemap_extent)];
	struct fiemap *fm = (struct fiemap *)buf;
	unsigned long long expect = 0;
	struct stat st;
	unsigned i;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return die("open");
	if (fstat(fd, &st))
		return die("fstat");

	memset(buf, 0, sizeof(buf));
	fm->fm_start = 0;
	fm->fm_length = st.st_size;
	fm->fm_flags = FIEMAP_FLAG_SYNC;
	fm->fm_extent_count = MAX_EXTENTS;
	if (ioctl(fd, FS_IOC_FIEMAP, fm))
		return die("FS_IOC_FIEMAP");

	for (i = 0; i < fm->fm_mapped_extents; i++) {
		struct fiemap_extent *e = &fm->fm_extents[i];

		/* Anything unwritten, inline or delalloc has no stable physical
		 * location, so refuse rather than hand back a bogus map.
		 *
		 * ENCODED|DATA_ENCRYPTED is expected and fine: under FBE the file's
		 * contents are encrypted but its block placement is not affected,
		 * and we address those blocks directly instead of via the
		 * filesystem. NOT_ALIGNED is rejected because we need block-exact
		 * offsets. */
		if (e->fe_flags & (FIEMAP_EXTENT_UNKNOWN | FIEMAP_EXTENT_DELALLOC |
				   FIEMAP_EXTENT_DATA_INLINE | FIEMAP_EXTENT_DATA_TAIL |
				   FIEMAP_EXTENT_NOT_ALIGNED |
				   FIEMAP_EXTENT_UNWRITTEN)) {
			fprintf(stderr, "qkx_fsmap: extent %u has unusable flags %#x\n",
				i, e->fe_flags);
			return 1;
		}
		if (e->fe_logical != expect) {
			fprintf(stderr, "qkx_fsmap: hole before extent %u "
				"(logical %llu, expected %llu)\n", i,
				(unsigned long long)e->fe_logical, expect);
			return 1;
		}
		expect = e->fe_logical + e->fe_length;

		if ((e->fe_logical | e->fe_physical | e->fe_length) % unit) {
			fprintf(stderr, "qkx_fsmap: extent %u is not a multiple "
				"of %llu\n", i, unit);
			return 1;
		}
		printf("%llu %llu %llu\n",
		       (unsigned long long)e->fe_logical / unit,
		       (unsigned long long)e->fe_physical / unit,
		       (unsigned long long)e->fe_length / unit);
	}

	if (expect < (unsigned long long)st.st_size) {
		fprintf(stderr, "qkx_fsmap: mapped %llu of %llu bytes; "
			"increase MAX_EXTENTS\n", expect,
			(unsigned long long)st.st_size);
		return 1;
	}
	close(fd);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc == 4 && !strcmp(argv[1], "alloc"))
		return do_alloc(argv[2], strtoull(argv[3], NULL, 0));
	if (argc == 3 && !strcmp(argv[1], "map"))
		return do_map(argv[2], 1);
	if (argc == 4 && !strcmp(argv[1], "mapdd")) {
		unsigned long long unit = strtoull(argv[3], NULL, 0);

		if (!unit) {
			fprintf(stderr, "qkx_fsmap: block size must be nonzero\n");
			return 1;
		}
		return do_map(argv[2], unit);
	}

	fprintf(stderr, "usage: qkx_fsmap alloc <path> <bytes>\n"
			"       qkx_fsmap map <path>\n"
			"       qkx_fsmap mapdd <path> <blocksize>\n");
	return 1;
}
