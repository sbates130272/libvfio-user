# Host MMIO Doorbell via Eventfd

## Overview

`VFIO_USER_HOST_DOORBELL_FD` (opcode 19) is a mechanism that lets a vfio-user
server process write to a QEMU-owned MMIO doorbell register with a single
`eventfd_write()` call, eliminating the per-write socket round-trip that would
otherwise be required.

The mechanism is the *inverse* of `VFIO_USER_DEVICE_GET_REGION_IO_FDS` (opcode
6), which lets the client ask the *server* for server-side doorbell eventfds.
Opcode 19 goes the other direction: the *client* (QEMU) pushes an eventfd for a
host-owned MMIO register to the *server*, so the server can ring it on demand.

## Background

When a device model in QEMU calls `memory_region_add_eventfd()` to register a
doorbell ioeventfd (for example, a virtio queue notify register, or any
IO-ops-backed BAR register), a new eventfd is created on QEMU's side with a
registered AIO handler. Whenever the guest writes the match value to the
doorbell GPA, KVM (or QEMU in TCG mode) signals the eventfd, and QEMU's
handler dispatches the write to the device's IO-ops callback.

Without this mechanism, a vfio-user server that wants to trigger the same
doorbell write must send a `VFIO_USER_REGION_WRITE` socket message for every
write — one round-trip per doorbell.

With `VFIO_USER_HOST_DOORBELL_FD`:

1. During session setup, QEMU creates an eventfd, registers an AIO handler
   (`address_space_write(gpa, datamatch, size)`), and sends the fd to the server
   via `HOST_DOORBELL_FD` with `SCM_RIGHTS`.
2. The server receives the fd and stores it.
3. To ring the doorbell: `eventfd_write(fd, 1)` — one syscall, no socket.
4. QEMU's AIO handler wakes on the iothread and calls `address_space_write()`,
   which dispatches through the flatview to the device's IO-ops handler.

## Wire protocol

**Opcode:** `VFIO_USER_HOST_DOORBELL_FD = 19`  
**Direction:** client → server  
**Flags:** `VFIO_USER_F_NO_REPLY` (fire-and-forget, no reply expected)  
**Ancillary data:** one fd sent via `SCM_RIGHTS` in the same `sendmsg()`

Payload structure (`vfio_user_host_doorbell_fd_t`, 16 bytes):

| Field       | Type     | Description |
|-------------|----------|-------------|
| `gpa`       | uint64_t | Guest physical address of the MMIO doorbell |
| `datamatch` | uint64_t | Value QEMU will write to `gpa` when the fd fires |
| `size`      | uint32_t | Access width in bytes (1, 2, 4, or 8) |
| `flags`     | uint32_t | Reserved, must be 0 |

## Server-side API

### Registering the callback

Call `vfu_setup_host_doorbell_cb()` before `vfu_realize_ctx()`:

```c
#include <libvfio-user.h>

static void my_doorbell_cb(vfu_ctx_t *ctx, uint64_t gpa,
                           uint64_t datamatch, uint32_t size, int fd)
{
    /*
     * Store (gpa, datamatch, size, fd) for later use.
     * Call eventfd_write(fd, 1) whenever you need QEMU to execute:
     *   address_space_write(gpa, datamatch, size)
     * The callee owns fd — close it when no longer needed.
     */
    my_device.doorbell_gpa  = gpa;
    my_device.doorbell_data = datamatch;
    my_device.doorbell_size = size;
    my_device.doorbell_fd   = fd;  /* keep alive; close on device teardown */
}

int main(void)
{
    vfu_ctx_t *ctx = vfu_create_ctx(...);
    ...
    vfu_setup_host_doorbell_cb(ctx, my_doorbell_cb);
    vfu_realize_ctx(ctx);
    ...
}
```

### Triggering a doorbell write

```c
static void trigger_doorbell(struct my_device *dev)
{
    uint64_t val = 1;
    if (write(dev->doorbell_fd, &val, sizeof(val)) < 0) {
        perror("eventfd_write");
    }
    /* QEMU's AIO handler wakes and calls address_space_write() */
}
```

### API reference

```c
/**
 * vfu_host_doorbell_cb_t
 *
 * Called when the client pushes a VFIO_USER_HOST_DOORBELL_FD message.
 *
 * @ctx:        the server context
 * @gpa:        guest physical address of the MMIO doorbell
 * @datamatch:  value the client writes to @gpa when the fd is rung
 * @size:       access width in bytes (1/2/4/8)
 * @fd:         the eventfd — write to it to trigger the MMIO write in QEMU.
 *              The callee takes ownership; close it when done.
 */
typedef void (*vfu_host_doorbell_cb_t)(vfu_ctx_t *ctx, uint64_t gpa,
                                       uint64_t datamatch, uint32_t size,
                                       int fd);

/**
 * vfu_setup_host_doorbell_cb
 *
 * Register the callback for VFIO_USER_HOST_DOORBELL_FD messages.
 * Must be called before vfu_realize_ctx().
 *
 * Returns 0 on success, -1 with errno set on failure.
 */
int vfu_setup_host_doorbell_cb(vfu_ctx_t *vfu_ctx, vfu_host_doorbell_cb_t cb);
```

## When doorbell fds are sent

The client (QEMU) pushes a `HOST_DOORBELL_FD` message for each ioeventfd that
a device model registers via `memory_region_add_eventfd()` with
`match_data = true`. Common sources:

- **virtio devices** — queue notify registers (e.g. `virtio-rng-pci`,
  `virtio-net-pci`, `virtio-blk-pci`) register per-queue doorbells when the
  guest driver initialises the device.
- **pci-testdev** — registers a test doorbell at BAR0 offset 0 (value `0xFA`,
  1-byte) unconditionally at realize time, making it useful for testing without
  requiring a guest virtio driver.
- **Any device** that calls `memory_region_add_eventfd(..., match_data=true,
  ...)` on a BAR or MMIO region in the guest physical address space.

Only `match_data = true` doorbells are forwarded; wildcard ioeventfds
(`match_data = false`) are not, because they do not carry a fixed write value.

## Comparison with related mechanisms

| Mechanism | Direction | Per-write cost | Requirement |
|---|---|---|---|
| `REGION_WRITE` socket message | server → QEMU | 1 socket round-trip | Always available |
| `DMA_WRITE` (guest RAM) | server → QEMU → guest RAM | 1 socket round-trip | GPA must be in DMA space |
| Peer memfd BAR (`vfu_sgl_get`) | peer → target directly | 0 (memcpy) | Target BAR must be memfd-backed |
| **Host doorbell fd** (this doc) | server → QEMU MMIO | 1 eventfd write (kernel-only) | Device must use `memory_region_add_eventfd()` |
| pci-mmio-bridge ring | server → QEMU ring → any MMIO | 1 memcpy + poll delay | Requires bridge device in guest |
