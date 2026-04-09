# Plan: VFIO-PCI Device Data Movement Support

## Overview

This document plans the changes required to extend libvfio-user
so that an emulated device server can move data to and from
memory regions exposed by a real PCI device bound to the kernel
`vfio-pci` driver. Today, the library's DMA subsystem operates
exclusively on client (guest) memory that arrives via
`VFIO_USER_DMA_MAP` messages. The goal is to add first-class
support for mapping a physical PCI device's BARs through kernel
VFIO and making those BAR regions available as targets for the
existing SGL-based data movement primitives.

### Motivating use cases

- A virtual NVMe controller (libvfio-user server) performing
  GPU-direct storage reads/writes into a physical GPU's BAR
  memory.
- An emulated smart-NIC moving packet data directly into a
  physical accelerator's doorbell or buffer regions.
- Any peer-to-peer (P2P) workload where a userspace-emulated
  device must transfer data to or from physical device memory
  without bouncing through host DRAM.

## Terminology

| Term            | Meaning                                     |
|-----------------|---------------------------------------------|
| **Server**      | The libvfio-user device-emulation process.  |
| **Client**      | QEMU or other VMM speaking vfio-user.       |
| **Device BAR**  | A memory region on a physical PCI device,   |
|                 | accessed via kernel VFIO mmap.              |
| **Guest DMA**   | Memory registered by the client via         |
|                 | `VFIO_USER_DMA_MAP`.                        |
| **P2P transfer**| Data movement between guest DMA memory and  |
|                 | a physical device BAR, or between two       |
|                 | device BARs.                                |

## Current architecture (summary)

The data movement path today works as follows:

1. The client sends `VFIO_USER_DMA_MAP` messages, each carrying
   an fd and offset. `dma_controller_add_region()` mmaps these
   into the server process.
2. The server builds an SGL with `vfu_addr_to_sgl()`, which
   looks up guest IOVAs in `dma_controller_t.regions[]`.
3. `vfu_sgl_get()` converts the SGL into an iovec array
   pointing at the mmap'd guest memory.
4. The server reads/writes through the iovec pointers.
5. `vfu_sgl_put()` releases the mapping and marks dirty pages.

Alternatively, `vfu_sgl_read()` / `vfu_sgl_write()` send
explicit `VFIO_USER_DMA_READ` / `VFIO_USER_DMA_WRITE` messages
for non-mappable regions.

Key files involved:

- `lib/dma.h` / `lib/dma.c` -- DMA controller, SGL, regions
- `lib/libvfio-user.c` -- protocol handling, `vfu_sgl_*` API
- `include/libvfio-user.h` -- public API surface
- `lib/private.h` -- internal `vfu_ctx` structure

## Proposed changes

The work is broken into five layers, each building on the
previous one. All new code lives behind a Meson build option
(`-Dvfio-pci=true`, default false) and a compile-time guard
(`#ifdef VFIO_PCI_SUPPORT`) so the core library can still be
built without kernel VFIO headers beyond `linux/vfio.h`.

### Layer 1 -- VFIO PCI device handle (`lib/vfio_pci.c/.h`)

A thin wrapper around the kernel VFIO API for opening a
physical PCI device, querying its regions, and mmapping its
BARs.

#### Proposed interface

```c
typedef struct vfu_vfio_pci vfu_vfio_pci_t;

/*
 * Open a VFIO PCI device.
 *
 * @group_path: e.g. "/dev/vfio/42"
 * @bdf:        PCI BDF string, e.g. "0000:03:00.0"
 *
 * Returns an opaque handle or NULL on error (errno set).
 */
vfu_vfio_pci_t *
vfu_vfio_pci_open(const char *group_path, const char *bdf);

/*
 * Close the device, unmapping all BARs.
 */
void
vfu_vfio_pci_close(vfu_vfio_pci_t *dev);

/*
 * Query a BAR region.
 *
 * On success, *size, *offset, and *flags are populated.
 * Returns 0 on success or -1 on error (errno set).
 */
int
vfu_vfio_pci_get_region_info(vfu_vfio_pci_t *dev,
                             int bar_idx,
                             uint64_t *size,
                             uint64_t *offset,
                             uint32_t *flags);

/*
 * Memory-map a (sub-)range of a BAR.
 *
 * Returns a pointer to the mapped memory, or MAP_FAILED on
 * error (errno set). The caller must not munmap() this
 * directly; use vfu_vfio_pci_bar_unmap() or
 * vfu_vfio_pci_close().
 */
void *
vfu_vfio_pci_bar_mmap(vfu_vfio_pci_t *dev, int bar_idx,
                      uint64_t offset, size_t length,
                      int prot);

/*
 * Unmap a previously mapped BAR range.
 */
int
vfu_vfio_pci_bar_unmap(vfu_vfio_pci_t *dev, void *addr,
                       size_t length);
```

