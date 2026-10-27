#ifndef BOAROS_KERNEL_XATTR_H
#define BOAROS_KERNEL_XATTR_H
#include <stddef.h>
#include <stdint.h>
enum kernel_xattr_operation { KERNEL_XATTR_SET, KERNEL_XATTR_GET, KERNEL_XATTR_LIST, KERNEL_XATTR_REMOVE };
#define KERNEL_XATTR_CREATE 1U
#define KERNEL_XATTR_REPLACE 2U
#define KERNEL_XATTR_NAME_MAX 255U
#define KERNEL_XATTR_SIZE_MAX 65536U
/* Internal list visibility, computed from the actual caller's credentials. */
#define KERNEL_XATTR_LIST_TRUSTED 1U
#endif
