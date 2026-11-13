# `vfu_mmio_write`: DMA_WRITE to any guest physical address

A vfio-user server normally writes to client memory with `vfu_sgl_get()` /
`vfu_sgl_write()`, and both require the target address to be in the server's
DMA table. That table is filled by `VFIO_USER_DMA_MAP` messages, and QEMU only
sends those for guest RAM. The address of another device's MMIO register (for
example a doorbell) is never in the table, so `vfu_addr_to_sgl()` fails with
`ENOENT`.

`vfu_mmio_write(ctx, gpa, data, size)` skips the table lookup and sends a
`VFIO_USER_DMA_WRITE` for `gpa` directly:

```c
/* Write 1 byte (value 0xFA) to pci-testdev's BAR0 MMIO doorbell */
uint8_t val = 0xFA;
int ret = vfu_mmio_write(ctx, 0xfebe5000, &val, sizeof(val));
if (ret < 0) {
    perror("vfu_mmio_write");
}
```

It returns 0 on success, or -1 with `errno` set. An error reply from the client
(for example `ENODEV`) is returned as `errno`.

## What the client does with it

`DMA_WRITE` is handled entirely by the client, so what the write reaches is
client behavior, not a libvfio-user property. With stock QEMU,
`vfio_user_dma_write()` calls `pci_dma_write()` on the writing device, which
goes through the device's bus-master address space:

- Guest RAM is written into backing memory.
- An MMIO address is dispatched to the target device's `MemoryRegionOps.write`
  handler (device BAR registers, doorbells, and so on), whether the target is a
  QEMU built-in device or another vfio-user server (as a `REGION_WRITE`).
- The bus-master address space is only enabled while the writing device has PCI
  Bus Master Enable set. With it clear, the write fails and QEMU replies
  `ENODEV`. The guest (or `setpci`) must enable bus mastering on the writing
  device first.

## Cost and caveats

- One socket round trip per call: the request is synchronous and waits for the
  reply. Use it for doorbells and register pokes, not bulk data.
- Do not call it from within a `vfu_run_ctx()` callback if the client may
  simultaneously send a `REGION_READ` or similar command back. That risks a
  single-socket deadlock (see issue #279).
- `size` must not exceed the client's maximum data transfer size, and `1`, `2`,
  `4` or `8` bytes is recommended for MMIO alignment.
- QEMU splits an unaligned access to a register, so it can arrive at the
  target's handler as several smaller writes.
