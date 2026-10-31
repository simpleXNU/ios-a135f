#!/usr/bin/env python3
"""Build the minimal Apple Device Tree that the A135F xnu needs to boot.

Target: Samsung Galaxy A13 (SM-A135F), Exynos850. Only what the early boot
path actually reads; every MMIO address comes from the mainline
exynos850.dtsi / A13 device tree. Usage:  mkadt.py OUT.adt [--dump] [--qemu] [--ram 3gb|4gb] [--touch-fw FILE]
"""
import hashlib, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adt  # noqa: E402

# Kernel-managed RAM: the 2 GiB block at 0x8_8000_0000 (last <reg> entry of the
# memory node). The low banks (0x80000000.., 0xc0000000.., 0xe0d00000..) are
# left alone: the shim and the eMMC DMA window (0xc8000000, see /a135f-mmc) use them.
PHYS_BASE = 0x880000000
# RAM variants (last <reg> entry of the memory node in the board dts):
#   3gb: exynos850-a135f-3gb.dts  <0x8 0x80000000 0x0 0x40000000>  -> 1 GiB
#   4gb: exynos850-a135f-4gb.dts  <0x8 0x80000000 0x0 0x80000000>  -> 2 GiB
RAM_VARIANTS = {'3gb': 0x40000000, '4gb': 0x80000000}
RAM_DEFAULT = '3gb'


def ram_variant():
    """--ram 3gb|4gb or $A135F_RAM (default 3gb)."""
    v = os.environ.get('A135F_RAM', RAM_DEFAULT)
    if '--ram' in sys.argv:
        v = sys.argv[sys.argv.index('--ram') + 1]
    if v not in RAM_VARIANTS:
        sys.exit('--ram must be one of: ' + ', '.join(RAM_VARIANTS))
    return v


MEM_SIZE = RAM_VARIANTS[ram_variant()]
# --qemu keeps a small low-memory layout for QEMU virt (tools/qemu-a135f.py)
QEMU_PHYS_BASE = 0xb0000000
QEMU_MEM_SIZE = 0x40000000   # ends at 0xf0000000, below the framebuffer at 0xfa000000
# /arm-io children are relative to this; it must not be 0 (serial_init bails)
SOC_BASE = 0x10000000

# ARM GIC-400 (GICv2): distributor 0x12a01000, CPU interface 0x12a02000
GICD, GICD_SIZE = 0x12a01000, 0x1000
GICC, GICC_SIZE = 0x12a02000, 0x2000
# simple-framebuffer left by the bootloader: 1080x2408 a8r8g8b8
FB, FB_SIZE = 0xfa000000, 0xa00000
A135F_ECID = 0x4131333546000001  # 'A135F' + 1, synthetic
FB_W, FB_H, FB_STRIDE = 1080, 2408, 1080 * 4
# Internal eMMC: DW-MSHC mmc_0 (bsd/dev/a135f_mmc.c); DMA window below 4 GiB, unmanaged by xnu
MMC, MMC_SIZE = 0x12100000, 0x2000
MMC_DMA_BASE = 0xc8000000
# Exynos850 arch timer (oscclk, the dts sets clock-frequency = 26 MHz)
TIMEBASE = 26000000
VTIMER_INTID = 27
# QEMU virt (-M virt,gic-version=2 -cpu cortex-a55): GICD/GICC and the generic timer's CNTFRQ
QEMU_GICD, QEMU_GICC, QEMU_TIMEBASE = 0x08000000, 0x08010000, 62500000
QEMU_VTIMER_INTID = 27

BOOT_MANIFEST_HASH = hashlib.sha384(b'a135f boot manifest').digest()

# The iOS userland is the iPhone X (iPhone10,3 / D22AP) IPSW. MobileGestalt
# derives product type, device class, home-button-less UI, artwork etc. from
# the root identity and /product of the real D22AP device tree. That tree is
# an Apple artifact (kept out of git): extract it from the IPSW with
#   tools/remotezip.py URL DeviceTree.d22ap.im4p DIR
#   ipsw img4 im4p extract DIR/Firmware/all_flash/DeviceTree.d22ap.im4p -o d22ap.adt
D22AP_ADT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../research/ipsw-x/d22ap.adt')


