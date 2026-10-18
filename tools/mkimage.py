#!/usr/bin/env python3
"""
mkimage.py — Pure-Python GPT + FAT32 ESP + Physical exFAT RootFS Disk Image Builder
for the Cortez Kernel (runs natively on Windows, Linux, and macOS without root/losetup).

The generated image is spec-compliant so that real UEFI firmware (e.g. Lenovo InsydeH2O,
AMI Aptio) as well as OVMF, Linux (blkid / fsck.fat / fsck.exfat) and Windows can read it:

  * Protective MBR + primary/backup GPT with valid CRCs and real (RFC 4122) GUIDs.
  * Partition 1: FAT32 EFI System Partition with >= 65525 clusters (the hard minimum for a
    volume to be recognised as FAT32 — below that, firmware treats it as FAT16 and fails to
    find \\EFI\\BOOT\\BOOTX64.EFI), proper FSInfo, backup boot sector and LFN entries.
  * Partition 2: exFAT root filesystem with a checksummed main + backup boot region,
    allocation bitmap, up-case table and FAT chains.
"""

import os
import sys
import ssl
import struct
import uuid
import zlib
import urllib.request

SECTOR_SIZE   = 512
TOTAL_SECTORS = 262144  # 128 MiB total disk image

ESP_START_LBA = 2048
ESP_SECTORS   = 131072  # 64 MiB FAT32 EFI System Partition (Partition 1: nvme0n1p1)
ESP_END_LBA   = ESP_START_LBA + ESP_SECTORS - 1

ROOT_START_LBA = ESP_END_LBA + 1  # 133120
ROOT_SECTORS   = 126976           # 62 MiB exFAT RootFS Partition (Partition 2: nvme0n1p2)
ROOT_END_LBA   = ROOT_START_LBA + ROOT_SECTORS - 1

GPT_ENTRY_COUNT   = 128
GPT_ENTRY_SIZE    = 128
GPT_ENTRY_SECTORS = (GPT_ENTRY_COUNT * GPT_ENTRY_SIZE) // SECTOR_SIZE  # 32
FIRST_USABLE_LBA  = 2 + GPT_ENTRY_SECTORS                              # 34
LAST_USABLE_LBA   = TOTAL_SECTORS - 1 - GPT_ENTRY_SECTORS - 1          # TOTAL - 34

ROOTFS_PART_LABEL = "Cortez-RootFS"  # Kernel only mounts/formats partitions with this GPT label

# Fixed timestamp for all files: 2026-01-01 00:00:00
TS_YEAR, TS_MONTH, TS_DAY = 2026, 1, 1
DOS_DATE = ((TS_YEAR - 1980) << 9) | (TS_MONTH << 5) | TS_DAY
DOS_TIME = 0

# Deterministic GUIDs so rebuilding produces byte-identical images
_GUID_NS = uuid.UUID("6c6e0b3e-7f2a-4c55-9a0e-c0e7e2026000")

LIMINE_EFI_URLS = (
    "https://codeberg.org/Limine/Limine/raw/branch/v8.x-binary/BOOTX64.EFI",
    "https://raw.githubusercontent.com/limine-bootloader/limine/v8.x-binary/BOOTX64.EFI",
)


def guid(name: str) -> bytes:
    return uuid.uuid5(_GUID_NS, name).bytes_le


def is_valid_pe_efi(data: bytes) -> bool:
    if len(data) < 0x200 or data[0:2] != b"MZ":
        return False
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    return pe_off + 6 <= len(data) and data[pe_off : pe_off + 4] == b"PE\x00\x00"


def ensure_limine_efi(cache_path: str) -> bytes:
    if os.path.exists(cache_path):
        with open(cache_path, "rb") as f:
            data = f.read()
        if is_valid_pe_efi(data):
            return data
        print(f"[mkimage] Cached {cache_path} is not a valid EFI binary; re-downloading.")

    last_exc = None
    for url in LIMINE_EFI_URLS:
        print(f"[mkimage] Downloading Limine UEFI bootloader (BOOTX64.EFI) from {url} ...")
        try:
            ctx = ssl._create_unverified_context()
            req = urllib.request.Request(url, headers={"User-Agent": "CortezKernel-Builder/1.0"})
            with urllib.request.urlopen(req, timeout=30, context=ctx) as resp:
                data = resp.read()
            if not is_valid_pe_efi(data):
                raise ValueError("downloaded file is not a PE32+ EFI application")
            os.makedirs(os.path.dirname(cache_path), exist_ok=True)
            with open(cache_path, "wb") as f:
                f.write(data)
            return data
        except Exception as exc:  # noqa: BLE001 — try next mirror
            last_exc = exc

    # Never silently produce an unbootable image.
    print(f"[mkimage] ERROR: Could not obtain Limine BOOTX64.EFI ({last_exc}).")
    print(f"[mkimage]        Download it manually and place it at: {cache_path}")
    sys.exit(1)


