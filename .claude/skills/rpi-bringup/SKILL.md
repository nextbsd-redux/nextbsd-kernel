---
name: rpi-bringup
description: Bringing NextBSD up on a Raspberry Pi 5-family board (Pi 5 B, Pi 500/500+, CM5 — BCM2712 + RP1). Getting a console on a board that fails silently, running unattended boot tests over tryboot without needing a human to power cycle, and the register/patch/device-tree traps that each cost a boot cycle. Use when booting NextBSD on real Pi hardware, writing a NEXTBSD-<board> config, or touching bcm2838_pci, bcm2712_mip, or rp1.
---

# Raspberry Pi board bring-up

Hard-won on the Pi 500+. The Pi 5 B and CM5 are the same BCM2712 + RP1, so
the kernel work carries over; only the console routing and the DTB name differ.

## The one thing to internalise

**Everything on this board fails as silence.** These all present identically —
a black screen and nothing on the wire:

- the kernel was never entered (wrong file, wrong header, wrong load address)
- it crashed in `locore` before any C ran
- it is running perfectly and has no console
- a driver just reset the controller the console lived behind
- an MSI was allocated fine and routed to a SPI nobody listens on

You cannot guess between these. Most of a bring-up is building the instruments
that tell them apart, and doing that *first* is faster than it feels.

Corollary: assert everything you can in CI and in the build, because a check
that fails loudly on the host is worth ten boots. Verify the `Image` magic, the
FDT magic, the config's arch — none of that costs a power cycle.

## The unattended test harness

This is the highest-value part. It is what lets a bring-up run while the human
who owns the board is asleep.

### tryboot: a one-shot boot that reverts itself

```sh
# on the Pi, with the FAT boot partition at /boot/firmware
cp config.txt tryboot.txt.new     # start from the working config, then edit
mv tryboot.txt.new tryboot.txt
reboot '0 tryboot'
```

The firmware reads `tryboot.txt` **instead of** `config.txt` for exactly one
boot. Any failure — and any subsequent power cycle — comes back up on
`config.txt`. `config.txt` is never touched, so the fallback is *structural*
rather than something you have to remember to restore.

### BOOT_ORDER so the default is always a working OS

`rpi-eeprom-config` holds `BOOT_ORDER`, whose nibbles are read **LSB-first**:

| nibble | means |
|---|---|
| `0` | stop |
| `1` | SD card |
| `2` | network |
| `4` | USB mass storage |
| `6` | NVMe |
| `f` | restart the sequence |

So `0xf461` = SD → NVMe → USB → restart, and `0xf614` = USB → SD → NVMe →
restart (the one to use when testing from a stick, since an absent stick just
falls through to the installed OS).

**`rpi-eeprom-config --apply` rebuilds from the newest bootloader image on
disk.** Changing `BOOT_ORDER` therefore silently upgrades the firmware as a
side effect. Record the before/after of `vcgencmd bootloader_version`, and say
so — two variables moved, not one.

### Recovering a board stuck at `mountroot>` with no hands on it

This is what closes the loop. At the `mountroot>` prompt, send a bare newline:

```
mountroot> <CR>
```

Manual input aborts → the mount genuinely fails → the kernel panics →
it auto-reboots → `tryboot` is already spent → the board comes up on
`config.txt` into the working OS → ssh answers again.

So a kernel that reaches `mountroot` and no further is fully recoverable over
the wire, with no power cycle.

**The limit:** a kernel that *hangs* — no panic, no reset — still needs a
physical power cycle. Budget for it, and stop testing on hardware the moment
you are hanging rather than panicking; work out why on the host first. A hung
board costs a human interruption every attempt, and that is the resource that
actually runs out.

### Watching from the host

The Debug Probe is a USB device on the **Mac**, not on the Pi, so it stays
enumerated no matter what the board does. No reconnect logic is needed.

macOS resets the line discipline when the fd closes, so a logger has to hold
the device open for the life of the capture:

```sh
exec 3<> /dev/cu.usbmodem*
stty -f /dev/cu.usbmodem* 115200 raw -echo
cat <&3 | tee boot.log
```

Only one reader at a time. If a human has `screen` attached, they must quit it
(`C-a k`) first — otherwise the capture reads nothing and looks exactly like a
dead board.

## Which UART, and the trap

Two consoles exist on a Pi 5-family board, and the choice is load-bearing.

