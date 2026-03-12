#!/usr/bin/env python3
"""
Package a directory tree into an HFS disk image.

Reads AppleDouble sidecar files (._name) for resource forks and Finder Info.
Produces a raw HFS disk image (.img) suitable for use with emulators or
classic Mac utilities.
"""

import argparse
import os
import struct
import sys
import time


# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

# Seconds between Unix epoch (1970-01-01) and Mac epoch (1904-01-01)
MAC_EPOCH_OFFSET = 2082844800

# AppleDouble
APPLE_DOUBLE_MAGIC = 0x00051607

# HFS special CNIDs
CNID_ROOT_PARENT = 1
CNID_ROOT_DIR = 2
CNID_EXTENTS_FILE = 3
CNID_CATALOG_FILE = 4
CNID_FIRST_USER = 16

# B-tree node types
NODE_HEADER = 1
NODE_MAP = 2
NODE_INDEX = 0
NODE_LEAF = 0xFF

# Catalog record types
REC_DIR = 1
REC_FILE = 2
REC_DIR_THREAD = 3
REC_FILE_THREAD = 4


def mac_time(unix_time=None):
    """Convert Unix timestamp to Mac timestamp."""
    if unix_time is None:
        unix_time = int(time.time())
    return unix_time + MAC_EPOCH_OFFSET


# ---------------------------------------------------------------------------
# AppleDouble Parser
# ---------------------------------------------------------------------------

def parse_apple_double(data):
    """Parse an AppleDouble file. Returns (resource_fork, finder_info)."""
    if len(data) < 26:
        return None, None
    magic = struct.unpack('>I', data[0:4])[0]
    if magic != APPLE_DOUBLE_MAGIC:
        return None, None
    num_entries = struct.unpack('>H', data[24:26])[0]
    rsrc = None
    finfo = None
    for i in range(num_entries):
        eoff = 26 + i * 12
        if eoff + 12 > len(data):
            break
        eid, estart, elen = struct.unpack('>III', data[eoff:eoff + 12])
        if estart + elen > len(data):
            continue
        if eid == 2:
            rsrc = data[estart:estart + elen]
        elif eid == 9:
            finfo = data[estart:estart + elen]
    return rsrc, finfo


# ---------------------------------------------------------------------------
# HFS B-tree Builder
# ---------------------------------------------------------------------------

def _build_node(node_size, flink, blink, node_type, height, records):
    """Build a single B-tree node."""
    node = bytearray(node_size)
    # Node descriptor (14 bytes)
    struct.pack_into('>I', node, 0, flink)
    struct.pack_into('>I', node, 4, blink)
    node[8] = node_type
    node[9] = height
    struct.pack_into('>H', node, 10, len(records))

    # Write records and build offset table
    offsets = []
    pos = 14
    for rec in records:
        offsets.append(pos)
        node[pos:pos + len(rec)] = rec
        pos += len(rec)
        if pos % 2:
            pos += 1  # pad to even boundary
    offsets.append(pos)  # free space offset

    # Offset table grows backwards from end of node
    for i, off in enumerate(offsets):
        struct.pack_into('>H', node, node_size - 2 * (i + 1), off)

    return bytes(node)


def _records_fit(node_size, existing, new_record):
    """Check if a new record fits in a node with existing records."""
    used = 14
    for r in existing:
        used += len(r)
        if used % 2:
            used += 1
    used += len(new_record)
    if used % 2:
        used += 1
    offset_entries = len(existing) + 2  # +1 for new record, +1 for free space
    return used + offset_entries * 2 <= node_size