#### Internal structure

```c
struct vfu_vfio_pci {
    int container_fd;
    int group_fd;
    int device_fd;

    struct {
        void   *addr;
        size_t  length;
        int     prot;
    } bar_mappings[PCI_STD_NUM_BARS];
};
```

#### Implementation notes

- `vfu_vfio_pci_open()` performs the standard kernel VFIO
  sequence: open container (`/dev/vfio/vfio`), open group,
  `VFIO_GROUP_SET_CONTAINER`, `VFIO_SET_IOMMU`, and
  `VFIO_GROUP_GET_DEVICE_FD`.
- BARs are mmap'd via the device fd using the region offset
  from `VFIO_DEVICE_GET_REGION_INFO`.
- The IOMMU type (`VFIO_TYPE1_IOMMU` vs `VFIO_TYPE1v2_IOMMU`)
  should be probed and selected automatically.

#### Files to create / modify

| File                     | Action |
|--------------------------|--------|
| `lib/vfio_pci.c`         | New    |
| `lib/vfio_pci.h`         | New    |
| `include/libvfio-user.h` | Add public prototypes |
| `lib/meson.build`        | Conditional source    |
| `meson_options.txt`      | Add `vfio-pci` option |

### Layer 2 -- P2P region registration in the DMA controller

Extend `dma_controller_t` to track device-BAR-backed regions
alongside guest-DMA regions. A new region type flag
distinguishes the two.

#### Changes to `dma_memory_region_t`

```c
#define DMA_REGION_FLAG_P2P  (1 << 0)

typedef struct {
    vfu_dma_info_t info;
    int fd;
    off_t offset;
    uint8_t *dirty_bitmap;
    uint32_t region_flags;        /* NEW: DMA_REGION_FLAG_* */
    vfu_vfio_pci_t *pci_dev;     /* NEW: if P2P, owning dev */
    int bar_idx;                  /* NEW: which BAR           */
} dma_memory_region_t;
```

#### New internal helper

```c
int
dma_controller_add_p2p_region(dma_controller_t *dma,
                              vfu_dma_addr_t dma_addr,
                              uint64_t size,
                              vfu_vfio_pci_t *dev,
                              int bar_idx,
                              uint64_t bar_offset,
                              uint32_t prot);
```

This function follows the same overlap/duplicate checks as
`dma_controller_add_region()` but obtains the virtual address
from the BAR mapping rather than mmapping an fd. Dirty-page
tracking is not applicable for device-BAR memory (the device
owns the memory) and will be skipped.

#### Files to modify

| File          | Nature of change                       |
|---------------|----------------------------------------|
| `lib/dma.h`  | Extended struct, new helper prototype  |
| `lib/dma.c`  | Implement `dma_controller_add_p2p_region`, adjust unmap to skip munmap for P2P regions |

### Layer 3 -- Public API extensions

Expose the P2P region facility in the public API so server
applications can register physical device BARs as DMA targets.

#### New public functions (`include/libvfio-user.h`)

```c
/*
 * Register a VFIO PCI device BAR as a DMA region.
 *
 * After this call, data movement APIs (vfu_addr_to_sgl,
 * vfu_sgl_get, etc.) can target addresses in the range
 * [dma_addr, dma_addr+size).
 *
 * @vfu_ctx:    the libvfio-user context
 * @dma_addr:   DMA address to assign to this region
 * @size:       size of the region (must be <= BAR size)
 * @dev:        VFIO PCI device handle from
 *              vfu_vfio_pci_open()
 * @bar_idx:    BAR index on the physical device
 * @bar_offset: offset within the BAR
 * @prot:       PROT_READ, PROT_WRITE, or both
 *
 * Returns 0 on success or -1 on error (errno set).
 */
int
vfu_register_p2p_region(vfu_ctx_t *vfu_ctx,
                        vfu_dma_addr_t dma_addr,
                        size_t size,
                        vfu_vfio_pci_t *dev,
                        int bar_idx,
                        uint64_t bar_offset,
                        int prot);

/*
 * Remove a previously registered P2P region.
 *
 * Returns 0 on success or -1 on error (errno set).
 */
int
vfu_unregister_p2p_region(vfu_ctx_t *vfu_ctx,
                          vfu_dma_addr_t dma_addr,
                          size_t size);
```

