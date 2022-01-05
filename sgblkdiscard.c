#include <stdio.h>
#include <err.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <locale.h>
#include <ctype.h>
#include <getopt.h>
#include <inttypes.h>
#include <scsi/sg.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#ifdef HAVE_LIBBLKID
#include <blkid/blkid.h>
#endif

#include "sgblkdiscard_config.h"
#include "utils.h"

static void print_stats(char *path, uint64_t trim_start_offset, uint64_t trimmed_bytes)
{
    printf("%s: Discarded %" PRIu64 " bytes from the offset %" PRIu64 "\n",
           path, trimmed_bytes, trim_start_offset);
}

static void usage(const char *program_name)
{
    FILE *out = stdout;
    fputs(USAGE_HEADER, out);
    fprintf(out, " %s [options] <device>\n", program_name);

    fputs(USAGE_SEPARATOR, out);
    fputs("Discard the content of sectors on a device.\n", out);

    fputs(USAGE_OPTIONS, out);
    fputs(" -f, --force         disable all checking\n", out);
    fputs(" -i, --interactive   interactive mode\n", out);
    fputs(" -o, --offset <num>  offset in bytes to discard from\n", out);
    fputs(" -l, --length <num>  length of bytes to discard from the offset\n", out);
    fputs(" -p, --step <num>    size of the discard iterations within the offset\n", out);
    fputs(" -v, --verbose       print aligned length and offset\n", out);

    fputs(USAGE_SEPARATOR, out);
    printf(USAGE_HELP_OPTIONS(21));

    fputs(USAGE_ARGUMENTS, out);
    printf(USAGE_ARG_SIZE("<num>"));

    exit(EXIT_SUCCESS);
}

#ifdef HAVE_LIBBLKID
/*
 * Check existing signature on the open fd
 * Returns	0  signature found
 * 		1  no signature
 * 		<0 error
 */
static int probe_device(int fd, char *path)
{
    const char *type;
    blkid_probe pr = NULL;
    int ret = -1;

    pr = blkid_new_probe();
    if (!pr || blkid_probe_set_device(pr, fd, 0, 0))
        return ret;

    blkid_probe_enable_superblocks(pr, true);
    blkid_probe_enable_partitions(pr, true);

    ret = blkid_do_fullprobe(pr);
    if (ret)
        goto out;

    if (!blkid_probe_lookup_value(pr, "TYPE", &type, NULL))
    {
        warnx("%s contains existing file system (%s).", path, type);
    }
    else if (!blkid_probe_lookup_value(pr, "PTTYPE", &type, NULL))
    {
        warnx("%s contains existing partition (%s).", path, type);
    }
    else
    {
        warnx("%s contains existing signature.", path);
    }

out:
    blkid_free_probe(pr);
    return ret;
}
#endif /* HAVE_LIBBLKID */

