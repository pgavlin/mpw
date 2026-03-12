#!/usr/bin/env python3
"""
Set up an MPW environment from ETO or MPW-GM disk images.

Reads HFS disk images directly (no hfsutils or mounting required),
extracts Tools, Interfaces, and Libraries, and generates Environment.text.
Preserves resource forks via xattr on macOS, AppleDouble sidecars elsewhere.
"""

import argparse
import os
import shutil
import stat
import struct
import sys


# ---------------------------------------------------------------------------
# HFS Volume Parser
# ---------------------------------------------------------------------------

class HFSFile:
    __slots__ = ('parent_id', 'name', 'cnid', 'data_fork_size', 'data_extents',
                 'rsrc_fork_size', 'rsrc_extents', 'finder_type', 'finder_creator')

    def __init__(self, parent_id, name, rec):
        self.parent_id = parent_id
        self.name = name
        self.cnid = struct.unpack('>I', rec[20:24])[0]
        self.finder_type = rec[4:8]
        self.finder_creator = rec[8:12]
        self.data_fork_size = struct.unpack('>I', rec[26:30])[0]
        self.data_extents = []
        for j in range(3):
            o = 74 + j * 4
            s, c = struct.unpack('>HH', rec[o:o + 4])
            self.data_extents.append((s, c))
        self.rsrc_fork_size = struct.unpack('>I', rec[36:40])[0]
        self.rsrc_extents = []
        for j in range(3):
            o = 86 + j * 4
            s, c = struct.unpack('>HH', rec[o:o + 4])
            self.rsrc_extents.append((s, c))