# =============================================================================
# exFAT Volume Builder (Partition 2: Physical RootFS)
# =============================================================================

def exfat_upcase_table() -> bytes:
    """Compressed up-case table: explicit ASCII mappings, then identity for the rest of the BMP."""
    table = [ord(chr(c).upper()) if 0x61 <= c <= 0x7A else c for c in range(128)]
    table += [0xFFFF, 0x10000 - 128]  # Compression: next 65408 code points map to themselves
    return struct.pack(f"<{len(table)}H", *table)


def exfat_table_checksum(data: bytes) -> int:
    chk = 0
    for b in data:
        chk = (((chk >> 1) | ((chk & 1) << 31)) + b) & 0xFFFFFFFF
    return chk


def exfat_boot_checksum(region: bytes) -> int:
    """Checksum over the first 11 sectors of the boot region, skipping VolumeFlags and PercentInUse."""
    chk = 0
    for i, b in enumerate(region):
        if i in (106, 107, 112):
            continue
        chk = (((chk >> 1) | ((chk & 1) << 31)) + b) & 0xFFFFFFFF
    return chk


def exfat_name_hash(name: str) -> int:
    upper_utf16 = name.upper().encode("utf-16le")
    h = 0
    for b in upper_utf16:
        h = (((h << 15) | (h >> 1)) + b) & 0xFFFF
    return h


def exfat_entry_set_checksum(entries_bytes: bytes) -> int:
    chk = 0
    for i, b in enumerate(entries_bytes):
        if i in (2, 3):
            continue
        chk = (((chk << 15) | (chk >> 1)) + b) & 0xFFFF
    return chk


def make_exfat_entry_set(name: str, is_dir: bool, first_cluster: int, data_len: int, no_fat_chain: bool = False) -> bytes:
    name_utf16 = name.encode("utf-16le")
    char_len = len(name)
    name_entries_count = (char_len + 14) // 15
    secondary_count = 1 + name_entries_count

    exfat_ts = ((TS_YEAR - 1980) << 25) | (TS_MONTH << 21) | (TS_DAY << 16)

    attr = 0x0010 if is_dir else 0x0020
    file_entry = bytearray(32)
    file_entry[0] = 0x85
    file_entry[1] = secondary_count
    struct.pack_into("<H", file_entry, 4, attr)
    struct.pack_into("<I", file_entry, 8, exfat_ts)   # CreateTimestamp
    struct.pack_into("<I", file_entry, 12, exfat_ts)  # LastModifiedTimestamp
    struct.pack_into("<I", file_entry, 16, exfat_ts)  # LastAccessedTimestamp

    stream_entry = bytearray(32)
    stream_entry[0] = 0xC0
    stream_entry[1] = 0x03 if no_fat_chain else 0x01  # AllocationPossible | (NoFatChain)
    stream_entry[3] = char_len
    struct.pack_into("<H", stream_entry, 4, exfat_name_hash(name))
    struct.pack_into("<Q", stream_entry, 8, data_len)
    struct.pack_into("<I", stream_entry, 20, first_cluster)
    struct.pack_into("<Q", stream_entry, 24, data_len)

    name_entries = bytearray()
    for i in range(name_entries_count):
        ne = bytearray(32)
        ne[0] = 0xC1
        ne[1] = 0
        chunk = name_utf16[i * 30 : (i + 1) * 30]
        ne[2 : 2 + len(chunk)] = chunk
        name_entries.extend(ne)

    combined = bytearray(file_entry) + stream_entry + name_entries
    chk = exfat_entry_set_checksum(combined)
    struct.pack_into("<H", combined, 2, chk)
    return bytes(combined)


