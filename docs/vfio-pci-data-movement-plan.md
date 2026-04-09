# Plan: Data Movement to vfio-pci Device Memory

## 1. Problem Statement

libvfio-user today emulates PCI devices entirely in userspace. A
server creates a virtual device, a client (e.g. QEMU) connects
over a UNIX socket, and the two exchange register accesses and
DMA operations using the vfio-user protocol.

Data movement in the current design is limited to two targets:

- **Guest DMA regions** (IOVA space mapped via `VFIO_USER_DMA_MAP`)
  accessed through the SGL APIs (`vfu_addr_to_sgl`, `vfu_sgl_get`,
  `vfu_sgl_read`, `vfu_sgl_write`).
- **Emulated BAR backing memory** (files/shared memory behind
  `vfu_setup_region`), accessed via region callbacks or direct
  mmap.

Neither path supports data movement to or from memory locations
exposed by a **real** PCI device bound to the kernel `vfio-pci`
driver. This plan describes the changes required to add that
capability, enabling a libvfio-user server to:

1. Open a physical PCI device via `/dev/vfio/`.
2. Map the device's BARs into the server process.
3. Transfer data between guest DMA regions and physical device
   BAR memory.
4. Optionally configure the IOMMU so the physical device can DMA
   directly to/from guest memory (peer-to-peer or device-to-host
   transfers).

## 2. Architecture Overview

```
  +----------------------------------------------------+
  |                     Guest VM                        |
  |  +-------+   +----------------------------------+  |
  |  | Driver |-->| Guest RAM (DMA regions / IOVAs)  |  |
  |  +-------+   +----------------------------------+  |
  +--------|--------------------|---------------------+
           |                    |
     vfio-user socket      DMA_MAP fd
           |                    |
  +--------v--------------------v---------------------+
  |            libvfio-user server process             |
  |                                                    |
  |  +----------------+     +----------------------+   |
  |  | DMA controller |     | vfio-pci device ctx  |   |
  |  | (existing)     |     | (NEW)                |   |
  |  |                |     |  - /dev/vfio/N       |   |
  |  | guest regions  |     |  - BAR mappings      |   |
  |  | vaddr/iova map |     |  - IOMMU group       |   |
  |  +-------+--------+     +---------+------------+   |
  |          |                        |                |
  |     +----v------------------------v----+           |
  |     |     Data Movement Engine (NEW)   |           |
  |     |  - memcpy (CPU)                  |           |
  |     |  - device-initiated DMA          |           |
  |     |  - future: GPU DMA / P2P         |           |
  |     +----------------------------------+           |
  +----------------------------------------------------+
           |
     /dev/vfio/*
           |
  +--------v-------------------------------------------+
  |  Physical PCI device (bound to vfio-pci driver)    |
  |  BAR0..BAR5 memory regions                         |
  +----------------------------------------------------+
```

## 3. Subsystem Breakdown

### 3.1 New module: `lib/vfio_pci.c` / `lib/vfio_pci.h`

A self-contained abstraction for opening and managing a real
vfio-pci device from userspace. This module is orthogonal to the
existing emulated-device machinery; it wraps the kernel VFIO
`ioctl()` interface.

**Proposed types:**

```c
typedef struct vfu_vfio_pci_bar {
    uint32_t index;
    uint32_t flags;
    uint64_t size;
    uint64_t offset;
    void    *mmap_base;
    size_t   mmap_len;
} vfu_vfio_pci_bar_t;

typedef struct vfu_vfio_pci_ctx {
    int container_fd;
    int group_fd;
    int device_fd;
    char bdf[16];
    int nr_bars;
    vfu_vfio_pci_bar_t bars[6];
    bool iommu_attached;
} vfu_vfio_pci_ctx_t;
```

**Key functions:**

| Function | Purpose |
|----------|---------|
| `vfu_vfio_pci_open(bdf)` | Open container, group, device; return ctx |
| `vfu_vfio_pci_map_bar(ctx, idx)` | `mmap()` a specific BAR region |
| `vfu_vfio_pci_unmap_bar(ctx, idx)` | `munmap()` a BAR |
| `vfu_vfio_pci_bar_ptr(ctx, idx, off)` | Return pointer into a mapped BAR |
| `vfu_vfio_pci_close(ctx)` | Tear down everything |
| `vfu_vfio_pci_iommu_map(ctx, iova, vaddr, size, prot)` | Map guest memory into device IOMMU |
| `vfu_vfio_pci_iommu_unmap(ctx, iova, size)` | Remove an IOMMU mapping |

This module uses only standard Linux VFIO ioctls
(`VFIO_GET_API_VERSION`, `VFIO_SET_IOMMU`, `VFIO_GROUP_GET_STATUS`,
`VFIO_GROUP_SET_CONTAINER`, `VFIO_GROUP_GET_DEVICE_FD`,
`VFIO_DEVICE_GET_REGION_INFO`, `VFIO_IOMMU_MAP_DMA`,
`VFIO_IOMMU_UNMAP_DMA`). It has **no dependency** on the
vfio-user socket protocol and can be tested independently.