#### SGL compatibility

No changes are required to `vfu_addr_to_sgl()`,
`vfu_sgl_get()`, or `vfu_sgl_put()`. Because P2P regions store
a valid `vaddr` in `dma_memory_region_t.info`, the existing SGL
and iovec paths work transparently: `dma_sgl_get()` returns an
iovec whose `iov_base` points at the BAR mapping, and the
server can `memcpy` (or use write-combining stores) exactly as
it would with guest memory.

The only behavioural differences:

1. **Dirty page tracking is skipped** for P2P regions (device
   BAR memory is not migratable guest RAM).
2. **`vfu_sgl_read()` / `vfu_sgl_write()`** (message-based DMA)
   are not supported for P2P regions, since there is no remote
   peer to message. These will return `-1` with `errno=ENOTSUP`
   when the SGL targets a P2P region.

#### Impact on existing public API

- `vfu_dma_info_t` gains no new fields; the existing `vaddr`,
  `iova`, and `mapping` members are populated from the BAR
  mapping.
- `vfu_dma_register_cb_t` / `vfu_dma_unregister_cb_t` are
  invoked for P2P regions just as for guest DMA regions, so
  server code that walks DMA callbacks will see them.
- `vfu_sg_is_mappable()` returns `true` for P2P regions (they
  are always mapped).

### Layer 4 -- Data movement helpers

While raw iovec access is sufficient for simple memcpy, device
BAR memory often requires special access patterns (e.g.,
write-combining, 64-bit aligned stores). Provide optional
convenience helpers.

```c
/*
 * Copy data between two SGL entries, supporting any
 * combination of guest-DMA and P2P-BAR-backed regions.
 *
 * @vfu_ctx: the libvfio-user context
 * @dst_sg:  destination scatter-gather entry
 * @src_sg:  source scatter-gather entry
 * @len:     number of bytes to copy
 *
 * Returns number of bytes copied or -1 on error.
 */
ssize_t
vfu_sgl_copy(vfu_ctx_t *vfu_ctx,
             dma_sg_t *dst_sg,
             dma_sg_t *src_sg,
             size_t len);

/*
 * Fill a region described by an SGL entry with a constant
 * byte value. Useful for clearing device buffers.
 *
 * @vfu_ctx: the libvfio-user context
 * @sg:      target scatter-gather entry
 * @c:       fill byte
 * @len:     number of bytes to fill
 *
 * Returns 0 on success or -1 on error.
 */
int
vfu_sgl_memset(vfu_ctx_t *vfu_ctx,
               dma_sg_t *sg,
               int c,
               size_t len);
```

These helpers use `vfu_sgl_get()` internally and select the
appropriate memory-access strategy (normal stores vs.
write-combining via `__builtin_nontemporal_store` where
available) based on the region flags.

#### Files to create / modify

| File                     | Action                          |
|--------------------------|---------------------------------|
| `lib/libvfio-user.c`    | Implement `vfu_sgl_copy`,       |
|                          | `vfu_sgl_memset`                |
| `include/libvfio-user.h` | Prototypes                     |

### Layer 5 -- Sample and tests

#### New sample: `samples/p2p_dma.c`

A minimal server that:

1. Opens a VFIO PCI device whose BDF is passed on the command
   line.
2. Registers one of its BARs as a P2P DMA region.
3. On a BAR-write trigger from the client, copies data from
   guest DMA memory into the physical device BAR (and back),
   verifying with a CRC.

#### Unit tests (`test/unit-tests.c`)

- `test_dma_controller_add_p2p_region` -- verifies region
  creation, overlap rejection, and the P2P flag.
- `test_dma_sgl_get_p2p` -- confirms that `dma_sgl_get()`
  returns a valid iovec pointing into BAR memory.
- `test_dma_sgl_put_p2p_no_dirty` -- confirms that
  `dma_sgl_put()` skips dirty tracking for P2P regions.

#### Python integration tests (`test/py/`)

- `test_p2p_region.py` -- exercises
  `vfu_register_p2p_region()` and `vfu_unregister_p2p_region()`
  via the pipe transport using a mock VFIO device backed by
  anonymous mmap memory.

