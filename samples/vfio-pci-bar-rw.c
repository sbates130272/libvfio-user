/*
 * Copyright (c) 2026, Nutanix Inc. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

/*
 * Minimal userspace VFIO sample that opens a real vfio-pci device, maps one
 * BAR region, reads a 32-bit register, optionally writes a 32-bit value, and
 * then reads it back.
 *
 * Usage:
 *   vfio-pci-bar-rw <iommu-group> <bdf> <bar-index> <bar-offset>
 *                  [--write <value-hex>]
 *
 * Example:
 *   sudo build/samples/vfio-pci-bar-rw 17 0000:65:00.0 0 0x0
 *   sudo build/samples/vfio-pci-bar-rw 17 0000:65:00.0 0 0x20 --write 0x1
 *
 * Notes:
 * - Requires a host with IOMMU enabled and the device bound to vfio-pci.
 * - Writes can change device state. Only write to documented safe registers.
 */

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/limits.h>
#include <linux/vfio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

typedef struct vfio_handles {
    int container;
    int group;
    int device;
    void *bar_map;
    size_t bar_map_len;
} vfio_handles_t;

static void
cleanup(vfio_handles_t *h)
{
    if (h->bar_map != NULL && h->bar_map != MAP_FAILED) {
        munmap(h->bar_map, h->bar_map_len);
        h->bar_map = NULL;
    }
    if (h->device >= 0) {
        close(h->device);
        h->device = -1;
    }
    if (h->group >= 0) {
        close(h->group);
        h->group = -1;
    }
    if (h->container >= 0) {
        close(h->container);
        h->container = -1;
    }
}

static int
parse_u32(const char *s, uint32_t *out)
{
    char *end = NULL;
    unsigned long v;

    if (s == NULL || out == NULL) {
        return -1;
    }

    errno = 0;
    v = strtoul(s, &end, 0);
    if (errno != 0 || end == s || *end != '\0' || v > UINT32_MAX) {
        return -1;
    }

    *out = (uint32_t)v;
    return 0;
}

static int
parse_u64(const char *s, uint64_t *out)
{
    char *end = NULL;
    unsigned long long v;

    if (s == NULL || out == NULL) {
        return -1;
    }

    errno = 0;
    v = strtoull(s, &end, 0);
    if (errno != 0 || end == s || *end != '\0') {
        return -1;
    }

    *out = (uint64_t)v;
    return 0;
}

static void
usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s <iommu-group> <bdf> <bar-index> <bar-offset> "
            "[--write <value-hex>]\n",
            prog);
}

static int
open_vfio(const char *group_id, const char *bdf, vfio_handles_t *h)
{
    struct vfio_group_status status = { .argsz = sizeof(status) };
    char group_path[PATH_MAX];
    int ret;

    h->container = open("/dev/vfio/vfio", O_RDWR);
    if (h->container < 0) {
        return -1;
    }

    ret = snprintf(group_path, sizeof(group_path), "/dev/vfio/%s", group_id);
    if (ret < 0 || (size_t)ret >= sizeof(group_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    h->group = open(group_path, O_RDWR);
    if (h->group < 0) {
        return -1;
    }

    ret = ioctl(h->group, VFIO_GROUP_GET_STATUS, &status);
    if (ret < 0) {
        return -1;
    }
    if ((status.flags & VFIO_GROUP_FLAGS_VIABLE) == 0) {
        errno = EBUSY;
        return -1;
    }

    ret = ioctl(h->group, VFIO_GROUP_SET_CONTAINER, &h->container);
    if (ret < 0) {
        return -1;
    }

    ret = ioctl(h->container, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU);
    if (ret < 0) {
        return -1;
    }

    h->device = ioctl(h->group, VFIO_GROUP_GET_DEVICE_FD, bdf);
    if (h->device < 0) {
        return -1;
    }

    return 0;
}

static int
map_bar(vfio_handles_t *h, uint32_t bar_index,
        uint64_t *region_offset, uint64_t *region_size)
{
    struct vfio_region_info region = {
        .argsz = sizeof(region),
        .index = bar_index
    };

    int ret = ioctl(h->device, VFIO_DEVICE_GET_REGION_INFO, &region);
    if (ret < 0) {
        return -1;
    }

    if ((region.flags & VFIO_REGION_INFO_FLAG_MMAP) == 0) {
        errno = ENOTSUP;
        return -1;
    }

    h->bar_map_len = region.size;
    h->bar_map = mmap(NULL, h->bar_map_len, PROT_READ | PROT_WRITE, MAP_SHARED,
                      h->device, region.offset);
    if (h->bar_map == MAP_FAILED) {
        h->bar_map = NULL;
        return -1;
    }

    *region_offset = region.offset;
    *region_size = region.size;
    return 0;
}

int
main(int argc, char **argv)
{
    vfio_handles_t h = { .container = -1, .group = -1, .device = -1 };
    uint32_t bar_index;
    uint64_t bar_offset;
    uint32_t write_value = 0;
    bool do_write = false;
    uint64_t region_offset = 0;
    uint64_t region_size = 0;
    volatile uint32_t *reg;
    uint32_t before;
    uint32_t after;

    if (argc != 5 && argc != 7) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    if (argc == 7) {
        if (strcmp(argv[5], "--write") != 0) {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
        if (parse_u32(argv[6], &write_value) < 0) {
            errx(EXIT_FAILURE, "invalid --write value: %s", argv[6]);
        }
        do_write = true;
    }
    if (parse_u32(argv[3], &bar_index) < 0) {
        errx(EXIT_FAILURE, "invalid bar-index: %s", argv[3]);
    }
    if (parse_u64(argv[4], &bar_offset) < 0) {
        errx(EXIT_FAILURE, "invalid bar-offset: %s", argv[4]);
    }

    if (open_vfio(argv[1], argv[2], &h) < 0) {
        cleanup(&h);
        err(EXIT_FAILURE, "failed to open/configure vfio");
    }

    if (map_bar(&h, bar_index, &region_offset, &region_size) < 0) {
        cleanup(&h);
        err(EXIT_FAILURE, "failed to map BAR %u", bar_index);
    }

    if (bar_offset + sizeof(uint32_t) > region_size) {
        cleanup(&h);
        errx(EXIT_FAILURE, "offset %#" PRIx64 " is outside BAR size %#" PRIx64,
             bar_offset, region_size);
    }

    reg = (volatile uint32_t *)((uint8_t *)h.bar_map + bar_offset);
    before = *reg;
    printf("bdf=%s bar=%u offset=%#" PRIx64 " value(before)=0x%08" PRIx32 "\n",
           argv[2], bar_index, bar_offset, before);
    printf("region-offset=%#" PRIx64 " region-size=%#" PRIx64 "\n",
           region_offset, region_size);

    if (do_write) {
        *reg = write_value;
        after = *reg;
        printf("wrote=0x%08" PRIx32 " value(after)=0x%08" PRIx32 "\n",
               write_value, after);
    }

    cleanup(&h);
    return EXIT_SUCCESS;
}

/* ex: set tabstop=4 shiftwidth=4 softtabstop=4 expandtab: */
