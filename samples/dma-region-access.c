/*
 * Copyright (c) 2026, Nutanix Inc. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common.h"
#include "dma.h"
#include "libvfio-user.h"

#define DMA_BACKEND_WINDOW_START ((uintptr_t)0x100000000ULL)
#define DMA_BACKEND_WINDOW_SIZE  (64 * 1024U)
#define MAX_DMA_BACKEND_REGIONS  8

typedef struct dma_backend_region {
    struct iovec iova;
    uint8_t *storage;
} dma_backend_region_t;

typedef struct server_state {
    uint32_t control_reg;
    dma_backend_region_t regions[MAX_DMA_BACKEND_REGIONS];
} server_state_t;

static void
log_cb(vfu_ctx_t *vfu_ctx UNUSED, int level UNUSED, const char *msg)
{
    fprintf(stderr, "dma-region-access[%d]: %s\n", getpid(), msg);
}

static bool
range_in_backend_window(vfu_dma_info_t *info)
{
    uintptr_t start = (uintptr_t)info->iova.iov_base;
    uintptr_t end;
    uintptr_t window_end = DMA_BACKEND_WINDOW_START + DMA_BACKEND_WINDOW_SIZE;

    if (info->iova.iov_len > UINTPTR_MAX - start) {
        return false;
    }
    end = start + info->iova.iov_len;

    return start >= DMA_BACKEND_WINDOW_START && end <= window_end;
}

static int
backend_resolve_sg(dma_backend_region_t *region, dma_sg_t *sg, uint8_t **ptr)
{
    if (sg->offset > region->iova.iov_len ||
        sg->length > region->iova.iov_len - sg->offset) {
        return ERROR_INT(EINVAL);
    }

    *ptr = region->storage + sg->offset;
    return 0;
}

static bool
backend_is_sg_mappable(vfu_ctx_t *vfu_ctx UNUSED, dma_sg_t *sg UNUSED,
                       void *private)
{
    dma_backend_region_t *region = private;

    return region != NULL && region->storage != NULL;
}

static int
backend_map_sg(vfu_ctx_t *vfu_ctx UNUSED, dma_sg_t *sg, struct iovec *iov,
               void *private)
{
    dma_backend_region_t *region = private;
    uint8_t *ptr;
    int ret;

    if (region == NULL || iov == NULL) {
        return ERROR_INT(EINVAL);
    }

    ret = backend_resolve_sg(region, sg, &ptr);
    if (ret < 0) {
        return ret;
    }

    iov->iov_base = ptr;
    iov->iov_len = sg->length;
    return 0;
}

static void
backend_unmap_sg(vfu_ctx_t *vfu_ctx UNUSED, dma_sg_t *sg UNUSED,
                 struct iovec *iov UNUSED, void *private UNUSED)
{
    /*
     * No-op: this sample backend maps to a persistent host buffer instead of a
     * transient mapping.
     */
}

static int
backend_read_sg(vfu_ctx_t *vfu_ctx UNUSED, dma_sg_t *sg, void *data,
                void *private)
{
    dma_backend_region_t *region = private;
    uint8_t *ptr;
    int ret;

    if (region == NULL || data == NULL) {
        return ERROR_INT(EINVAL);
    }

    ret = backend_resolve_sg(region, sg, &ptr);
    if (ret < 0) {
        return ret;
    }

    memcpy(data, ptr, sg->length);
    return 0;
}

static int
backend_write_sg(vfu_ctx_t *vfu_ctx UNUSED, dma_sg_t *sg, const void *data,
                 void *private)
{
    dma_backend_region_t *region = private;
    uint8_t *ptr;
    int ret;

    if (!sg->writeable) {
        return ERROR_INT(EPERM);
    }
    if (region == NULL || data == NULL) {
        return ERROR_INT(EINVAL);
    }

    ret = backend_resolve_sg(region, sg, &ptr);
    if (ret < 0) {
        return ret;
    }

    memcpy(ptr, data, sg->length);
    return 0;
}

static void
backend_release(vfu_ctx_t *vfu_ctx UNUSED, vfu_dma_info_t *info UNUSED,
                void *private)
{
    dma_backend_region_t *region = private;

    if (region == NULL) {
        return;
    }

    free(region->storage);
    region->storage = NULL;
    region->iova.iov_base = NULL;
    region->iova.iov_len = 0;
}

static const vfu_dma_region_access_ops_t dma_backend_ops = {
    .is_sg_mappable = backend_is_sg_mappable,
    .map_sg = backend_map_sg,
    .unmap_sg = backend_unmap_sg,
    .read_sg = backend_read_sg,
    .write_sg = backend_write_sg,
    .release = backend_release,
};

