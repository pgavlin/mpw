# Phase 2: PEF Loader

**Goal:** Parse and load PEF (Preferred Executable Format) containers into emulated memory, with full relocation and import resolution.

**Depends on:** Phase 1 (PPC CPU core — needed for memory allocation via MM::Native, though not for execution).

---

## Overview

PEF is the executable format for PowerPC Mac OS. A PEF container holds code, data, and loader information. The loader must:

1. Parse the container and section headers
2. Allocate memory for each section and load/decompress its contents
3. Parse the loader section to find imported libraries, imported symbols, exported symbols, and relocation instructions
4. Resolve imports via a callback (the caller provides addresses for imported symbols)
5. Apply relocations to patch absolute addresses into the loaded sections
6. Determine the entry point, init routine, and TOC base

---

## PEF Binary Format Reference

### Container Header (40 bytes)
```
+0x00: uint32 tag1          = 0x4A6F7921 ('Joy!')
+0x04: uint32 tag2          = 0x70656666 ('peff')
+0x08: uint32 architecture  = 0x70777063 ('pwpc') for PowerPC
+0x0C: uint32 formatVersion = 1
+0x10: uint32 dateTimeStamp
+0x14: uint32 oldDefVersion
+0x18: uint32 oldImpVersion
+0x1C: uint32 currentVersion
+0x20: uint16 sectionCount
+0x22: uint16 instSectionCount
+0x24: uint32 reservedA
```

### Section Header (28 bytes each, immediately after container header)
```
+0x00: int32  nameOffset       (-1 if unnamed)
+0x04: uint32 defaultAddress
+0x08: uint32 totalSize        (size in memory, includes zero-fill)
+0x0C: uint32 unpackedSize     (size before pattern expansion)
+0x10: uint32 packedSize       (size in file)
+0x14: uint32 containerOffset  (offset from file start to section data)
+0x18: uint8  sectionKind      (0=code, 1=data, 2=pidata, 3=const, 4=loader)
+0x19: uint8  shareKind
+0x1A: uint8  alignment        (log2)
+0x1B: uint8  reservedA
```

### Section Kinds
- **0 (code)**: Copy `packedSize` bytes from file, remaining `totalSize - packedSize` is zero
- **1 (unpacked data)**: Same as code — copy raw bytes
- **2 (pattern-initialized data)**: Decompress pidata from `packedSize` bytes into `unpackedSize` bytes, zero-fill remainder up to `totalSize`
- **3 (constant)**: Same as code
- **4 (loader)**: Not loaded into emulated memory; parsed for import/export/relocation info

### Loader Section Header (56 bytes at start of loader section data)
```
+0x00: int32  mainSection           (-1 if no main entry)
+0x04: uint32 mainOffset
+0x08: int32  initSection           (-1 if no init)
+0x0C: uint32 initOffset
+0x10: int32  termSection           (-1 if no term)
+0x14: uint32 termOffset
+0x18: uint32 importedLibraryCount
+0x1C: uint32 totalImportedSymbolCount
+0x20: uint32 relocSectionCount
+0x24: uint32 relocInstrOffset      (from loader section start)
+0x28: uint32 loaderStringsOffset   (from loader section start)
+0x2C: uint32 exportHashOffset      (from loader section start)
+0x30: uint32 exportHashTablePower  (log2)
+0x34: uint32 exportedSymbolCount
```

### Imported Library (24 bytes each, starting at loader+56)
```
+0x00: uint32 nameOffset           (into loader string table)
+0x04: uint32 oldImpVersion
+0x08: uint32 currentVersion
+0x0C: uint32 importedSymbolCount  (number of symbols imported from this library)
+0x10: uint32 firstImportedSymbol  (zero-based index of the first entry in the imported symbol table for this library)
+0x14: uint8  options              (contains bit flags for the import)
+0x15: uint8  reservedA
+0x16: uint16 reservedB
```

### Imported Library options

- **High-order bit (mask 0x80)**: controls the order that the import libraries are initialized. If set to 0, the default initialization order is used, which specifies that the Code Fragment Manager should _try_ to initialize the import library before the fragment that imports it. When set to 1, the import library **must** be initialized before the client fragment.
- **Next bit (maks 0x40)**: controls whether the import library is weak. When set to 1 (weak import), the Code Fragment Manager continues preparation of the client fragment (and does not generate an error) even if the import library cannot be found. If the import library is not found, all imported symbols from that library have their addresses set to 0. You can use this information to determine whether a weak import library is actually present.

