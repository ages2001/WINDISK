# WINDISK 1.1 - disk driver for DOS, Windows 3.x and Windows 95 / 98 / ME

WINDISK drives modern disk controllers on old systems: **IDE** (also SATA in IDE mode),
**SATA AHCI** (and RAID controllers working as AHCI), **NVMe**, IDE / AHCI **CD/DVD** drives,
**SD card readers** and **eMMC**. DOS gets drive letters for every FAT partition (MBR and GPT,
disks over 2 TB, 512 / 4096-byte sectors), Windows 3.1 gets 32-bit disk access (FastDisk)
and Windows 95 / 98 / ME a Plug and Play driver with 32-bit drive letters.

WINDISK is one of the four WINDRV packages, each in its own repository and usable alone or
together with the others:

| Package | What it drives |
|---------|----------------|
| **WINDISK**  | IDE, SATA AHCI and NVMe disks, IDE/AHCI CD/DVD drives, SD card readers and eMMC |
| **WINUSB**   | USB host controllers and devices, PS/2 mouse and keyboard, touchpads, USB4 / Thunderbolt |
| **WINAUDIO** | HD Audio, AC'97 and the Intel audio DSP (SST / SOF), General MIDI synthesizer, Sound Blaster 16 emulation for DOS programs |
| **WINUDF**   | the UDF file system (DVD, Blu-ray, packet-written CDs, UDF disks), read and write |

---

## Supported systems

| System | WINDISK |
|--------|------|
| MS-DOS 5.0 / 6.x | disks, CD/DVD (MSCDEX), SD / eMMC |
| MS-DOS 7.0 / 7.1 / 8.0, FreeDOS | the same; FAT32 partitions on 7.1, 8.0 and FreeDOS (with Windows 95/98/ME installed the DOS driver is for its MS-DOS mode, `/DOS`) |
| Windows 1.0x, Windows 2.x (real mode) | disks (as DOS drives) |
| Windows/386 2.x | **does not work** (see "Known limitations") |
| Windows 3.0 Real / Standard / 386 Enhanced Mode | disks, CD/DVD |
| Windows 3.1, WfW 3.11 | everything + 32-bit disk access (FastDisk) |
| Windows 95 / 98 / ME | IDE, AHCI, NVMe and SD / eMMC controllers as Plug and Play devices: `WINDISK.VXD` + the IOS port driver `WINDISK.PDR` (32-bit drive letters, CD/DVD through CDFS) (section 4a) |

## Installation

From DOS (not inside Windows 3.x), in `bin\DOS` of this package:

```
INSTDISK                     into the Windows directory, CONFIG.SYS and SYSTEM.INI updated
INSTDISK C:\DRV              another directory
INSTDISK /O /NVME            with driver options (everything after /O goes to WINDISK.SYS)
```

The installer finds every Windows of the machine, copies the files, adds the driver to
CONFIG.SYS after HIMEM.SYS (also on plain MS-DOS 7.x / 8.0 and FreeDOS) and sets up Windows
3.1's 32-bit disk access; the originals are saved as `*.WND`. On Windows 95 / 98 / ME it
prepares the Plug and Play driver: then pick the controller in **Device Manager**
(Windows 98 / ME: Update Driver -> Have Disk -> `C:\WINDOWS\INF`; Windows 95: Remove +
Refresh). Uninstall: `UNINDISK`.

### By hand

1. Copy `bin\DOS` to a directory, e.g. `C:\WINDRV`.
2. CONFIG.SYS:
   ```
   DEVICE=C:\DOS\HIMEM.SYS
   DEVICE=C:\WINDRV\WINDISK.SYS
   DEVICE=C:\WINDRV\WINCD.SYS            (CD/DVD drives)
   LASTDRIVE=Z
   ```
   AUTOEXEC.BAT for CD/DVD drives: `C:\DOS\MSCDEX.EXE /D:WINCD001`
3. Windows 3.x: copy `bin\WIN3X\WINDISK.386` next to WINDISK.SYS (it is loaded by itself
   when 386 Enhanced Mode starts). For 32-bit disk access add to `[386Enh]` of SYSTEM.INI:
   `device=*int13` and `32BitDiskAccess=on`.
4. Windows 95 / 98 / ME: no CONFIG.SYS line; Device Manager -> the disk controller -> Update
   Driver -> Have Disk -> `bin\WIN9X` of this package.

### Most used options (WINDISK.SYS)

| Option | Meaning |
|--------|---------|
| `/NOIDE`, `/NOAHCI`, `/NONVME` | leave that kind of controller to the BIOS |
| `/ALL` | also the partitions DOS does not use on disks the BIOS serves |
| `/NVME` | take over the NVMe controller the BIOS boots from too |
| `/PIO` | IDE without DMA |
| `/S` | show the driver log at boot (diagnosis) |

## Repository layout

```
bin/DOS     DOS: the driver, its 32-bit core, tools, installer and uninstaller (also all
            Windows 1.0x / 2.x need)
bin/WIN3X   Windows 3.0, 3.1 and Windows for Workgroups 3.11
bin/WIN9X   Windows 95, 98 and ME (Plug and Play drivers and their INF files: the folder
            to give to "Have Disk" / "Update Driver")
src         the source code (see "Building")
```

