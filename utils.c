#include <stdint.h>
#include <string.h>
#include <err.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <ctype.h>
#include <locale.h>
#include <math.h>

#include <scsi/sg.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <time.h>
#include <inttypes.h>

#include "utils.h"

static int do_scale_by_power(uint64_t *x, int base, int power)
{
    while (power--)
    {
        if (UINT64_MAX / base < *x)
            return -ERANGE;
        *x *= base;
    }
    return 0;
}

static int parse_size(const char *str, uint64_t *result, int *power_index)
{
    const char *p;
    char *end;
    uint64_t x, frac = 0;
    int base = 1024, rc = 0, pwr = 0, frac_zeros = 0;

    static const char *suf = "KMGTPEZY";
    static const char *suf2 = "kmgtpezy";
    const char *sp;

    *result = 0;

    if (!str || !*str)
    {
        rc = -EINVAL;
        goto err;
    }

    /* Only positive numbers are acceptable
	 *
	 * Note that this check is not perfect, it would be better to
	 * use lconv->negative_sign. But coreutils use the same solution,
	 * so it's probably good enough...
	 */
    p = str;
    while (isspace((unsigned char)*p))
        p++;
    if (*p == '-')
    {
        rc = -EINVAL;
        goto err;
    }

    errno = 0, end = NULL;
    x = strtoull(str, &end, 0);

    if (end == str ||
        (errno != 0 && (x == UINT64_MAX || x == 0)))
    {
        rc = errno ? -errno : -EINVAL;
        goto err;
    }
    if (!end || !*end)
        goto done; /* without suffix */
    p = end;

    /*
	 * Check size suffixes
	 */
check_suffix:
    if (*(p + 1) == 'i' && (*(p + 2) == 'B' || *(p + 2) == 'b') && !*(p + 3))
        base = 1024; /* XiB, 2^N */
    else if ((*(p + 1) == 'B' || *(p + 1) == 'b') && !*(p + 2))
        base = 1000; /* XB, 10^N */
    else if (*(p + 1))
    {
        struct lconv const *l = localeconv();
        const char *dp = l ? l->decimal_point : NULL;
        size_t dpsz = dp ? strlen(dp) : 0;

        if (frac == 0 && *p && dp && strncmp(dp, p, dpsz) == 0)
        {
            const char *fstr = p + dpsz;

            for (p = fstr; *p == '0'; p++)
                frac_zeros++;
            fstr = p;
            if (isdigit(*fstr))
            {
                errno = 0, end = NULL;
                frac = strtoull(fstr, &end, 0);
                if (end == fstr ||
                    (errno != 0 && (frac == UINT64_MAX || frac == 0)))
                {
                    rc = errno ? -errno : -EINVAL;
                    goto err;
                }
            }
            else
                end = (char *)p;

            if (frac && (!end || !*end))
            {
                rc = -EINVAL;
                goto err; /* without suffix, but with frac */
            }
            p = end;
            goto check_suffix;
        }
        rc = -EINVAL;
        goto err; /* unexpected suffix */
    }

    sp = strchr(suf, *p);
    if (sp)
        pwr = (sp - suf) + 1;
    else
    {
        sp = strchr(suf2, *p);
        if (sp)
            pwr = (sp - suf2) + 1;
        else
        {
            rc = -EINVAL;
            goto err;
        }
    }

    rc = do_scale_by_power(&x, base, pwr);
    if (power_index)
        *power_index = pwr;
    if (frac && pwr)
    {
        int i;
        uint64_t frac_div = 10, frac_poz = 1, frac_base = 1;

        /* mega, giga, ... */
        do_scale_by_power(&frac_base, base, pwr);

        /* maximal divisor for last digit (e.g. for 0.05 is
		 * frac_div=100, for 0.054 is frac_div=1000, etc.)
		 *
		 * Reduce frac if too large.
		 */
        while (frac_div < frac)
        {
            if (frac_div <= UINT64_MAX / 10)
                frac_div *= 10;
            else
                frac /= 10;
        }

        /* 'frac' is without zeros (5 means 0.5 as well as 0.05) */
        for (i = 0; i < frac_zeros; i++)
        {
            if (frac_div <= UINT64_MAX / 10)
                frac_div *= 10;
            else
                frac /= 10;
        }

        /*
		 * Go backwardly from last digit and add to result what the
		 * digit represents in the frac_base. For example 0.25G
		 *
		 *  5 means 1GiB / (100/5)
		 *  2 means 1GiB / (10/2)
		 */
        do
        {
            unsigned int seg = frac % 10;           /* last digit of the frac */
            uint64_t seg_div = frac_div / frac_poz; /* what represents the segment 1000, 100, .. */

            frac /= 10; /* remove last digit from frac */
            frac_poz *= 10;

            if (seg && seg_div / seg)
                x += frac_base / (seg_div / seg);
        } while (frac);
    }
done:
    *result = x;
err:
    if (rc < 0)
        errno = -rc;
    return rc;
}

