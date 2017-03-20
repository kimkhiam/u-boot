
#include "os_support.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <sha1.h>
#include "fdt_host.h"
#include <image.h>
#include <linux/ctype.h>
#include <mtd/mtd-user.h>
#include <libgen.h>

#define MAX_LEVEL	32		/* how deeply nested we will go */

static void usage(void);

struct fdtview_params {
	char *path;
	char *propname;
	char *imagefile;
	char *cmdname;
	int lflag;
	int oflag;
	int pflag;
	int iflag;
} params;

/*
 * Heuristic to guess if this is a string or concatenated strings.
 */
static int is_printable_string(const void *data, int len)
{
	const char *s = data;

	/* zero length is not */
	if (len == 0)
		return 0;

	/* must terminate with zero */
	if (s[len - 1] != '\0' && s[len - 1] != '\n')
		return 0;

	/* printable or a null byte (concatenated strings) */
	while (((*s == '\0') || isprint(*s) || isspace(*s)) && (len > 0)) {
		/*
		 * If we see a null, there are three possibilities:
		 * 1) If len == 1, it is the end of the string, printable
		 * 2) Next character also a null, not printable.
		 * 3) Next character not a null, continue to check.
		 */
		if (s[0] == '\0') {
			if (len == 1)
				return 1;
			if (s[1] == '\0')
				return 0;
		}
		s++;
		len--;
	}

	/* Not the null termination, or not done yet: not printable */
	if (*s != '\0' || len != 0)
		return 0;

	return 1;
}

/*
 * Print the property in the best format, a heuristic guess.  Print as
 * a string, concatenated strings, a byte, word, double word, or (if all
 * else fails) it is printed as a stream of bytes.
 */
static void print_data(const void *data, int len)
{
	int j;

	/* no data, don't print */
	if (len == 0)
		return;

	/*
	 * It is a string, but it may have multiple strings (embedded '\0's).
	 */
	if (is_printable_string(data, len)) {
		printf("\"");
		j = 0;
		while (j < len) {
			if (j > 0)
				printf("\", \"");
			printf(data);
			j    += strlen(data) + 1;
			data += strlen(data) + 1;
		}
		printf("\"");
		return;
	}

	if ((len %4) == 0) {
		const unsigned int *p;

		printf("<");
		for (j = 0, p = data; j < len/4; j ++)
			printf("0x%x%s", fdt32_to_cpu(p[j]), j < (len/4 - 1) ? " " : "");
		printf(">");
	} else { /* anything else... hexdump */
		const unsigned char *s;

		printf("[");
		for (j = 0, s = data; j < len; j++)
			printf("%02x%s", s[j], j < len - 1 ? " " : "");
		printf("]");
	}
}

/*
 * Recursively print (a portion of) the working_fdt.  The depth parameter
 * determines how deeply nested the fdt is printed.
 */
static int fdt_print(unsigned char *working_fdt, const char *pathp,
			char *prop, int depth)
{
	static char tabs[MAX_LEVEL+1] =
		"\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t"
		"\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t";
	const void *nodep;	/* property node pointer */
	int  nodeoffset;	/* node offset from libfdt */
	int  nextoffset;	/* next node offset from libfdt */
	uint32_t tag;		/* tag */
	int  len;		/* length of the property */
	int  level = 0;		/* keep track of nesting level */
	const struct fdt_property *fdt_prop;

	nodeoffset = fdt_path_offset(working_fdt, pathp);
	if (nodeoffset < 0) {
		/*
		 * Not found or something else bad happened.
		 */
		printf ("libfdt fdt_path_offset() returned %s\n",
			fdt_strerror(nodeoffset));
		return 1;
	}
	/*
	 * The user passed in a property as well as node path.
	 * Print only the given property and then return.
	 */
	if (prop) {
		nodep = fdt_getprop(working_fdt, nodeoffset, prop, &len);
		if (len == 0) {
			/* no property value */
			printf("%s %s\n", pathp, prop);
			return 0;
		} else if (len > 0) {
			printf("%s = ", prop);
			print_data (nodep, len);
			printf("\n");
			return 0;
		} else {
			printf("libfdt fdt_getprop(): %s\n",
				fdt_strerror(len));
			return 1;
		}
	}

	/*
	 * The user passed in a node path and no property,
	 * print the node and all subnodes.
	 */
	while(level >= 0) {
		tag = fdt_next_tag(working_fdt, nodeoffset, &nextoffset);
		switch(tag) {
		case FDT_BEGIN_NODE:
			pathp = fdt_get_name(working_fdt, nodeoffset, NULL);
			if (level <= depth) {
				if (pathp == NULL)
					pathp = "/* NULL pointer error */";
				if (*pathp == '\0')
					pathp = "/";	/* root is nameless */
				printf("%s%s {\n",
					&tabs[MAX_LEVEL - level], pathp);
			}
			level++;
			if (level >= MAX_LEVEL) {
				printf("Nested too deep, aborting.\n");
				return 1;
			}
			break;
		case FDT_END_NODE:
			level--;
			if (level <= depth)
				printf("%s};\n", &tabs[MAX_LEVEL - level]);
			if (level == 0) {
				level = -1;		/* exit the loop */
			}
			break;
		case FDT_PROP:
			fdt_prop = fdt_offset_ptr(working_fdt, nodeoffset,
					sizeof(*fdt_prop));
			pathp    = fdt_string(working_fdt,
					fdt32_to_cpu(fdt_prop->nameoff));
			len      = fdt32_to_cpu(fdt_prop->len);
			nodep    = fdt_prop->data;
			if (len < 0) {
				printf ("libfdt fdt_getprop(): %s\n",
					fdt_strerror(len));
				return 1;
			} else if (len == 0) {
				/* the property has no value */
				if (level <= depth)
					printf("%s%s;\n",
						&tabs[MAX_LEVEL - level],
						pathp);
			} else {
				if (level <= depth) {
					printf("%s%s = ",
						&tabs[MAX_LEVEL - level],
						pathp);
					print_data (nodep, len);
					printf(";\n");
				}
			}
			break;
		case FDT_NOP:
			printf("%s/* NOP */\n", &tabs[MAX_LEVEL - level]);
			break;
		case FDT_END:
			return 1;
		default:
			if (level <= depth)
				printf("Unknown tag 0x%08X\n", tag);
			return 1;
		}
		nodeoffset = nextoffset;
	}
	return 0;
}