| | 40-pin header | 3-pin JST-SH on the board |
|---|---|---|
| what | RP1 UART0 | UART10, inside the BCM2712 |
| address | `0x1c_0003_0000` (firmware map) | `0x10_7d00_1000` |
| access | no disassembly | usually needs the board out of the case |
| survives PCIe reset | **no** | yes |
| shows boot ROM / SDRAM training | no | yes |

RP1 is a PCIe endpoint. The 40-pin UART is *behind* that link, so **it dies the
instant a driver takes ownership of the PCIe controller** — and a working
driver then looks exactly like a dead one. This will happen to you at the worst
possible moment, on the first boot where PCIe actually comes up.

**Rule: 40-pin until you touch PCIe, then move to the internal connector. Find
that connector before starting the PCIe work, not during it.**

Find it physically before you need it, and look at the board rather than at
photographs — it is small, unlabelled, and easy to mistake for a crystal.

## Diagnosing a mute kernel, in order

Three instruments. They answer different questions and are **not**
interchangeable — reach for them in this order.

**1. Assembly markers in `locore.S`.** Needs no C, no console driver, no page
tables, no stack. Poke the UART data register directly. Answers only "was the
kernel entered at all, and how far did it get before the MMU came up?" — which
is the question nothing else can answer.

**2. `SOCDEV_PA` + `EARLY_PRINTF`.** Gets you `printf` early. Note the ceiling:
it covers `printf` **only up to `cninit()`**, which drops `early_putc`
unconditionally. A boot that prints and then goes quiet at exactly that point
is not a hang — it is this.

```
options 	SOCDEV_PA=0x107d001000
options 	EARLY_PRINTF=pl011
```

**3. `hw.uart.console`.** A console for the whole boot. Route (a) has no
loader, so there is no `loader.conf` — the only way to set a tunable is
config(5)'s `env` directive with a compiled-in file.

## The boot path (route (a))

The BCM2712 bootloader lives in EEPROM. It reads `config.txt` from the first
FAT partition, loads and patches the DTB named there, loads the kernel, and
enters it **at EL2 with `x0` = the physical address of the FDT**.

- `kernel.bin` is the kernel ELF-stripped with a 64-byte arm64 `Image` header
  prepended. `kernel8.img` is a copy of it under the name the firmware wants.
  `/boot/kernel/kernel` is an ELF and the firmware will jump straight into its
  header.
- Magic is `0x644d5241` at **offset 56** — bytes `41 52 4d 64`. Check it in CI.
  Trusting a build's exit code has cost a boot cycle before.
- **No loader, no UEFI ⇒ nothing can be `kldload`ed at boot.** Every
  boot-critical driver must be compiled in.
- **An ISO can never boot this board.** The firmware needs a FAT partition
  holding `config.txt`; ISO 9660 has neither, so it is never opened. This is
  not a gap to fill later.
- A 2712 boot partition needs exactly `config.txt`, a DTB, and the kernel.
  `bootcode.bin`, `start*.elf` and `fixup*.dat` are the pre-2712 VideoCore
  chain; Raspberry Pi OS ships them only because one partition serves every
  model back to the Pi 2.

## The board config pattern

`NEXTBSD-<board>` includes the generic config and then subtracts and adds:

```
include NEXTBSD
ident		NEXTBSD-RPI500
options 	SOCDEV_PA=0x107d001000
options 	EARLY_PRINTF=pl011
env		"rpi500.env"
nooptions	SOC_BRCM_BCM2837
nooptions	SOC_BRCM_BCM2838
options 	SOC_BRCM_BCM2712
```

The `nooptions` lines matter: wrong-SoC drivers match by accident on compatible
strings the vendor tree shares across generations.

The env file is one line, and **every tag in it is load-bearing**:

```
hw.uart.console="mm:0x107d001000,br:115200,dt:pl011,rs:2,rw:4,xo:0"
```

| tag | why it cannot be omitted |
|---|---|
| `dt:pl011` | `uart_cpu_getdev()` defaults to `uart_ns8250_class` |
| `rs:2,rw:4` | `uart_getenv()` hardcodes `regshft=0, regiowidth=1` and does **not** inherit them from the class |
| `xo:0` | sets `rclk_guess` |

Drop any one and you get a console that initialises cleanly and emits nothing,
or line noise — both of which read as a dead kernel.

Also: `enable_uart=1` in `config.txt` is still required even though the console
is chosen by the compiled-in tunable. It pins the VPU core clock; without it
the divisor moves with clock scaling and the console degrades to noise partway
through boot.

## Traps, each of which cost a boot cycle