def d22ap_product(path=D22AP_ADT):
    """/product properties of the real D22AP tree, without the syscfg
    placeholders iBoot would fill in (flag bit set) and the node bookkeeping."""
    if not os.path.exists(path):
        print(f'warning: {path} missing, no /product node', file=sys.stderr)
        return None
    root, _ = adt.parse(open(path, 'rb').read())
    for c in root.children:
        if dict((k, v) for k, f, v in c.props).get('name') == b'product\0':
            return [(k, v) for k, f, v in c.props if not f and k not in ('name', 'AAPL,phandle')]
    raise SystemExit(f'{path}: no /product node')

u32 = lambda *v: struct.pack('<%dI' % len(v), *v)
u64 = lambda *v: struct.pack('<%dQ' % len(v), *v)
cstr = lambda s: s.encode() + b'\0'


def node(name, props=(), children=()):
    n = adt.Node()
    n.props = [('name', 0, cstr(name))] + [(k, 0, v) for k, v in props]
    n.children = list(children)
    return n


def load_touch_firmware():
    """Extract the ROMBOOT_APP_CODE area from the vendor TCM image (td4375_a13_boe.bin) and
    verify its CRC32, exactly as ovt_tcm_find_romboot_fw() does. Path: --touch-fw FILE or
    $A135F_TOUCH_FW. Returns the raw application bytes, or None (touch then needs an already
    running controller)."""
    import os, zlib
    path = os.environ.get('A135F_TOUCH_FW')
    if '--touch-fw' in sys.argv:
        path = sys.argv[sys.argv.index('--touch-fw') + 1]
    if not path:
        print('warning: no --touch-fw: touch only works if the controller already runs firmware', file=sys.stderr)
        return None
    img = open(path, 'rb').read()
    le4 = lambda o: struct.unpack_from('<I', img, o)[0]
    if le4(0) != 0x4818472b:
        sys.exit('touch firmware: bad image magic')
    off = 8
    for _ in range(le4(4)):
        addr = le4(off); off += 4
        if addr + 36 > len(img) or le4(addr) != 0x7c05e516:
            continue
        if not img[addr + 4:addr + 20].startswith(b'ROMBOOT_APP_CODE'):
            continue
        length, want = le4(addr + 28), le4(addr + 32)
        data = img[addr + 36:addr + 36 + length]
        if len(data) != length or (zlib.crc32(data) & 0xffffffff) != want:
            sys.exit('touch firmware: ROMBOOT_APP_CODE checksum mismatch')
        print(f'touch firmware: {length} bytes from {path}', file=sys.stderr)
        return data
    sys.exit('touch firmware: no ROMBOOT_APP_CODE area')


