# Plan: vfio-pci Device Memory Data Movement

## Overview

This document plans the changes needed so that a libvfio-user
server can move data to and from memory regions (BARs) exposed
by a **real** vfio-pci device on the host. Today the library
already lets a server:

- access **guest RAM** directly (via `vfu_sgl_get` /
  `vfu_sgl_put`) or indirectly (via `vfu_sgl_read` /
  `vfu_sgl_write` messages), and
- expose **emulated BARs** to the client (backed by local
  buffers or files).

The missing piece is a first-class path for a server to open a
kernel vfio-pci device, map its BARs into the server process,
and then efficiently shuttle data between those device BARs and
the guest DMA regions the client has registered.

The primary use-case is a "proxy" or "mediated" device server
that fronts real hardware: guest writes land in emulated
regions, the server copies them into the physical device's BAR,
and vice-versa for reads. A secondary use-case is
device-to-device transfers where two vfio-pci resources need
to exchange data through the server.

---

## 1. Terminology

| Term              | Meaning                                  |
|-------------------|------------------------------------------|
| **server**        | The libvfio-user process emulating a PCI |
|                   | device for a VMM client.                 |
| **client / VMM**  | QEMU, cloud-hypervisor, or similar.      |
| **vfio-pci dev**  | A real PCI device bound to the kernel    |
|                   | `vfio-pci` driver.                       |
| **BAR**           | Base Address Register; a PCI device      |
|                   | memory or I/O region.                    |
| **IOVA**          | I/O Virtual Address, i.e. a guest        |
|                   | physical address in the DMA map.         |
| **SGL**           | Scatter-gather list (`dma_sg_t`).        |

---

## 2. Components That Must Change

### 2.1 New: `lib/vfio_pci_dev.c` / `lib/vfio_pci_dev.h`

A helper module that wraps the kernel VFIO interface for
opening and mapping a real vfio-pci device. Responsibilities:

1. **Open the device** given a VFIO group path and BDF.
   Internally performs the `VFIO_GROUP_GET_DEVICE_FD` ioctl
   sequence (open container, open group, set IOMMU type,
   get device fd).
2. **Query region info** via `VFIO_DEVICE_GET_REGION_INFO`
   for each BAR the caller is interested in.
3. **mmap BAR regions** into the server address space.
4. **Provide accessors** that return `(void *, size_t)` pairs
   for each mapped BAR so the data-movement layer can
   `memcpy` or use them directly.
5. **Teardown** — `munmap`, close fds.

Proposed types:

```c
typedef struct vfu_pci_dev vfu_pci_dev_t;

typedef struct vfu_pci_dev_bar_info {
    int      index;
    void    *addr;       /* mmap'd pointer */
    size_t   size;
    uint32_t flags;      /* VFIO_REGION_INFO_FLAG_* */
} vfu_pci_dev_bar_info_t;
```

Proposed public API (installed header):

```c
vfu_pci_dev_t *
vfu_pci_dev_open(const char *group_path, const char *bdf);

int
vfu_pci_dev_map_bar(vfu_pci_dev_t *dev, int bar_idx,
                    vfu_pci_dev_bar_info_t *info_out);

int
vfu_pci_dev_unmap_bar(vfu_pci_dev_t *dev, int bar_idx);

void
vfu_pci_dev_close(vfu_pci_dev_t *dev);
```

### 2.2 New: Data-Movement API

New functions that combine the DMA/SGL layer with a
`vfu_pci_dev_t` to copy data between guest memory and a
device BAR. These sit in `libvfio-user.h` (public) and are
implemented in `lib/libvfio-user.c` or a separate
`lib/vfio_pci_xfer.c`.