Files used by several systems are in each of their folders. The installer is run from
`bin\DOS`; it takes the files of the other systems from the folders next to it, so the
`bin` tree has to stay together (copy all of it to a floppy or a directory).

| Folder | Files |
|--------|-------|
| `bin/DOS` | `DSKBENCH.EXE`, `INSTDISK.EXE`, `MANUAL.TXT`, `README.TXT`, `UNINDISK.EXE`, `WINCD.SYS`, `WINDCORE.BIN`, `WINDISK.SYS`, `WININFO.EXE` |
| `bin/WIN3X` | `WIN30FIX.COM`, `WINDISK.386`, `WINDISK.TXT`, `WINGRP.EXE` |
| `bin/WIN9X` | `WIN4G.VXD`, `WINDISK.INF`, `WINDISK.PDR`, `WINDISK.TXT`, `WINDISK.VXD`, `WINDISKC.INF`, `WINDISKD.INF`, `WINGRP.EXE` |

## More

**MANUAL.md** (`MANUAL.TXT` in `bin\DOS`, `WINDISK.TXT` for Windows) describes everything
else: all options, how the driver works, hardware and chipset notes, the programming
interface, building from source, tests and known limitations.

## Version history

**1.1 (26 October 2026)**

Changelog:

- Added the audio functions of CD/DVD drives for MSCDEX (play, pause, resume, stop, Q-channel position, UPC, audio status; requests 132, 133, 136 and IOCTL 12, 14, 15) and SCSI commands with data to the drive (MODE SELECT) in DOS and Windows 95/98/ME: CD players (Windows' CD Player in MS-DOS mode, DOS CD players) play audio CDs, the disc no longer disappears when Play is pressed.
- Fixed the BIOS drives of Windows 9x not being matched to their disks when a FAT16 drive gives its size in the 32-bit field of the BPB ("none of 1 disk(s) matches", the drive left on the BIOS path).
- A CD/DVD drive that is busy for a moment (playing audio, spinning up) no longer loses its disc: the medium is gone only when the drive says "medium not present" or does not answer three times in a row (Windows' CD Player showed track 00 and the disc again each time Play was pressed).
- WINDISK.SYS, WINUSB.SYS and WINAUDIO.SYS can be loaded high (`DEVICEHIGH`); BIOS disks are recognized from an upper memory block too.
- Windows 95/98/ME: WINDISK.VXD loaded at the start takes only the controllers whose installed driver is WINDISK (the others belong to Windows' own drivers, which start them too).
- Fixed "CPU without time stamp counter" on CPUs that have one but do not report it through CPUID (Vortex86 DX / SX / EX ...): the TSC is tried directly.
- The TSC rate is measured with a time limit and with the PIT counter when the timer interrupt does not come (freezes at the start under some memory managers).
- WINDISK.SYS loaded with DEVLOAD or another device loader finds WINDCORE.BIN in its own directory.
- The installer tells Windows 98, 98 Second Edition and ME apart by the version of USER.EXE (Windows 98 was shown as Windows 95).
- Added MS-DOS 7.0, 7.1, 8.0 and FreeDOS support: drive letters on plain MS-DOS 7.x / 8.0 and FreeDOS, FAT32 partitions on MS-DOS 7.10 / 8.0 and FreeDOS, no doubled drives on FreeDOS.
- The installer adds WINDISK.SYS to CONFIG.SYS on plain MS-DOS 7.x / 8.0 and FreeDOS too (only Windows 95/98/ME skip it) and installs the DOS driver alone when no Windows is found.
- Fixed BIOS disks on controllers the driver takes over (NVMe with /NVME): their INT 13h calls are served by the driver.
- Partitions Windows 9x reports as unformatted (UDF disks) are matched to their BIOS drives.
- The documentation is split into README.md (overview, installation) and MANUAL.md (all details).

**1.0 (5 October 2026)** - first release.

**1.0 update (14 October 2026):**

* Windows 95 / 98 / ME Plug and Play driver (`WINDISK.VXD`, `WINDISK.PDR`; IDE, AHCI, NVMe,
  SD / eMMC, CD/DVD), a separate INF and class for every device kind, precise device names.
* eMMC and non-removable SD / MMC disks (PCI and ACPI SD controllers, "borrow" mode for a
  card the BIOS uses) on all systems.
* Disks larger than 2 TB and 1024 / 2048 / 4096-byte sectors.
* PCI resources above 4 GB: the boot-time move applies only to WINDRV's devices (`/ALL4G`
  for all), WIN4G.VXD on Windows 9x.
* Windows 95 stability fixes: the DDB reference of the port driver (fatal exception 0E,
  Explorer / MSGSRV32 closing), the SYSTEM.INI service when installing while Windows runs.
* Installer: Start menu groups on Windows 95/98/ME, "Read Me" items, no duplicate entries
  when installing again; the package is split into `bin/DOS`, `bin/WIN3X` and `bin/WIN9X`.
