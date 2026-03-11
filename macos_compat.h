#ifndef MACOS_COMPAT_H
#define MACOS_COMPAT_H

#ifdef __APPLE__

#include <sys/paths.h>
#include <sys/attr.h>
#include <sys/xattr.h>
#include <machine/endian.h>

#else

/* _PATH_RSRCFORKSPEC: macOS path to access resource forks via the filesystem */
#ifndef _PATH_RSRCFORKSPEC
#define _PATH_RSRCFORKSPEC "/..namedfork/rsrc"
#endif

/* macOS xattr constants */
#ifndef XATTR_FINDERINFO_NAME
#define XATTR_FINDERINFO_NAME "com.apple.FinderInfo"
#endif

#ifndef XATTR_RESOURCEFORK_NAME
#define XATTR_RESOURCEFORK_NAME "com.apple.ResourceFork"
#endif

/* Endianness: use <endian.h> on Linux */
#include <endian.h>
#ifndef BYTE_ORDER
#define BYTE_ORDER __BYTE_ORDER
#endif
#ifndef BIG_ENDIAN
#define BIG_ENDIAN __BIG_ENDIAN
#endif
#ifndef LITTLE_ENDIAN
#define LITTLE_ENDIAN __LITTLE_ENDIAN
#endif

/* st_birthtime does not exist on Linux; fall back to st_mtime */
#ifndef st_birthtime
#define st_birthtime st_mtime
#endif

/*
 * macOS getxattr/setxattr/fgetxattr have extra position and options args.
 * Provide inline wrappers that accept the macOS 6-arg signature on Linux.
 */
#include <sys/types.h>
#include <sys/xattr.h>
#include <errno.h>

static inline ssize_t getxattr_compat(const char *path, const char *name,
                                       void *value, size_t size,
                                       uint32_t /*position*/, int /*options*/)
{
    return ::getxattr(path, name, value, size);
}

static inline ssize_t fgetxattr_compat(int fd, const char *name,
                                        void *value, size_t size,
                                        uint32_t /*position*/, int /*options*/)
{
    return ::fgetxattr(fd, name, value, size);
}

static inline int setxattr_compat(const char *path, const char *name,
                                   const void *value, size_t size,
                                   uint32_t /*position*/, int /*options*/)
{
    return ::setxattr(path, name, value, size, 0);
}

/* Override the system calls with the compat wrappers */
#define getxattr getxattr_compat
#define fgetxattr fgetxattr_compat
#define setxattr setxattr_compat

/*
 * setattrlist() does not exist on Linux.
 * Provide a stub that returns -1 with ENOTSUP so callers fall through
 * to their utimes() fallback.
 */
struct attrlist {
    uint16_t bitmapcount;
    uint16_t reserved;
    uint32_t commonattr;
    uint32_t volattr;
    uint32_t dirattr;
    uint32_t fileattr;
    uint32_t forkattr;
};

#define ATTR_BIT_MAP_COUNT 5
#define ATTR_CMN_CRTIME    0x00000200
#define ATTR_CMN_MODTIME   0x00000400
#define ATTR_CMN_BKUPTIME  0x00020000

static inline int setattrlist(const char * /*path*/, struct attrlist * /*attrlist*/,
                              void * /*attrBuf*/, size_t /*attrBufSize*/,
                              unsigned long /*options*/)
{
    errno = ENOTSUP;
    return -1;
}

#endif /* __APPLE__ */

#endif /* MACOS_COMPAT_H */