```c
/*
 * Copy `len` bytes from the vfio-pci BAR at
 * `bar_offset` into guest memory at `dma_addr`.
 */
int
vfu_pci_dev_read_bar_to_dma(vfu_ctx_t *vfu_ctx,
                            vfu_pci_dev_t *dev,
                            int bar_idx,
                            uint64_t bar_offset,
                            vfu_dma_addr_t dma_addr,
                            size_t len);

/*
 * Copy `len` bytes from guest memory at `dma_addr`
 * into the vfio-pci BAR at `bar_offset`.
 */
int
vfu_pci_dev_write_dma_to_bar(vfu_ctx_t *vfu_ctx,
                             vfu_pci_dev_t *dev,
                             int bar_idx,
                             uint64_t bar_offset,
                             vfu_dma_addr_t dma_addr,
                             size_t len);

/*
 * Copy between two memory regions: one identified
 * by an SGL (guest DMA) and one by a raw pointer
 * (device BAR mapped address). Direction controlled
 * by `to_device`.
 */
int
vfu_pci_dev_xfer_sgl(vfu_ctx_t *vfu_ctx,
                     dma_sg_t *sgl, size_t sg_cnt,
                     void *dev_addr,
                     size_t dev_offset,
                     size_t len,
                     bool to_device);
```

The high-level helpers (`read_bar_to_dma`,
`write_dma_to_bar`) will internally:

1. Look up the BAR mapping from `vfu_pci_dev_t`.
2. Build an SGL via `vfu_addr_to_sgl`.
3. If mappable, call `vfu_sgl_get` to obtain iovecs, then
   `memcpy` between the iovec buffers and the BAR pointer.
4. If not mappable, fall back to `vfu_sgl_read`/`write`
   (message-based DMA) copying through an intermediate
   bounce buffer on the stack or heap.
5. Call `vfu_sgl_put` to release and dirty-track.

### 2.3 Extend `lib/dma.h` / `lib/dma.c`

No fundamental changes to the DMA controller are needed;
the existing SGL and iovec machinery is reused as-is. Minor
additions:

- A helper `dma_sg_is_mappable` is already present; the
  transfer code will use it to choose the fast path (direct
  memcpy) versus the slow path (message-based).
- If we want to support **IOMMU-mapped device DMA** (the
  physical device itself performing DMA into guest memory
  without going through the server CPU), we would need to
  program the host IOMMU via `VFIO_IOMMU_MAP_DMA`. This is
  an optional Phase 2 enhancement (see Section 5).

### 2.4 Changes to `lib/private.h`

Add an optional `vfu_pci_dev_t *` pointer to `struct vfu_ctx`
so the context can track an associated physical device. This
is optional — the user can also manage the `vfu_pci_dev_t`
externally and just pass it to the transfer functions.

### 2.5 Build System (`meson.build`)

- Add new source files to `lib/meson.build`.
- Add a Meson option `vfio-pci-dev` (boolean, default false)
  to gate compilation. The feature requires `<linux/vfio.h>`
  and `sys/ioctl.h` (both already used) but also requires a
  host with a real VFIO setup, so it should be optional.
- Install new public header `include/vfio_pci_dev.h`.

### 2.6 New Sample: `samples/proxy_server.c`

A complete sample that:

1. Opens a vfio-pci device (BDF from command line).
2. Maps BAR0 of the physical device.
3. Creates a libvfio-user context exposing an emulated PCI
   device whose BAR0 region has a callback.
4. In the BAR0 write callback, copies the written data from
   the emulated BAR into the physical device's BAR0 via
   `vfu_pci_dev_write_dma_to_bar` or direct memcpy.
5. In the BAR0 read callback, reads from the physical
   device's BAR0 and returns the data to the client.
6. Registers for DMA and demonstrates a device-initiated
   transfer from BAR to guest memory.

### 2.7 Tests

#### 2.7.1 Unit Tests (`test/unit-tests.c`)

- Mock the VFIO ioctl interface (container, group, device).
- Test `vfu_pci_dev_open`, `vfu_pci_dev_map_bar`,
  `vfu_pci_dev_close` with mocked fds.
- Test `vfu_pci_dev_xfer_sgl` with an in-memory fake BAR and
  a fake DMA region.

#### 2.7.2 Python Tests (`test/py/test_pci_dev_xfer.py`)

- Because real hardware is not available in CI, these tests
  will focus on the library-side logic:
  - Ensure the new APIs are exported from the `.so`.
  - Test error paths (bad BAR index, unmapped BAR, NULL
    pointers).
  - Use the existing pipe transport to simulate a
    client/server pair and verify that data copied into a
    fake "BAR" buffer appears correctly on the DMA side.

