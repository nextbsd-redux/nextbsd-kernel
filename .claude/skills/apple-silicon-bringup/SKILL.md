---
name: apple-silicon-bringup
description: Bringing NextBSD up on an Apple silicon Mac (M4 / t8132, Mac16,10 / J773gAP; the M-series generally). Driving the target entirely from a second Mac over USB-C VDM, getting a firmware console over DebugUSB/KIS, the LocalPolicy boot-object dance that is the only supported way to run third-party code, and the traps that each cost a boot cycle or a human trip to the machine. Use when booting NextBSD on Apple silicon, writing a NEXTBSD-<board> config for a t-series SoC, or touching m1n1, macvdmtool, bputil or kmutil.
---

# Apple silicon board bring-up

Hard-won on a Mac mini M4 (`Mac16,10`, board `J773gAP`, SoC **t8132**), driven
from a second Mac over one USB-C cable. Other M-series parts differ in SoC ID
and peripheral addresses; the boot-policy machinery and the console path below
are the same on all of them.

Read [`rpi-bringup`](../rpi-bringup/SKILL.md) first if you have not. Much of it
transfers — everything fails as silence, build the instruments first, verify a
patch series as a series. This file is about what is *different*, and the
differences are large enough to be disorienting.

## The one thing to internalise

**On a Pi, the hard part is getting a console. Here you get one for free, and
the hard part is being allowed to boot at all.**

*(Both halves of that are now confirmed on hardware: the console worked on the
first attempt and needed no Linux host, while getting permission to boot cost a
macOS install, a trip to 1TR, and a wired keyboard. The proportions are real.)*

The firmware talks. From the first boot, over the same USB-C cable you already
have, with no soldering, no header, no probe. What you cannot do is put your
own code in front of it without a **Secure Enclave-signed boot policy**, and
every change to that policy costs a human standing at the machine holding the
power button.

That inverts the economics of the whole bring-up:

| | Raspberry Pi | Apple silicon |
|---|---|---|
| Console | the scarce resource | free, from boot ROM onward |
| Retry cost | cheap (`tryboot` reverts itself) | cheap **once stage 1 is installed** |
| Getting stage 1 installed | copy a file to FAT | 1TR + owner credentials + a human |
| Unbootable board | power cycle | DFU, also a human |
| Failure mode | silence | silence, *and* a firmware log telling you where |

So the sequencing is the reverse of the Pi. There, you fight for a console and
then iterate freely. Here, you have the console immediately, spend real effort
once to earn the right to run code, and then iterate freely over USB **without
ever touching the disk again**.

## Two machines, and the role inversion that will bite you

Everything is driven from a **second Mac**. Not optional: `macvdmtool` needs a
host, DFU recovery needs a host, and the console arrives on the host's USB.

**The trap:** general Apple documentation and desktop guides cast the desktop
as the host and the laptop as the target. In this project it is the other way
round — the mini is the port subject, the laptop drives it. SSH into the target
is a convenience that **evaporates the moment it reboots into DFU or debugusb**,
which is exactly when you need it. Put the driving seat on the host and keep it
there.

Symptom of getting it backwards, seen in practice: `sudo macvdmtool dfu`
returning `command not found` because the shell was still inside an SSH session
on the target. Harmless that time. Check the hostname in your prompt.

### What the host actually needs

Not an M-series chip, and not Thunderbolt. This was driven successfully from an
**A18 Pro** laptop with `system_profiler SPThunderboltDataType` reporting *"No
hardware was found."* VDM rides the USB-C configuration channel, not the
Thunderbolt controller.

What it needs is a port publishing an **Apple UVDM endpoint**. Test it in one
line, before trusting anything else:

```sh
ioreg -rc IOAccessoryManager -w0 | grep -E "\+-o|PortType"
```

A usable port:

```
+-o Port-USB-C@1        <class AppleHPMInterfaceType18>
  +-o CC                <class IOPortTransportStateCC>
  | +-o SOP             <class IOPortTransportComponentCCUSBPDSOP>
  | | +-o AppleUVDM      <class IOPortTransportProtocolAppleUVDM>   <-- this
  | |   +-o DataEP1      <class AppleUVDMEndpoint>                  <-- and this
```