def build_exfat_partition(total_sectors: int, kernel_bytes: bytes, limine_conf_bytes: bytes) -> bytearray:
    vol = bytearray(total_sectors * SECTOR_SIZE)
    spc_shift = 3  # 8 sectors/cluster = 4096B clusters
    spc = 1 << spc_shift
    cluster_size = SECTOR_SIZE * spc

    fat_offset = 32  # Main boot region (0-11) + backup boot region (12-23) fit below this
    max_clusters = (total_sectors - 128) // spc
    fat_length = ((max_clusters + 2) * 4 + SECTOR_SIZE - 1) // SECTOR_SIZE
    heap_offset = ((fat_offset + fat_length + spc - 1) // spc) * spc
    cluster_count = (total_sectors - heap_offset) // spc

    fat = [0] * (cluster_count + 2)
    fat[0] = 0xFFFFFFF8
    fat[1] = 0xFFFFFFFF

    bitmap = bytearray((cluster_count + 7) // 8)
    if len(bitmap) > cluster_size:
        raise RuntimeError("exFAT allocation bitmap must fit in a single cluster for the Cortez kernel driver")

    def mark_allocated(cl: int):
        idx = cl - 2
        bitmap[idx // 8] |= (1 << (idx % 8))

    def cluster_byte_offset(cl: int) -> int:
        return (heap_offset + (cl - 2) * spc) * SECTOR_SIZE

    next_free_cluster = 2

    def alloc_clusters(data: bytes) -> tuple[int, int]:
        nonlocal next_free_cluster
        if len(data) == 0:
            cl = next_free_cluster
            next_free_cluster += 1
            mark_allocated(cl)
            fat[cl] = 0xFFFFFFFF
            return cl, 0
        num = (len(data) + cluster_size - 1) // cluster_size
        start_cl = next_free_cluster
        for i in range(num):
            c = start_cl + i
            mark_allocated(c)
            fat[c] = (c + 1) if (i + 1 < num) else 0xFFFFFFFF
            chunk = data[i * cluster_size : (i + 1) * cluster_size]
            off = cluster_byte_offset(c)
            vol[off : off + len(chunk)] = chunk
        next_free_cluster += num
        return start_cl, len(data)

    # Reserve Cluster 2 (Bitmap), Cluster 3 (Upcase), Cluster 4 (Root Directory)
    # (same layout as ExFatFileSystem::format() in the kernel)
    mark_allocated(2); fat[2] = 0xFFFFFFFF
    mark_allocated(3); fat[3] = 0xFFFFFFFF
    mark_allocated(4); fat[4] = 0xFFFFFFFF
    next_free_cluster = 5

    # Allocate /boot directory (Cluster 5)
    boot_dir_cl, _ = alloc_clusters(b"")
    # Allocate /etc directory (Cluster 6)
    etc_dir_cl, _ = alloc_clusters(b"")
    # Allocate /root directory (Cluster 7)
    root_home_cl, _ = alloc_clusters(b"")
    # Allocate /home directory (Cluster 8)
    home_dir_cl, _ = alloc_clusters(b"")
    # Allocate /var directory (Cluster 9)
    var_dir_cl, _ = alloc_clusters(b"")

    # Populate /boot: kernel.elf and limine.conf
    k_cl, k_len = alloc_clusters(kernel_bytes)
    lc_cl, lc_len = alloc_clusters(limine_conf_bytes)
    boot_entries = (
        make_exfat_entry_set("kernel.elf", False, k_cl, k_len) +
        make_exfat_entry_set("limine.conf", False, lc_cl, lc_len)
    )
    vol[cluster_byte_offset(boot_dir_cl) : cluster_byte_offset(boot_dir_cl) + len(boot_entries)] = boot_entries

    # Populate /etc: os-release, fstab, hostname
    os_rel = (
        b'NAME="Cortez Kernel"\n'
        b'VERSION="1.0.0-x86_64"\n'
        b'ID=cortez\n'
        b'PRETTY_NAME="Cortez Kernel 1.0.0 (x86_64 Monolithic C++20)"\n'
    )
    fstab_data = b"/dev/nvme0n1p2  /       exfat  rw,defaults  0 1\n/dev/nvme0n1p1  /boot   vfat   ro,defaults  0 2\n"
    host_data  = b"cortez-laptop\n"

    os_cl, os_len = alloc_clusters(os_rel)
    fs_cl, fs_len = alloc_clusters(fstab_data)
    hn_cl, hn_len = alloc_clusters(host_data)
    etc_entries = (
        make_exfat_entry_set("os-release", False, os_cl, os_len) +
        make_exfat_entry_set("fstab", False, fs_cl, fs_len) +
        make_exfat_entry_set("hostname", False, hn_cl, hn_len)
    )
    vol[cluster_byte_offset(etc_dir_cl) : cluster_byte_offset(etc_dir_cl) + len(etc_entries)] = etc_entries

    # Populate /root: README.txt
    readme_data = (
        b"Welcome to the Cortez Kernel (x86_64 C++20 Monolithic OS)!\n"
        b"All files and directories on '/' reside on the physical NVMe SSD (/dev/nvme0n1p2).\n"
        b"Try commands: ls, cat /etc/os-release, df, nvme, lsblk, lspci, ps, free, dmesg\n"
    )
    rm_cl, rm_len = alloc_clusters(readme_data)
    root_user_entries = make_exfat_entry_set("README.txt", False, rm_cl, rm_len)
    vol[cluster_byte_offset(root_home_cl) : cluster_byte_offset(root_home_cl) + len(root_user_entries)] = root_user_entries

    # Up-case Table at Cluster 3
    upcase = exfat_upcase_table()
    vol[cluster_byte_offset(3) : cluster_byte_offset(3) + len(upcase)] = upcase

    # Build Root Directory at Cluster 4
    lbl_entry = bytearray(32)
    lbl_entry[0] = 0x83
    label_str = "CORTEZ-OS"
    lbl_entry[1] = len(label_str)
    lbl_utf16 = label_str.encode("utf-16le")
    lbl_entry[2 : 2 + len(lbl_utf16)] = lbl_utf16

    bm_entry = bytearray(32)
    bm_entry[0] = 0x81
    struct.pack_into("<I", bm_entry, 20, 2)
    struct.pack_into("<Q", bm_entry, 24, len(bitmap))

    uc_entry = bytearray(32)
    uc_entry[0] = 0x82
    struct.pack_into("<I", uc_entry, 4, exfat_table_checksum(upcase))
    struct.pack_into("<I", uc_entry, 20, 3)
    struct.pack_into("<Q", uc_entry, 24, len(upcase))

    root_dir_bytes = (
        bytes(lbl_entry) +
        bytes(bm_entry) +
        bytes(uc_entry) +
        make_exfat_entry_set("boot", True, boot_dir_cl, cluster_size) +
        make_exfat_entry_set("etc", True, etc_dir_cl, cluster_size) +
        make_exfat_entry_set("root", True, root_home_cl, cluster_size) +
        make_exfat_entry_set("home", True, home_dir_cl, cluster_size) +
        make_exfat_entry_set("var", True, var_dir_cl, cluster_size)
    )
    vol[cluster_byte_offset(4) : cluster_byte_offset(4) + len(root_dir_bytes)] = root_dir_bytes

    # Write Allocation Bitmap to Cluster 2
    vol[cluster_byte_offset(2) : cluster_byte_offset(2) + len(bitmap)] = bitmap

    # Write FAT table
    fat_byte_off = fat_offset * SECTOR_SIZE
    for idx, val in enumerate(fat):
        struct.pack_into("<I", vol, fat_byte_off + idx * 4, val)

    # ---- Main Boot Region (sectors 0..11) -----------------------------------
    region = bytearray(12 * SECTOR_SIZE)

    # Sector 0: Main Boot Sector
    region[0:3] = b"\xEB\x76\x90"
    region[3:11] = b"EXFAT   "
    # bytes 11..63 MustBeZero
    struct.pack_into("<Q", region, 64, ROOT_START_LBA)  # PartitionOffset
    struct.pack_into("<Q", region, 72, total_sectors)   # VolumeLength
    struct.pack_into("<I", region, 80, fat_offset)
    struct.pack_into("<I", region, 84, fat_length)
    struct.pack_into("<I", region, 88, heap_offset)
    struct.pack_into("<I", region, 92, cluster_count)
    struct.pack_into("<I", region, 96, 4)              # FirstClusterOfRootDirectory
    struct.pack_into("<I", region, 100, 0xC047E226)    # VolumeSerialNumber
    struct.pack_into("<H", region, 104, 0x0100)        # FileSystemRevision 1.00
    struct.pack_into("<H", region, 106, 0)             # VolumeFlags (clean)
    region[108] = 9                                    # 2^9 = 512B sector
    region[109] = spc_shift                            # 2^3 = 8 sectors/cluster
    region[110] = 1                                    # NumberOfFats
    region[111] = 0x80                                 # DriveSelect
    region[112] = 0xFF                                 # PercentInUse (unknown)
    struct.pack_into("<H", region, 510, 0xAA55)

    # Sectors 1..8: Main Extended Boot Sectors (signature 0xAA550000 at end)
    for s in range(1, 9):
        struct.pack_into("<I", region, s * SECTOR_SIZE + SECTOR_SIZE - 4, 0xAA550000)
    # Sector 9 (OEM Parameters) and Sector 10 (Reserved) stay zero.

    # Sector 11: Main Boot Checksum (checksum repeated across the sector)
    chk = exfat_boot_checksum(bytes(region[0 : 11 * SECTOR_SIZE]))
    for off in range(0, SECTOR_SIZE, 4):
        struct.pack_into("<I", region, 11 * SECTOR_SIZE + off, chk)

    vol[0 : 12 * SECTOR_SIZE] = region                    # Main Boot Region
    vol[12 * SECTOR_SIZE : 24 * SECTOR_SIZE] = region     # Backup Boot Region

    return vol


# =============================================================================
# FAT32 EFI System Partition Builder (Partition 1: /EFI/BOOT/BOOTX64.EFI + /boot/kernel.elf)
# =============================================================================

FAT_ATTR_VOLUME_ID = 0x08
FAT_ATTR_DIRECTORY = 0x10
FAT_ATTR_ARCHIVE   = 0x20
FAT_ATTR_LFN       = 0x0F

_SFN_VALID = set(b"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!#$%&'()-@^_`{}~")


def _sfn_checksum(name11: bytes) -> int:
    chk = 0
    for b in name11:
        chk = (((chk & 1) << 7) + (chk >> 1) + b) & 0xFF
    return chk


def _make_sfn(name: str, used: set) -> tuple[bytes, bool]:
    """Return (11-byte short name, needs_lfn)."""
    if "." in name and not name.startswith("."):
        base, ext = name.rsplit(".", 1)
    else:
        base, ext = name, ""
    ub = bytes(c for c in base.upper().encode("ascii", "replace") if c in _SFN_VALID)
    ue = bytes(c for c in ext.upper().encode("ascii", "replace") if c in _SFN_VALID)

    exact = (
        name == name.upper()
        and len(base) <= 8 and len(ext) <= 3
        and ub == base.encode("ascii", "replace")
        and ue == ext.encode("ascii", "replace")
        and len(ub) > 0
    )
    if exact:
        sfn = ub.ljust(8, b" ") + ue.ljust(3, b" ")
        if sfn not in used:
            used.add(sfn)
            return sfn, False

    lossless_but_case = len(ub) <= 8 and len(ue) <= 3 and len(ub) == len(base) and len(ue) == len(ext) and len(ub) > 0
    if lossless_but_case:
        sfn = ub.ljust(8, b" ") + ue.ljust(3, b" ")
        if sfn not in used:
            used.add(sfn)
            return sfn, True

    for n in range(1, 1000000):
        tail = f"~{n}".encode()
        sfn = (ub[: 8 - len(tail)] + tail).ljust(8, b" ") + ue[:3].ljust(3, b" ")
        if sfn not in used:
            used.add(sfn)
            return sfn, True
    raise RuntimeError(f"Cannot generate short name for {name}")


def _lfn_entries(name: str, sfn: bytes) -> bytes:
    chk = _sfn_checksum(sfn)
    units = list(struct.unpack(f"<{len(name.encode('utf-16le')) // 2}H", name.encode("utf-16le")))
    if len(units) % 13:
        units.append(0x0000)
        while len(units) % 13:
            units.append(0xFFFF)
    count = len(units) // 13
    out = bytearray()
    for seq in range(count, 0, -1):
        part = units[(seq - 1) * 13 : seq * 13]
        e = bytearray(32)
        e[0] = seq | (0x40 if seq == count else 0)
        struct.pack_into("<5H", e, 1, *part[0:5])
        e[11] = FAT_ATTR_LFN
        e[12] = 0
        e[13] = chk
        struct.pack_into("<6H", e, 14, *part[5:11])
        struct.pack_into("<H", e, 26, 0)
        struct.pack_into("<2H", e, 28, *part[11:13])
        out.extend(e)
    return bytes(out)


def _sfn_entry(name11: bytes, attr: int, cluster: int, size: int) -> bytes:
    ent = bytearray(32)
    ent[0:11] = name11
    ent[11] = attr
    struct.pack_into("<H", ent, 14, DOS_TIME)  # Creation time
    struct.pack_into("<H", ent, 16, DOS_DATE)  # Creation date
    struct.pack_into("<H", ent, 18, DOS_DATE)  # Last access date
    struct.pack_into("<H", ent, 20, (cluster >> 16) & 0xFFFF)
    struct.pack_into("<H", ent, 22, DOS_TIME)  # Write time
    struct.pack_into("<H", ent, 24, DOS_DATE)  # Write date
    struct.pack_into("<H", ent, 26, cluster & 0xFFFF)
    struct.pack_into("<I", ent, 28, size)
    return bytes(ent)


def build_fat32_esp(total_sectors: int, tree: dict, volume_label: bytes = b"CORTEZ-EFI ") -> bytearray:
    """
    Build a FAT32 volume. `tree` maps names to either bytes (file contents) or nested dicts
    (subdirectories). Every file/directory gets its own contiguous, non-overlapping cluster chain.
    """
    vol = bytearray(total_sectors * SECTOR_SIZE)
    spc = 1           # 1 sector per cluster (512B) -> keeps cluster count above the FAT32 minimum
    rsvd_sectors = 32
    num_fats = 2
    cluster_bytes = SECTOR_SIZE * spc

    # Solve for FAT size: each FAT must index every data cluster (+2 reserved entries)
    fat_sectors = 1
    while True:
        data_clusters = (total_sectors - rsvd_sectors - num_fats * fat_sectors) // spc
        needed = ((data_clusters + 2) * 4 + SECTOR_SIZE - 1) // SECTOR_SIZE
        if needed <= fat_sectors:
            break
        fat_sectors = needed
    data_start = rsvd_sectors + num_fats * fat_sectors

    if not (65525 <= data_clusters < 0x0FFFFFF5):
        raise RuntimeError(f"ESP has {data_clusters} clusters; FAT32 requires >= 65525. Increase ESP_SECTORS.")

    fat = [0] * (data_clusters + 2)
    fat[0] = 0x0FFFFF00 | 0xF8
    fat[1] = 0x0FFFFFFF

    next_cl = 2

    def cl_off(cl: int) -> int:
        return (data_start + (cl - 2) * spc) * SECTOR_SIZE

    def alloc(nbytes: int) -> int:
        nonlocal next_cl
        n = max(1, (nbytes + cluster_bytes - 1) // cluster_bytes)
        first = next_cl
        if first + n > data_clusters + 2:
            raise RuntimeError("ESP is full")
        for i in range(n):
            c = first + i
            fat[c] = (c + 1) if (i + 1 < n) else 0x0FFFFFFF
        next_cl += n
        return first

    def dir_entry_count(node: dict, is_root: bool) -> int:
        count = 1 if is_root else 2  # volume label | "." + ".."
        for name in node:
            units = len(name.encode("utf-16le")) // 2
            count += 1 + (units + 12) // 13  # worst case: every name gets LFN entries
        return count

    def place_dir(node: dict, parent_cl: int, is_root: bool) -> int:
        dir_bytes = dir_entry_count(node, is_root) * 32
        my_cl = alloc(dir_bytes)

        used = {b".          ", b"..         "}
        out = bytearray()
        if is_root:
            out += _sfn_entry(volume_label.ljust(11, b" ")[:11], FAT_ATTR_VOLUME_ID, 0, 0)
        else:
            out += _sfn_entry(b".          ", FAT_ATTR_DIRECTORY, my_cl, 0)
            # ".." pointing at the root directory must use cluster 0 per the FAT spec
            out += _sfn_entry(b"..         ", FAT_ATTR_DIRECTORY, parent_cl, 0)

        for name, child in node.items():
            sfn, needs_lfn = _make_sfn(name, used)
            if needs_lfn:
                out += _lfn_entries(name, sfn)
            if isinstance(child, dict):
                child_cl = place_dir(child, 0 if is_root else my_cl, False)
                out += _sfn_entry(sfn, FAT_ATTR_DIRECTORY, child_cl, 0)
            else:
                data = bytes(child)
                if data:
                    fcl = alloc(len(data))
                    vol[cl_off(fcl) : cl_off(fcl) + len(data)] = data
                else:
                    fcl = 0
                out += _sfn_entry(sfn, FAT_ATTR_ARCHIVE, fcl, len(data))

        vol[cl_off(my_cl) : cl_off(my_cl) + len(out)] = out
        return my_cl

    root_cl = place_dir(tree, 0, True)
    assert root_cl == 2

    # FAT32 Boot Sector / BPB
    bpb = bytearray(512)
    bpb[0:3] = b"\xEB\x58\x90"
    bpb[3:11] = b"MSWIN4.1"
    struct.pack_into("<H", bpb, 11, SECTOR_SIZE)
    bpb[13] = spc
    struct.pack_into("<H", bpb, 14, rsvd_sectors)
    bpb[16] = num_fats
    struct.pack_into("<H", bpb, 17, 0)        # RootEntCnt (0 for FAT32)
    struct.pack_into("<H", bpb, 19, 0)        # TotSec16
    bpb[21] = 0xF8
    struct.pack_into("<H", bpb, 22, 0)        # FATSz16 (0 for FAT32)
    struct.pack_into("<H", bpb, 24, 63)
    struct.pack_into("<H", bpb, 26, 255)
    struct.pack_into("<I", bpb, 28, ESP_START_LBA)
    struct.pack_into("<I", bpb, 32, total_sectors)
    struct.pack_into("<I", bpb, 36, fat_sectors)
    struct.pack_into("<H", bpb, 40, 0)        # ExtFlags: FAT mirroring enabled
    struct.pack_into("<H", bpb, 42, 0)        # FSVer 0.0
    struct.pack_into("<I", bpb, 44, root_cl)  # Root cluster
    struct.pack_into("<H", bpb, 48, 1)        # FSInfo sector
    struct.pack_into("<H", bpb, 50, 6)        # Backup boot sector
    bpb[64] = 0x80
    bpb[66] = 0x29
    struct.pack_into("<I", bpb, 67, 0x2026C0DE)
    bpb[71:82] = volume_label.ljust(11, b" ")[:11]
    bpb[82:90] = b"FAT32   "
    struct.pack_into("<H", bpb, 510, 0xAA55)

    # FSInfo Sector
    fsinfo = bytearray(512)
    struct.pack_into("<I", fsinfo, 0, 0x41615252)
    struct.pack_into("<I", fsinfo, 484, 0x61417272)
    struct.pack_into("<I", fsinfo, 488, data_clusters - (next_cl - 2))  # Free cluster count
    struct.pack_into("<I", fsinfo, 492, next_cl)                        # Next free hint
    struct.pack_into("<I", fsinfo, 508, 0xAA550000)

    vol[0:512] = bpb
    vol[512:1024] = fsinfo
    vol[6 * 512 : 7 * 512] = bpb      # Backup boot sector
    vol[7 * 512 : 8 * 512] = fsinfo   # Backup FSInfo

    fat_bytes = struct.pack(f"<{len(fat)}I", *fat)
    for fat_idx in range(num_fats):
        f_off = (rsvd_sectors + fat_idx * fat_sectors) * SECTOR_SIZE
        vol[f_off : f_off + len(fat_bytes)] = fat_bytes

    return vol


# =============================================================================
# GPT Disk Image Assembler
# =============================================================================

def make_gpt_entry(type_guid: bytes, part_guid: bytes, start_lba: int, end_lba: int, name: str) -> bytes:
    ent = bytearray(GPT_ENTRY_SIZE)
    ent[0:16] = type_guid
    ent[16:32] = part_guid
    struct.pack_into("<Q", ent, 32, start_lba)
    struct.pack_into("<Q", ent, 40, end_lba)
    struct.pack_into("<Q", ent, 48, 0)
    utf16 = name.encode("utf-16le")[:72]
    ent[56 : 56 + len(utf16)] = utf16
    return bytes(ent)


def make_gpt_header(my_lba: int, alt_lba: int, entries_lba: int, disk_guid: bytes, entries_crc: int) -> bytes:
    hdr = bytearray(SECTOR_SIZE)
    hdr[0:8] = b"EFI PART"
    struct.pack_into("<I", hdr, 8, 0x00010000)
    struct.pack_into("<I", hdr, 12, 92)
    struct.pack_into("<I", hdr, 16, 0)
    struct.pack_into("<Q", hdr, 24, my_lba)
    struct.pack_into("<Q", hdr, 32, alt_lba)
    struct.pack_into("<Q", hdr, 40, FIRST_USABLE_LBA)
    struct.pack_into("<Q", hdr, 48, LAST_USABLE_LBA)
    hdr[56:72] = disk_guid
    struct.pack_into("<Q", hdr, 72, entries_lba)
    struct.pack_into("<I", hdr, 80, GPT_ENTRY_COUNT)
    struct.pack_into("<I", hdr, 84, GPT_ENTRY_SIZE)
    struct.pack_into("<I", hdr, 88, entries_crc)
    struct.pack_into("<I", hdr, 16, zlib.crc32(hdr[0:92]) & 0xFFFFFFFF)
    return bytes(hdr)


def main():
    if len(sys.argv) < 4:
        print("Usage: mkimage.py <kernel.elf> <limine.conf> <output.img>")
        sys.exit(1)

    kernel_path = sys.argv[1]
    conf_path   = sys.argv[2]
    output_path = sys.argv[3]

    with open(kernel_path, "rb") as f:
        kernel_bytes = f.read()
    with open(conf_path, "rb") as f:
        conf_bytes = f.read()

    efi_cache = os.path.join(os.path.dirname(os.path.abspath(__file__)), "BOOTX64.EFI")
    efi_bytes = ensure_limine_efi(efi_cache)

    assert ROOT_END_LBA <= LAST_USABLE_LBA, "RootFS partition overlaps backup GPT"

    disk = bytearray(TOTAL_SECTORS * SECTOR_SIZE)

    # 1. Protective MBR at LBA 0
    mbr = bytearray(512)
    mbr[446 + 0] = 0x00
    mbr[446 + 1] = 0x00
    mbr[446 + 2] = 0x02
    mbr[446 + 3] = 0x00
    mbr[446 + 4] = 0xEE  # GPT Protective Partition Type
    mbr[446 + 5] = 0xFF
    mbr[446 + 6] = 0xFF
    mbr[446 + 7] = 0xFF
    struct.pack_into("<I", mbr, 446 + 8, 1)
    struct.pack_into("<I", mbr, 446 + 12, min(TOTAL_SECTORS - 1, 0xFFFFFFFF))
    struct.pack_into("<H", mbr, 510, 0xAA55)
    disk[0:512] = mbr

    # 2. GPT Partition Entries (128 entries x 128 bytes = 32 sectors)
    # EFI System Partition GUID: C12A7328-F81F-11D2-BA4B-00A0C93EC93B
    esp_type_guid = uuid.UUID("C12A7328-F81F-11D2-BA4B-00A0C93EC93B").bytes_le
    # Microsoft Basic Data / exFAT GUID: EBD0A0A2-B9E5-4433-87C0-68B6B72699C7
    data_type_guid = uuid.UUID("EBD0A0A2-B9E5-4433-87C0-68B6B72699C7").bytes_le

    part_entries = bytearray(GPT_ENTRY_COUNT * GPT_ENTRY_SIZE)
    part_entries[0:128] = make_gpt_entry(
        esp_type_guid, guid("cortez-esp"), ESP_START_LBA, ESP_END_LBA, "EFI System Partition")
    part_entries[128:256] = make_gpt_entry(
        data_type_guid, guid("cortez-rootfs"), ROOT_START_LBA, ROOT_END_LBA, ROOTFS_PART_LABEL)
    entries_crc = zlib.crc32(part_entries) & 0xFFFFFFFF
    disk_guid = guid("cortez-disk")

    backup_hdr_lba     = TOTAL_SECTORS - 1
    backup_entries_lba = TOTAL_SECTORS - 1 - GPT_ENTRY_SECTORS

    primary_hdr = make_gpt_header(1, backup_hdr_lba, 2, disk_guid, entries_crc)
    backup_hdr  = make_gpt_header(backup_hdr_lba, 1, backup_entries_lba, disk_guid, entries_crc)

    disk[1 * SECTOR_SIZE : 2 * SECTOR_SIZE] = primary_hdr
    disk[2 * SECTOR_SIZE : FIRST_USABLE_LBA * SECTOR_SIZE] = part_entries
    disk[backup_entries_lba * SECTOR_SIZE : backup_hdr_lba * SECTOR_SIZE] = part_entries
    disk[backup_hdr_lba * SECTOR_SIZE : TOTAL_SECTORS * SECTOR_SIZE] = backup_hdr

    # 3. Populate Partition 1 (FAT32 ESP)
    #    Limine 8 searches for limine.conf in /, /limine, /boot, /boot/limine and /EFI/BOOT.
    esp_tree = {
        "EFI": {
            "BOOT": {
                "BOOTX64.EFI": efi_bytes,
                "limine.conf": conf_bytes,
            },
        },
        "boot": {
            "kernel.elf": kernel_bytes,
            "limine.conf": conf_bytes,
        },
        "limine.conf": conf_bytes,
    }
    esp_vol = build_fat32_esp(ESP_SECTORS, esp_tree)
    disk[ESP_START_LBA * SECTOR_SIZE : (ESP_END_LBA + 1) * SECTOR_SIZE] = esp_vol

    # 4. Populate Partition 2 (Physical exFAT RootFS)
    root_vol = build_exfat_partition(ROOT_SECTORS, kernel_bytes, conf_bytes)
    disk[ROOT_START_LBA * SECTOR_SIZE : (ROOT_END_LBA + 1) * SECTOR_SIZE] = root_vol

    os.makedirs(os.path.dirname(os.path.abspath(output_path)), exist_ok=True)
    with open(output_path, "wb") as f:
        f.write(disk)

    print(f"[mkimage] Created bootable GPT image: {output_path} ({len(disk) // (1024 * 1024)} MiB)")
    print(f"          Partition 1 (ESP   /dev/nvme0n1p1): LBA {ESP_START_LBA}..{ESP_END_LBA} (FAT32)")
    print(f"          Partition 2 (RootFS /dev/nvme0n1p2): LBA {ROOT_START_LBA}..{ROOT_END_LBA} (exFAT, '{ROOTFS_PART_LABEL}')")


if __name__ == "__main__":
    main()
