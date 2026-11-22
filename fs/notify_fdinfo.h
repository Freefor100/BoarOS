#ifndef BOAROS_FS_NOTIFY_FDINFO_H
#define BOAROS_FS_NOTIFY_FDINFO_H

#include <kernel/vfs.h>
#include <string.h>

static inline char *notify_fdinfo_field(char *out, const char *label, uint64_t value)
{
    static const char digits[] = "0123456789abcdef";
    size_t length = strlen(label);
    memcpy(out, label, length);
    out += length;
    unsigned shift = 60;
    while (shift && !(value >> shift)) shift -= 4;
    do {
        *out++ = digits[(value >> shift) & 15U];
        if (!shift) break;
        shift -= 4;
    } while (1);
    return out;
}

static inline uint64_t notify_fdinfo_device(uint64_t device)
{
    /* VFS stores the userspace dev_t encoding; fdinfo uses the kernel dev_t. */
    uint64_t major = (device >> 8) & 0xfffU;
    uint64_t minor = (device & 0xffU) | ((device >> 12) & 0xfff00U);
    return (major << 20) | minor;
}

static inline char *notify_fdinfo_handle(char *out, const struct kernel_vfs_handle *handle,
    unsigned type)
{
    static const char label[] = " f_handle:";
    static const char digits[] = "0123456789abcdef";
    out = notify_fdinfo_field(out, " fhandle-bytes:", sizeof(*handle));
    out = notify_fdinfo_field(out, " fhandle-type:", type);
    memcpy(out, label, sizeof(label) - 1);
    out += sizeof(label) - 1;
    const unsigned char *bytes = (const unsigned char *)handle;
    for (size_t i = 0; i < sizeof(*handle); ++i) {
        *out++ = digits[bytes[i] >> 4];
        *out++ = digits[bytes[i] & 15U];
    }
    return out;
}

#endif