int main(int argc, char **argv)
{
    const char *program_name = argv[0];

    static const struct option longopts[] = {
        {"help", no_argument, NULL, 'h'},
        {"version", no_argument, NULL, 'V'},
        {"offset", required_argument, NULL, 'o'},
        {"force", no_argument, NULL, 'f'},
        {"length", required_argument, NULL, 'l'},
        {"step", required_argument, NULL, 'p'},
        {"verbose", no_argument, NULL, 'v'},
        {"interactive", no_argument, NULL, 'i'},
        {NULL, 0, NULL, 0}};

    setlocale(LC_ALL, "");

    bool force = false;
    bool verbose = false;
    bool interactive = false;
    uint64_t offset = 0;
    uint64_t length = UINT64_MAX;
    uint64_t step = 0;
    int c;
    while ((c = getopt_long(argc, argv, "hfVvio:l:p:", longopts, NULL)) != -1)
    {
        switch (c)
        {
        case 'f':
            force = true;
            break;
        case 'l':
            length = strtosize_or_err(optarg, "failed to parse length");
            break;
        case 'o':
            offset = strtosize_or_err(optarg, "failed to parse offset");
            break;
        case 'p':
            step = strtosize_or_err(optarg, "failed to parse step");
            break;
        case 'v':
            verbose = true;
            break;
        case 'h':
            usage(program_name);
            break;
        case 'i':
            interactive = true;
            break;
        case 'V':
            printf("%s version %d.%d", PROJECT_NAME, PROJECT_VERSION_MAJOR, PROJECT_VERSION_MINOR);
            exit(EXIT_SUCCESS);
            break;
        default:
            errtryhelp(program_name, EXIT_FAILURE);
        }
    }

    if (force)
    {
        interactive = false;
    }

    if (optind == argc)
        errx(EXIT_FAILURE, "no device specified");

    char *path = argv[optind++];

    if (optind != argc)
    {
        warnx("unexpected number of arguments");
        errtryhelp(program_name, EXIT_FAILURE);
    }

    int fd = open(path, O_RDWR | (force ? 0 : O_EXCL));
    if (fd < 0)
    {
        err(EXIT_FAILURE, "cannot open %s", path);
    }

    struct stat sb;
    if (fstat(fd, &sb) == -1)
    {
        err(EXIT_FAILURE, "stat of %s failed", path);
    }
    if (!S_ISBLK(sb.st_mode))
    {
        errx(EXIT_FAILURE, "%s: not a block device", path);
    }

    device_info_t info = {0};
    if (sg_get_device_info(fd, &info))
    {
        err(EXIT_FAILURE, "%s: failed to get device info", path);
    }

    if (!info.support_unmap)
    {
        errx(EXIT_FAILURE, "%s: not support unmap", path);
    }

    /* check offset alignment to the sector size */
    if (offset % info.sector_size)
    {
        errx(EXIT_FAILURE, "%s: offset %" PRIu64 " is not aligned "
                           "to sector size %i",
             path, offset, info.sector_size);
    }

    /* is the range end behind the end of the device ?*/
    if (offset > info.device_size)
    {
        errx(EXIT_FAILURE, "%s: offset is greater than device size", path);
    }
    uint64_t end_offset = offset + length;
    if (end_offset < offset || end_offset > info.device_size)
    {
        end_offset = info.device_size;
    }

    length = (step > 0) ? step : end_offset - offset;

    /* check length alignment to the sector size */
    if (length % info.sector_size)
    {
        errx(EXIT_FAILURE, "%s: length %" PRIu64 " is not aligned "
                           "to sector size %i",
             path, length, info.sector_size);
    }

    char last_char = path[strlen(path) - 1];
    if (isdigit(last_char))
    {
        if (interactive)
        {
            if (!ask_for_yn("Operation is applied to disk instead of partition. Continue?"))
            {
                exit(EXIT_FAILURE);
            }
        }
        else
        {
            errx(EXIT_FAILURE,
                 "Operation is applied to disk instead of partition. "
                 "Use the -f option to override.");
        }
    }

#ifdef HAVE_LIBBLKID
    if (force)
    {
        warnx("Operation forced, data will be lost!");
    }
    else
    {
        /* Check for existing signatures on the device */
        switch (probe_device(fd, path))
        {
        case 0: /* signature detected */
            if (interactive)
            {
                if (!ask_for_yn("This is destructive operation, data will be lost! Continue?"))
                {
                    exit(EXIT_FAILURE);
                }
            }
            else
            {
                errx(EXIT_FAILURE,
                     "This is destructive operation, data will "
                     "be lost! Use the -f option to override.");
            }

            break;
        case 1: /* no signature */
            break;
        default: /* error */
            err(EXIT_FAILURE, "Failed to probe the device.");
            break;
        }
    }
#endif /* HAVE_LIBBLKID */

    uint64_t trim_start_offset = offset;
    uint64_t trimmed_bytes = 0;

    struct timeval now = {0}, last = {0};
    gettime_monotonic(&last);

    for (/* nothing */; offset < end_offset; offset += length)
    {
        if (offset + length > end_offset)
        {
            length = end_offset - offset;
        }

        if (sg_unmap(fd, &info, offset, length))
        {
            err(EXIT_FAILURE, "%s: unmap failed", path);
        }

        trimmed_bytes += length;

        /* reporting progress at most once per second */
        if (verbose && step)
        {
            gettime_monotonic(&now);
            if (now.tv_sec > last.tv_sec &&
                (now.tv_usec >= last.tv_usec || now.tv_sec - last.tv_sec > 1))
            {
                print_stats(path, trim_start_offset, trimmed_bytes);
                trim_start_offset += trimmed_bytes;
                trimmed_bytes = 0;
                last = now;
            }
        }
    }

    if (verbose && trimmed_bytes)
    {
        print_stats(path, trim_start_offset, trimmed_bytes);
    }

    close(fd);
    return EXIT_SUCCESS;
}