### 2.8 Documentation

- This file (`docs/vfio-pci-data-movement.md`) becomes the
  reference once the feature is implemented.
- Update `docs/memory-mapping.md` to cross-reference the new
  device-memory data path.
- Update `README.md` to mention the capability.

---

## 3. Data Flow Diagrams

### 3.1 Guest Write to Physical Device BAR

```
Guest VM
  |
  | VFIO_USER_REGION_WRITE (BAR0, offset, data)
  v
libvfio-user server
  |
  | bar0_access callback fires
  | memcpy(pci_dev_bar0_ptr + offset, buf, count)
  v
Physical PCI device BAR0  (mmap'd via vfio-pci)
```

### 3.2 Device-Initiated DMA: BAR to Guest Memory

```
Physical PCI device BAR0 (mmap'd)
  |
  | server reads from BAR0
  v
libvfio-user server
  |
  | vfu_addr_to_sgl(dma_addr, len, sgl, ...)
  | vfu_sgl_get(sgl, iov, ...)  [if mappable]
  | memcpy(iov.iov_base, bar0_ptr + off, len)
  | vfu_sgl_put(sgl, iov, ...)
  |   -- or --
  | vfu_sgl_write(sgl, 1, bar0_ptr + off)  [if !mappable]
  v
Guest VM memory (DMA region)
```

### 3.3 High-Level Helper

```
vfu_pci_dev_read_bar_to_dma(ctx, dev, bar, off, dma, len)
  |
  +-- vfu_pci_dev_bar_info(dev, bar) --> bar_ptr
  +-- vfu_addr_to_sgl(ctx, dma, len, sgl, N, PROT_WRITE)
  +-- for each sg entry:
  |     if mappable:
  |       vfu_sgl_get -> iov
  |       memcpy(iov.iov_base, bar_ptr + running_off, sg.len)
  |       vfu_sgl_put
  |     else:
  |       vfu_sgl_write(ctx, sg, 1, bar_ptr + running_off)
  +-- return 0
```

---

## 4. Implementation Order

Below is the recommended sequence of patches/commits. Each
step should be independently reviewable and testable.

### Step 1 — Interfaces and Types

Define the new types and function prototypes in header files.
No implementation yet; just the API surface for review.

Files touched:
- `include/vfio_pci_dev.h` (new)
- `include/libvfio-user.h` (new prototypes for the
  high-level transfer helpers, if we choose to put them
  there)

### Step 2 — `vfu_pci_dev` Implementation

Implement `vfu_pci_dev_open`, `vfu_pci_dev_map_bar`,
`vfu_pci_dev_unmap_bar`, `vfu_pci_dev_close` in
`lib/vfio_pci_dev.c`.

Files touched:
- `lib/vfio_pci_dev.c` (new)
- `lib/meson.build` (add source, conditional on option)
- `meson_options.txt` (new option `vfio-pci-dev`)
- `include/meson.build` (install new header)

### Step 3 — Data-Movement Helpers

Implement `vfu_pci_dev_read_bar_to_dma`,
`vfu_pci_dev_write_dma_to_bar`, and `vfu_pci_dev_xfer_sgl`.

Files touched:
- `lib/vfio_pci_xfer.c` (new) or appended to
  `lib/vfio_pci_dev.c`
- `lib/meson.build`

### Step 4 — Unit Tests

Add cmocka-based tests for the new module, mocking the VFIO
ioctl layer.

Files touched:
- `test/unit-tests.c` (extend)
- `test/meson.build` (add new source to unit test target)
- `test/mocks.c` (add mocks for open/ioctl/mmap as needed)

### Step 5 — Python Functional Tests

Add pytest tests that exercise the new `.so` exports through
ctypes.

Files touched:
- `test/py/test_pci_dev_xfer.py` (new)
- `test/py/libvfio_user.py` (add ctypes wrappers)
- `test/py/meson.build` (register new test)

### Step 6 — Proxy Server Sample

Write a complete sample demonstrating the feature.

Files touched:
- `samples/proxy_server.c` (new)
- `samples/meson.build` (add executable)

### Step 7 — Documentation

Finalize this plan doc, update existing docs.