An idle port has a bare `CC` and nothing under it.

**What this check does not prove:** every USB-C port has a CC line and a PD
controller, so **a live UVDM endpoint does not confirm you are on the target's
DFU port.** Observed directly: the UVDM stack came up on both ends while no USB
device enumerated at all. The *device* is the stronger signal, and it is a
three-state tell:

| Enumerates as | ID | Target state |
|---|---|---|
| `Mac` | `05ac:1905` | normal macOS |
| `Debug USB` | `05ac:1881` | DFU **or** debugusb |
| nothing | — | wrong port, or target not up |

Confirm identity by serial: the `Mac` device's `USB Serial Number` equals the
target's hardware serial. That is how you prove the cable runs between the two
machines you think it does rather than to a dock.

### The port on the target

**M4-era hardware moved the DFU port.** Apple's table
(<https://support.apple.com/en-us/120694>) says **middle rear USB-C** for the
Mac mini (2024). `macvdmtool`'s own README says *"the port nearest to the power
plug"* — that is M1-era and **wrong for `Mac16,10`**. Follow Apple's table.

DFU never works over Thunderbolt cables-as-Thunderbolt, and `debugusb` works
over USB 2.0-only cables. `macvdmtool serial` mode needs a SuperSpeed cable;
`debugusb` does not.

## The unattended harness

### Everything is driven from the host, including recovery

```sh
sudo ./macvdmtool dfu              # target -> DFU
sudo ./macvdmtool reboot           # target -> normal macOS
sudo ./macvdmtool reboot debugusb  # reboot, then switch the link to debug USB
```

**The host can pull the target back out of DFU. No power button.** Measured:
DFU in, Finder offered Revive, `macvdmtool reboot` returned it to macOS in
about 40 seconds with SSH restored and nobody at the machine.

This contradicts the folklore, which says the only way out of DFU is a
ten-second power hold. That folklore is true *only if you have no VDM host* —
Finder genuinely has no "exit DFU" button, it leaves DFU only by completing a
Revive or Restore. With `macvdmtool` the power hold is the fallback, not the
procedure.

Reading the source makes it obvious why it works — these are VDM messages to
the target's power-delivery controller, which stays alive in DFU:

```c
DoReboot     { 0x5ac8012, 0x105, 0x80000000 }   // reboot into normal mode
DoDFU        { 0x5ac8012, 0x106, 0x80010000 }   // reboot into DFU
DoDebugUSB   { 0x5ac8012, 0x1824606 }           // switch link to debug USB
```

Note `reboot debugusb` calls `DoReboot` **first** — it reboots into *normal*
mode and then switches the link. So debugusb is a link-level mode layered on a
normal boot, not a persistent boot mode.

Two consequences, both measured:

- **A reboot clears it.** After the target reboots, the KIS nodes are gone and
  the console looks dead. It is not; the link is simply back in normal mode.
- **`macvdmtool debugusb` alone re-arms it on a *live* target**, with no reboot
  and no disturbance to whatever is running. All eight channels come straight
  back. This is the right tool when you have a running m1n1 and want a console
  without restarting it.

So the natural sequence for a first boot is `macvdmtool reboot debugusb`, which
reboots and arms the console in one step — better than typing `reboot` on the
target, because the console is armed before anything prints.

### Iterating without touching the disk

This is the Apple equivalent of `tryboot`, and it is better. Once m1n1 stage 1
is installed **once**, everything after pushes over USB:

```sh
proxyclient/tools/chainload.py -r build/m1n1.bin   # replace m1n1 itself
proxyclient/tools/chainload.py kernel.bin          # push a kernel
proxyclient/tools/shell.py                         # interactive
```

No reboot, no disk write, no policy change, no human. Phases that only need
this need **no root filesystem at all** — first light, interrupt controller,
timer, scheduler and `mountroot>` all happen before storage matters.

**Corollary for planning:** get stage 1 installed as early as you can stand,
because it is the last step that costs a human until you need storage.

## The escape hatch, and the one way it can actually hurt you

Prove DFU recovery works **before** touching boot policy — and prove it by
observing, not by using it.

1. `sudo ./macvdmtool dfu`
2. Finder → confirm the target appears and offers **Revive**
3. `sudo ./macvdmtool reboot` — back out without clicking anything

**Revive and Restore are not interchangeable.** Apple: a *revive* "updates the
firmware and updates recoveryOS to the latest version… designed to not make any
changes to the startup volume, the user's data volume, or any other volumes." A
*restore* "erases and installs the latest version of macOS on your internal
storage," and destroys the Owner Identity Key.

**The failure mode worth knowing before you need it:** a restore **wipes first
and personalises after**. If the machine is Activation-Locked, or Apple's
servers (`17.0.0.0/8`) are unreachable at that moment, you can be left with a
wiped Mac you cannot bring back offline. The risk is *higher after* the restore
than before it. Revive is the escape hatch; restore is the last resort.

Nothing is modified by a DFU visit you never restore against.

**Do not fear bricking.** DFU works even if NOR flash is wiped — it is the
recovery mechanism, not a hazard. The realistic risks are a wrong Finder click
and a human trip to the machine, in that order.

## Getting a console

```sh
sudo ./macvdmtool reboot debugusb
```

Then look for KIS device nodes on the **host**:

```sh
ls /dev/cu.kis-*
```

**These do appear on macOS.** The M4 plan predicted they would not — on the
evidence that no KIS driver was present in the *target's* boot kernel
collection — and recommended Linux `kisd` as the safe bet. That prediction was
wrong, and wrong in an instructive way: **the driver has to exist on the host,
so checking the target's kernel collection answers nothing.** No Linux box is
needed.

Two things the node name tells you:

- **It encodes the USB location ID.** `kis-120000-ch-N` corresponds to
  `Debug USB@00120000`. Published examples say `kis-100000-ch-0`. **Glob it,
  never hard-code it.**
- **Channel count distinguishes the two states that share the `Debug USB`
  name:** DFU exposes only `ch-0`; debugusb exposes **eight**, `ch-0`..`ch-7`.

### Arm the capture before the reboot

All the interesting output lands in the first seconds of firmware boot, and the
device node does not exist until the target enumerates. Opening it once, after
the fact, catches nothing. **Poll for the node, then hold it open.**

```python
# poll for the device, reopen across disconnects, log with timestamps
while True:
    nodes = sorted(glob.glob("/dev/cu.kis-*"))
    if not nodes: time.sleep(0.4); continue
    fd = os.open(nodes[0], os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    # ... termios raw, then read until the node disappears
```

`picocom` is fine once something is listening, but it is the wrong tool for
catching a boot:

```sh
picocom -q --omap crlf --imap lfcrlf /dev/cu.kis-120000-ch-0
```

### What comes out

The complete firmware boot — iBoot Stage 1, Stage 2, and the handoff to XNU:

```
======== End of iBootStage1 serial output. ========
======== Start of iBootStage2 serial output. ========
...
:: Microkernel iBootStage2 for j773g Copyright 2007-2026, Apple Inc.
::      Local boot, Board 0x2a (j773gap)/Rev 0x6
::      BUILD_TAG: mBoot-18000.161.10
::      BUILD_STYLE: RELEASE
::      USB_SERIAL_NUMBER: SDOM:01 CPID:8132 CPRV:11 CPFM:03 SCEP:01 BDID:2A
::                         ECID:<redacted> IBFL:BC SIKA:00 SRNM:[<redacted>]
======== End of iBootStage2 serial output. ========
```

The banner is a free cross-check on everything else you believe about the
machine: `CPID` is the SoC (`8132`), `BDID` the board (`2A` = `j773gap`),
`CPFM:03` production fusing — which should agree with `CPRO`/`CSEC` = 1 from
`bputil -d`. `Local boot` should agree with `lobo: 1` in the LocalPolicy.

**The banner prints the ECID and the serial number. Redact both before pasting
a log anywhere public** — the ECID is the identifier Apple's signing server
personalises against.

Stage 2 also logs every firmware payload it loads, with offsets and lengths.
This is free reconnaissance, every boot:

| Image | Size (M4) | What it is |
|---|---|---|
| `ansf` | 1302 KiB | **ANS firmware — the NVMe coprocessor.** The storage milestone's target. |
| `dcpf` | 3967 KiB | DCP, display coprocessor |
| `illb` | 1276 KiB | LLB — immutable by design |
| `aubt` | 282 KiB | audio |
| `ciof` `rlg1` `rlg2` `tmuf` `recm` `logo` | 12–188 KiB | misc, recovery, boot logo |

### What RELEASE firmware costs you

A production build elides what you most want. In one captured boot: **50
`<<PTR>>` redactions** and **256 log lines compressed to `<hash>:<line>`** over
33 unique hashes. You get sequence and structure — useful for pinning a hang to
a stage — and not one readable message.

### Input works. This was the open question; it is closed

**Both directions work.** Settled on hardware 2026-08-27: `proxyclient`
connected to a running m1n1 over `/dev/cu.kis-*` on a macOS host, took commands,
and streamed a 448 KB ADT back. That is a protocol round-tripping, not a one-way
console.

```
TTY> Heap limit: 0x1000d248000 (128 MiB)
Have fun!
m1n1 base: 0x10003380000
Fetching ADT (0x00070000 bytes)...
>>> print('CHIPID 0x%x' % u.adt['/chosen'].chip_id)
CHIPID 0x8132
```

So **interactive DDB is available** and the harness is not printf-only. Plan for
a real debugger.

The caveat that made this look doubtful is about **`kisd`**, the *Linux* daemon,
whose README says it is *"not known how the correct write addresses to use in
the DebugUSB messages for input / key presses are determined."* **The macOS path
does not use `kisd` at all** — the nodes come from Apple's driver — so that
limitation never applied. Do not inherit the pessimism from Linux-side docs.

**Silence is not death.** Once iBoot hands off, all eight channels go quiet, and
a *running* m1n1 is quiet too: it prints a banner at startup and then waits for
proxy commands. If you attach after boot you see nothing. Attaching
`proxyclient` is the liveness test, not listening.

## The boot path, and why a volume group is not optional

```
SecureROM (mask ROM, Apple root CA fused at fabrication)
  -> LLB / iBoot1        [immutable]
  -> LocalPolicy lookup  [iSCPreboot, inside Apple_APFS_ISC = disk0s1]
  -> iBoot2
  -> the object named by the policy's `coih`
```

iBoot does not scan disks for bootable things. It resolves a **volume group
UUID** to a **SEP-signed LocalPolicy**, and jumps to whatever that policy's
`coih` field hashes. Apple's words: *"an SHA-384 hash of CustomOS Image4
manifest. The payload for that manifest is used by iBoot (instead of the XNU
kernel) to transfer control."*

**The inversion worth internalising:** Permissive Security does not *break* the
chain of trust, it **re-points one link**. `coih` moves signing authority for
exactly one object from Apple's RSA key to a per-machine key in *your* Secure
Enclave, which you wield after authenticating as machine owner in paired 1TR.
iBoot still verifies a hash — against a policy you signed. This is a deliberate
Apple feature, not a bypass.

Which is why **the APFS volume group is the only container the platform will
ever hand your code control from.** Not incidental packaging. Everything else —
the container, the partition, the stub — is wrapping around that one fact.

### Closed doors, so nobody re-derives them

| Idea | Why it fails |
|---|---|
| Custom IPSW restored over DFU | An IM4P carries **no signature of its own**. Authorisation is a separate IM4M chaining to an Apple root key fused in mask ROM, minted per-restore by TSS against your ECID and a nonce entangled with the fused UID key (every chip since T8020/A12). You cannot mint one, replay one, or bypass the check — **no SecureROM exploit has ever been published for any M-series part**. |
| Demotion (dev-fusing) | Circular: demotion is requested via `DPRO`/`DSEC` in a manifest iBoot only honours after Apple-root validation, and TSS declines to sign those for production hardware (`STATUS=94`). |
| One-shot / non-persistent boot | No such mechanism. |
| Permissive Security relaxing DFU | LocalPolicy is loaded **by LLB, from internal storage**. In DFU, SecureROM has not loaded LLB and reads no disk — it never consults LocalPolicy. Structurally invisible. |
| A distributable "NextBSD for Macs" USB image | LocalPolicy is ECID-bound and SEP-signed. You can ship bits; you cannot ship policy. `bputil` + `kmutil` must run locally, in 1TR, per machine. |

`smb0`/`smb1`/`smb2` are narrow and enumerated: `smb0` permits a *globally*
Apple-signed manifest (version rollback, not self-signing); `smb1`/`smb2` cover
the custom kernel collection and the AKC. **None extends to LLB, iBoot, SEP or
coprocessor firmware.**

### Root filesystem is a completely separate question

Nothing requires APFS for NextBSD itself — there is no APFS support in the
kernel and none is needed. UFS on a partition, on external media, or over NFS
are all fine. **APFS holds ~1 MB of m1n1. It never holds NextBSD.** Conflating
the boot object with the root filesystem is what makes the APFS requirement look
far larger than it is.

## The boot policy dance

Reversible throughout. `bputil -n` on the volume group puts it back to booting
macOS.

```sh
# 1. from macOS: reduced security + set as startup disk
sudo bputil -g -v <VGID>
sudo bless --setBoot --mount <system volume>

# 2. shut down (NOT reboot), hold power from off to reach 1TR
# 3. in 1TR:
sudo bputil -nc -v <VGID>
sudo kmutil configure-boot -c m1n1-stage1.bin --raw \
     --entry-point 2048 --lowest-virtual-address 0 -v <system volume>
```

**Two policy operations, not one.** Plans that list only the 1TR step
under-budget this.

`bputil -n` **recreates the policy from scratch** — *"it does not preserve any
existing security policy options"*. Pass every downgrade in one invocation.

#### The ordering trap: `kmutil` goes LAST

**Nothing that writes policy may run after `kmutil configure-boot`.** Paid for on
hardware: `kmutil` reported `installing boot object... done.`, and then a re-run
of `bputil -nc` silently discarded it — `coih` is a LocalPolicy field, and `-n`
rebuilds the policy keeping only what it was passed.

The give-away is that the policy was visibly re-minted:

| | after `kmutil` | after the stray `bputil -nc` |
|---|---|---|
| `coih` | a hash | **`absent`** |
| `lpnh` | `C028D3A0…` | `1E9017E1…` |
| `stng` | 24 | 25 |

Correct order, once: `bputil -nc` → `kmutil configure-boot` → **stop**. Verify
with `bputil -d`, which only displays; reaching for `-nc` again "just to check"
destroys the thing you are checking for.

`kmutil` also announces *"going to change bootpolicy to permissive"* and does
that itself, so `bputil -nc` is really only buying the CTRR disable (`-c`). It
preserves `sip2` when it rewrites, so simply re-running `kmutil` is a safe
repair after this mistake.

### Getting the second volume group: three things that will stop you

**`startosinstall --volume` no longer exists.** Probed on macOS 26.6.2: passing
it prints the usage block rather than complaining the volume is missing, and it
is absent from that list. Modern `startosinstall` installs to the *current*
system volume or makes a new one with `--eraseinstall`. **Installing a second
macOS to a chosen volume is now a GUI operation** — open the installer app and
pick the destination. (The license flag is `--agreetolicense`.)

Create the target volume first with `diskutil apfs addVolume <container> APFS
<name>`; it is additive and reversible. Note the installer then uses *your*
volume as the **Data** half and creates its own System volume alongside, so the
resulting group is `<name>` (System, new) + `<name> - Data` (yours).

**1TR needs a wired keyboard.** Bluetooth is not reliably available in a
pre-boot environment, and `bputil -nc` requires typing a username and password
there. A Bluetooth-only setup stalls in front of a screen that looks broken.
Check before starting:

```sh
ioreg -rc IOHIDDevice -w0 | grep -iE '"Product" =|"Transport" ='
```

You want `"Transport" = "USB"`, not `"Bluetooth Low Energy"`. A Logi Bolt-style
receiver counts — it enumerates as plain USB HID and is live in 1TR.

**The installer app lies about its own version.** `Install macOS *.app`'s
`Info.plist` carries `DTPlatformVersion` and `DTSDKBuild` describing the *app
bundle*, not the payload. Match the running build — an older macOS sharing a
container with a newer one is the thing to avoid — and check the real value:

```sh
hdiutil attach -nobrowse -readonly -mountpoint "$MP" "$APP/Contents/SharedSupport/SharedSupport.dmg"
/usr/libexec/PlistBuddy -c "Print :Assets:0:OSVersion" \
    "$MP/com_apple_MobileAsset_MacSoftwareUpdate/com_apple_MobileAsset_MacSoftwareUpdate.xml"
```

Seen in practice: `Info.plist` said 26.6.1 / 25G74 while the payload was
26.6.2 / 25G83, which was the correct one.

### Pairing, and a reading that wastes an evening

Security policy is **per volume group**, by Apple's explicit design: *"security
policies on a Mac with Apple silicon are for each installed operating system."*
Note what that does not mention: containers. And the safety property:

> Every installation of macOS 12 is paired to a recoveryOS stored on the
> corresponding APFS volume group… **The paired recoveryOS can downgrade
> security settings for the paired macOS installation, but not any other macOS
> installation.**

So a second volume group's 1TR is *structurally incapable* of altering your main
macOS install's policy. Pairing lives per-VGID in the policy store — each group
carries its own `<lpnh>.img4` plus a matching `<lpnh>.recovery.img4` under
`iSCPreboot/<VGID>/LocalPolicy/` — **not** by owning a Recovery volume. A shared
Recovery volume is therefore not an obstacle to a second volume group.

**CONFIRMED on hardware 2026-08-27.** A second macOS installed into the existing
container minted its own paired policy pair under its own VGID —
`<hash>.img4` alongside `<hash>.recovery.img4` — and 1TR booted from that group
reported `OS Type: one true recoveryOS` / `OS Pairing Status: Paired`. The
sibling install kept Full Security throughout, exactly as the man page
promises.

**The reading that wastes an evening:** `bputil -d` run from **full macOS**
reports `OS Pairing Status: Not Paired` even for the running system's own
volume group. That is correct and expected — the man page defines pairing as the
relationship *"between the booted OS and the target LocalPolicy,"* and a booted
macOS is not a recoveryOS. `Pairing Integrity: Valid` is the field that says the
structure is sound. **The `: Paired` / `one true recoveryOS` check is a 1TR-time
check.** Do not go hunting for a problem that is not there.

### Hazards

| Hazard | Detail |
|---|---|
| `bputil -z` | *"will also remove local policies for bootable volumes on external drives that are not currently connected"*, and **Pairing requirements: None** — any recoveryOS can purge policies it is not paired with. Worse, it only runs from recoveryOS/1TR, exactly when external disks are least likely attached. Use `-r <VGID>` deliberately, or neither. |
| `bputil -f` | Returning to Full Security does an online check; if the installed macOS is no longer current, **Full Security becomes permanently unreachable** short of a DFU restore. |
| Firmware ratchets | Any newer macOS raises iBoot/recoveryOS permanently. They never come back down. Record `BUILD_TAG` from the console every time it moves. |
| `sudo bputil -d` needs root | And 1TR needs a human. Passwordless sudo does not help; that friction is the security model. |

## The board config pattern

Same shape as `NEXTBSD-RPI5` — include the generic config, subtract wrong-SoC
drivers, add this SoC, point the early console at a real address:

```
include NEXTBSD
ident		NEXTBSD-MACMINI

options 	SOC_APPLE_T8132
options 	SOCDEV_PA=0x3AD200000
options 	EARLY_PRINTF=s5l
env		"macmini.env"
```

`locore.S` maps one 2 MB device block at `SOCDEV_PA` and keeps the offset within
it, so give the exact register address.

**`EARLY_PRINTF` covers `printf` only up to `cninit()`,** which drops
`early_putc` unconditionally. A boot that prints and then goes quiet at exactly
that point is not a hang — it is this. The `env` file is what gives you a
console for the whole boot; every tag in it is load-bearing
(`sys/dev/uart/uart_subr.c:uart_getenv`), and `dt:` especially, because
`uart_cpu_getdev()` defaults to `uart_ns8250_class`.

**Do not trust an rclk that merely divides evenly.** The Pi bring-up got this
wrong and the board said so immediately. Identify the clock by *which device it
feeds*, not by arithmetic that flatters a guess.

## Traps

**The generic timer arrives on FIQ, not IRQ.** Masking FIQ gives a silent hang
with a dead scheduler — no panic, no output, nothing to read. This is the
single most expensive trap on the platform.

**Avoid `WFI`/`WFIT` in the idle loop and in `delay()`.** Secondary cores lose
architectural state.

**Never touch `CPU_OVRD` or `L2C_ERR_STS`.** XNU's `NO_CPU_OVRD` applies.

**m1n1 patches the FDT at runtime.** Roughly twenty categories of fixup, applied
to the blob it hands you in `x0`. A statically compiled DTB arrives without any
of it — the same lesson as the Pi firmware patching its own DTB, with a longer
list.

**U-Boot has no t8132 case.** Its `mem_map` chain stops at t8122, so there is no
UEFI and no `loader.efi` path at all. That *forces* direct m1n1 → kernel entry
over the Linux boot protocol — which is the `kernel.bin` + `Image`-header lane
the Pi work already built. Do not plan around `freebsd.py`.

**No loader ⇒ nothing can be `kldload`ed at boot.** Every boot-critical driver
compiles in. Same as the Pi, same reason.

**Verify a patch series as a series.** Rediscovered on the Pi four times; the
hardware does not make it less true.

**Asahi's installer will not run on M4 at all.** There is no `0x8132` in
`CHIP_MIN_VER` and no `j773gap` in `DEVICES`; `main.py` resolves both to `None`
and rejects the machine *before* IPSW selection. The oldest IPSW for `J773gAP`
is 15.1 and the installer supports 12.3–14.8.3, so the stub could not be built
even if the tables were extended. Plan on a plain macOS install for the volume
group, not an Asahi stub.

## Where the code lives

| | |
|---|---|
| overlay repo | `nextbsd-kernel` — `patches/`, `config/`, `ci/`, `src-overlay/` |
| bootloader | `AsahiLinux/m1n1` — track **`main`**, not a release tag |
| VDM host tool | `AsahiLinux/macvdmtool` |
| device tree | Asahi's `t8132-j773g.dts` (MIT) |

**Track m1n1 `main`.** The t8132 support postdates the last tag: `nvme: support
T8132`, `kboot_atc: t8132 support`, `sart: add SARTv4 support`, and Sven Peter's
NVMe strictness series are all in `main` and none is in `v1.6.1`.

### Building m1n1 on a macOS host

Three Homebrew keg-only landmines, in the order you hit them:

1. `TOOLCHAIN ?= $(shell llvm-config --bindir)/` expands to a bare `/` with no
   `llvm` installed → `/bin/sh: /clang: No such file or directory`, which reads
   like a corrupt checkout.
2. Substituting `aarch64-elf-gcc` does not dodge it — `TOOLCHAIN` is still
   prefixed on the GCC branch. You must also pass `TOOLCHAIN=`.
3. `brew install rustup` ships **no `rustup-init`** on the normal path, so
   `rustup-init -y` silently installs nothing and `cargo` stays missing.

```sh
brew install llvm lld rustup picocom
/opt/homebrew/opt/rustup/bin/rustup-init -y --no-modify-path
rustup target add aarch64-unknown-none-softfloat
export PATH="/opt/homebrew/opt/llvm/bin:/opt/homebrew/opt/rustup/bin:/opt/homebrew/bin:$PATH"
make -j8
```

`macvdmtool` builds with Command Line Tools alone — no Xcode.app.

m1n1 also exports `RUSTC_BOOTSTRAP=1` to force a stable `rustc` to accept
nightly-only features. That is an unsupported escape hatch by Rust's own rules,
so the build can break on a toolchain bump with nothing in m1n1 changing. Pin
the toolchain.

## Porting from other BSDs

**OpenBSD is the primary source for this platform, not Linux** — which makes the
legal position licensed reuse rather than a clean-room argument. `aplintc.c`
(AICv3), `aplpmgr.c`, `aplsart.c`, `aplmbox.c`, `rtkit.c` and `aplns.c` are all
ISC.

**No GPL and no APSL on the critical path.** Never vendor U-Boot (GPL-2.0+,
though it may run as a binary). Do not import XNU — APSL with a field-of-use
rider on core files. Where Linux and OpenBSD both describe the same block, take
OpenBSD's; where a Linux file is dual MIT/GPL, take the MIT arm.