static inline void u16_to_big_endian_bytes(uint16_t val, void *p)
{
    ((uint8_t *)p)[0] = (uint8_t)(val >> 8);
    ((uint8_t *)p)[1] = (uint8_t)val;
}

static inline void u32_to_big_endian_bytes(uint32_t val, void *p)
{
    u16_to_big_endian_bytes(val >> 16, p);
    u16_to_big_endian_bytes(val, (uint8_t *)p + 2);
}

static inline void u64_to_big_endian_bytes(uint64_t val, void *p)
{
    u32_to_big_endian_bytes(val >> 32, p);
    u32_to_big_endian_bytes(val, (uint8_t *)p + 4);
}

static inline uint16_t u16_from_big_endian_bytes(const void *p)
{
    return ((const uint8_t *)p)[0] << 8 | ((const uint8_t *)p)[1];
}

static inline uint32_t u32_from_big_endian_bytes(const void *p)
{
    return (uint32_t)u16_from_big_endian_bytes(p) << 16 |
           u16_from_big_endian_bytes((const uint8_t *)p + 2);
}

static inline uint64_t u64_from_big_endian_bytes(const void *p)
{
    return (uint64_t)u32_from_big_endian_bytes(p) << 32 |
           u32_from_big_endian_bytes((const uint8_t *)p + 4);
}

#define SG_TIMEOUT 60000
#define SG_INQUIRY_CMD 0x12
#define SG_INQUIRY_CMD_LEN 6
#define SG_READ_CAPACITY16_CMD 0x9e
#define SG_READ_CAPACITY16_CMD_LEN 16
#define SG_READ_CAPACITY16_SERVICE_ACTION 0x10
#define SG_READ_CAPACITY16_REPLY_LEN 32
#define SG_BLOCK_LIMITS_VPD_PAGE_CODE 0xb0
#define SG_BLOCK_LIMITS_VPD_PAGE_LEN 64
#define SG_UNMAP_CMD 0x42
#define SG_UNMAP_CMD_LEN 10
#define SG_UNMAP_PARAMETER_LEN (8 + 16) //length info + block descriptor

static int sg_read_capacity16(int fd, device_info_t *info)
{
    uint8_t sense_buffer[UINT8_MAX] = {0};
    sg_io_hdr_t io_hdr;
    uint8_t command[SG_READ_CAPACITY16_CMD_LEN] = {SG_READ_CAPACITY16_CMD, SG_READ_CAPACITY16_SERVICE_ACTION};
    uint8_t reply[SG_READ_CAPACITY16_REPLY_LEN] = {0};
    u32_to_big_endian_bytes(sizeof(reply), command + 10);
    memset(&io_hdr, 0, sizeof(io_hdr));
    io_hdr.interface_id = 'S';
    io_hdr.dxfer_direction = SG_DXFER_FROM_DEV;
    io_hdr.cmdp = command;
    io_hdr.cmd_len = SG_READ_CAPACITY16_CMD_LEN;
    io_hdr.dxferp = reply;
    io_hdr.dxfer_len = sizeof(reply);
    io_hdr.sbp = sense_buffer;
    io_hdr.mx_sb_len = sizeof(sense_buffer);
    io_hdr.timeout = SG_TIMEOUT;

    int ret = ioctl(fd, SG_IO, &io_hdr);
    if (ret)
    {
        return ret;
    }

    info->last_block_address = u64_from_big_endian_bytes(reply);
    info->sector_size = u32_from_big_endian_bytes(reply + 8);
    info->device_size = (info->last_block_address + 1) * info->sector_size;

    return ret;
}

static int sg_inquiry_limits_vdp(int fd, device_info_t *info)
{
    uint8_t sense_buffer[UINT8_MAX] = {0};
    sg_io_hdr_t io_hdr;
    uint8_t command[SG_INQUIRY_CMD_LEN] = {SG_INQUIRY_CMD, 1, SG_BLOCK_LIMITS_VPD_PAGE_CODE};
    uint8_t reply[SG_BLOCK_LIMITS_VPD_PAGE_LEN] = {0};
    u16_to_big_endian_bytes(sizeof(reply), command + 3);
    memset(&io_hdr, 0, sizeof(io_hdr));
    io_hdr.interface_id = 'S';
    io_hdr.dxfer_direction = SG_DXFER_FROM_DEV;
    io_hdr.cmdp = command;
    io_hdr.cmd_len = SG_INQUIRY_CMD_LEN;
    io_hdr.dxferp = reply;
    io_hdr.dxfer_len = sizeof(reply);
    io_hdr.sbp = sense_buffer;
    io_hdr.mx_sb_len = sizeof(sense_buffer);
    io_hdr.timeout = SG_TIMEOUT;

    int ret = ioctl(fd, SG_IO, &io_hdr);
    if (ret)
    {
        return ret;
    }

    info->maximum_transfer_length = u32_from_big_endian_bytes(reply + 8);
    info->optimal_transfer_length = u32_from_big_endian_bytes(reply + 12);
    info->maximum_unmap_lba_count = u32_from_big_endian_bytes(reply + 20);
    info->maximum_unmap_block_descriptor_count = u32_from_big_endian_bytes(reply + 24);
    info->optimal_unmap_granularity = u32_from_big_endian_bytes(reply + 28);
    info->support_unmap = info->maximum_unmap_lba_count != 0;

    return ret;
}

