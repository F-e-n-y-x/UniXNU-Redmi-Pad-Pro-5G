/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* asteroids: WiFi firmware files an owner installed on the Data volume, below /private/var/firmware/ath11k/WCN6750/hw1.0 (ath11k's own
 * directory name), read through VFS on the WiFi worker. Built with kern_exec's BSD flags, as AdspFirmware.c. sheng's WifiFirmware.c
 * reader with this board's directory and without its file-name list.
 * ruan (macOS/docs/research/wifi/port): unchanged but for join.txt below; ruan has no Data add-on (native_gpu_addon_stage), so the
 * file is read from the root (/private/etc/wifi/join.txt) or the Data volume (/private/var/wifi-test/join.txt). */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/vnode.h>
#include <sys/uio.h>
#include <sys/fcntl.h>
#include <sys/kauth.h>
#include <IOKit/IOLib.h>

#define ASTEROIDS_WIFI_FIRMWARE_ROOT "/private/var/firmware/ath11k/WCN6750/hw1.0"

int asteroids_wifi_vfs_read(const char *name, uint8_t **data, size_t *size);

int asteroids_wifi_vfs_read(const char *name, uint8_t **data, size_t *size)
{
    *data = NULL; *size = 0;
    if (!name[0] || strchr(name, '/')) return EINVAL;
    char path[160];
    snprintf(path, sizeof path, "%s/%s", ASTEROIDS_WIFI_FIRMWARE_ROOT, name);
    vfs_context_t ctx = vfs_context_kernel();
    vnode_t vp = NULL;
    int error = vnode_open(path, FREAD, 0, 0, &vp, ctx);
    uint8_t *buffer = NULL;
    size_t length = 0;
    if (!error) {
        struct vnode_attr va;
        VATTR_INIT(&va);
        VATTR_WANTED(&va, va_data_size);
        error = vnode_getattr(vp, &va, ctx);
        if (!error && (vnode_vtype(vp) != VREG || !va.va_data_size || va.va_data_size > 16u * 1024 * 1024)) error = EINVAL;
        if (!error) {
            length = (size_t)va.va_data_size;
            buffer = IOMallocData(length);
            if (!buffer) error = ENOMEM;
            else {
                int resid = 0;
                error = vn_rdwr(UIO_READ, vp, (caddr_t)buffer, (int)length, 0, UIO_SYSSPACE, IO_NOAUTH, vfs_context_ucred(ctx), &resid,
                                vfs_context_proc(ctx));
                if (!error && resid) error = EIO;
            }
        }
    }
    if (vp) vnode_close(vp, FREAD, ctx);
    if (error) {if (buffer) IOFreeData(buffer, length); return error;}
    *data = buffer; *size = length;
    return 0;
}

/* The unattended join test's file (RuanWifi.cpp joinStep, WifiNet's auto-join). ruan: on Data, mounted before launchd (ram-data=1 or
 * the UFS variant), so by the time the radio is up a missing file is really missing. Never in git or a release: it holds a network key. */
int asteroids_wifi_join_read(uint8_t **data, size_t *size);
static int read_path(const char *path, uint8_t **data, size_t *size)
{
    *data = NULL; *size = 0;
    vfs_context_t ctx = vfs_context_kernel();
    vnode_t vp = NULL;
    int error = vnode_open(path, FREAD, 0, 0, &vp, ctx);
    uint8_t *buffer = NULL;
    size_t length = 0;
    if (!error) {
        struct vnode_attr va;
        VATTR_INIT(&va);
        VATTR_WANTED(&va, va_data_size);
        error = vnode_getattr(vp, &va, ctx);
        if (!error && (vnode_vtype(vp) != VREG || !va.va_data_size || va.va_data_size > 4096)) error = EINVAL;
        if (!error) {
            length = (size_t)va.va_data_size;
            buffer = IOMallocData(length);
            int resid = 0;
            error = buffer ? vn_rdwr(UIO_READ, vp, (caddr_t)buffer, (int)length, 0, UIO_SYSSPACE, IO_NOAUTH, vfs_context_ucred(ctx), &resid,
                                     vfs_context_proc(ctx)) : ENOMEM;
            if (!error && resid) error = EIO;
        }
    }
    if (vp) vnode_close(vp, FREAD, ctx);
    if (error) {if (buffer) IOFreeData(buffer, length); return error;}
    *data = buffer; *size = length;
    return 0;
}
int asteroids_wifi_join_read(uint8_t **data, size_t *size)
{
    /* ruan: the root's copy first (build-wifi-userspace.py --join, 0600; the flashed root image, never git), then Data's */
    const int error = read_path("/private/etc/wifi/join.txt", data, size);
    return error == ENOENT ? read_path("/private/var/wifi-test/join.txt", data, size) : error;
}

/* ruan: the IPv4 default route, for WifiNet.cpp's USB `net` report (struct rtentry is visible to the BSD flags this file is built with).
 * The interface pointer is only compared and named, never kept. */
#include <net/route.h>
#include <netinet/in.h>
int ruan_wifi_default_route(ifnet_t *ifp, uint32_t *gateway, uint32_t *flags);
int ruan_wifi_default_route(ifnet_t *ifp, uint32_t *gateway, uint32_t *flags)
{
    struct sockaddr_in any;
    memset(&any, 0, sizeof any);
    any.sin_len = sizeof any;
    any.sin_family = AF_INET;
    struct rtentry *rt = rtalloc1((struct sockaddr *)&any, 0, 0);
    if (!rt) return -1;
    RT_LOCK(rt);
    *ifp = rt->rt_ifp;
    *flags = rt->rt_flags;
    *gateway = rt->rt_gateway && rt->rt_gateway->sa_family == AF_INET ? ((struct sockaddr_in *)(void *)rt->rt_gateway)->sin_addr.s_addr : 0;
    RT_UNLOCK(rt);
    rtfree(rt);
    return 0;
}