### 3.2 Data movement APIs (public header additions)

New functions in `include/libvfio-user.h` to copy data between
the two address domains already available to the server: guest
DMA regions and vfio-pci BAR memory.

```c
int
vfu_dma_to_device_bar(vfu_ctx_t *vfu_ctx,
                      vfu_vfio_pci_ctx_t *pci,
                      dma_sg_t *sgl, size_t sg_cnt,
                      int bar_idx, uint64_t bar_offset);

int
vfu_device_bar_to_dma(vfu_ctx_t *vfu_ctx,
                      vfu_vfio_pci_ctx_t *pci,
                      int bar_idx, uint64_t bar_offset,
                      dma_sg_t *sgl, size_t sg_cnt);

int
vfu_device_bar_rw(vfu_vfio_pci_ctx_t *pci,
                  int bar_idx, uint64_t offset,
                  void *buf, size_t len, bool is_write);
```

These are thin wrappers: `vfu_dma_to_device_bar` resolves the
SGL via `vfu_sgl_get`, obtains the BAR pointer via
`vfu_vfio_pci_bar_ptr`, and performs a `memcpy` (or, in future, a
device-initiated DMA). Symmetrically for the reverse direction.

`vfu_device_bar_rw` is a simple helper for reading/writing a
device BAR from a plain buffer, useful for register accesses.

### 3.3 IOMMU integration for device-initiated DMA

When the physical device has a DMA engine (e.g. an NVMe
controller, a NIC, a GPU), the server may want to program that
engine to transfer data directly between the device and guest
memory, bypassing the CPU `memcpy` path. This requires IOMMU
mappings so the device can see the guest's IOVA space.

**Hook into existing DMA callbacks:**

The `vfu_dma_register_cb_t` / `vfu_dma_unregister_cb_t` callback
pair already fires whenever the client adds or removes a DMA
region. The server application can call
`vfu_vfio_pci_iommu_map()` from the register callback and
`vfu_vfio_pci_iommu_unmap()` from the unregister callback,
establishing a 1:1 IOVA passthrough so the physical device sees
the same addresses as the guest.

This requires that the DMA regions carry an fd (i.e. the client
shares memory via file descriptors), which is the common case
when QEMU is the client.

**Lifecycle:**

```
client DMA_MAP  -->  dma_register_cb
                       |
                       +--> vfu_vfio_pci_iommu_map(pci, iova, vaddr, size)
                              |
                              +--> ioctl(container_fd, VFIO_IOMMU_MAP_DMA, ...)

client DMA_UNMAP --> dma_unregister_cb
                       |
                       +--> vfu_vfio_pci_iommu_unmap(pci, iova, size)
                              |
                              +--> ioctl(container_fd, VFIO_IOMMU_UNMAP_DMA, ...)
```

### 3.4 Changes to existing code

| File | Change | Invasiveness |
|------|--------|--------------|
| `lib/meson.build` | Add `vfio_pci.c` to library sources | Trivial |
| `include/libvfio-user.h` | Add new public API prototypes | Additive |
| `include/vfio-user.h` | None required | None |
| `lib/dma.h` / `lib/dma.c` | None; existing SGL/iovec path is reused as-is | None |
| `lib/libvfio-user.c` | None for the core; data movement functions live in the new module | None |
| `lib/private.h` | Optionally add a `vfu_vfio_pci_ctx_t *` to `vfu_ctx` if we want library-managed lifetime; otherwise the server app owns it | Minimal |
| `meson_options.txt` | Add `vfio-pci` boolean option (disabled by default, since it requires `<linux/vfio.h>` ioctls and a real device for testing) | Trivial |

The key design decision is that the vfio-pci module is
**opt-in and side-loaded**: it does not change any existing code
paths. Servers that do not use real devices are completely
unaffected.

### 3.5 Sample: `samples/vfio_pci_bridge.c`

A new sample demonstrating the end-to-end flow:

1. Open a real vfio-pci device (BDF from command line).
2. Create a libvfio-user context exposing the same PCI identity
   (vendor/device IDs, BAR sizes) as the real device.
3. Forward BAR accesses from the guest to the real device BARs
   via `vfu_device_bar_rw`.
4. On DMA map/unmap, mirror the IOMMU mappings so the real
   device can DMA into guest memory.
5. Forward interrupts from the real device (via eventfd) to the
   guest (via `vfu_irq_trigger`).

This sample is intentionally minimal and not a full passthrough
solution (which would require handling reset, FLR, MSI-X table
emulation, etc.), but it demonstrates the data movement plumbing.

### 3.6 Tests

| Test | Type | Purpose |
|------|------|---------|
| `test/unit-tests-vfio-pci.c` | C/cmocka | Unit-test the vfio-pci module with mocked ioctls (mock `open`, `ioctl`, `mmap`) |
| `test/py/test_vfio_pci_data_movement.py` | Python/pytest | Integration test using the pipe transport; exercises `vfu_dma_to_device_bar` and reverse with a fake BAR backed by shared memory |

