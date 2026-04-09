# Data Movement to vfio-pci Device Memory Regions

This document describes how to extend libvfio-user to support data
movement between a libvfio-user emulated device and memory locations
exposed by a real hardware vfio-pci device.  The goal is to allow a
libvfio-user server to read from or write to BARs (or other MMIO
regions) of a physical PCI device that has been bound to the kernel
`vfio-pci` driver.

## Motivation

Today libvfio-user emulates PCI devices entirely in userspace.  DMA
operations move data between the emulated device and guest RAM
regions shared by the client (QEMU).  There is no facility for the
emulated device to interact with real hardware resources, e.g. to
offload computation to an accelerator whose BARs are mapped through
`/dev/vfio`.

Adding vfio-pci device memory access enables several use cases:

- **Hardware-accelerated emulation** -- the emulated device can
  delegate work to a physical accelerator, reading results back from
  device BARs.
- **GPU Direct Storage style flows** -- data can move between guest
  DMA regions and device BARs without an extra copy through host
  userspace buffers.
- **Pass-through assist** -- a libvfio-user server can mediate access
  to a subset of a physical device's resources while still presenting
  a virtual PCI function to the guest.

## Current Architecture (Summary)

| Component | Role |
|-----------|------|
| `include/libvfio-user.h` | Public API: context, regions, DMA, IRQs, SGL helpers |
| `include/vfio-user.h` | Wire-protocol structs (`VFIO_USER_DMA_MAP`, etc.) |
| `lib/dma.c` / `lib/dma.h` | DMA controller: region tracking, SGL, mmap, dirty pages |
| `lib/libvfio-user.c` | Core server loop, `handle_dma_map` / `handle_dma_unmap` |
| `lib/pci.c` | PCI config space emulation, BAR writes |
| `lib/private.h` | Internal structures: `vfu_ctx`, `vfu_reg_info_t`, `pci_dev` |
| `samples/server.c` | Reference server demonstrating BAR + DMA callbacks |
| `samples/client.c` | Reference client exercising the protocol |

Data movement today works in two modes:

1. **Direct mapping** -- the client passes an fd; libvfio-user
   `mmap()`s it and the server accesses guest memory through the
   resulting `vaddr`.  APIs: `vfu_addr_to_sgl` -> `vfu_sgl_get` /
   `vfu_sgl_put`.
2. **Message-based** -- the server sends `VFIO_USER_DMA_READ` /
   `VFIO_USER_DMA_WRITE` messages over the socket.  APIs:
   `vfu_sgl_read` / `vfu_sgl_write`.

Both modes operate on *guest* DMA regions (IOVA space).  There is no
concept of a *device* memory target backed by real hardware.

## Proposed Changes

The work is divided into four layers.  Each layer can be implemented
and tested independently.

### Layer 0 -- vfio-pci Device Handle

Introduce a helper module (`lib/vfio_pci.c`, `lib/vfio_pci.h`) that
wraps the Linux VFIO kernel interface for opening and mapping a
physical PCI device.  This keeps all `/dev/vfio` interactions
isolated from the rest of the library.

#### Structures

```c
typedef struct vfu_vfio_pci_region {
    int         index;        /* VFIO region index */
    uint64_t    size;         /* region size in bytes */
    uint64_t    offset;       /* mmap offset from VFIO */
    void       *mmap_base;   /* userspace mapping, or NULL */
    size_t      mmap_len;    /* length of mapping */
    uint32_t    flags;        /* VFIO_REGION_INFO_FLAG_* */
} vfu_vfio_pci_region_t;

typedef struct vfu_vfio_pci_dev {
    int         container_fd; /* /dev/vfio/vfio */
    int         group_fd;     /* /dev/vfio/<group> */
    int         device_fd;    /* VFIO device fd */
    int         nr_regions;
    vfu_vfio_pci_region_t *regions;
} vfu_vfio_pci_dev_t;
```

#### Key Functions

```c
vfu_vfio_pci_dev_t *
vfu_vfio_pci_open(const char *sysfs_path);

void
vfu_vfio_pci_close(vfu_vfio_pci_dev_t *dev);

int
vfu_vfio_pci_map_region(vfu_vfio_pci_dev_t *dev, int region_index,
                        uint32_t prot);

void
vfu_vfio_pci_unmap_region(vfu_vfio_pci_dev_t *dev, int region_index);
```

`vfu_vfio_pci_open()` performs the standard VFIO sequence: open
container, set IOMMU type, open group, get device fd, query region
info.  `vfu_vfio_pci_map_region()` calls `mmap()` on the device fd
at the region's offset to produce a userspace pointer to the BAR.