## Build system changes

### `meson_options.txt`

```
option('vfio-pci',
       type: 'boolean',
       value: false,
       description: 'Enable VFIO PCI device data movement')
```

### `lib/meson.build`

Conditionally add `vfio_pci.c` to the library sources and pass
`-DVFIO_PCI_SUPPORT` when the option is enabled.

### `meson.build` (root)

No new external dependencies are required. The kernel VFIO
headers (`linux/vfio.h`) are already a build requirement.

## IOMMU considerations

When the server mmaps a physical device BAR, the IOMMU must
be configured to allow the server process to access that
memory. This is handled by the standard VFIO container/group
setup in Layer 1. Specifically:

- `VFIO_SET_IOMMU` on the container fd establishes an IOMMU
  domain.
- `VFIO_IOMMU_MAP_DMA` is **not** needed for BAR mmap access
  (the kernel maps BARs through the IOMMU as part of device
  open).
- If the server wants the physical device to DMA into guest
  memory (true P2P DMA through the IOMMU), it would need to
  call `VFIO_IOMMU_MAP_DMA` to map guest pages into the
  device's IOMMU domain. This is an advanced use case that can
  be deferred to a follow-up.

## Migration impact

P2P regions represent physical device memory, which is not
part of the guest's migratable state. Therefore:

- P2P regions are excluded from dirty page logging.
- During migration, P2P regions are not transferred. The
  destination server is expected to re-open the physical device
  and re-register its BARs independently.
- The migration state-transition callback can be used by the
  server to tear down and re-establish P2P mappings.

## Security considerations

- The server process must have access to the VFIO group
  (`/dev/vfio/<n>`), which normally requires membership in a
  specific group or `CAP_SYS_RAWIO`.
- BAR mmaps bypass the kernel's page-cache coherency; the
  server must ensure correct ordering (memory barriers) when
  communicating with the physical device.
- No new privilege is granted to the vfio-user **client**
  (QEMU); the client only sees emulated regions. The physical
  device BAR is accessed solely by the server process.

## Rollout / ordering of work

1. **Layer 1** (VFIO PCI handle) can be developed and unit
   tested in isolation.
2. **Layer 2** (DMA controller extension) depends on Layer 1
   for the `vfu_vfio_pci_t` type but can be stubbed.
3. **Layer 3** (public API) depends on Layers 1 and 2.
4. **Layer 4** (helpers) depends on Layer 3.
5. **Layer 5** (sample + tests) depends on all of the above.

Each layer can be delivered as a separate commit or pull
request. Layer 1 is fully independent and is the natural
starting point.

## Open questions

1. **IOMMU group sharing** -- If the physical device shares an
   IOMMU group with other devices, the current plan opens only
   the target device. Should we expose group-level management?
2. **Write-combining** -- Should `vfu_sgl_copy()` auto-detect
   WC-mapped BARs and use non-temporal stores, or should the
   caller opt in via a flag?
3. **Multi-BAR P2P** -- Should `vfu_register_p2p_region()`
   support registering multiple BARs of the same device under
   different DMA address ranges in a single call, or is
   per-BAR registration sufficient?
4. **Interrupt forwarding** -- If the physical device raises
   interrupts, should the library provide any integration with
   `vfu_irq_trigger()`, or is that left to the server?
5. **Container sharing** -- If multiple physical devices are
   used, should they share a VFIO container, or should each
   `vfu_vfio_pci_open()` create its own?

## Summary of files changed

| File                       | Change type |
|----------------------------|-------------|
| `lib/vfio_pci.c`           | New         |
| `lib/vfio_pci.h`           | New         |
| `lib/dma.h`                | Modified    |
| `lib/dma.c`                | Modified    |
| `lib/libvfio-user.c`       | Modified    |
| `include/libvfio-user.h`   | Modified    |
| `lib/private.h`            | Minor mod   |
| `lib/meson.build`          | Modified    |
| `meson_options.txt`         | Modified    |
| `samples/p2p_dma.c`        | New         |
| `samples/meson.build`      | Modified    |
| `test/unit-tests.c`        | Modified    |
| `test/py/test_p2p_region.py` | New       |
| `test/py/libvfio_user.py`  | Modified    |
| `test/py/meson.build`      | Modified    |
| `docs/vfio-pci-data-movement.md` | This file |