static int sg_unmap_scsi(int fd, uint64_t offset_lba, uint64_t length_lba)
{
    uint8_t sense_buffer[UINT8_MAX] = {0};
    sg_io_hdr_t io_hdr;
    uint8_t unmap_command[SG_UNMAP_CMD_LEN] = {SG_UNMAP_CMD};
    u16_to_big_endian_bytes(SG_UNMAP_PARAMETER_LEN, unmap_command + 7);
    uint8_t parameter[SG_UNMAP_PARAMETER_LEN] = {0};
    memset(&io_hdr, 0, sizeof(io_hdr));
    io_hdr.interface_id = 'S';
    io_hdr.dxfer_direction = SG_DXFER_TO_DEV;
    io_hdr.cmd_len = SG_UNMAP_CMD_LEN;
    io_hdr.mx_sb_len = sizeof(sense_buffer);
    io_hdr.iovec_count = 0;
    io_hdr.dxfer_len = sizeof(parameter);
    io_hdr.dxferp = parameter;
    io_hdr.cmdp = unmap_command;
    io_hdr.sbp = sense_buffer;
    io_hdr.timeout = SG_TIMEOUT;

    u16_to_big_endian_bytes(SG_UNMAP_PARAMETER_LEN - 2, parameter);
    u16_to_big_endian_bytes(SG_UNMAP_PARAMETER_LEN - 8, parameter + 2);
    u64_to_big_endian_bytes(offset_lba, parameter + 8);
    u32_to_big_endian_bytes(length_lba, parameter + 16);

    return ioctl(fd, SG_IO, &io_hdr);
}

uint64_t strtosize_or_err(const char *str, const char *errmesg)
{
    uint64_t num;

    if (parse_size(str, &num, NULL) == 0)
        return num;

    if (errno)
        err(EXIT_FAILURE, "%s: '%s'", errmesg, str);

    errx(EXIT_FAILURE, "%s: '%s'", errmesg, str);
}

int sg_get_device_info(int fd, device_info_t *info)
{
    if (info == NULL)
    {
        return -1;
    }

    int ret = 0;
    if ((ret = sg_read_capacity16(fd, info)) || (ret = sg_inquiry_limits_vdp(fd, info)))
    {
        return ret;
    }

    return ret;
}

int sg_unmap(int fd, const device_info_t *info, uint64_t offset, uint64_t length)
{
    uint64_t offset_lba = offset / info->sector_size;
    uint64_t length_lba = length / info->sector_size;
    uint32_t maximum_unmap_lba_count = info->maximum_unmap_lba_count;

    uint64_t block_descriptor_count = length_lba / maximum_unmap_lba_count;
    if (length_lba % maximum_unmap_lba_count > 0)
    {
        block_descriptor_count++;
    }

    for (uint64_t i = 0; i < block_descriptor_count; i++)
    {
        uint64_t current_offset = offset_lba;
        uint64_t current_length = 0;
        if (length_lba > maximum_unmap_lba_count)
        {
            current_length = maximum_unmap_lba_count;
            offset_lba += maximum_unmap_lba_count;
            length_lba -= maximum_unmap_lba_count;
        }
        else
        {
            current_length = length_lba;
        }

        int ret = sg_unmap_scsi(fd, current_offset, current_length);
        if (ret)
        {
            return ret;
        }
    }

    return 0;
}

void errtryhelp(const char *program_name, int exit_code)
{
    fprintf(stderr, "Try '%s --help' for more information.\n", program_name);
    exit(exit_code);
}

int gettime_monotonic(struct timeval *tv)
{
    /* Can slew only by ntp and adjtime */
    int ret;
    struct timespec ts;

    /* Linux specific, can't slew */
    if (!(ret = clock_gettime(CLOCK_MONOTONIC, &ts)))
    {
        tv->tv_sec = ts.tv_sec;
        tv->tv_usec = ts.tv_nsec / 1000;
    }
    return ret;
}