### Imported Symbol (4 bytes each, after all ImportedLibrary entries)
```
+0x00: uint32 classAndName
         bits 31-28: symbol flags (0x80=weak, does not have to be present at prep time)
         bits 27-24: symbol class (0=code, 1=data, 2=tvector, 3=toc, 4=glue)
         bits 23-0:  name offset into loader string table
```

### Export Hash Table
Located at `loader + exportHashOffset`:
```
Hash table: (1 << exportHashTablePower) entries of uint32
  bits 31-18: chain count (number of exports in this bucket)
  bits 17-0:  chain index (index into export key + symbol tables)

Export key table: exportedSymbolCount entries of uint32
  bits 31-16: string length
  bits 15-0:  hash word (for verification)

Exported symbol table: exportedSymbolCount entries of 10 bytes each
  +0x00: uint32 classAndName
  +0x04: uint32 symbolValue  (offset within section)
  +0x08: int16  sectionIndex (-2 for abs symbols, -3 for re-exported symbols)
```

#### Name to Hash Word function

**NOTE**: this function uses Macintosh data types. Integers are big-endian.

```c
/* Computes a hash word for a given string. nameText points to the */
/* first character of the string (not the Pascal length byte). The */
/* string may be null terminated. */
enum {
    kPEFHashLengthShift = 16,
    kPEFHashValueMask = 0x0000FFFF
};

UInt32 PEFComputeHashWord(BytePtr nameText, UInt32 nameLength)
{
    BytePtr charPtr = nameText;
    SInt32 hashValue = 0;
    UInt32 length = 0;
    UInt32 limit;
    UInt32 result;
    UInt8 currChar;

    #define PseudoRotate(x) ( ( (x) << 1 ) - ( (x) >> 16 ))

    for (limit = nameLength; limit > 0; limit -= 1)
    {
        currChar = *charPtr++;
        if (currChar == NULL) break;
        length += 1;
        hashValue = PseudoRotate (hashValue) ^ currChar;
    }

    result = (length << kPEFHashLengthShift) |
    ((UInt16) ((hashValue ^ (hashValue >> 16)) & kPEFHashValueMask));
    return result;
} /* PEFComputeHashWord () */
```

#### Hash word to index function

**NOTE**: this function uses Macintosh data types. Integers are big-endian.

```c
#define PEFHashTableIndex(fullHashWord,hashTablePower) \
    ( ( (fullHashWord) ^ ((fullHashWord) >> (hashTablePower)) ) & \
    ((1 << (hashTablePower)) - 1) )
```

### Relocation Headers

After the imported symbols, `relocSectionCount` headers of 12 bytes:

```
+0x00: uint16 sectionIndex (section number to which this relocation header refers)
+0x02: uint16 reservedA
+0x04: uint32 relocCount (number of 16-bit relocation instructions)
+0x08: uint32 firstRelocOffset (from relocInstrOffset)
```

### Relocation Opcodes (16-bit instructions)

Relocations use a virtual machine with:

- `relocAddress`: current address being patched (initialized to section start)
- `sectionC`: memory address of an instantiated section within the PEF container (initial value is the memory address of section 0 if that section is present and instantiated. Otherwise the initial value is 0)
- `sectionD`: memory address of an instantiated section within the PEF container (initial value is the memory address of section 1 if that section is present and instantiated. Otherwise the initial value is 0)
- `importIndex`: current import index (initialized to 0)

**NOTE**: The sectionC and sectionD variables actually contain the memory address of an instantiated section minus the default address for that section. The default address for a section is contained in the defaultAddress field of the section header. However, in almost all cases the default address should be 0, so the simplified definition suffices.

All opcodes (upper bits determine opcode, lower bits are operand):