int main (int argc, char **argv)
{
	int ifd = -1;
	struct stat sbuf;
	unsigned char *working_fdt;
	int retval = 0;
	struct mtd_info_user mtd;
	off_t file_size = 0;
	int mmap_failed = 0;

	params.cmdname = basename(*argv);
	params.lflag = 0;
	params.oflag = 0;
	params.pflag = 0;
	params.iflag = 0;

	while (--argc > 0 && **++argv == '-') {
		while (*++*argv) {
			switch (**argv) {
			case 'l':
				if (argc <= 1)
					usage();
				if (argc >= 3) {
					params.path = *++argv;
					--argc;
				} else {
					params.path = NULL;
				}
				if (argc == 3) {
					params.propname = *++argv;
					--argc;
				} else {
					params.propname = NULL;
				}
				params.lflag = 1;
				goto NXTARG;
			case 'o':
				--argc;
				if (--argc <= 1)
					usage();
				params.path = *++argv;
				params.propname = *++argv;
				params.oflag = 1;
				goto NXTARG;
			case 'p':
				if (argc <= 1)
					usage();
				if (argc >= 3) {
					params.path = *++argv;
					--argc;
				} else {
					params.path = NULL;
				}
				if (argc == 3) {
					params.propname = *++argv;
					--argc;
				} else {
					params.propname = NULL;
				}
				params.pflag = 1;
				goto NXTARG;
			case 'i':
				if (argc <= 1)
					usage();
				params.iflag = 1;
				goto NXTARG;
			default:
				usage();
			}
		}
NXTARG:		;
	}

	if (argc != 1)
		usage();

	if (!params.lflag && !params.oflag && !params.pflag && !params.iflag)
		usage();

	params.imagefile = *argv;

	ifd = open(params.imagefile, O_RDONLY|O_BINARY|O_SYNC);

	if (ifd < 0) {
		fprintf(stderr, "%s: Can't open %s: %s\n",
			params.cmdname, params.imagefile,
			strerror(errno));
		exit(EXIT_FAILURE);
	}

	if (fstat(ifd, &sbuf) < 0) {
		fprintf(stderr, "%s: Can't stat %s: %s\n",
			params.cmdname, params.imagefile,
			strerror(errno));
		exit(EXIT_FAILURE);
	} else {
		file_size = sbuf.st_size;
	}

	if((sbuf.st_rdev & 0xFF00) == 0x1F00) {
		fprintf(stderr, "%s: Can't access the block interface to %s\n",
			params.cmdname, params.imagefile);
		exit(EXIT_FAILURE);
	}
	if((sbuf.st_rdev & 0xFF00) == 0x5A00) {
		if (ioctl (ifd, MEMGETINFO, mtd) < 0) {
			fprintf(stderr, "%s: Can't query mem info for %s: %s\n",
				params.cmdname, params.imagefile,
				strerror(errno));
			exit(EXIT_FAILURE);
		} else {
			file_size = mtd.size;
		}
	}

	if ((unsigned)file_size < sizeof(image_header_t)) {
		fprintf(stderr,
			"%s: Bad size: \"%s\" is not valid image\n",
			params.cmdname, params.imagefile);
		exit(EXIT_FAILURE);
	}

	working_fdt = mmap(0, file_size, PROT_READ, MAP_SHARED, ifd, 0);
	if (working_fdt == MAP_FAILED) {
		int readsize;
		int totalsize;
		mmap_failed = 1;

		printf("mmap() failed.  Falling back to read.");
		working_fdt = malloc(sizeof(image_header_t));
		if (working_fdt == NULL) {
			fprintf(stderr, "%s: malloc of %d bytes (header) failed: %s\n",
				params.cmdname, (int) file_size,
				strerror(errno));
			retval = 1;
			goto error;
		}
		if ((readsize = read(ifd, working_fdt, sizeof(image_header_t))) < 0) {
			fprintf(stderr, "%s: Can't read header from %s: %s\n",
				params.cmdname, params.imagefile,
				strerror(errno));
			retval = readsize;
			goto error;
		}
		if ((retval = fdt_check_header((void *)working_fdt)) < 0) {
			if (!params.iflag || !image_check_magic(
			    (image_header_t *)working_fdt)) {
				fprintf(stderr, "%s: Invalid image %s\n",
					params.cmdname, params.imagefile);
				goto error;
			}
		}
		totalsize = fdt_totalsize(working_fdt);
		if (totalsize < 0) {
			fprintf(stderr, "%s: Invalid image size: %d\n",
				params.cmdname, totalsize);
			retval = totalsize;
			goto error;
		}
		if (file_size > totalsize) file_size = totalsize;
		working_fdt = realloc(working_fdt, file_size);
		if (working_fdt == NULL) {
			fprintf(stderr, "%s: malloc of %d bytes failed: %s\n",
				params.cmdname, (int) file_size,
				strerror(errno));
			retval = 1;
			goto error;
		}
		if (read(ifd, working_fdt + readsize, file_size - readsize) < 0) {
			fprintf(stderr, "%s: Can't read %s: %s\n",
				params.cmdname, params.imagefile,
				strerror(errno));
			retval = readsize;
			goto error;
		}
	}

	if ((retval = fdt_check_header((void *)working_fdt)) < 0) {
		if (!params.iflag || !image_check_magic(
		    (image_header_t *)working_fdt)) {
			fprintf(stderr, "%s: Invalid image %s\n",
				params.cmdname, params.imagefile);
			goto error;
		}
	}

	if (params.oflag) {
		int offset;
		offset = fdt_path_offset(working_fdt, params.path);
		if (offset > 0) {
			int propLength = 0;
			const unsigned char* propData = (const uint8_t*)fdt_getprop(working_fdt, offset, params.propname, &propLength);
			if (propLength >= 0) {
				printf("%d %d\n", propData - working_fdt, propLength);
			}
		} else {
			retval = offset;
			goto error;
		}
	} else if (params.lflag || params.pflag) {
		int depth = MAX_LEVEL;	/* how deep to print */
		static char root[2] = "/";

		/*
		 * list is an alias for print, but limited to 1 level
		 */
		if (params.lflag)
			depth = 1;

		/*
		 * Get the starting path.  The root node is an oddball,
		 * the offset is zero and has no name.
		 */
		if (params.path == NULL)
			params.path = root;

		retval = fdt_print(working_fdt, params.path, params.propname, depth);
	} else if (params.iflag) {
		if (retval) {
			void *hdr = working_fdt;

			retval = 0;
			puts("   Legacy image found\n");

			if (!image_check_hcrc(hdr)) {
				puts("   Bad Header Checksum\n");
				retval = 1;
			} else {
				image_print_contents(hdr);

				puts("   Verifying Checksum ... ");
				if (!image_check_dcrc(hdr)) {
					puts("   Bad Data CRC\n");
					retval = 1;
				}
			}
		} else {
			puts("   FIT image found\n");

			if (!fit_check_format(working_fdt)) {
				puts("Bad FIT image format!\n");
				retval = 1;
			} else {
				fit_print_contents(working_fdt);

				if (!fit_all_image_verify(working_fdt)) {
					puts("Bad hash in FIT image!\n");
					retval = 1;
				}
			}
		}
	} else {
	/*
		int depth = 0;
		int offset;
		int imagePathOffset = fdt_path_offset(working_fdt, "/images");
		if (imagePathOffset < 0) {
			fprintf (stderr, "%s: Invalid FIT blob %s\n",
				params.cmdname, params.imagefile);
			exit (EXIT_FAILURE);
		}
		offset = fdt_next_node(working_fdt, imagePathOffset, &depth);
		while ((offset >= 0) && (depth > 0))
		{
			if (depth == 1)
			{
				const char* imageName = fdt_get_name(working_fdt, offset, NULL);
				int imageLength = 0;
				const unsigned char* imageData = (const uint8_t*)fdt_getprop(working_fdt, offset, "data", &imageLength);
				printf ("Name: %s  Offset: 0x%08X  Size: 0x%08X\n", imageName, imageData - working_fdt, imageLength);
			}
			offset = fdt_next_node(working_fdt, offset, &depth);
		}
	*/
	}

error:
	if (mmap_failed) {
		free(working_fdt);
	} else {
		munmap((void *)working_fdt, file_size);
	}
	close (ifd);

	exit (retval);
}


static void usage ()
{
	fprintf (stderr, "       %s [-l [path [propname]] | -o path propname | -p [path [propname]] | -i] image\n"
			 "          -l ==> Print one level starting at <path>\n"
			 "          -o ==> print the offset and size of a property\n"
			 "          -p ==> Recursive print starting at <path>\n"
			 "          -i ==> Print Image Info and validate hashes in u-boot image file\n",
		params.cmdname);

	exit (EXIT_FAILURE);
}
