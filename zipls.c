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
ls(uint8_t *zip, size_t len, uint8_t *cd)
{
	uint16_t	 flen, xlen, clen;
	uint8_t		*end;
	char		 filename[PATH_MAX];

	end = zip + len;
	while (cd < end - 46 && memcmp(cd, "\x50\x4b\x01\x02", 4) == 0) {
		memcpy(&flen, cd + 28, sizeof(flen));
		memcpy(&xlen, cd + 28 + 2, sizeof(xlen));
		memcpy(&clen, cd + 28 + 2 + 2, sizeof(clen));

		flen = le16toh(flen);
		xlen = le16toh(xlen);
		clen = le16toh(clen);

		memset(filename, 0, sizeof(filename));
		memcpy(filename, cd + 46, MIN(sizeof(filename)-1, flen));

		printf("%s\n", filename);

		cd += 46 + flen + xlen + clen;
	}
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
	int	 fd;
	void	*zip, *cd;
	size_t	 len;

	if (argc != 2)
		errx(1, "missing file to inspect");

	if ((fd = open(argv[1], O_RDONLY)) == -1)
		err(1, "can't open %s", argv[1]);

	zip = map_file(fd, &len);
	if ((cd = find_central_directory(zip, len)) == NULL)
		errx(1, "can't find the central directory");

	ls(zip, len, cd);

	munmap(zip, len);
	close(fd);

	return 0;
}