| Opcode | Bits | Meaning |
|--------|------|---------|
| RelocBySectDWithSkip | `00ssssss ssrrrrrr` | Increment relocAddress by s words, then add sectionD to the next `r+1` words |
| RelocBySectC | `0100000l llllllll` | Add sectionC to the next l+1 words |
| RelocBySectD | `0100001l llllllll` | Add sectionD to the next l+1 words |
| RelocTVector12 | `0100010l llllllll` | Patch l+1 12-byte TVectors (add sectionC to word 0, sectionD to word 1, and 0 to word 2) |
| RelocTVector8 | `0100011l llllllll` | Patch l+1 8-byte TVectors (add sectionC to word 0 and sectionD to word 1) |
| RelocVTable8 | `0100100l llllllll` | Patch l+1 8-byte VTables (add sectionD to word 0 and 0 to word 1) |
| RelocImportRun | `0100100l llllllll` | Patch l+1 words with import addresses. importIndex is incremented after every word |
| RelocSmByImport | `0110000i iiiiiiii` | Patch one word with import[i], then set importIndex to i+1 |
| RelocSmSetSectC | `0110001i iiiiiiii` | Set sectionC = section[i].address |
| RelocSmSetSectD | `0110010i iiiiiiii` | Set sectionD = section[i].address |
| RelocSmBySection | `0110011i iiiiiiii` | Patch one word with the seciton[i].address |
| RelocIncrPosition | `1000oooo oooooooo` | Advance relocAddress by o+1 words |
| RelocSmRepeat | `1001bbbb rrrrrrrr` | Repeat the preceeding b+1 relocation blocks r+1 times |
| RelocSetPosition | `101000oo oooooooo oooooooo oooooooo` | Set relocAddress to section offset o |
| RelocLgByImport | `101001oo oooooooo oooooooo oooooooo` | Patch one word with import[o], then set importIndex to o+1 |
| RelocLgRepeat | `101100bb bbrrrrrr rrrrrrrr rrrrrrrr` | Repeat the preceeding b+1 relocation blocks r times |
| RelocLgBySection | `10110100 00iiiiii iiiiiiii` | Patch one word with the seciton[i].address |
| RelocLgSetSectC | `10110100 01iiiiii iiiiiiii` | Set sectionC = section[i].address |
| RelocLgSetSectD | `10110100 10iiiiii iiiiiiii` | Set sectionD = section[i].address |

### Pattern-Initialized Data (pidata) Decompression

The pidata format uses a stream of opcodes:
```
Repeat until output is complete:
  Read opcode byte:
    bits 7-5: opcode type
    bits 4-0: count

  Opcode 0 (000xxxxx): Zero fill — write `count` zero bytes
  Opcode 1 (001xxxxx): Block copy — copy `count` bytes from packed stream
  Opcode 2 (010xxxxx): Repeat block — read packed `repeatCount`, copy `count` bytes from packed stream `repeatCount+1` times
  Opcode 3 (011xxxxx): Interleave repeat block with block copy — read packed `customSize` and `repeatCount`, then read `count` bytes of commonData, then `repeatCount` blocks of `customSize` bytes. Then loop: for i < repeatCount, write commonData, then customData[i], then increment i. Finally write one last commonData.
  Opcode 4 (100xxxxx): Interleave repeat block with zero - read packed `customSize` and `repeatCount`, then `repeatCount` blocks of `customSize` bytes. Then loop: for i < repeatCount, write count bytes of 0, then customData[i], then increment i. Finally write one last block of count 0 bytes.

  Counts > 31 use "packed count" encoding: if high bit set, read additional 7-bit bytes until a byte < 0x80 is found.
```

---

## Files to Create

### `toolbox/pef.h`

PEF structure definitions (packed structs matching the binary format). Use `#pragma pack(push, 1)` for correct layout. Include:
- `PEF::ContainerHeader`
- `PEF::SectionHeader`
- `PEF::LoaderHeader`
- `PEF::ImportedLibrary`
- `PEF::ImportedSymbol`
- `PEF::ExportedSymbol`
- `PEF::RelocHeader`
- Constants: magic tags, section kinds, symbol classes, relocation opcodes

### `toolbox/pef_loader.h`