Since real hardware is not available in CI, the unit tests will
mock the kernel VFIO ioctls. The Python integration tests will
use shared-memory-backed fake BARs to exercise the data movement
code paths without requiring a physical device.

## 4. Implementation Order

The following sequence minimises risk by building from the
bottom up, with each step independently testable:

1. **`lib/vfio_pci.c` + `lib/vfio_pci.h`** -- the kernel VFIO
   wrapper. Write the open/close/map_bar/unmap_bar functions.
   Add unit tests with mocked ioctls. This step has zero
   impact on existing library code.

2. **IOMMU helpers** -- `vfu_vfio_pci_iommu_map` /
   `vfu_vfio_pci_iommu_unmap`. Extend the unit tests to cover
   `VFIO_IOMMU_MAP_DMA` / `VFIO_IOMMU_UNMAP_DMA`.

3. **Data movement functions** -- `vfu_dma_to_device_bar`,
   `vfu_device_bar_to_dma`, `vfu_device_bar_rw`. These compose
   the existing SGL path with the new BAR mapping. Add Python
   integration tests.

4. **Public header & build integration** -- export the new API in
   `include/libvfio-user.h`, add the meson option, wire
   into `lib/meson.build`.

5. **Sample `vfio_pci_bridge.c`** -- end-to-end demonstration.

6. **Documentation** -- update `docs/memory-mapping.md` and
   `README.md`.

## 5. Design Decisions & Trade-offs

### 5.1 Separate module vs. extending `dma_controller_t`

The DMA controller (`lib/dma.c`) manages guest memory regions.
Device BARs are a fundamentally different address domain (MMIO,
not RAM; different cacheability, different alignment
requirements). Mixing them into the DMA controller would
complicate the SGL fast path and conflate two distinct concepts.
Keeping a separate `vfu_vfio_pci_ctx_t` is cleaner and avoids
regressing existing DMA performance.

### 5.2 CPU memcpy vs. device DMA engine

The initial implementation uses CPU `memcpy` between `mmap`'d BAR
regions and `mmap`'d guest memory. This is simple and correct
but not optimal for large transfers, especially with devices that
have their own DMA engines (GPUs, NVMe, NICs). A future
enhancement can add a pluggable "transfer engine" callback:

```c
typedef int (*vfu_xfer_engine_fn)(void *dst, const void *src,
                                  size_t len, void *opaque);
```

The default would be `memcpy`; a GPU-aware server could supply an
engine that uses `hipMemcpy` or similar. This is out of scope for
the initial implementation but the API is designed to not
preclude it.

### 5.3 IOMMU type

The implementation assumes VFIO Type1 IOMMU
(`VFIO_TYPE1v2_IOMMU`), which is the most common on x86
platforms. Support for other IOMMU types (e.g. ARM SMMU,
`VFIO_SPAPR_TCE_v2_IOMMU`) can be added later via a
configuration enum without changing the data movement APIs.

### 5.4 Thread safety

The existing library is explicitly **not** thread-safe (see the
header comment in `libvfio-user.h`). The new vfio-pci module
follows the same convention: callers must serialise access
externally. The `mmap`'d BAR pointers themselves are
thread-safe for concurrent reads (MMIO is inherently
serialised by the PCI bus), but the bookkeeping structures
are not protected.

### 5.5 Compile-time gating

Since the vfio-pci module depends on Linux kernel VFIO headers
and ioctls that may not be available on all build platforms
(e.g. macOS CI, containers without `/dev/vfio`), it is gated
behind a meson option (`-Dvfio-pci=enabled`). When disabled, the
module is not compiled, and the data movement APIs are not
available. The existing library builds and tests are completely
unaffected.

## 6. Risks and Mitigations

| Risk | Mitigation |
|------|------------|
| Real hardware needed for full testing | Mock ioctls in unit tests; use shared-memory fake BARs in integration tests; manual testing on hardware documented in sample README |
| BAR access ordering / cacheability | Use `volatile` pointers or explicit memory barriers for MMIO reads/writes; document that BAR memory is uncacheable |
| IOMMU mapping failures (IOVA conflicts, DMAR faults) | Return clear error codes; log diagnostics; sample demonstrates proper error handling |
| ABI stability (library is `0.x`) | New API is additive; existing API and ABI are unchanged; the library already declares itself unstable |
| Performance of CPU memcpy for large transfers | Document as a known limitation; design the API to accommodate future DMA engine backends |

## 7. File Inventory (New and Modified)

```
NEW   lib/vfio_pci.c
NEW   lib/vfio_pci.h
NEW   samples/vfio_pci_bridge.c
NEW   test/unit-tests-vfio-pci.c
NEW   test/py/test_vfio_pci_data_movement.py
MOD   include/libvfio-user.h          (new prototypes)
MOD   lib/meson.build                 (add vfio_pci.c)
MOD   samples/meson.build             (add vfio_pci_bridge)
MOD   test/meson.build                (add unit-tests-vfio-pci)
MOD   test/py/meson.build             (add test_vfio_pci_data_movement)
MOD   meson_options.txt               (add vfio-pci option)
MOD   docs/memory-mapping.md          (document new path)
```