def build_btree(sorted_records, node_size, max_key_len):
    """Build a complete B-tree file from sorted (key_bytes, data_bytes) pairs.

    Returns the B-tree data as bytes.
    """
    # Pack records into leaf nodes
    leaf_nodes = []
    current = []
    for key_bytes, data_bytes in sorted_records:
        rec = key_bytes + data_bytes
        if current and not _records_fit(node_size, current, rec):
            leaf_nodes.append(current)
            current = []
        current.append(rec)
    if current:
        leaf_nodes.append(current)
    if not leaf_nodes:
        leaf_nodes = [[]]  # at least one empty leaf

    num_leaves = len(leaf_nodes)

    # Tree structure:
    #   Node 0 = header
    #   Nodes 1..num_leaves = leaf nodes
    #   Node num_leaves+1 = index node (if depth == 2)
    if num_leaves == 1:
        depth = 1
        root_node = 1
        total_nodes = 2
    else:
        depth = 2
        root_node = num_leaves + 1
        total_nodes = num_leaves + 2

    first_leaf = 1
    last_leaf = num_leaves
    total_records = sum(len(recs) for recs in leaf_nodes)

    # Header node (node 0): 3 records
    # Record 0: B-tree header (30 bytes)
    hdr_rec = struct.pack('>HIIIIHHII',
                          depth, root_node, total_records,
                          first_leaf, last_leaf,
                          node_size, max_key_len,
                          total_nodes, 0)  # 0 free nodes
    # Record 1: reserved (128 bytes)
    reserved_rec = bytes(128)
    # Record 2: node allocation bitmap
    # Calculate available space for the map record
    pos = 14 + len(hdr_rec)
    if pos % 2:
        pos += 1
    pos += len(reserved_rec)
    if pos % 2:
        pos += 1
    map_start = pos
    map_size = node_size - map_start - 4 * 2  # 4 offset entries
    if map_size < 1:
        map_size = 1
    map_rec = bytearray(map_size)
    for i in range(min(total_nodes, map_size * 8)):
        map_rec[i // 8] |= (0x80 >> (i % 8))

    header_node = _build_node(node_size, 0, 0, NODE_HEADER, 0,
                              [hdr_rec, reserved_rec, bytes(map_rec)])

    # Build leaf nodes
    encoded_leaves = []
    for i, recs in enumerate(leaf_nodes):
        node_num = first_leaf + i
        flink = node_num + 1 if i < num_leaves - 1 else 0
        blink = node_num - 1 if i > 0 else 0
        leaf = _build_node(node_size, flink, blink, NODE_LEAF, 1, recs)
        encoded_leaves.append(leaf)

    # Build index node if needed
    encoded_index = None
    if depth == 2:
        index_recs = []
        for i, recs in enumerate(leaf_nodes):
            node_num = first_leaf + i
            first_rec = recs[0]
            key_len = first_rec[0]
            key_bytes = first_rec[:1 + key_len]
            if len(key_bytes) % 2:
                key_bytes += b'\x00'
            pointer = struct.pack('>I', node_num)
            index_recs.append(key_bytes + pointer)
        encoded_index = _build_node(node_size, 0, 0, NODE_INDEX, 2,
                                    index_recs)

    # Assemble
    result = bytearray(total_nodes * node_size)
    result[0:node_size] = header_node
    for i, leaf in enumerate(encoded_leaves):
        off = (first_leaf + i) * node_size
        result[off:off + node_size] = leaf
    if encoded_index is not None:
        off = root_node * node_size
        result[off:off + node_size] = encoded_index

    return bytes(result)


# ---------------------------------------------------------------------------
# Catalog Record Encoding
# ---------------------------------------------------------------------------

def _encode_catalog_key(parent_id, name):
    """Encode an HFS catalog key."""
    if isinstance(name, str):
        name_bytes = name.encode('mac_roman', errors='replace')
    else:
        name_bytes = name
    name_bytes = name_bytes[:31]
    key_len = 1 + 4 + 1 + len(name_bytes)  # reserved + parentID + nameLen + name
    data = bytearray()
    data.append(key_len)
    data.append(0)  # reserved
    data.extend(struct.pack('>I', parent_id))
    data.append(len(name_bytes))
    data.extend(name_bytes)
    if len(data) % 2:
        data.append(0)  # pad to even
    return bytes(data)


def _encode_dir_record(cnid, valence, cr_date, mod_date):
    """Encode a 70-byte catalog directory record."""
    rec = bytearray(70)
    rec[0] = REC_DIR
    struct.pack_into('>H', rec, 4, valence)
    struct.pack_into('>I', rec, 6, cnid)
    struct.pack_into('>I', rec, 10, cr_date)
    struct.pack_into('>I', rec, 14, mod_date)
    return bytes(rec)


def _encode_file_record(cnid, data_log_size, data_phys_size, data_extents,
                        rsrc_log_size, rsrc_phys_size, rsrc_extents,
                        finder_type, finder_creator, cr_date, mod_date):
    """Encode a 102-byte catalog file record."""
    rec = bytearray(102)
    rec[0] = REC_FILE
    rec[2] = 0x02  # filFlags: thread record exists
    # Finder Info (FInfo) at offset 4
    rec[4:8] = finder_type
    rec[8:12] = finder_creator
    struct.pack_into('>I', rec, 20, cnid)       # filFlNum
    # Data fork
    struct.pack_into('>I', rec, 26, data_log_size)   # filLgLen
    struct.pack_into('>I', rec, 30, data_phys_size)  # filPyLen
    # Resource fork
    struct.pack_into('>I', rec, 36, rsrc_log_size)   # filRLgLen
    struct.pack_into('>I', rec, 40, rsrc_phys_size)  # filRPyLen
    # Dates
    struct.pack_into('>I', rec, 44, cr_date)    # filCrDat
    struct.pack_into('>I', rec, 48, mod_date)   # filMdDat
    # Data fork extents at offset 74
    for i, (start, count) in enumerate(data_extents[:3]):
        struct.pack_into('>HH', rec, 74 + i * 4, start, count)
    # Resource fork extents at offset 86
    for i, (start, count) in enumerate(rsrc_extents[:3]):
        struct.pack_into('>HH', rec, 86 + i * 4, start, count)
    return bytes(rec)


def _encode_thread_record(rec_type, parent_id, name):
    """Encode a 46-byte catalog thread record."""
    if isinstance(name, str):
        name_bytes = name.encode('mac_roman', errors='replace')
    else:
        name_bytes = name
    name_bytes = name_bytes[:31]
    rec = bytearray(46)
    rec[0] = rec_type
    struct.pack_into('>I', rec, 10, parent_id)  # thdParID
    rec[14] = len(name_bytes)                   # thdCName length
    rec[15:15 + len(name_bytes)] = name_bytes
    return bytes(rec)


def _catalog_sort_key(parent_id, name):
    """Sort key for catalog records (case-insensitive)."""
    if isinstance(name, str):
        return (parent_id, name.upper())
    return (parent_id, name.upper())


# ---------------------------------------------------------------------------
# HFS Image Builder
# ---------------------------------------------------------------------------

class HFSImageBuilder:
    """Build an HFS disk image from files."""

    def __init__(self, volume_name='Untitled'):
        self.volume_name = volume_name[:27]
        self.next_cnid = CNID_FIRST_USER
        self.directories = {}
        self.file_entries = []
        # Create root directory
        self.directories[CNID_ROOT_DIR] = {
            'parent': CNID_ROOT_PARENT,
            'name': self.volume_name,
            'valence': 0,
        }

    def _alloc_cnid(self):
        cnid = self.next_cnid
        self.next_cnid += 1
        return cnid

    def _ensure_directory(self, parts):
        """Ensure directory path exists, return CNID of deepest directory."""
        parent = CNID_ROOT_DIR
        for part in parts:
            found = None
            for cnid, info in self.directories.items():
                if info['parent'] == parent and info['name'].lower() == part.lower():
                    found = cnid
                    break
            if found is not None:
                parent = found
            else:
                cnid = self._alloc_cnid()
                self.directories[cnid] = {
                    'parent': parent,
                    'name': part,
                    'valence': 0,
                }
                self.directories[parent]['valence'] += 1
                parent = cnid
        return parent

    def add_file(self, path_parts, data_fork, rsrc_fork=b'',
                 finder_type=b'\x00\x00\x00\x00',
                 finder_creator=b'\x00\x00\x00\x00'):
        """Add a file given its path components relative to volume root."""
        if not path_parts:
            return
        name = path_parts[-1]
        parent = self._ensure_directory(path_parts[:-1])
        cnid = self._alloc_cnid()
        self.directories[parent]['valence'] += 1
        self.file_entries.append({
            'cnid': cnid,
            'parent': parent,
            'name': name,
            'data_fork': data_fork,
            'rsrc_fork': rsrc_fork,
            'finder_type': (finder_type + b'\x00\x00\x00\x00')[:4],
            'finder_creator': (finder_creator + b'\x00\x00\x00\x00')[:4],
        })

    def build(self):
        """Build and return the complete HFS disk image as bytes."""
        node_size = 512

        # Choose allocation block size
        total_data = sum(len(f['data_fork']) + len(f['rsrc_fork'])
                         for f in self.file_entries)
        alloc_block_size = 1024
        while total_data / alloc_block_size > 60000:
            alloc_block_size *= 2

        def blocks_for(size):
            if size == 0:
                return 0
            return (size + alloc_block_size - 1) // alloc_block_size

        # Phase 1: provisional file data allocation (starting at block 0)
        file_allocs = []
        next_block = 0
        for f in self.file_entries:
            db = blocks_for(len(f['data_fork']))
            rb = blocks_for(len(f['rsrc_fork']))
            file_allocs.append({
                'data_start': next_block, 'data_blocks': db,
                'rsrc_start': next_block + db, 'rsrc_blocks': rb,
            })
            next_block += db + rb
        total_file_blocks = next_block

        # Phase 2: build catalog to measure its size
        now = mac_time()
        cat_records = self._make_catalog_records(file_allocs, 0, now,
                                                 alloc_block_size)
        cat_data = build_btree(cat_records, node_size, max_key_len=37)
        cat_blocks = max(blocks_for(len(cat_data)), 1)

        # Empty extents overflow
        ext_data = build_btree([], node_size, max_key_len=7)
        ext_blocks = max(blocks_for(len(ext_data)), 1)

        # Phase 3: shift file blocks past system files
        offset = ext_blocks + cat_blocks
        for alloc in file_allocs:
            alloc['data_start'] += offset
            alloc['rsrc_start'] += offset

        # Phase 4: rebuild catalog with final block numbers
        cat_records = self._make_catalog_records(file_allocs, 0, now,
                                                 alloc_block_size)
        cat_data = build_btree(cat_records, node_size, max_key_len=37)
        assert blocks_for(len(cat_data)) <= cat_blocks

        # Pad B-tree files to their allocated size
        cat_data = cat_data.ljust(cat_blocks * alloc_block_size, b'\x00')
        ext_data = ext_data.ljust(ext_blocks * alloc_block_size, b'\x00')

        # Image layout
        total_alloc_blocks = offset + total_file_blocks
        free_blocks = max(2, total_alloc_blocks // 20)
        total_alloc_blocks += free_blocks

        bitmap_bytes = (total_alloc_blocks + 7) // 8
        bitmap_sectors = (bitmap_bytes + 511) // 512

        # Alloc blocks start after boot(2) + MDB(1) + bitmap
        alloc_start_sector = 3 + bitmap_sectors
        sectors_per_ab = alloc_block_size // 512
        if alloc_start_sector % sectors_per_ab:
            alloc_start_sector += sectors_per_ab - (alloc_start_sector % sectors_per_ab)

        # Total image: alloc area + 2 trailing sectors for alt MDB
        total_sectors = (alloc_start_sector
                         + total_alloc_blocks * sectors_per_ab + 2)
        image = bytearray(total_sectors * 512)

        # Volume bitmap
        used_blocks = total_alloc_blocks - free_blocks
        bitmap = bytearray(bitmap_bytes)
        for i in range(used_blocks):
            bitmap[i // 8] |= (0x80 >> (i % 8))
        bm_off = 3 * 512
        image[bm_off:bm_off + len(bitmap)] = bitmap

        # Write extents overflow B-tree (alloc blocks 0..)
        ab_base = alloc_start_sector * 512
        image[ab_base:ab_base + len(ext_data)] = ext_data

        # Write catalog B-tree (alloc blocks ext_blocks..)
        cat_off = ab_base + ext_blocks * alloc_block_size
        image[cat_off:cat_off + len(cat_data)] = cat_data

        # Write file data
        for i, f in enumerate(self.file_entries):
            a = file_allocs[i]
            if f['data_fork']:
                off = ab_base + a['data_start'] * alloc_block_size
                image[off:off + len(f['data_fork'])] = f['data_fork']
            if f['rsrc_fork']:
                off = ab_base + a['rsrc_start'] * alloc_block_size
                image[off:off + len(f['rsrc_fork'])] = f['rsrc_fork']

        # MDB
        mdb = self._build_mdb(
            num_alloc_blocks=total_alloc_blocks,
            alloc_block_size=alloc_block_size,
            alloc_start=alloc_start_sector,
            free_blocks=free_blocks,
            ext_file_size=len(ext_data),
            ext_blocks=ext_blocks,
            cat_file_size=len(cat_data),
            cat_blocks=cat_blocks,
            now=now,
        )
        image[1024:1024 + len(mdb)] = mdb
        # Alternate MDB at second-to-last sector
        alt_off = (total_sectors - 2) * 512
        image[alt_off:alt_off + len(mdb)] = mdb

        return bytes(image)

    def _make_catalog_records(self, file_allocs, block_offset, now,
                              alloc_block_size):
        """Build sorted catalog records as (key_bytes, data_bytes) pairs."""
        records = []

        # Directory records + threads
        for cnid, info in self.directories.items():
            parent = info['parent']
            name = info['name']
            # Directory record: key=(parent, name)
            key = _encode_catalog_key(parent, name)
            data = _encode_dir_record(cnid, info['valence'], now, now)
            sk = _catalog_sort_key(parent, name)
            records.append((sk, key, data))
            # Thread record: key=(cnid, "")
            tkey = _encode_catalog_key(cnid, '')
            tdata = _encode_thread_record(REC_DIR_THREAD, parent, name)
            records.append((_catalog_sort_key(cnid, ''), tkey, tdata))

        # File records + threads
        for i, f in enumerate(self.file_entries):
            a = file_allocs[i]
            data_ext = ([(a['data_start'], a['data_blocks'])]
                        if a['data_blocks'] else [])
            rsrc_ext = ([(a['rsrc_start'], a['rsrc_blocks'])]
                        if a['rsrc_blocks'] else [])
            data_phys = a['data_blocks'] * alloc_block_size
            rsrc_phys = a['rsrc_blocks'] * alloc_block_size

            key = _encode_catalog_key(f['parent'], f['name'])
            data = _encode_file_record(
                f['cnid'],
                len(f['data_fork']), data_phys, data_ext,
                len(f['rsrc_fork']), rsrc_phys, rsrc_ext,
                f['finder_type'], f['finder_creator'],
                now, now,
            )
            records.append((_catalog_sort_key(f['parent'], f['name']),
                            key, data))
            # Thread
            tkey = _encode_catalog_key(f['cnid'], '')
            tdata = _encode_thread_record(REC_FILE_THREAD, f['parent'],
                                          f['name'])
            records.append((_catalog_sort_key(f['cnid'], ''), tkey, tdata))

        records.sort(key=lambda r: r[0])
        return [(key, data) for _, key, data in records]

    def _build_mdb(self, num_alloc_blocks, alloc_block_size, alloc_start,
                   free_blocks, ext_file_size, ext_blocks,
                   cat_file_size, cat_blocks, now):
        """Build the 162-byte Master Directory Block."""
        mdb = bytearray(162)
        struct.pack_into('>H', mdb, 0, 0x4244)              # drSigWord
        struct.pack_into('>I', mdb, 2, now)                  # drCrDate
        struct.pack_into('>I', mdb, 6, now)                  # drLsMod
        # drAtrb at 10: 0
        root_files = sum(1 for f in self.file_entries
                         if f['parent'] == CNID_ROOT_DIR)
        struct.pack_into('>H', mdb, 12, root_files)         # drNmFls
        struct.pack_into('>H', mdb, 14, 3)                  # drVBMSt
        used = num_alloc_blocks - free_blocks
        struct.pack_into('>H', mdb, 16, used)               # drAllocPtr
        struct.pack_into('>H', mdb, 18, num_alloc_blocks)   # drNmAlBlks
        struct.pack_into('>I', mdb, 20, alloc_block_size)   # drAlBlkSiz
        struct.pack_into('>I', mdb, 24, alloc_block_size)   # drClpSiz
        struct.pack_into('>H', mdb, 28, alloc_start)        # drAlBlSt
        struct.pack_into('>I', mdb, 30, self.next_cnid)     # drNxtCNID
        struct.pack_into('>H', mdb, 34, free_blocks)        # drFreeBks
        # Volume name
        name_bytes = self.volume_name.encode('mac_roman', errors='replace')[:27]
        mdb[36] = len(name_bytes)
        mdb[37:37 + len(name_bytes)] = name_bytes
        # drVolBkUp(64), drVSeqNum(68): 0
        struct.pack_into('>I', mdb, 70, 1)                  # drWrCnt
        struct.pack_into('>I', mdb, 74, alloc_block_size)   # drXTClpSiz
        struct.pack_into('>I', mdb, 78, alloc_block_size)   # drCTClpSiz
        root_dirs = sum(1 for info in self.directories.values()
                        if info['parent'] == CNID_ROOT_DIR)
        struct.pack_into('>H', mdb, 82, root_dirs)          # drNmRtDirs
        struct.pack_into('>I', mdb, 84, len(self.file_entries))  # drFilCnt
        num_dirs = len(self.directories) - 1  # exclude root
        struct.pack_into('>I', mdb, 88, num_dirs)           # drDirCnt
        # drFndrInfo(92): 32 bytes zeros
        # drVCSize(124), drVBMCSize(126), drCtlCSize(128): 0
        struct.pack_into('>I', mdb, 130, ext_file_size)     # drXTFlSize
        # drXTExtRec at 134: (start=0, count=ext_blocks)
        struct.pack_into('>HH', mdb, 134, 0, ext_blocks)
        struct.pack_into('>I', mdb, 146, cat_file_size)     # drCTFlSize
        # drCTExtRec at 150: (start=ext_blocks, count=cat_blocks)
        struct.pack_into('>HH', mdb, 150, ext_blocks, cat_blocks)
        return bytes(mdb)


# ---------------------------------------------------------------------------
# Directory Scanner
# ---------------------------------------------------------------------------

def scan_directory(root_path):
    """Scan a directory tree, reading AppleDouble sidecars.

    Yields (path_parts, data_fork, rsrc_fork, finder_type, finder_creator).
    """
    root_path = os.path.abspath(root_path)
    for dirpath, dirnames, filenames in os.walk(root_path):
        dirnames.sort()
        filenames.sort()

        rel_dir = os.path.relpath(dirpath, root_path)
        dir_parts = [] if rel_dir == '.' else rel_dir.split(os.sep)

        for fname in filenames:
            if fname.startswith('._') or fname == '.DS_Store':
                continue
            filepath = os.path.join(dirpath, fname)
            if not os.path.isfile(filepath):
                continue

            with open(filepath, 'rb') as f:
                data_fork = f.read()

            rsrc_fork = b''
            finder_type = b'\x00\x00\x00\x00'
            finder_creator = b'\x00\x00\x00\x00'

            # Check for AppleDouble sidecar
            sidecar = os.path.join(dirpath, '._' + fname)
            if os.path.isfile(sidecar):
                with open(sidecar, 'rb') as f:
                    ad_data = f.read()
                rsrc, finfo = parse_apple_double(ad_data)
                if rsrc:
                    rsrc_fork = rsrc
                if finfo and len(finfo) >= 8:
                    finder_type = finfo[0:4]
                    finder_creator = finfo[4:8]

            # On macOS, try native resource fork and xattr as fallback
            if sys.platform == 'darwin':
                if not rsrc_fork:
                    try:
                        with open(filepath + '/..namedfork/rsrc', 'rb') as f:
                            rsrc_fork = f.read()
                    except OSError:
                        pass
                if finder_type == b'\x00\x00\x00\x00':
                    try:
                        import xattr
                        fi = xattr.getxattr(filepath, 'com.apple.FinderInfo')
                        if len(fi) >= 8:
                            finder_type = fi[0:4]
                            finder_creator = fi[4:8]
                    except (ImportError, OSError, KeyError):
                        pass

            yield dir_parts + [fname], data_fork, rsrc_fork, \
                finder_type, finder_creator


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description='Package a directory tree into an HFS disk image.')
    parser.add_argument('directory',
                        help='Directory to package')
    parser.add_argument('-o', '--output', default=None,
                        help='Output image path (default: <directory>.img)')
    parser.add_argument('-n', '--name', default=None,
                        help='HFS volume name (default: directory basename)')
    args = parser.parse_args()

    directory = os.path.abspath(args.directory)
    if not os.path.isdir(directory):
        print(f"Error: {args.directory} is not a directory", file=sys.stderr)
        sys.exit(1)

    vol_name = args.name or os.path.basename(directory)
    output = args.output or (directory.rstrip(os.sep) + '.img')

    builder = HFSImageBuilder(vol_name)

    print(f"Scanning {directory}...")
    count = 0
    for path_parts, data, rsrc, ftype, fcreator in scan_directory(directory):
        builder.add_file(path_parts, data, rsrc, ftype, fcreator)
        count += 1
        if count % 200 == 0:
            print(f"  {count} files...")

    print(f"  {count} files found")
    print(f"Building HFS image '{vol_name}'...")
    image = builder.build()

    with open(output, 'wb') as f:
        f.write(image)

    size_mb = len(image) / (1024 * 1024)
    print(f"Written {len(image)} bytes ({size_mb:.1f} MB) to {output}")


if __name__ == '__main__':
    main()
