// SPDX-License-Identifier: GPL-2.0
/*
 * qkx_rawcp - copy an image into a pinned file's raw blocks.
 *
 * Userdata is metadata-encrypted, so writing through the filesystem produces
 * ciphertext that the kexec target cannot read. We instead write the image
 * straight to the partition blocks that f2fs allocated to the pinned file, which
 * the target reads back verbatim.
 *
 *   qkx_rawcp zero <map> <dev>              write zeros over every extent
 *   qkx_rawcp copy <map> <dev> <src>|-      copy image (sparse: skips zero blocks)
 *   qkx_rawcp verify <map> <dev> <src>|-    compare image against the raw blocks
 *
 * The map is qkx_fsmap's byte-unit output: "<file_offset> <phys_offset> <length>".
 * Writing only ever touches those extents, never the surrounding filesystem.
 *
 * Build: aarch64-linux-gnu-gcc -static -O2 qkx_rawcp.c -o qkx_rawcp
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHUNK (1u << 20)
#define BLOCK 4096

enum mode { MODE_ZERO, MODE_COPY, MODE_VERIFY };

struct extent {
	unsigned long long file;
	unsigned long long phys;
	unsigned long long len;
};

static unsigned char buf[CHUNK];
static unsigned char cmp[CHUNK];
static struct extent *ext;
static unsigned n_ext;

static int die(const char *what)
{
	fprintf(stderr, "qkx_rawcp: %s: %s\n", what, strerror(errno));
	return 1;
}

static int load_map(const char *path, unsigned long long *total)
{
	unsigned long long expect = 0;
	char line[256];
	unsigned cap = 64;
	FILE *f = fopen(path, "r");

	if (!f)
		return die(path);
	ext = malloc(cap * sizeof(*ext));
	if (!ext)
		return die("malloc");

	while (fgets(line, sizeof(line), f)) {
		struct extent e;

		if (line[0] == '#' || line[0] == '\n')
			continue;
		if (sscanf(line, "%llu %llu %llu", &e.file, &e.phys, &e.len) != 3) {
			fprintf(stderr, "qkx_rawcp: bad map line: %s", line);
			return 1;
		}
		if (e.file != expect) {
			fprintf(stderr, "qkx_rawcp: map has a hole at %llu\n", e.file);
			return 1;
		}
		if (e.len % BLOCK || e.phys % BLOCK) {
			fprintf(stderr, "qkx_rawcp: extent is not %u-aligned\n", BLOCK);
			return 1;
		}
		expect = e.file + e.len;
		if (n_ext == cap) {
			cap *= 2;
			ext = realloc(ext, cap * sizeof(*ext));
			if (!ext)
				return die("realloc");
		}
		ext[n_ext++] = e;
	}
	fclose(f);
	if (!n_ext) {
		fprintf(stderr, "qkx_rawcp: empty map\n");
		return 1;
	}
	*total = expect;
	return 0;
}

static int is_zero(const unsigned char *p, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		if (p[i])
			return 0;
	return 1;
}

/* Pipes return short reads; the image must be consumed in exact steps. */
static size_t read_full(int fd, unsigned char *p, size_t n)
{
	size_t done = 0;

	while (done < n) {
		ssize_t r = read(fd, p + done, n - done);

		if (r < 0) {
			if (errno == EINTR)
				continue;
			return done;
		}
		if (!r)
			break;
		done += r;
	}
	return done;
}

static int write_at(int fd, const unsigned char *p, size_t n, unsigned long long off)
{
	while (n) {
		ssize_t w = pwrite(fd, p, n, (off_t)off);

		if (w < 0) {
			if (errno == EINTR)
				continue;
			return die("pwrite");
		}
		p += w;
		n -= w;
		off += w;
	}
	return 0;
}

int main(int argc, char **argv)
{
	unsigned long long total, done = 0, written = 0, srcleft = ~0ULL;
	int devfd, srcfd = -1, mode;
	unsigned long long mismatch = 0;
	const char *src = NULL;
	unsigned i;

	if (argc >= 4 && !strcmp(argv[1], "zero"))
		mode = MODE_ZERO;
	else if (argc == 5 && !strcmp(argv[1], "copy"))
		mode = MODE_COPY;
	else if (argc == 5 && !strcmp(argv[1], "verify"))
		mode = MODE_VERIFY;
	else {
		fprintf(stderr, "usage: qkx_rawcp zero   <map> <dev>\n"
				"       qkx_rawcp copy   <map> <dev> <src>|-\n"
				"       qkx_rawcp verify <map> <dev> <src>|-\n");
		return 1;
	}

	if (load_map(argv[2], &total))
		return 1;

	devfd = open(argv[3], mode == MODE_VERIFY ? O_RDONLY : O_RDWR);
	if (devfd < 0)
		return die(argv[3]);

	if (mode != MODE_ZERO) {
		src = argv[4];
		srcfd = !strcmp(src, "-") ? STDIN_FILENO : open(src, O_RDONLY);
		if (srcfd < 0)
			return die(src);
	} else {
		memset(buf, 0, CHUNK);
	}

	for (i = 0; i < n_ext; i++) {
		unsigned long long off = 0;

		while (off < ext[i].len) {
			size_t n = CHUNK;
			size_t got;

			if (ext[i].len - off < n)
				n = ext[i].len - off;

			if (mode == MODE_ZERO) {
				if (write_at(devfd, buf, n, ext[i].phys + off))
					return 1;
				written += n;
				off += n;
				done += n;
				continue;
			}

			got = srcleft ? read_full(srcfd, buf, n) : 0;
			if (got < n) {
				/* Short image: the rest of the file stays as the
				 * zeros written by the preceding zero pass. */
				srcleft = 0;
				memset(buf + got, 0, n - got);
			}

			if (mode == MODE_VERIFY) {
				if (pread(devfd, cmp, n, (off_t)(ext[i].phys + off)) != (ssize_t)n)
					return die("pread");
				if (memcmp(buf, cmp, n))
					mismatch += n;
			} else if (!is_zero(buf, n)) {
				if (write_at(devfd, buf, n, ext[i].phys + off))
					return 1;
				written += n;
			}
			off += n;
			done += n;
			if (!(done % (256ULL << 20)))
				fprintf(stderr, "qkx_rawcp: %llu MiB / %llu MiB\n",
					done >> 20, total >> 20);
		}
	}

	if (mode != MODE_VERIFY && fsync(devfd))
		return die("fsync");
	close(devfd);

	if (mode == MODE_VERIFY) {
		if (mismatch) {
			fprintf(stderr, "qkx_rawcp: %llu bytes differ\n", mismatch);
			return 1;
		}
		fprintf(stderr, "qkx_rawcp: verified %llu MiB\n", done >> 20);
		return 0;
	}
	fprintf(stderr, "qkx_rawcp: covered %llu MiB, wrote %llu MiB\n",
		done >> 20, written >> 20);
	return 0;
}