static int
dma_region_access_resolver(vfu_ctx_t *vfu_ctx, vfu_dma_info_t *info,
                           uint32_t prot, const vfu_dma_region_access_ops_t **ops,
                           void **private)
{
    server_state_t *state = vfu_get_private(vfu_ctx);
    int i;

    assert(state != NULL);

    if (ops == NULL || private == NULL || info == NULL) {
        return ERROR_INT(EINVAL);
    }

    *ops = NULL;
    *private = NULL;

    /*
     * Demonstrate policy: only ranges in this IOVA window are redirected to
     * the custom backend. All other ranges use libvfio-user's default logic.
     */
    if (!range_in_backend_window(info)) {
        return 0;
    }

    for (i = 0; i < MAX_DMA_BACKEND_REGIONS; i++) {
        dma_backend_region_t *region = &state->regions[i];

        if (region->storage != NULL) {
            continue;
        }

        region->storage = calloc(1, info->iova.iov_len);
        if (region->storage == NULL) {
            return ERROR_INT(ENOMEM);
        }

        region->iova = info->iova;
        *ops = &dma_backend_ops;
        *private = region;

        vfu_log(vfu_ctx, LOG_INFO,
                "custom DMA backend for iova=[%p, %p) len=%zu prot=%#x",
                info->iova.iov_base,
                (void *)((uintptr_t)info->iova.iov_base + info->iova.iov_len),
                info->iova.iov_len, prot);
        return 0;
    }

    return ERROR_INT(ENOSPC);
}

static void
dma_register(vfu_ctx_t *vfu_ctx, vfu_dma_info_t *info)
{
    vfu_log(vfu_ctx, LOG_DEBUG, "DMA map iova=[%p, %p) vaddr=%p",
            info->iova.iov_base,
            (void *)((uintptr_t)info->iova.iov_base + info->iova.iov_len),
            info->vaddr);
}

static void
dma_unregister(vfu_ctx_t *vfu_ctx, vfu_dma_info_t *info)
{
    vfu_log(vfu_ctx, LOG_DEBUG, "DMA unmap iova=[%p, %p)",
            info->iova.iov_base,
            (void *)((uintptr_t)info->iova.iov_base + info->iova.iov_len));
}

static ssize_t
bar0_access(vfu_ctx_t *vfu_ctx, char *buf, size_t count, loff_t offset,
            bool is_write)
{
    server_state_t *state = vfu_get_private(vfu_ctx);
    size_t end = offset + count;

    if (end > sizeof(state->control_reg)) {
        return ERROR_INT(EINVAL);
    }

    if (is_write) {
        memcpy(((uint8_t *)&state->control_reg) + offset, buf, count);
    } else {
        memcpy(buf, ((uint8_t *)&state->control_reg) + offset, count);
    }

    return count;
}

int
main(int argc, char **argv)
{
    server_state_t state = { 0 };
    vfu_ctx_t *vfu_ctx;
    int ret;

    if (argc != 2) {
        errx(EXIT_FAILURE, "usage: %s <vfio-user-socket>", argv[0]);
    }

    vfu_ctx = vfu_create_ctx(VFU_TRANS_SOCK, argv[1], 0, &state,
                             VFU_DEV_TYPE_PCI);
    if (vfu_ctx == NULL) {
        err(EXIT_FAILURE, "failed to create context");
    }

    ret = vfu_setup_log(vfu_ctx, log_cb, LOG_DEBUG);
    if (ret < 0) {
        err(EXIT_FAILURE, "failed to setup log");
    }

    ret = vfu_pci_init(vfu_ctx, VFU_PCI_TYPE_CONVENTIONAL,
                       PCI_HEADER_TYPE_NORMAL, 0);
    if (ret < 0) {
        err(EXIT_FAILURE, "vfu_pci_init failed");
    }

    vfu_pci_set_id(vfu_ctx, 0x1234, 0x1000, 0x1234, 0x1000);

    ret = vfu_setup_region(vfu_ctx, VFU_PCI_DEV_BAR0_REGION_IDX,
                           sizeof(state.control_reg), bar0_access,
                           VFU_REGION_FLAG_RW, NULL, 0, -1, 0);
    if (ret < 0) {
        err(EXIT_FAILURE, "failed to setup BAR0");
    }

    ret = vfu_setup_device_dma(vfu_ctx, dma_register, dma_unregister);
    if (ret < 0) {
        err(EXIT_FAILURE, "failed to setup DMA");
    }

    ret = vfu_setup_device_dma_region_access(vfu_ctx,
                                             dma_region_access_resolver);
    if (ret < 0) {
        err(EXIT_FAILURE, "failed to setup DMA region access resolver");
    }

    ret = vfu_realize_ctx(vfu_ctx);
    if (ret < 0) {
        err(EXIT_FAILURE, "failed to realize context");
    }

    ret = vfu_attach_ctx(vfu_ctx);
    if (ret < 0) {
        err(EXIT_FAILURE, "failed to attach context");
    }

    ret = vfu_run_ctx(vfu_ctx);
    if (ret < 0 && errno != ENOTCONN && errno != ESHUTDOWN) {
        err(EXIT_FAILURE, "vfu_run_ctx failed");
    }

    vfu_destroy_ctx(vfu_ctx);
    return 0;
}

/* ex: set tabstop=4 shiftwidth=4 softtabstop=4 expandtab: */