**Register names in the existing driver lie.** They were named for an earlier
SoC and the meaning moved. Read the datasheet semantics, never the FreeBSD
name:

| name says | actually is |
|---|---|
| `REG_DMA_CONFIG` | MISC_CTRL — and its "enable" bits are `SCB_ACCESS_EN｜CFG_READ_UR_MODE`, without which the controller never starts |
| `BRIDGE_DISABLE_FLAG` | PERST — moved to `MISC_PCIE_CTRL` on 2712 with **inverted** sense, so the device sits in reset |
| `REG_BRIDGE_GISB_WINDOW` | `RC_BAR1_CONFIG_LO` |

**A patch series must be verified as a series.** Each patch applying to a
pristine tree proves nothing when an earlier patch rewrites the same lines.
Apply the whole series to a fresh checkout, with absolute paths, and inspect
the resulting file — not the exit status. (This one has been rediscovered four
times. Once the "verification" passed against a malformed patch that contained
two conflicting diffs.)

**Assembler immediates.** `mov xN, #imm` encodes one 16-bit field. An address
that assembled before may have done so by luck (`0x1c << 32` fits; a real
peripheral address does not). Use `ldr xN, =expr`.

**Half-applied offsets are worse than missing ones.** `brcm,msi-offset` applied
to the MSI *data* but not the *SPI* allocates cleanly, attaches cleanly, and
then never delivers — surfacing as a timeout much later, not as an error.
Whenever a quirk offset exists, find every place the quantity is used.

**The MSI doorbell is an inbound window.** A device raising an MSI does a
posted write, which needs the same address translation as any other DMA. Get
`dma-ranges`/inbound windows wrong and MSI and DMA break together, in ways that
look like two unrelated bugs. (RAM at PCI `0x1000000000` → CPU `0x0`; the
doorbell at `0xfffffff000` is itself a window onto the MIP registers.)

**Verify before you overwrite — especially device trees.** Disabling `pcie2` by
editing the base DTB would have left the kernel with no PCIe at all, because
`pcie1` is `disabled` *on disk* and the firmware enables it at boot. Read what
the firmware hands over, not what the file says. Use a DT overlay; it is the
mechanism designed for this.

**Do not allocate resources the CPU never touches.** The MIP doorbell was
allocated `RF_ACTIVE`, which maps it; nothing ever dereferences it.
`bus_get_resource()` is the right call.

## Board mechanics worth knowing

- **Identification:** `/proc/device-tree/model`, `/proc/device-tree/compatible`,
  and `Revision` in `/proc/cpuinfo`. Note that a **Pi 500+ reports itself as a
  Pi 500** (`raspberrypi,500`) and the firmware selects `bcm2712-rpi-500.dtb`
  for it. Do not go looking for a `500plus` DTB.
- **The board powers on when USB-C power is applied** — `WAIT_FOR_POWER_BUTTON
  0` in the EEPROM config. No keyboard and no button needed, which is what
  makes working on a disassembled board practical.
- The DTB the firmware loads is Raspberry Pi's own, and it patches that blob
  (memory size, MAC, the RP1 window) before handover. A self-compiled DTB
  arrives without any of it.
- Overlays (`dtoverlay=`, `dtparam=`) are written against **Linux** driver
  bindings. One that renames or reparents a node moves it out from under the
  FreeBSD driver's compatible string. Add them one at a time, each with a boot
  that proves it.

## Where the code lives

| | |
|---|---|
| overlay repo | `nextbsd-kernel` — `patches/`, `config/`, `ci/` |
| PCIe controller | `sys/arm/broadcom/bcm2835/bcm2838_pci.c` |
| MSI | `sys/arm/broadcom/bcm2835/bcm2712_mip.c` — a translator, not a dispatcher: writing the doorbell with data N raises GIC SPI `spi_base + N` |
| RP1 | `sys/arm/broadcom/bcm2835/rp1.c` — bridge (a `simple-bus` behind BAR1) and PIC (61 MSI-X vectors mapped **1:1** to internal IRQs) |
| image lane | `nextbsd` — `BOARD=rpi500 sh build.sh` |

RP1 has **no `reg` property**, so `generic_pcie_ofw_bus_attach()` never
associates it; the driver walks the parent's OFW children looking for name
`rp1` plus compatible `simple-bus`.

## Porting from other BSDs

OpenBSD's `rpone(4)` is the reference for RP1's register layout and its
`IACK_EN` semantics. Prefer ISC/BSD-licensed sources over GPL-2.0-only ones
when both describe the same hardware.
