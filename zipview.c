/*
 * This is free and unencumbered software released into the public domain.
 */

#include <sys/mman.h>

#include <endian.h>
#include <err.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

#define COMPRESSION_NONE	0x00
#define COMPRESSION_DEFLATE	0x08

#define MIN(a, b) ((a) < (b) ? (a) : (b))

void *
find_central_directory(uint8_t *addr, size_t len)
{
	uint32_t	 offset;
	uint16_t	 clen;
	uint8_t		*p, *end;

	/*
	 * At -22 bytes from the end there is the end of the central
	 * directory assuming an empty comment.  It's a sensible place
	 * from which start.
	 */
	if (len < 22)
		return NULL;
	end = addr + len;
	p = end - 22;

again:
	for (; p > addr; --p)
		if (memcmp(p, "\x50\x4b\x05\x06", 4) == 0)
			break;

	if (p == addr)
		return NULL;

	/* read comment length */
	memcpy(&clen, p + 20, sizeof(clen));
	clen = le16toh(clen);

	/* false signature inside a comment? */
	if (clen + 22 != end - p) {
		p--;
		goto again;
	}

	/* read the offset for the central directory */
	memcpy(&offset, p + 16, sizeof(offset));
	offset = le32toh(offset);

	if (addr + offset > p)
		return NULL;

	return addr + offset;
}

void
unzip_none(uint8_t *data, size_t size, unsigned long ocrc)
{
	unsigned long crc = 0;

	fwrite(data, 1, size, stdout);

	crc = crc32(0, data, size);
	if (crc != ocrc)
		errx(1, "CRC mismatch");
}

void
unzip_deflate(uint8_t *data, size_t size, unsigned long ocrc)
{
	z_stream	stream;
	size_t		have;
	unsigned long	crc = 0;
	char		buf[BUFSIZ];

	stream.zalloc = Z_NULL;
	stream.zfree = Z_NULL;
	stream.opaque = Z_NULL;
	stream.next_in = data;
	stream.avail_in = size;
	stream.next_out = Z_NULL;
	stream.avail_out = 0;
	if (inflateInit2(&stream, -15) != Z_OK)
		err(1, "inflateInit failed");

	do {
		stream.next_out = buf;
		stream.avail_out = sizeof(buf);

		switch (inflate(&stream, Z_BLOCK)) {
		case Z_STREAM_ERROR:
			errx(1, "stream error");
		case Z_NEED_DICT:
			errx(1, "need dict");
		case Z_DATA_ERROR:
			errx(1, "data error: %s", stream.msg);
		case Z_MEM_ERROR:
			errx(1, "memory error");
		}

		have = sizeof(buf) - stream.avail_out;
		fwrite(buf, 1, have, stdout);
		crc = crc32(crc, buf, have);
	} while (stream.avail_out == 0);

	inflateEnd(&stream);

	if (crc != ocrc)
		errx(1, "CRC mismatch");
}

void
unzip(uint8_t *zip, size_t len, uint8_t *entry)
{
	uint32_t	 size, osize, crc, off;
	uint16_t	 flags, compression;
	uint16_t	 flen, xlen;
	uint8_t		*data, *offset;

	/* read the offset of the file record */
	memcpy(&off, entry + 42, sizeof(off));
	offset = zip + le32toh(off);

	if (offset > zip + len - 4 ||
	    memcmp(offset, "\x50\x4b\x03\x04", 4) != 0)
		errx(1, "invalid offset or file header signature");

	memcpy(&flags, offset + 6, sizeof(flags));
	memcpy(&compression, offset + 8, sizeof(compression));

	flags = le16toh(flags);
	compression = le16toh(compression);

	memcpy(&crc, entry + 16, sizeof(crc));
	memcpy(&size, entry + 20, sizeof(size));
	memcpy(&osize, entry + 24, sizeof(osize));

	crc = le32toh(crc);
	size = le32toh(size);
	osize = le32toh(osize);

	memcpy(&flen, offset + 26, sizeof(flen));
	memcpy(&xlen, offset + 28, sizeof(xlen));

	flen = le16toh(flen);
	xlen = le16toh(xlen);

	data = offset + 30 + flen + xlen;
	if (data + size > zip + len)
		errx(1, "corrupted zip, offset out of file");

	switch (compression) {
	case COMPRESSION_NONE:
                unzip_none(data, size, crc);
		break;
	case COMPRESSION_DEFLATE:
                unzip_deflate(data, size, crc);
		break;
	default:
		errx(1, "unknown compression method 0x%02x",
		    compression);
	}
}