class HFSVolume:
    """Parse an HFS volume from raw image bytes."""

    def __init__(self, data, offset=0):
        self.data = data
        self.vol_offset = offset

        mdb = data[offset + 1024:offset + 1024 + 162]
        sig = struct.unpack('>H', mdb[0:2])[0]
        if sig != 0x4244:
            raise ValueError(f"Not an HFS volume (sig=0x{sig:04x})")

        self.num_alloc_blocks = struct.unpack('>H', mdb[18:20])[0]
        self.alloc_block_size = struct.unpack('>I', mdb[20:24])[0]
        self.alloc_start = struct.unpack('>H', mdb[28:30])[0]
        name_len = mdb[36]
        self.vol_name = mdb[37:37 + name_len].decode('mac_roman', errors='replace')

        self._ext_overflow = {}
        self._dir_names = {1: '', 2: self.vol_name}
        self._dir_parents = {2: 1}
        self._files = []

        self._find_btrees()
        self._load_extents_overflow()
        self._walk_catalog()

    def _ab_offset(self, ab):
        return self.vol_offset + self.alloc_start * 512 + ab * self.alloc_block_size

    def _read_fork(self, extents, size):
        if size == 0:
            return b''
        result = bytearray()
        for start, count in extents:
            if count == 0:
                continue
            off = self._ab_offset(start)
            length = count * self.alloc_block_size
            end = min(off + length, len(self.data))
            if off < end:
                result.extend(self.data[off:end])
        return bytes(result[:size])

    def _find_btrees(self):
        data = self.data
        self._cat_info = None
        self._xt_info = None

        for off in range(self.vol_offset, len(data) - 512, 512):
            node = data[off:off + 512]
            if node[8] == 1 and node[9] == 0:  # header node
                nd_nrecs = struct.unpack('>H', node[10:12])[0]
                if nd_nrecs == 3:
                    bth = node[14:]
                    bthNodeSize = struct.unpack('>H', bth[18:20])[0]
                    bthDepth = struct.unpack('>H', bth[0:2])[0]
                    bthKeyLen = struct.unpack('>H', bth[20:22])[0]
                    if 0 < bthDepth < 20 and 512 <= bthNodeSize <= 8192:
                        info = {
                            'base': off,
                            'nodeSize': bthNodeSize,
                            'depth': bthDepth,
                            'root': struct.unpack('>I', bth[2:6])[0],
                            'nRecs': struct.unpack('>I', bth[6:10])[0],
                            'firstLeaf': struct.unpack('>I', bth[10:14])[0],
                            'lastLeaf': struct.unpack('>I', bth[14:18])[0],
                            'keyLen': bthKeyLen,
                            'nNodes': struct.unpack('>I', bth[22:26])[0],
                        }
                        if bthKeyLen == 37 and self._cat_info is None:
                            self._cat_info = info
                        elif bthKeyLen == 7 and self._xt_info is None:
                            self._xt_info = info
            if self._cat_info and self._xt_info:
                break
            if (self._cat_info and self._xt_info is None
                    and off > self._cat_info['base'] + 1024 * 1024):
                break

        if self._cat_info is None:
            raise ValueError("Could not find catalog b-tree")

    def _load_extents_overflow(self):
        if self._xt_info is None:
            return
        xi = self._xt_info
        xb = xi['base']
        xns = xi['nodeSize']
        node_num = xi['firstLeaf']
        while node_num != 0:
            noff = xb + node_num * xns
            if noff + xns > len(self.data):
                break
            node = self.data[noff:noff + xns]
            nd_flink = struct.unpack('>I', node[0:4])[0]
            if node[8] != 0xFF:
                node_num = nd_flink
                continue
            nd_nrecs = struct.unpack('>H', node[10:12])[0]
            for i in range(nd_nrecs):
                rp = xns - 2 * (i + 1)
                ro = struct.unpack('>H', node[rp:rp + 2])[0]
                if ro + 8 > xns:
                    break
                key_len = node[ro]
                fork_type = node[ro + 1]
                cnid = struct.unpack('>I', node[ro + 2:ro + 6])[0]
                do = ro + 1 + key_len
                if do % 2 == 1:
                    do += 1
                exts = []
                for j in range(3):
                    eo = do + j * 4
                    if eo + 4 <= xns:
                        s, c = struct.unpack('>HH', node[eo:eo + 4])
                        exts.append((s, c))
                key = (cnid, fork_type)
                if key not in self._ext_overflow:
                    self._ext_overflow[key] = []
                self._ext_overflow[key].extend(exts)
            node_num = nd_flink

    def _walk_catalog(self):
        ci = self._cat_info
        cat_base = ci['base']
        node_size = ci['nodeSize']

        node_num = ci['firstLeaf']
        while node_num != 0:
            noff = cat_base + node_num * node_size
            if noff + node_size > len(self.data):
                break
            node = self.data[noff:noff + node_size]
            nd_flink = struct.unpack('>I', node[0:4])[0]
            if node[8] != 0xFF:
                node_num = nd_flink
                continue
            nd_nrecs = struct.unpack('>H', node[10:12])[0]

            for i in range(nd_nrecs):
                rp = node_size - 2 * (i + 1)
                if rp < 14:
                    break
                ro = struct.unpack('>H', node[rp:rp + 2])[0]
                if ro + 7 > node_size:
                    break
                kl = node[ro]
                if kl == 0:
                    continue
                parent = struct.unpack('>I', node[ro + 2:ro + 6])[0]
                nl = node[ro + 6]
                if ro + 7 + nl > node_size:
                    continue
                name = node[ro + 7:ro + 7 + nl].decode('mac_roman', errors='replace')

                do = ro + 1 + kl
                if do % 2 == 1:
                    do += 1
                if do + 2 > node_size:
                    continue
                rtype = node[do]

                if rtype == 1 and do + 70 <= node_size:  # directory
                    dir_cnid = struct.unpack('>I', node[do + 6:do + 10])[0]
                    self._dir_names[dir_cnid] = name
                    self._dir_parents[dir_cnid] = parent
                elif rtype == 2 and do + 98 <= node_size:  # file
                    rec = bytes(node[do:do + 98])
                    self._files.append(HFSFile(parent, name, rec))

            node_num = nd_flink

    def _get_path(self, cnid):
        parts = []
        seen = set()
        while cnid in self._dir_names and cnid != 1 and cnid not in seen:
            seen.add(cnid)
            parts.append(self._dir_names[cnid])
            cnid = self._dir_parents.get(cnid, 1)
        parts.reverse()
        return '/'.join(parts)

    def list_files(self):
        """Return list of (path, HFSFile) tuples."""
        result = []
        for f in self._files:
            path = self._get_path(f.parent_id)
            full_path = (path + '/' + f.name) if path else f.name
            result.append((full_path, f))
        return result

    def list_dirs(self):
        """Return list of directory paths."""
        result = []
        for cnid in self._dir_names:
            if cnid <= 2:
                continue
            result.append(self._get_path(cnid))
        return result

    def read_data_fork(self, f):
        exts = list(f.data_extents)
        if f.data_fork_size > sum(c for _, c in exts) * self.alloc_block_size:
            if (f.cnid, 0x00) in self._ext_overflow:
                exts.extend(self._ext_overflow[(f.cnid, 0x00)])
        return self._read_fork(exts, f.data_fork_size)

    def read_rsrc_fork(self, f):
        exts = list(f.rsrc_extents)
        if f.rsrc_fork_size > sum(c for _, c in exts) * self.alloc_block_size:
            if (f.cnid, 0xFF) in self._ext_overflow:
                exts.extend(self._ext_overflow[(f.cnid, 0xFF)])
        return self._read_fork(exts, f.rsrc_fork_size)