Files touched:
- `docs/vfio-pci-data-movement.md` (finalize)
- `docs/memory-mapping.md` (cross-reference)
- `README.md` (mention new capability)

---

## 5. Phase 2 (Future): Hardware DMA Path

In Phase 1 all data movement goes through the server CPU
(`memcpy`). A future Phase 2 could add an IOMMU-programming
path so the physical device's own DMA engine can read/write
guest memory directly:

1. When `VFIO_USER_DMA_MAP` arrives, the server would call
   `VFIO_IOMMU_MAP_DMA` on the physical device's container
   to establish a host IOVA mapping to the same guest
   memory.
2. The server would then program the physical device's DMA
   descriptors with the host IOVA.
3. On `VFIO_USER_DMA_UNMAP`, the server tears down the
   mapping with `VFIO_IOMMU_UNMAP_DMA`.

This requires careful coordination with dirty-page tracking
(the physical device's DMA writes must still be caught for
live migration) and is substantially more complex. It is
explicitly out of scope for the initial implementation but
the API should be designed so it can be extended.

---

## 6. Risks and Considerations

### 6.1 IOMMU Group Isolation

Opening a vfio-pci device requires the entire IOMMU group to
be bound to vfio-pci. The server process needs appropriate
permissions (typically `CAP_SYS_RAWIO` or membership in the
`vfio` group). This is an operational requirement, not a
library concern, but should be documented.

### 6.2 BAR Access Ordering

PCI MMIO BARs are typically mapped as uncacheable or
write-combining. `memcpy` into/out of such memory may need
special handling (e.g. `volatile` pointers, memory barriers,
or width-specific accesses). The helper functions should
document that callers may need to use `__builtin_memcpy` or
width-matched reads/writes for devices that are sensitive to
access size or ordering.

### 6.3 Error Handling

MMIO reads from a device that has suffered a fatal error
return `0xFFFFFFFF`. The library cannot distinguish this from
valid data, so error handling remains the server
implementation's responsibility.

### 6.4 Thread Safety

The existing libvfio-user API is not thread-safe. The new
APIs follow the same model: callers must serialize access
externally.

### 6.5 Testing Without Hardware

CI environments will not have real vfio-pci devices. Unit
tests must mock the VFIO ioctl layer. Functional tests should
use `memfd_create` or `tmpfile` to simulate BAR memory. A
helper in the test infrastructure can create a fake
`vfu_pci_dev_t` with pre-filled mappings for this purpose.

---

## 7. Dependencies

| Dependency             | Version | Notes                    |
|------------------------|---------|--------------------------|
| Linux kernel headers   | >= 4.15 | `<linux/vfio.h>` already |
|                        |         | required                 |
| json-c                 | >= 0.11 | Existing dependency      |
| cmocka                 | any     | Unit tests only          |
| Meson                  | >= 0.53 | Existing requirement     |

No new external library dependencies are introduced.

---

## 8. Open Questions

1. **Should `vfu_pci_dev_t` be embedded in `vfu_ctx_t`?**
   Embedding it couples the physical-device lifecycle to the
   emulated-device lifecycle. Keeping it external gives more
   flexibility (e.g. one server proxying multiple physical
   devices, or multiple emulated devices sharing one physical
   device). The recommendation is to keep it external.

2. **Should the transfer helpers live in `libvfio-user.h` or
   in a separate `vfio_pci_dev.h`?** Putting them in a
   separate header keeps the core API lean but requires
   users to include two headers. The recommendation is to
   use a separate header and have the high-level helpers
   there.

3. **Should we support partial-BAR mappings?** Some devices
   have very large BARs (e.g. GPU VRAM). Mapping the entire
   BAR may not be feasible. The `vfu_pci_dev_map_bar` API
   should accept optional offset/size parameters or a
   separate `vfu_pci_dev_map_bar_range` variant. This can be
   deferred to a follow-up if the initial implementation
   only targets moderate-size BARs.

4. **Write-combining vs. uncacheable mapping?** The default
   `mmap` of a VFIO BAR is uncacheable. For bulk transfers,
   write-combining can be significantly faster. We could
   accept an optional `mmap_flags` parameter. Deferred to
   follow-up.
