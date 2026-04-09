qemu usage walkthrough
======================

In this walk-through, we'll use a buildroot image VM along with the
[gpio sample server](../samples/gpio-pci-idio-16.c) to emulate a very simple GPIO
device.

Building qemu
-------------

You will need QEMU 10.1.1 or later. Let's build it:

```
cd ~/src/
curl -L https://download.qemu.org/qemu-10.1.1.tar.xz | tar xJf -
cd ~/src/qemu-10.1.1

./configure --enable-kvm --enable-vnc --target-list=x86_64-softmmu --enable-trace-backends=log --enable-debug
make -j
```

Starting the server
-------------------

Start the `gpio` server process:

```
rm -f /tmp/vfio-user.sock
./build/samples/gpio-pci-idio-16 -v /tmp/vfio-user.sock &
```

Starting the client
-------------------

Our client in this case will be a Linux image with the pci-idio-16 kernel
driver. Let's grab the images:

```
curl https://github.com/mcayland-ntx/libvfio-user-test/raw/refs/heads/main/images/bzImage
curl https://github.com/mcayland-ntx/libvfio-user-test/raw/refs/heads/main/images/rootfs.ext2
```

Now use the qemu you've built to start the VM as follows:

```
~/src/qemu/build/qemu-system-x86_64 \
    -accel kvm \
    -nographic \
    -display none \
    -m 1G \
    -net none \
    -kernel ./bzImage \
    -hda ./rootfs.ext2 \
    -append "console=ttyS0 root=/dev/sda" \
    -device '{"driver":"vfio-user-pci","socket":{"path": "/tmp/vfio-user.sock", "type": "unix"}'
```

Log in to this VM as root (no password). We should be able to interact with the
device:

```
lspci -k # confirm the pci-idio-16 driver is loaded
gpioinfo
gpioset -c gpiochip0 -t 0 OUT0=1
gpioget -c gpiochip0 --numeric OUT0
```

and the server should output something like:

```
gpio: region2: read 0 from (0:1)
gpio: region2: wrote 0x1 to (0:1)
gpio: region2: read 0 from (0:1)
```

QEMU changes for DMA region access backends
-------------------------------------------

The new `vfu_setup_device_dma_region_access()` API is server-side only. QEMU
does not need to understand this API directly, but it does need to provide DMA
map/unmap behavior that lets the server select the backend path for the right
ranges.

At a high level, QEMU should be updated as follows:

1. Define backend-target DMA address ranges
   - Reserve one or more IOVA windows for memory that should be served by a
     custom backend (for example, memory exposed by a vfio-pci BAR mapping).
   - Keep these ranges disjoint from normal guest RAM ranges that already use
     standard `VFIO_USER_DMA_MAP` handling.

2. Emit `VFIO_USER_DMA_MAP`/`VFIO_USER_DMA_UNMAP` for those ranges
   - Ensure the vfio-user client side in QEMU sends map and unmap events for
     backend-target ranges at the same lifecycle points used for guest RAM
     ranges (creation, invalidation, reset, and teardown).
   - Preserve protection bits so the server can enforce write permissions in
     backend callbacks.

3. Carry enough information for backend selection
   - Today, backend routing can be done by server policy over IOVA ranges
     (as shown in `samples/dma-region-access.c`).
   - For production use, QEMU and the server should agree on a stable contract
     for identifying backend-target ranges. A protocol capability or map
     metadata extension is cleaner than relying only on implicit IOVA windows.

4. Handle migration and dirty tracking ownership explicitly
   - If writes are executed in the server backend path, define whether QEMU or
     the backend is the source of truth for dirty accounting for those ranges.
   - Ensure stop-and-copy and precopy transitions keep map state and dirtiness
     semantics consistent.

5. Add end-to-end coverage in QEMU tests
   - Add tests that verify QEMU issues map/unmap for backend-target ranges.
   - Add functional coverage for DMA reads/writes hitting backend-target
     ranges and for teardown/reset behavior.

Notes:

- This repository currently documents and tests with QEMU 10.1.1 or later.
- See [memory mapping notes](./memory-mapping.md) for current DMA semantics.
- See [backend sample](../samples/dma-region-access.c) for a concrete resolver
  and callback implementation.