# ---------------------------------------------------------------------------
# Image Format Detection
# ---------------------------------------------------------------------------

def unwrap_macbinary(data):
    """If data is MacBinary-wrapped, strip the 128-byte header and return
    (data_fork, rsrc_fork). Otherwise return None."""
    if len(data) < 128:
        return None
    # MacBinary checks: byte 0 == 0, byte 74 == 0, byte 82 == 0
    if data[0] != 0 or data[74] != 0 or data[82] != 0:
        return None
    name_len = data[1]
    if not (1 <= name_len <= 63):
        return None
    # Version byte at 122 should be >= 129 for MacBinary II+
    if data[122] < 129:
        return None
    data_fork_len = struct.unpack('>I', data[83:87])[0]
    rsrc_fork_len = struct.unpack('>I', data[87:91])[0]
    if 128 + data_fork_len > len(data):
        return None
    data_fork = data[128:128 + data_fork_len]
    rsrc_fork = b''
    if rsrc_fork_len > 0:
        # Resource fork follows data fork, padded to 128-byte boundary
        rsrc_start = 128 + ((data_fork_len + 127) // 128 * 128)
        if rsrc_start + rsrc_fork_len <= len(data):
            rsrc_fork = data[rsrc_start:rsrc_start + rsrc_fork_len]
    return data_fork, rsrc_fork


# ---------------------------------------------------------------------------
# NDIF (DiskCopy 6.x compressed image) support
# ---------------------------------------------------------------------------

def _adc_decompress(src):
    """Decompress Apple Data Compression (ADC) data."""
    out = bytearray()
    i = 0
    while i < len(src):
        cmd = src[i]
        if cmd & 0x80:
            # Literal: copy (cmd & 0x7F) + 1 bytes
            count = (cmd & 0x7F) + 1
            i += 1
            out.extend(src[i:i + count])
            i += count
        elif cmd & 0x40:
            # 3-byte long match
            if i + 2 >= len(src):
                break
            count = (cmd & 0x3F) + 4
            offset = (src[i + 1] << 8) | src[i + 2]
            i += 3
            for _ in range(count):
                out.append(out[-(offset + 1)] if offset < len(out) else 0)
        else:
            # 2-byte short match
            if i + 1 >= len(src):
                break
            count = ((cmd >> 2) & 0x0F) + 3
            offset = ((cmd & 0x03) << 8) | src[i + 1]
            i += 2
            for _ in range(count):
                out.append(out[-(offset + 1)] if offset < len(out) else 0)
    return bytes(out)


def _parse_rsrc_fork(rsrc_data):
    """Parse a resource fork and return dict of {(type, id): data}."""
    if len(rsrc_data) < 16:
        return {}
    data_off = struct.unpack('>I', rsrc_data[0:4])[0]
    map_off = struct.unpack('>I', rsrc_data[4:8])[0]
    if map_off + 28 > len(rsrc_data):
        return {}
    rm = rsrc_data[map_off:]
    type_list_off = struct.unpack('>H', rm[24:26])[0]
    num_types = struct.unpack('>h', rm[type_list_off:type_list_off + 2])[0] + 1
    resources = {}
    tl_base = type_list_off + 2
    for i in range(num_types):
        to = tl_base + i * 8
        if to + 8 > len(rm):
            break
        rtype = rm[to:to + 4].decode('mac_roman', errors='replace')
        rcount = struct.unpack('>H', rm[to + 4:to + 6])[0] + 1
        ref_off = struct.unpack('>H', rm[to + 6:to + 8])[0]
        for j in range(rcount):
            ro = type_list_off + ref_off + j * 12
            if ro + 8 > len(rm):
                break
            rid = struct.unpack('>H', rm[ro:ro + 2])[0]
            rdata_off = (rm[ro + 4] & 0x0F) << 16
            rdata_off |= struct.unpack('>H', rm[ro + 6:ro + 8])[0]
            doff = data_off + rdata_off
            if doff + 4 > len(rsrc_data):
                continue
            dlen = struct.unpack('>I', rsrc_data[doff:doff + 4])[0]
            resources[(rtype, rid)] = rsrc_data[doff + 4:doff + 4 + dlen]
    return resources


def decompress_ndif(data_fork, rsrc_fork):
    """Decompress an NDIF (DiskCopy 6.x) image using the bcem resource.
    Returns raw disk data."""
    resources = _parse_rsrc_fork(rsrc_fork)
    bcem = resources.get(('bcem', 128))
    if bcem is None:
        return None

    # Parse block map entries starting at offset 128 in bcem data
    # Each entry: 3 bytes sector_start + 1 byte type + 4 bytes data_offset + 4 bytes data_length
    entries = []
    off = 128
    while off + 12 <= len(bcem):
        info, doff, dlen = struct.unpack('>III', bcem[off:off + 12])
        sector_start = (info >> 8) & 0xFFFFFF
        btype = info & 0xFF
        entries.append((sector_start, btype, doff, dlen))
        off += 12
        if btype == 0xFF:
            break

    if not entries or entries[-1][1] != 0xFF:
        return None

    total_sectors = entries[-1][0]
    disk = bytearray(total_sectors * 512)

    for i in range(len(entries) - 1):
        sector_start, btype, doff, dlen = entries[i]
        next_sector = entries[i + 1][0]
        byte_offset = sector_start * 512

        if btype == 0x00:
            pass  # zero fill (already zeroed)
        elif btype == 0x02:
            disk[byte_offset:byte_offset + dlen] = data_fork[doff:doff + dlen]
        elif btype == 0x83:
            decompressed = _adc_decompress(data_fork[doff:doff + dlen])
            disk[byte_offset:byte_offset + len(decompressed)] = decompressed
        else:
            raise ValueError(f"Unknown NDIF block type 0x{btype:02x}")

    return bytes(disk)


def find_hfs_partition(data):
    """Scan for Apple Partition Map and return offset of Apple_HFS partition."""
    if len(data) < 1024:
        return None
    sig = struct.unpack('>H', data[512:514])[0]
    if sig != 0x504D:
        return None
    num_parts = struct.unpack('>I', data[516:520])[0]
    for i in range(num_parts):
        entry_off = 512 * (1 + i)
        if entry_off + 512 > len(data):
            break
        entry = data[entry_off:entry_off + 512]
        esig = struct.unpack('>H', entry[0:2])[0]
        if esig != 0x504D:
            continue
        ptype = entry[48:80].split(b'\x00', 1)[0].decode('ascii', errors='replace')
        if ptype == 'Apple_HFS':
            pblock_start = struct.unpack('>I', entry[8:12])[0]
            return pblock_start * 512
    return None


def open_hfs_image(data):
    """Detect format and return an HFSVolume."""
    # Try MacBinary unwrap first
    rsrc_fork = b''
    result = unwrap_macbinary(data)
    if result is not None:
        data, rsrc_fork = result

    # Try direct HFS at offset 0
    if len(data) > 1024 + 2:
        sig = struct.unpack('>H', data[1024:1026])[0]
        if sig == 0x4244:
            try:
                return HFSVolume(data, 0)
            except ValueError:
                pass

    # Try NDIF (DiskCopy 6.x compressed image)
    if rsrc_fork:
        raw_disk = decompress_ndif(data, rsrc_fork)
        if raw_disk is not None:
            return HFSVolume(raw_disk, 0)

    # Try Apple Partition Map
    part_offset = find_hfs_partition(data)
    if part_offset is not None:
        return HFSVolume(data, part_offset)

    raise ValueError("Could not find HFS volume in image")


# ---------------------------------------------------------------------------
# MPW Directory Discovery
# ---------------------------------------------------------------------------

# Search patterns: (target_name, path_patterns)
MPW_TARGETS = [
    ('Tools', ['MPW/Tools', 'Tools']),
    ('Interfaces', ['Interfaces&Libraries/Interfaces', 'Interfaces']),
    ('Libraries', ['Interfaces&Libraries/Libraries', 'Libraries']),
]


def find_mpw_content(dirs):
    """Locate Tools, Interfaces, Libraries directories in the volume.
    Returns dict mapping target name to volume path, e.g.
    {'Tools': 'E.T.O. #23/MPW/Tools', ...}
    """
    results = {}
    for target, patterns in MPW_TARGETS:
        # Collect all matching directories for each pattern, in priority order
        candidates = []
        for pat in patterns:
            pat_lower = pat.lower()
            for d in dirs:
                dl = d.lower()
                if dl.endswith('/' + pat_lower) or dl == pat_lower:
                    candidates.append(d)
            # If we found matches for a more-specific pattern, use those
            if candidates:
                break
        if candidates:
            # Prefer the shortest path (most top-level match)
            candidates.sort(key=lambda d: d.count('/'))
            results[target] = candidates[0]

    return results


# ---------------------------------------------------------------------------
# File Writing with Resource Fork Preservation
# ---------------------------------------------------------------------------

def create_apple_double(rsrc_data, finder_info=None):
    """Create AppleDouble sidecar content with Finder Info and/or resource fork."""
    entries = []
    if finder_info:
        # Pad Finder Info to 32 bytes
        fi = bytearray(32)
        fi[:len(finder_info)] = finder_info
        entries.append((9, bytes(fi)))  # Entry ID 9 = Finder Info
    if rsrc_data:
        entries.append((2, rsrc_data))  # Entry ID 2 = Resource fork

    if not entries:
        return None

    num_entries = len(entries)
    header_size = 26 + num_entries * 12  # header + entry table

    # Calculate total size
    total = header_size
    for _, data in entries:
        total += len(data)

    out = bytearray(total)
    # Magic
    struct.pack_into('>I', out, 0, 0x00051607)
    # Version
    struct.pack_into('>I', out, 4, 0x00020000)
    # Filler: 16 bytes of zeros (already zeroed)
    # Number of entries
    struct.pack_into('>H', out, 24, num_entries)

    # Entry table and data
    data_offset = header_size
    for i, (entry_id, data) in enumerate(entries):
        entry_off = 26 + i * 12
        struct.pack_into('>I', out, entry_off, entry_id)
        struct.pack_into('>I', out, entry_off + 4, data_offset)
        struct.pack_into('>I', out, entry_off + 8, len(data))
        out[data_offset:data_offset + len(data)] = data
        data_offset += len(data)

    return bytes(out)


def write_file(dest, data_fork, rsrc_fork, finder_info=None):
    """Write a file with its data fork, optional resource fork, and Finder Info."""
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    with open(dest, 'wb') as f:
        f.write(data_fork)

    if sys.platform == 'darwin':
        if finder_info:
            try:
                import xattr
                # Pad to 32 bytes
                fi = bytearray(32)
                fi[:len(finder_info)] = finder_info
                xattr.setxattr(dest, 'com.apple.FinderInfo', bytes(fi))
            except (ImportError, OSError):
                pass
        if rsrc_fork:
            try:
                import xattr
                xattr.setxattr(dest, 'com.apple.ResourceFork', rsrc_fork)
            except ImportError:
                try:
                    rsrc_path = dest + '/..namedfork/rsrc'
                    with open(rsrc_path, 'wb') as f:
                        f.write(rsrc_fork)
                except OSError:
                    _write_apple_double(dest, rsrc_fork, finder_info)
        elif finder_info:
            _write_apple_double(dest, None, finder_info)
    else:
        if rsrc_fork or finder_info:
            _write_apple_double(dest, rsrc_fork, finder_info)


def _write_apple_double(dest, rsrc_fork, finder_info=None):
    """Write an AppleDouble sidecar file."""
    ad_data = create_apple_double(rsrc_fork, finder_info)
    if ad_data is None:
        return
    directory = os.path.dirname(dest)
    basename = os.path.basename(dest)
    sidecar = os.path.join(directory, '._' + basename)
    with open(sidecar, 'wb') as f:
        f.write(ad_data)


# ---------------------------------------------------------------------------
# Environment.text
# ---------------------------------------------------------------------------

DEFAULT_ENVIRONMENT = """\
# MPW Environment file
# ${var}, $var, and {var} substitution supported.
# $MPW and $Command are pre-defined
# $MPW uses MacOS : paths.
#
# $MPW includes the trailing ':'.
# Since '::' is equivalent to '..' in Unix,
# A ':'' after a variable (eg, ${MPW}:)
# will be dropped if it's duplicative.
#
#
# = assigns (replacing any existing value)
# ?= will not replace an existing value.
# += will append to any existing value.

MPWVersion ?= 3.2

ShellDirectory=$MPW
SysTempFolder=/tmp/
TempFolder=/tmp/

# comma-separated list of directories, like $PATH
# add . to include the current directory.
Commands=${MPW}Tools:

# MPW IIgs 1.1
AIIGSIncludes=${MPW}Interfaces:AIIGSIncludes:
RIIGSIncludes=${MPW}Interfaces:RIIGSIncludes:
CIIGSIncludes=${MPW}Interfaces:CIIGSIncludes:
CIIGSLibraries=${MPW}Libraries:CIIGSLibraries:
PIIGSIncludes=${MPW}Interfaces:PIIGSIncludes:
PIIGSLibraries=${MPW}Libraries:PIIGSLibraries:

# MPW IIgs 1.0 compatibility
AIIGSInclude=${MPW}Interfaces:AIIGSIncludes:
RIIGSInclude=${MPW}Interfaces:RIIGSIncludes:
CIIGSinclude=${MPW}Interfaces:CIIGSIncludes:
CIIGSLibrary=${MPW}Libraries:CIIGSIncludes:

# MPW Macintosh compilers
SCIncludes=${MPW}Interfaces:CIncludes:
CIncludes=${MPW}Interfaces:CIncludes:
AIncludes=${MPW}Interfaces:AIncludes:
RIncludes=${MPW}Interfaces:RIncludes:
PInterfaces=${MPW}Interfaces:PInterfaces:

Libraries=${MPW}Libraries:Libraries:
PLibraries=${MPW}Libraries:PLibraries:
CLibraries=${MPW}Libraries:CLibraries:

# power pc, etc
PPCLibraries=${MPW}Libraries:PPCLibraries:
SharedLibraries=${MPW}Libraries:SharedLibraries:
CFM68KLibraries=${MPW}Libraries:CFM68KLibraries:

#MetroWerks
MW68KLibraries=${MPW}Libraries:MW68KLibraries:
MWPPCLibraries=${MPW}Libraries:MWPPCLibraries:
MWCIncludes=${CIncludes}

# to support 3.2 and 3.5:
# use mpw -DMPVersion=3.5 ...
# and have versioned folders.
#Libraries=${MPW}Libraries:Libraries-${MPWVersion}:
#PLibraries=${MPW}Libraries:PLibraries-${MPWVersion}:
#CLibraries=${MPW}Libraries:CLibraries-${MPWVersion}:
"""


def install_verbatim_files(output_dir):
    """Copy verbatim support files (Environment.text, Errors.text, etc.)
    into the output directory. Skips files that already exist."""
    script_dir = os.path.dirname(os.path.abspath(__file__))
    verbatim_dir = os.path.join(script_dir, '..', 'verbatim')

    if os.path.isdir(verbatim_dir):
        for name in os.listdir(verbatim_dir):
            src = os.path.join(verbatim_dir, name)
            if not os.path.isfile(src):
                continue
            dest = os.path.join(output_dir, name)
            if os.path.exists(dest):
                continue
            with open(src, 'rb') as f:
                content = f.read()
            with open(dest, 'wb') as f:
                f.write(content)

    # Fall back to embedded default for Environment.text if not copied
    env_dest = os.path.join(output_dir, 'Environment.text')
    if not os.path.exists(env_dest):
        with open(env_dest, 'w') as f:
            f.write(DEFAULT_ENVIRONMENT)


def install_tools(output_dir):
    """Install mpw, disasm, and package_hfs.py into {output_dir}/bin.

    Searches for the binaries in the build directory relative to the
    source tree (../build/bin/) and also checks if they're installed
    alongside this script. Copies package_hfs.py from the same directory
    as this script.
    """
    script_dir = os.path.dirname(os.path.abspath(__file__))
    bin_dir = os.path.join(output_dir, 'bin')

    # Install compiled binaries
    binary_names = ['mpw', 'disasm']
    search_dirs = [
        os.path.join(script_dir, '..', 'build', 'bin'),
        script_dir,
    ]

    installed = []
    for name in binary_names:
        for search_dir in search_dirs:
            src = os.path.join(search_dir, name)
            if os.path.isfile(src):
                os.makedirs(bin_dir, exist_ok=True)
                dest = os.path.join(bin_dir, name)
                shutil.copy2(src, dest)
                st = os.stat(dest)
                os.chmod(dest, st.st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
                installed.append(name)
                break

    # Install Python scripts
    for name in ['package_hfs.py']:
        src = os.path.join(script_dir, name)
        if os.path.isfile(src):
            os.makedirs(bin_dir, exist_ok=True)
            dest = os.path.join(bin_dir, name)
            shutil.copy2(src, dest)
            st = os.stat(dest)
            os.chmod(dest, st.st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
            installed.append(name)

    if installed:
        print(f"Installed tools: {', '.join(installed)} -> {bin_dir}")
    else:
        print("Note: mpw/disasm binaries not found. Build the project first,")
        print("  then re-run setup or copy them manually.")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description='Set up an MPW environment from ETO or MPW-GM disk images.')
    parser.add_argument('images', metavar='IMAGE', nargs='+',
                        help='Path to a disk image (.toast, .img, .bin)')
    parser.add_argument('-o', '--output', default=os.path.expanduser('~/mpw'),
                        help='Output directory (default: ~/mpw)')
    parser.add_argument('--auto', action='store_true',
                        help='Use first match without prompting')
    args = parser.parse_args()

    output_dir = args.output
    os.makedirs(output_dir, exist_ok=True)

    for image_path in args.images:
        print(f"Reading {image_path}...")
        with open(image_path, 'rb') as f:
            data = f.read()

        vol = open_hfs_image(data)
        print(f"  Volume: {vol.vol_name}")
        print(f"  Alloc block size: {vol.alloc_block_size}, "
              f"count: {vol.num_alloc_blocks}")

        dirs = vol.list_dirs()
        files = vol.list_files()
        print(f"  {len(dirs)} directories, {len(files)} files")

        content = find_mpw_content(dirs)
        if not content:
            print(f"  WARNING: No MPW content found in {image_path}, skipping.")
            continue

        print(f"  Found MPW content:")
        for target, path in sorted(content.items()):
            print(f"    {target}: {path}")

        if not args.auto:
            try:
                answer = input("  Proceed with extraction? [Y/n] ").strip().lower()
                if answer and answer != 'y':
                    print("  Skipping.")
                    continue
            except (EOFError, KeyboardInterrupt):
                print("\n  Skipping.")
                continue

        # Extract files
        extracted = 0
        for target, src_prefix in content.items():
            target_dir = os.path.join(output_dir, target)
            prefix_with_slash = src_prefix + '/'

            for path, hfs_file in files:
                if not (path.startswith(prefix_with_slash)
                        or path == src_prefix):
                    continue

                rel = path[len(prefix_with_slash):]
                if not rel:
                    continue

                # Replace Mac path separators
                safe_rel = rel.replace('/', os.sep)
                dest = os.path.join(target_dir, safe_rel)

                data_fork = vol.read_data_fork(hfs_file)
                rsrc_fork = vol.read_rsrc_fork(hfs_file)
                finder_info = hfs_file.finder_type + hfs_file.finder_creator
                write_file(dest, data_fork, rsrc_fork, finder_info)
                extracted += 1
                if extracted % 200 == 0:
                    print(f"    {extracted} files extracted...")

        print(f"  Extracted {extracted} files from {vol.vol_name}")

    install_verbatim_files(output_dir)
    install_tools(output_dir)
    print(f"\nMPW environment set up in {output_dir}")
    print(f"Set MPW={output_dir} to use with the emulator.")


if __name__ == '__main__':
    main()