```cpp
#ifndef __mpw_pef_loader_h__
#define __mpw_pef_loader_h__

#include <cstdint>
#include <string>
#include <vector>
#include <functional>

namespace PEFLoader {

struct SectionInfo {
    uint32_t address;   // allocated emulated memory address
    uint32_t size;      // total size in memory
    uint8_t  kind;
    std::string name;
};

struct ExportedSymbolInfo {
    std::string name;
    uint32_t sectionIndex;
    uint32_t offset;        // offset within section
    uint8_t  symbolClass;
};

struct LoadResult {
    uint32_t entryPoint;    // main entry TVector address (0 if none)
    uint32_t initPoint;     // init entry TVector address (0 if none)
    uint32_t termPoint;     // term entry TVector address (0 if none)
    uint32_t tocBase;       // data section base (for r2)
    std::vector<SectionInfo> sections;
    std::vector<ExportedSymbolInfo> exports;
};

// Callback for resolving imported symbols.
// Returns the address to patch (typically a TVector address for code/tvector
// symbols, or a data address for data symbols). Returns 0 if unresolved.
using ImportResolver = std::function<uint32_t(
    const std::string &library,
    const std::string &symbol,
    uint8_t symbolClass)>;

bool IsPEF(const uint8_t *data, size_t size);
bool LoadPEF(const uint8_t *data, size_t size,
             ImportResolver resolver, LoadResult &result);
bool LoadPEFFile(const std::string &path,
                 ImportResolver resolver, LoadResult &result);
uint32_t FindExport(const LoadResult &result, const std::string &name);

}

#endif
```

### `toolbox/pef_loader.cpp`

Implementation (~600-700 lines). Key functions:

**`IsPEF(data, size)`** — Check for `'Joy!' 'peff'` magic in first 8 bytes.

**`decodePatternData(packed, packedSize, output, outputSize)`** — Pidata decompression. Implements the 5 opcode types:
- Zero fill
- Block copy
- Repeat block (with custom + common data)
- Interleave variants

Uses a packed-count reader for counts > 31 (variable-length 7-bit encoding).

**`relocateSection(relocInstrs, relocCount, sectionBases, importAddresses, relocAddress)`** — The relocation virtual machine. Processes 16-bit relocation instructions, maintaining state (`relocAddress`, `sectionC`, `sectionD`, `importIndex`). For each instruction:
- Decode opcode from upper bits
- Apply the relocation (add section base, write import address, etc.)
- Advance state

**`LoadPEF(data, size, resolver, result)`** — Main loader:
1. Validate container header
2. Parse section headers
3. Allocate emulated memory for each section (`MM::Native::NewPtr`)
4. Load/decompress section contents
5. Find the loader section (kind 4)
6. Parse loader header → imported libraries, imported symbols, exports, relocations
7. Resolve all imports via the callback
8. Apply relocations for each section
9. Compute entry points and TOC base
10. Build export table

**`LoadPEFFile(path, resolver, result)`** — Read file from disk, delegate to `LoadPEF`.

**`FindExport(result, name)`** — Search the export table for a named symbol. Return absolute address (section base + offset). For TVector symbols, the address is the TVector in emulated memory.

### Diagnostic Logging

The PEF loader should produce verbose diagnostic output to stderr when tracing is enabled. This is essential for debugging load failures in later phases. Loader tracing is activated by the existing `--trace-toolbox` flag (which covers all toolbox-level activity, including loading). Add a `SetTrace(bool)` function or accept a trace flag:

**Section loading log:**
```
PEF: Loading "StdCLib" (5 sections)
PEF:   Section 0: code    @ 0x00120000  size=0x10F20
PEF:   Section 1: data    @ 0x00131000  size=0x3A40
PEF:   Section 2: pidata  @ 0x00135000  size=0x1800 (packed=0x0A20)
PEF:   Section 3: loader  (not loaded, parsed)
```

**Import resolution log:**
```
PEF: Resolving 62 imports from 3 libraries:
PEF:   InterfaceLib (48 symbols):
PEF:     [0] NewPtr (tvector) -> 0x00100008
PEF:     [1] DisposePtr (tvector) -> 0x00100018
PEF:     ...
PEF:     [15] SomeFunction (tvector) -> UNRESOLVED
PEF:   MathLib (4 symbols):
PEF:     ...
```

**Relocation log (verbose, gated behind a separate flag):**
```
PEF: Relocating section 1 (data), 42 instructions:
PEF:   RelocBySectDWithLength count=4 @ 0x00131000
PEF:   RelocImportRun count=8 @ 0x00131010
PEF:   ...
```

**Export log:**
```
PEF: 156 exports registered:
PEF:   fprintf (tvector, section 0 + 0x68F8) -> 0x001268F8
PEF:   exit (tvector, section 0 + 0xD18C) -> 0x0012D18C
PEF:   __target_for_exit (data, section 1 + 0x0480) -> 0x00131480
PEF:   ...
```