def build(seed=None, qemu=False, highmem=True):
    """qemu=True: same tree, but with QEMU virt's GIC addresses and CNTFRQ (tools/qemu-a135f.py).
    highmem is kept for command-line compatibility; the phone layout is the only one."""
    phys_base, mem_size = (QEMU_PHYS_BASE, QEMU_MEM_SIZE) if qemu else (PHYS_BASE, MEM_SIZE)
    gicd, gicc, timebase = (QEMU_GICD, QEMU_GICC, QEMU_TIMEBASE) if qemu else (GICD, GICC, TIMEBASE)
    soc_base = QEMU_GICD if qemu else SOC_BASE      # QEMU's GIC sits below the Exynos SoC window
    seed = seed if seed is not None else os.urandom(64)
    phandle = iter(range(1, 1000))
    ph = lambda: ('AAPL,phandle', u32(next(phandle)))

    chosen = node('chosen', [
        ph(),
        ('dram-base', u64(phys_base)),
        ('dram-size', u64(mem_size)),
        ('random-seed', seed),
        ('debug-enabled', u32(1)),
        ('firmware-version', cstr('a135f-shim')),
        ('system-firmware-version', cstr('a135f-shim')),
        # iBoot's SHA-384 of the boot manifest; /sbin/mount -P 1 exits unless
        # it exists (bind mounts /private/preboot/<hex>/usr/standalone/firmware,
        # tolerating a missing directory). Fixed stand-in value.
        ('boot-manifest-hash', BOOT_MANIFEST_HASH),
        # libimg4 (cryptexd) aborts "failed to get crypto algorithm" without
        # it; real A11+ device trees say sha2-384.
        ('crypto-hash-method', cstr('sha2-384')),
        # MobileGestalt's screen class: display-scale (u32, default 0) comes
        # from /chosen, the pixel size from IOMobileFramebuffer. Scale 0 makes
        # SpringBoard's FBSDisplayMode "infxinf@0x" invalid. 1080x2400 @3x =
        # 360x800 points (like iPhone X class devices).
        ('display-scale', u32(3)),
        # XNU derives IOPlatformUUID (gethostuuid) from these on arm; without
        # them CFPreferences cannot use kCFPreferencesCurrentHost (no ByHost
        # files), so e.g. DataMigration never records LastSystemVersion and
        # migrates on every boot. Synthetic ECID (not the phone's); chip-id
        # of the A11 that the iPhone X userland expects.
        ('unique-chip-id', struct.pack('<Q', A135F_ECID)),
        ('chip-id', u32(0x8015)),
        # Security status as on a dev-fused, undemoted board (tuple 0,0,1,1):
        # MobileGestalt reads these from /chosen, and mobileactivationd's
        # DeviceType then takes its native "Hactivation" branch (state
        # Activated without an activation record; no SEP, no baseband here).
        ('certificate-production-status', u32(0)),
        ('effective-production-status-ap', u32(0)),
        ('certificate-security-mode', u32(1)),
        ('effective-security-mode-sep', u32(1)),
    ], [node('memory-map', [ph()]),
        # Local boot policy (kern_csr.c): SIP relaxed like `csrutil disable`
        # (lp-sip0: untrusted kexts, unrestricted FS, task_for_pid, kernel
        # debugger, dtrace, nvram), lp-sip1: unauthenticated (unsealed) root.
        # The iOS shared cache is in no trust cache, so dyld can only map it
        # with CSR_ALLOW_UNRESTRICTED_FS (vm_unix.c shared region checks).
        node('asmb', [ph(), ('lp-sip0', u64(0x6f)), ('lp-sip1', u32(1))])])

    # A135F SMP: all eight Cortex-A55 in physical-core order, presented as one
    # homogeneous cluster (MPIDR: cluster 0 = 0x000-0x003, cluster 1 =
    # 0x100-0x103, as in exynos850.dtsi). The kernel defaults to one
    # and uses cpus=N to select an initial prefix (N <= 8).
    # QEMU retains its original single-CPU tree: its MPIDRs differ from a135f.
    cpu_nodes = []
    for core in range(1 if qemu else 8):
        cpu_nodes.append(node(f'cpu{core}', [
            ph(),
            ('device_type', cstr('cpu')),
            ('compatible', cstr('arm,cortex-a55') + cstr('ARM,v8')),
            ('reg', u32(((core // 4) << 8) | (core % 4))),  # actual MPIDR Aff1:Aff0, NOT logical ID
            ('cpu-id', u32(core)),   # descriptive: XNU assigns IDs by DT order
            ('cluster-id', u32(0)),  # descriptive; see ml_parse_cpu_topology
            ('cluster-type', u32(0)),  # homogeneous CLUSTER_TYPE_SMP, not AMP
            ('die-cluster-id', u32(0)),
            ('cluster-core-id', u32(core)),
            ('l2-cache-id', u32(core)),  # distinct per-core L2 nodes in live FDT
            ('l3-cache-id', u32(0)),     # treated as one shared L3
            ('state', cstr('running' if core == 0 else 'stopped')),
            ('timebase-frequency', u32(timebase)),
            ('clock-frequency', u32(1846000000)),  # top OPP in exynos850.dtsi; informational
            ('bus-frequency', u32(100000000)),
            ('memory-frequency', u32(1650000000)),  # informational
            ('peripheral-frequency', u32(timebase)),
            ('fixed-frequency', u32(timebase)),
        ]))
    cpus = node('cpus', [ph(), ('#address-cells', u32(1)),
                         ('#size-cells', u32(0))], cpu_nodes)

    gic = node('gic', [
        ph(),
        ('compatible', cstr('arm,gic-400')),
        ('interrupt-controller', b''),
        ('virtual-timer-intid', u32(QEMU_VTIMER_INTID if qemu else VTIMER_INTID)),
        ('reg', u64(gicd - soc_base, GICD_SIZE, gicc - soc_base, GICC_SIZE)),
    ])
    arm_io = node('arm-io', [
        ph(),
        ('device_type', cstr('a135f-io')),
        ('compatible', cstr('a135f-io') + cstr('arm-io')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
        ('ranges', u64(0, soc_base, 0x100000000 - soc_base)),
    ], [gic,
        # No hardware: watchdogd checks IODeviceTree:/arm-io/wdt for no-pmu and
        # otherwise crashes trying to set the PMU boot stage (customer builds
        # ignore the -wdt_no_pmu boot-arg).
        node('wdt', [ph(), ('no-pmu', u32(1))]),
        # Main display: IOMobileFramebuffer.framework counts /arm-io children
        # with this device_type and reads the canvas size from the node
        # (the presented iPhone X size, see A135FFramebuffer.cpp);
        # the kernel's AppleCLCD matches the compatible string.
        node('disp0', [
            ph(),
            ('device_type', cstr('display-subsystem')),
            ('compatible', cstr('a135f,display')),
            ('canvas-width', u32(1125)),
            ('canvas-height', u32(2436)),
        ])])

    fbcon = node('a135f-fbcon', [
        ph(),
        ('reg', u64(FB, FB_SIZE)),
        ('width', u32(FB_W)),
        ('height', u32(FB_H)),
        ('stride', u32(FB_STRIDE)),
    ])

    # bsd/dev/a135f_mmc.c: the DW-MSHC the bootloader leaves running (absolute physical reg),
    # plus the below-4GiB DMA window (descriptors + 1 MiB bounce buffer) that xnu does not manage.
    mmc = node('a135f-mmc', [
        ph(),
        ('compatible', cstr('samsung,exynos7-dw-mshc')),
        ('reg', u64(MMC, MMC_SIZE)),
        ('dma-base', u64(MMC_DMA_BASE)),
    ])

    # bsd/dev/a135f_touch.c: TD4150 zeroflash application firmware (raw ROMBOOT_APP_CODE area)
    touch_nodes = []
    fw = load_touch_firmware()
    if fw and not qemu:
        touch_nodes = [node('a135f-touch', [
            ph(),
            ('compatible', cstr('ovt,td4150')),
            ('firmware', fw),
        ])]

    # pmap_compute_max_asids requires the node even when no overrides are set.
    # An absent pmap-max-asids property selects XNU's own MAX_ASIDS default.
    # No effaceable storage: keybagd then reads /private/var/keybags/systembag.kb
    # as a plain plist instead of decrypting it with the effaceable key.
    defaults = node('defaults', [ph(), ('no-effaceable-storage', u32(1))])

    # MobileGestalt HomeButtonType = IODeviceTree:/buttons home-button-type
    # (D22AP: 2, no Home button; absent = 0 = Home-button device).
    buttons = node('buttons', [
        ph(),
        ('button-names', cstr('volup') + cstr('voldown') + cstr('hold')),
        ('home-button-type', u32(2)),
        ('opposed-power-vol-buttons', u32(1)),
    ])

    product_props = d22ap_product()
    product = [node('product', [ph()] + product_props)] if product_props else []

    # Identity of the iPhone X whose userland runs here: compatible[0] is
    # hw.model/hw.target (D22AP), model is hw.machine/hw.product
    # (iPhone10,3). 'samsung,a135f' stays for A135FPlatformExpert matching.
    return node('device-tree', [
        ph(),
        ('compatible', cstr('D22AP') + cstr('iPhone10,3') + cstr('samsung,a135f') + cstr('samsung,exynos850') + cstr('AppleARM')),
        ('model', cstr('iPhone10,3')),
        ('target-type', cstr('D22')),
        ('target-sub-type', cstr('D22AP')),
        ('manufacturer', cstr('Apple Inc.')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
    ], [chosen, defaults, cpus, arm_io, fbcon, buttons] + product + ([] if qemu else [mmc]) + touch_nodes)


if __name__ == '__main__':
    root = build(qemu='--qemu' in sys.argv, highmem='--highmem' in sys.argv)
    blob = adt.serialize(root)
    open(sys.argv[1], 'wb').write(blob)
    if '--dump' in sys.argv:
        adt.dump(adt.parse(blob)[0])
    print(f'{sys.argv[1]}: {len(blob)} bytes, RAM {ram_variant()} (0x{PHYS_BASE:x} + 0x{MEM_SIZE:x})', file=sys.stderr)