void *
next(uint8_t *zip, size_t len, uint8_t *entry)
{
	uint16_t	 flen, xlen, clen;
	uint8_t		*next, *end;

	memcpy(&flen, entry + 28, sizeof(flen));
	memcpy(&xlen, entry + 28 + 2, sizeof(xlen));
	memcpy(&clen, entry + 28 + 2 + 2, sizeof(xlen));

	flen = le16toh(flen);
	xlen = le16toh(xlen);
	clen = le16toh(clen);

	next = entry + 46 + flen + xlen + clen;
	end = zip + len;
	if (next >= end - 46 ||
	    memcmp(next, "\x50\x4b\x01\x02", 4) != 0)
		return NULL;
	return next;
}

void
filename(uint8_t *zip, size_t len, uint8_t *entry, char *buf,
    size_t size)
{
	uint16_t	flen;
	size_t		s;

	memcpy(&flen, entry + 28, sizeof(flen));
	flen = le16toh(flen);

        s = MIN(size-1, flen);
	memcpy(buf, entry + 46, s);
	buf[s] = '\0';
}

void
ls(uint8_t *zip, size_t len, uint8_t *cd)
{
	char	name[PATH_MAX];

	do {
		filename(zip, len, cd, name, sizeof(name));
		printf("%s\n", name);
	} while ((cd = next(zip, len, cd)) != NULL);
}

void *
find_file(uint8_t *zip, size_t len, uint8_t *cd, const char *target)
{
	char	name[PATH_MAX];

	do {
		filename(zip, len, cd, name, sizeof(name));
		if (!strcmp(name, target))
			return cd;
	} while ((cd = next(zip, len, cd)) != NULL);

	return NULL;
}

int
extract_file(uint8_t *zip, size_t len, uint8_t *cd, const char *target)
{
	if ((cd = find_file(zip, len, cd, target)) == NULL)
		return -1;

	unzip(zip, len, cd);
	return 0;
}

void *
map_file(int fd, size_t *len)
{
	off_t	 jump;
	void	*addr;

	if ((jump = lseek(fd, 0, SEEK_END)) == -1)
		err(1, "lseek");

	if (lseek(fd, 0, SEEK_SET) == -1)
		err(1, "lseek");

	if ((addr = mmap(NULL, jump, PROT_READ, MAP_PRIVATE, fd, 0))
	    == MAP_FAILED)
                err(1, "mmap");

	*len = jump;
	return addr;
}

int
main(int argc, char **argv)
{
	int	 i, fd;
	void	*zip, *cd;
	size_t	 len;

	if (argc < 2) {
		fprintf(stderr, "Usage: %s archive.zip [files...]",
		    *argv);
		return 1;
	}

	if ((fd = open(argv[1], O_RDONLY)) == -1)
		err(1, "can't open %s", argv[1]);

	zip = map_file(fd, &len);

#ifdef __OpenBSD__
	if (pledge("stdio", NULL) == -1)
		err(1, "pledge");
#endif

	if ((cd = find_central_directory(zip, len)) == NULL)
		errx(1, "can't find the central directory");

        if (argc == 2)
		ls(zip, len, cd);
        else {
                for (i = 2; i < argc; ++i)
			extract_file(zip, len, cd, argv[i]);
	}

	munmap(zip, len);
	close(fd);

	return 0;
}