This logging is how we'll discover missing stubs, incorrect relocations, and TOC base issues without needing a PPC instruction trace.

**Analysis tool:** For deeper PEF inspection, use `mpw DumpPEF -do All -pi u -a -fmt on <path>` to get full disassembly and section dumps of the PEF binary. Cross-reference with loader output to verify addresses match.

**TOC base calculation (critical fix from prior work):**
```cpp
// Determine TOC base.
// If there's a main entry, TOC comes from the main section's data area.
// If there's only an init entry (e.g., shared libraries like StdCLib),
// read TOC from the init TVector's second word.
if (loaderHdr.mainSection >= 0) {
    result.tocBase = sectionBases[/* data section, typically section 1 */];
} else if (loaderHdr.initSection >= 0) {
    // The init entry is a TVector: {code_addr, toc_value}
    uint32_t initTVec = sectionBases[loaderHdr.initSection] + loaderHdr.initOffset;
    result.tocBase = memoryReadLong(initTVec + 4);  // second word = TOC
}
```

---

## Files to Modify

### `toolbox/CMakeLists.txt`

Add `pef_loader.cpp` to the TOOLBOX_LIB source list.

---

## Implementation Steps

1. Write `toolbox/pef.h` — all structure definitions with packed layout
2. Write `toolbox/pef_loader.h` — public interface
3. Write `toolbox/pef_loader.cpp`:
   a. Byte-order helpers (`readBE16`, `readBE32`)
   b. `IsPEF()`
   c. `decodePatternData()` — pidata decompression
   d. `relocateSection()` — relocation VM
   e. `LoadPEF()` — main loading logic
   f. `LoadPEFFile()` — file wrapper
   g. `FindExport()` — export lookup
4. Modify `toolbox/CMakeLists.txt` — add source file

---

## Validation

### Build test
```bash
cd build && cmake .. && make
```

### Unit test: Load Hello PEF

Write a temporary test that:

1. Calls `PEFLoader::LoadPEFFile("path/to/Hello", resolver, result)` with a resolver that logs all imports and returns 0 (unresolved):
   ```cpp
   auto resolver = [](const std::string &lib, const std::string &sym, uint8_t cls) -> uint32_t {
       fprintf(stderr, "  import: %s::%s (class %d)\n", lib.c_str(), sym.c_str(), cls);
       return 0;
   };
   ```
2. Prints all sections: name, address, size, kind
3. Prints all exports: name, section, offset
4. Prints entry point and TOC base
5. Verifies `IsPEF()` returns true for the Hello PEF and false for a 68K executable

**Expected output for Hello PEF:**
- 2-3 sections (code, data, possibly loader)
- Imports from StdCLib (`__start`, `__target_for_exit`, `_exit_status`, `main` — wait, `main` is in the tool itself)
- Actually, Hello imports from StdCLib and MPWCRuntime (e.g., `fprintf`, `__start`, `exit`)
- Entry point should be non-zero (pointing to `__start`)
- TOC base should point to the data section

### Unit test: Load StdCLib PEF

Same test with `~/mpw/SharedLibraries/StdCLib` (or wherever it is). Should show:
- Multiple sections
- Imports from InterfaceLib, MathLib, PrivateInterfaceLib
- Many exports (fprintf, exit, malloc, etc.)
- Init entry point (no main entry — it's a shared library)
- TOC base from init TVector's second word

### Pidata decompression test

Load a PEF known to have pattern-initialized data sections (StdCLib likely has one). Verify the section decompresses to `unpackedSize` bytes without overflow or underflow.

### Relocation test

With all imports returning dummy addresses (e.g., `0xDEAD0000 + index`), verify that relocation completes without errors and the relocated addresses in memory contain the expected values.

---

## Risk Notes

- **Relocation engine complexity**: The PEF relocation VM has ~15 opcodes with varying operand formats. This is the most error-prone part. Implement and test each opcode incrementally.
- **Pidata edge cases**: The repeat/interleave opcodes are rarely used but critical for StdCLib. Make sure to handle the packed-count encoding correctly (7-bit variable-length integers).
- **Export hash table**: The hash algorithm must match Apple's PEF spec exactly. The hash function is: `hash = (hash << 1) + (hash >> 23) + char` applied to each character of the symbol name, masked to 32 bits.
- **Section alignment**: Sections may require alignment (log2 specified in header). Allocated memory should respect this.
