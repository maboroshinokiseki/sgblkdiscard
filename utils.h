#include <stdbool.h>
#include <stdint.h>

#define USAGE_HEADER "\nUsage:\n"
#define USAGE_OPTIONS "\nOptions:\n"
#define USAGE_ARGUMENTS "\nArguments:\n"
#define USAGE_SEPARATOR "\n"
#define USAGE_OPTSTR_HELP "display this help"
#define USAGE_OPTSTR_VERSION "display version"

#define USAGE_HELP_OPTIONS(marg_dsc)      \
    "%-" #marg_dsc "s%s\n"                \
    "%-" #marg_dsc "s%s\n",               \
        " -h, --help", USAGE_OPTSTR_HELP, \
        " -V, --version", USAGE_OPTSTR_VERSION

#define USAGE_ARG_SIZE(_name)                                         \
    " %s arguments may be followed by the suffixes for\n"             \
    "   GiB, TiB, PiB, EiB, ZiB, and YiB (the \"iB\" is optional)\n", \
        _name

typedef struct device_info
{
    uint64_t last_block_address;
    uint32_t sector_size;
    uint64_t device_size;
    uint32_t maximum_transfer_length;
    uint32_t optimal_transfer_length;
    uint32_t maximum_unmap_lba_count;
    uint32_t maximum_unmap_block_descriptor_count;
    uint32_t optimal_unmap_granularity;
    bool support_unmap;
} device_info_t;

/**
 * @brief convert string to size (uint64_t)
 *
 * Supported suffixes:
 *
 * XiB or X for 2^N
 *     where X = {K,M,G,T,P,E,Z,Y}
 *        or X = {k,m,g,t,p,e}  (undocumented for backward compatibility only)
 * for example:
 *		10KiB	= 10240
 *		10K	= 10240
 *
 * XB for 10^N
 *     where X = {K,M,G,T,P,E,Z,Y}
 * for example:
 *		10KB	= 10000
 *
 *
 * The function also supports decimal point, for example:
 *              0.5MB   = 500000
 *              0.5MiB  = 512000
 *
 * Note that the function does not accept numbers with '-' (negative sign)
 * prefix.
 *
 * 
 * @param str string for parsing
 * @param errmesg error message
 * @return result 
 */
uint64_t strtosize_or_err(const char *str, const char *errmesg);

/**
 * @brief get device info
 * 
 * @param fd file descriptor
 * @param info pointer to info
 * @return returns 0 if there is no error. 
 */
int sg_get_device_info(int fd, device_info_t *info);

/**
 * @brief unmap certain area of a device.
 * 
 * @param fd file descriptor.
 * @param info device info.
 * @param offset offset in byte.
 * @param length length in byte.
 * @return returns 0 if there is no error.
 */
int sg_unmap(int fd, const device_info_t *info, uint64_t offset, uint64_t length);

void errtryhelp(const char *program_name, int exit_code);

int gettime_monotonic(struct timeval *tv);

bool ask_for_yn(const char *message);