##### Files Changed

- **New**: `lib/vfio_pci.c`, `lib/vfio_pci.h`
- **Modified**: `lib/meson.build` (add source + optional dep)
- **Modified**: `meson_options.txt` (add `vfio-pci` boolean option,
  default false)

##### Risks and Dependencies

- Requires Linux with `vfio-pci` module loaded and an IOMMU group
  configured.
- The code must be compiled with `#include <linux/vfio.h>` (already
  a dependency).
- Testing requires either a real PCI device or a mock; the initial
  test suite should use mocked ioctls.

### Layer 1 -- Attach Device to Context

Add a public API to attach a `vfu_vfio_pci_dev_t` to a
`vfu_ctx_t`, making the physical device's regions available for
data-movement operations.

```c
int
vfu_attach_vfio_pci_dev(vfu_ctx_t *vfu_ctx,
                        vfu_vfio_pci_dev_t *pci_dev);

void
vfu_detach_vfio_pci_dev(vfu_ctx_t *vfu_ctx);

vfu_vfio_pci_dev_t *
vfu_get_vfio_pci_dev(vfu_ctx_t *vfu_ctx);
```

Internally this stores a pointer in `struct vfu_ctx`:

```c
struct vfu_ctx {
    ...
    vfu_vfio_pci_dev_t *vfio_pci_dev;  /* optional */
};
```

##### Files Changed

- **Modified**: `include/libvfio-user.h` (new public functions)
- **Modified**: `lib/private.h` (`vfu_ctx` field)
- **Modified**: `lib/libvfio-user.c` (attach/detach impl, cleanup
  in `vfu_destroy_ctx`)
- **New**: forward declarations in `lib/vfio_pci.h`

##### Testing

- Unit test: attach a mock `vfu_vfio_pci_dev_t`, verify it is
  stored and retrievable.
- Destruction test: verify `vfu_destroy_ctx` cleans up properly.

### Layer 2 -- Data Movement Primitives

These are the core data-movement functions.  They transfer data
between a guest DMA region (identified by SGL) and a vfio-pci
device BAR.

```c
int
vfu_dma_to_device_bar(vfu_ctx_t *vfu_ctx,
                      dma_sg_t *sgl, size_t sg_cnt,
                      int bar_index, uint64_t bar_offset);

int
vfu_device_bar_to_dma(vfu_ctx_t *vfu_ctx,
                      int bar_index, uint64_t bar_offset,
                      dma_sg_t *sgl, size_t sg_cnt);
```

These functions combine the existing SGL infrastructure (to
resolve guest IOVA to a server-side `vaddr`) with the mapped BAR
pointer from Layer 0.

#### Implementation Sketch

Each function:

1. Calls `dma_sgl_get()` to obtain iovecs for the guest memory.
2. Obtains the BAR mapping pointer from the attached
   `vfu_vfio_pci_dev_t`.
3. Performs the copy:
   - `vfu_dma_to_device_bar`: copies each iovec's data into the
     BAR at the given offset, advancing the offset per iovec.
   - `vfu_device_bar_to_dma`: copies from the BAR into each
     iovec.
4. Calls `dma_sgl_put()` to release the guest mapping (and mark
   dirty if this was a write *from* device *to* guest).

For the non-mappable case (guest region has no `vaddr`), the
function falls back to message-based DMA combined with
intermediate buffer copies into/from the BAR.

For the non-mappable BAR case (device region cannot be
`mmap()`ed), the function falls back to `pread()`/`pwrite()` on
the VFIO device fd.

#### MMIO Ordering Considerations

Writes to device BARs are MMIO and may require ordering
guarantees.  On x86 this is naturally provided.  On other
architectures a write-combining or memory barrier may be needed.
The implementation should provide an architecture-dependent
barrier after writes, controllable via a flag.

##### Files Changed

- **Modified**: `include/libvfio-user.h` (new public functions)
- **New or modified**: `lib/vfio_pci.c` (transfer implementation)
- **Modified**: `lib/meson.build` (if new file)

##### Testing

- Unit test with mocked BAR mapping and mocked DMA region.
- Verify correct data copy in both directions.
- Verify fallback to `pread()`/`pwrite()` when BAR is not
  mappable.
- Verify fallback to message-based DMA when guest region has no
  `vaddr`.

### Layer 3 -- Sample and Documentation

#### New Sample: `samples/vfio_pci_dma.c`

A sample server that:

1. Opens a real vfio-pci device (BDF given on the command line).
2. Emulates a simple PCI device with one BAR.
3. On a write to its BAR, copies the written data from guest DMA
   into the real device's BAR (using `vfu_dma_to_device_bar`).
4. On a read, copies data from the real device's BAR into guest
   DMA (using `vfu_device_bar_to_dma`).

This serves as both documentation and an integration test for the
feature.

#### Documentation

Update or create:

- `docs/vfio-pci-data-movement.md` -- this file, expanded with
  final API reference.
- `docs/examples.md` -- add a section on the new sample.
- `README.md` -- mention vfio-pci data movement in the feature
  list.

##### Files Changed

- **New**: `samples/vfio_pci_dma.c`
- **Modified**: `samples/meson.build`
- **Modified**: `docs/examples.md`, `README.md`

## Build System Integration

The feature is gated behind a Meson option:

```meson
option('vfio-pci', type: 'boolean', value: false,
       description: 'Enable vfio-pci device data movement support')
```

When disabled (default), `lib/vfio_pci.c` is not compiled and the
public API functions are not available.  This keeps the default
build free of the VFIO kernel dependency beyond the existing
`<linux/vfio.h>` header usage.

When enabled, the library links against no additional shared
libraries (VFIO is ioctl-based), but the sample may need `-lpthread`
(already a dependency for samples).

## Testing Strategy

| Level | What | How |
|-------|------|-----|
| Unit | `lib/vfio_pci.c` helpers | cmocka, mocked ioctls |
| Unit | `vfu_dma_to_device_bar` / `vfu_device_bar_to_dma` | cmocka, mocked BAR + DMA |
| Python | Attach/detach lifecycle | ctypes wrapper in `test/py/` |
| Functional | End-to-end with real HW | `samples/vfio_pci_dma.c` + QEMU |

Mocked tests can run in CI without hardware.  The functional test
is manual and requires a machine with an available vfio-pci device.

## Dependency Graph

```
Layer 0: vfio_pci.c/h  (standalone, no libvfio-user deps)
  |
  v
Layer 1: vfu_attach_vfio_pci_dev  (depends on Layer 0 + private.h)
  |
  v
Layer 2: vfu_dma_to_device_bar / vfu_device_bar_to_dma
         (depends on Layers 0+1 + dma.h SGL infra)
  |
  v
Layer 3: sample + docs  (depends on Layers 0+1+2)
```

Each layer can be merged independently.  Layer 0 alone is useful
for projects that want to interact with vfio-pci devices outside
of the libvfio-user context.

## API Summary

| Function | Direction | Source | Destination |
|----------|-----------|--------|-------------|
| `vfu_dma_to_device_bar` | Guest -> Device | Guest DMA (SGL) | vfio-pci BAR |
| `vfu_device_bar_to_dma` | Device -> Guest | vfio-pci BAR | Guest DMA (SGL) |
| `vfu_vfio_pci_open` | N/A | Opens VFIO device | N/A |
| `vfu_vfio_pci_close` | N/A | Closes VFIO device | N/A |
| `vfu_vfio_pci_map_region` | N/A | Maps a BAR | N/A |
| `vfu_vfio_pci_unmap_region` | N/A | Unmaps a BAR | N/A |
| `vfu_attach_vfio_pci_dev` | N/A | Attaches to ctx | N/A |
| `vfu_detach_vfio_pci_dev` | N/A | Detaches from ctx | N/A |

## Open Questions

1. **DMA from device to device** -- should we also support
   transferring data between two vfio-pci BARs without going
   through guest DMA?  This could be useful for peer-to-peer
   (P2P) scenarios.  Defer for now; can be added as a Layer 2
   extension.

2. **IOMMU domain sharing** -- if the emulated device needs to
   program the real device's DMA engine, the guest IOVA space
   must be mapped into the physical device's IOMMU domain.  This
   is a significantly larger change (essentially IOMMU-aware DMA
   passthrough) and is out of scope for this initial plan.

3. **Interrupt forwarding** -- the physical device may generate
   interrupts that need to be forwarded to the guest.  This can
   be handled with existing `vfu_irq_trigger()` from a separate
   thread polling the VFIO device's eventfd.  No library changes
   are needed, but the sample should demonstrate this pattern.

4. **Thread safety** -- the library is explicitly not thread-safe.
   If the server uses a background thread for device interaction,
   it must serialize calls through the quiesce mechanism.  The
   plan does not change this contract.

5. **Non-PCI VFIO devices** -- the plan focuses on vfio-pci.
   Supporting `vfio-platform` or `vfio-ap` would follow the same
   pattern but is deferred.
