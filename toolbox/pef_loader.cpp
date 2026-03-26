/*
 * pef_loader.cpp
 *
 * PEF container parser, section loader, and relocation engine.
 * Reference: Apple "Mac OS Runtime Architectures" document.
 */

#include "pef_loader.h"
#include "pef.h"
#include "mm.h"

#include <cpu/m68k/defs.h>
#include <cpu/m68k/fmem.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <algorithm>

namespace PEFLoader {

static bool trace = false;

void SetTrace(bool t) { trace = t; }

// -- Big-endian readers --

static uint32_t readBE32(const uint8_t *p) {
	return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
	       (uint32_t(p[2]) << 8)  | uint32_t(p[3]);
}

static uint16_t readBE16(const uint8_t *p) {
	return (uint16_t(p[0]) << 8) | uint16_t(p[1]);
}

static int32_t readBE32s(const uint8_t *p) {
	return (int32_t)readBE32(p);
}

static int16_t readBE16s(const uint8_t *p) {
	return (int16_t)readBE16(p);
}

bool IsPEF(const uint8_t *data, size_t size) {
	if (size < 8) return false;
	return readBE32(data) == PEF::kPEFTag1 &&
	       readBE32(data + 4) == PEF::kPEFTag2;
}

// -- Pidata decompression --

// Read a variable-length packed count (7-bit encoding, high bit = continue).
static uint32_t readPackedCount(const uint8_t *packed, uint32_t packedSize,
                                uint32_t &pos) {
	uint32_t value = 0;
	uint8_t b;
	do {
		if (pos >= packedSize) return 0;
		b = packed[pos++];
		value = (value << 7) | (b & 0x7F);
	} while (b & 0x80);
	return value;
}

/*
 * Decode pattern-initialized data section.
 *
 * Opcodes (high 3 bits of first byte):
 *   0 = zero fill 'count' bytes
 *   1 = block copy 'count' bytes from packed stream
 *   2 = repeat block: repeatCount, then count bytes copied repeatCount+1 times
 *   3 = interleave repeat with block copy:
 *       commonSize=count, then customSize, repeatCount from stream,
 *       then commonData[commonSize], then customData[customSize]*repeatCount
 *       output: common, custom[0], common, custom[1], ..., common
 *   4 = interleave repeat with zero:
 *       zeroSize=count, then customSize, repeatCount from stream,
 *       then customData[customSize]*repeatCount
 *       output: zeros, custom[0], zeros, custom[1], ..., zeros
 *
 * Low 5 bits of first byte = count. If 0, read packed count from stream.
 */
static bool decodePatternData(const uint8_t *packed, uint32_t packedSize,
                              uint32_t destAddr, uint32_t totalSize) {
	uint32_t pos = 0;
	uint32_t outOff = 0;

	// Pre-zero the entire section
	uint8_t *dest = memoryPointer(destAddr);
	if (dest) memset(dest, 0, totalSize);

	while (pos < packedSize && outOff < totalSize) {
		uint8_t byte = packed[pos++];
		uint8_t op = (byte >> 5) & 0x07;
		uint32_t count = byte & 0x1F;
		if (count == 0)
			count = readPackedCount(packed, packedSize, pos);

		switch (op) {
		case 0: // zero fill (already zeroed)
			outOff += count;
			break;

		case 1: // block copy
			if (pos + count > packedSize) return false;
			for (uint32_t i = 0; i < count && outOff < totalSize; i++, outOff++)
				memoryWriteByte(packed[pos + i], destAddr + outOff);
			pos += count;
			break;

		case 2: { // repeat block
			uint32_t repeatCount = readPackedCount(packed, packedSize, pos);
			if (pos + count > packedSize) return false;
			for (uint32_t r = 0; r <= repeatCount; r++) {
				for (uint32_t i = 0; i < count && outOff < totalSize; i++, outOff++)
					memoryWriteByte(packed[pos + i], destAddr + outOff);
			}
			pos += count;
			break;
		}

		case 3: { // interleave repeat with block copy
			uint32_t commonSize = count;
			uint32_t customSize = readPackedCount(packed, packedSize, pos);
			uint32_t repeatCount = readPackedCount(packed, packedSize, pos);
			if (pos + commonSize > packedSize) return false;
			uint32_t commonStart = pos;
			pos += commonSize;
			// Output: common, (custom, common) * repeatCount
			for (uint32_t i = 0; i < commonSize && outOff < totalSize; i++, outOff++)
				memoryWriteByte(packed[commonStart + i], destAddr + outOff);
			for (uint32_t r = 0; r < repeatCount; r++) {
				if (pos + customSize > packedSize) return false;
				for (uint32_t i = 0; i < customSize && outOff < totalSize; i++, outOff++)
					memoryWriteByte(packed[pos + i], destAddr + outOff);
				pos += customSize;
				for (uint32_t i = 0; i < commonSize && outOff < totalSize; i++, outOff++)
					memoryWriteByte(packed[commonStart + i], destAddr + outOff);
			}
			break;
		}

		case 4: { // interleave repeat with zero
			uint32_t zeroSize = count;
			uint32_t customSize = readPackedCount(packed, packedSize, pos);
			uint32_t repeatCount = readPackedCount(packed, packedSize, pos);
			// Output: zeros, (custom, zeros) * repeatCount
			outOff += zeroSize; // already zeroed
			for (uint32_t r = 0; r < repeatCount; r++) {
				if (pos + customSize > packedSize) return false;
				for (uint32_t i = 0; i < customSize && outOff < totalSize; i++, outOff++)
					memoryWriteByte(packed[pos + i], destAddr + outOff);
				pos += customSize;
				outOff += zeroSize; // already zeroed
			}
			break;
		}

		default:
			fprintf(stderr, "PEF: unknown pidata opcode %d\n", op);
			return false;
		}
	}

	return true;
}

// -- Relocation engine --

static const char *sectionKindName(uint8_t kind) {
	switch (kind) {
	case PEF::kCode: return "code";
	case PEF::kUnpackedData: return "data";
	case PEF::kPatternInitData: return "pidata";
	case PEF::kConstant: return "const";
	case PEF::kLoader: return "loader";
	case PEF::kDebug: return "debug";
	case PEF::kExecutableData: return "xdata";
	case PEF::kException: return "except";
	case PEF::kTraceback: return "trace";
	default: return "?";
	}
}

static const char *symClassName(uint8_t cls) {
	switch (cls & 0x0F) {
	case PEF::kCodeSymbol: return "code";
	case PEF::kDataSymbol: return "data";
	case PEF::kTVectorSymbol: return "tvector";
	case PEF::kTOCSymbol: return "toc";
	case PEF::kGlueSymbol: return "glue";
	default: return "?";
	}
}

/*
 * Execute the relocation virtual machine for one section.
 *
 * See PPC_PHASE2.md for the full opcode table.
 */
static bool relocateSection(
	const uint8_t *relocData, uint32_t relocCount,
	const std::vector<SectionInfo> &sections,
	const std::vector<uint32_t> &importAddresses,
	uint32_t sectionAddr,
	int sectionIdx)
{
	uint32_t relocAddr = sectionAddr;
	uint32_t importIndex = 0;

	// Default sectionC = section 0 address, sectionD = section 1 address
	uint32_t sectionC = (sections.size() > 0 && sections[0].address) ? sections[0].address : 0;
	uint32_t sectionD = (sections.size() > 1 && sections[1].address) ? sections[1].address : 0;

	auto relocWord = [&](uint32_t base) {
		uint32_t val = memoryReadLong(relocAddr);
		memoryWriteLong(val + base, relocAddr);
		relocAddr += 4;
	};

	// relocCount = number of 16-bit words in the reloc stream
	uint32_t endPos = relocCount * 2;
	uint32_t pos = 0;

	while (pos < endPos) {
		uint16_t instr = readBE16(relocData + pos);
		pos += 2;

		uint16_t top2 = (instr >> 14) & 0x3;

		if (top2 == 0) {
			// 00ssssss ssrrrrrr — RelocBySectDWithSkip
			uint32_t skipCount = (instr >> 6) & 0xFF;
			uint32_t relocCnt = instr & 0x3F;
			relocAddr += skipCount * 4;
			for (uint32_t j = 0; j < relocCnt; j++)
				relocWord(sectionD);

		} else if (top2 == 1) {
			uint16_t top3 = (instr >> 13) & 0x7;

			if (top3 == 2) {
				// 010xxxxx xxxxxxxx — Run group
				uint32_t subop = (instr >> 9) & 0xF;
				uint32_t runLen = (instr & 0x1FF) + 1;

				switch (subop) {
				case 0: // RelocBySectC
					for (uint32_t j = 0; j < runLen; j++) relocWord(sectionC);
					break;
				case 1: // RelocBySectD
					for (uint32_t j = 0; j < runLen; j++) relocWord(sectionD);
					break;
				case 2: // RelocTVector12
					for (uint32_t j = 0; j < runLen; j++) {
						relocWord(sectionC);
						relocWord(sectionD);
						relocWord(0);
					}
					break;
				case 3: // RelocTVector8
					for (uint32_t j = 0; j < runLen; j++) {
						relocWord(sectionC);
						relocWord(sectionD);
					}
					break;
				case 4: // RelocVTable8
					for (uint32_t j = 0; j < runLen; j++) {
						relocWord(sectionD);
						relocWord(0);
					}
					break;
				case 5: // RelocImportRun
					for (uint32_t j = 0; j < runLen; j++) {
						if (importIndex < importAddresses.size())
							memoryWriteLong(importAddresses[importIndex], relocAddr);
						importIndex++;
						relocAddr += 4;
					}
					break;
				default:
					fprintf(stderr, "PEF: unknown Run subop %u in section %d\n", subop, sectionIdx);
					break;
				}

			} else if (top3 == 3) {
				// 011xxxxx xxxxxxxx — SmIndex group
				uint32_t subop = (instr >> 9) & 0xF;
				uint32_t index = instr & 0x1FF;

				switch (subop) {
				case 0: // RelocSmByImport
					if (index < importAddresses.size())
						memoryWriteLong(importAddresses[index], relocAddr);
					importIndex = index + 1;
					relocAddr += 4;
					break;
				case 1: // RelocSmSetSectC
					if (index < sections.size()) sectionC = sections[index].address;
					break;
				case 2: // RelocSmSetSectD
					if (index < sections.size()) sectionD = sections[index].address;
					break;
				case 3: // RelocSmBySection
					if (index < sections.size()) {
						uint32_t val = memoryReadLong(relocAddr);
						memoryWriteLong(val + sections[index].address, relocAddr);
					}
					relocAddr += 4;
					break;
				default:
					fprintf(stderr, "PEF: unknown SmIndex subop %u\n", subop);
					break;
				}

			} else {
				fprintf(stderr, "PEF: unexpected reloc instruction 0x%04X\n", instr);
			}

		} else if (top2 == 2) {
			uint16_t top4 = (instr >> 12) & 0xF;

			if (top4 == 8) {
				// 1000xxxx xxxxxxxx — RelocIncrPosition
				uint32_t offset = (instr & 0xFFF) + 1;
				relocAddr += offset;

			} else if (top4 == 9) {
				// 1001bbbb rrrrrrrr — RelocSmRepeat
				uint32_t chunks = ((instr >> 8) & 0xF) + 1;
				uint32_t repeatCount = (instr & 0xFF) + 1;
				// Re-execute the preceding 'chunks' instructions, 'repeatCount' more times.
				uint32_t repeatStart = pos - 2 - chunks * 2;
				uint32_t savedPos = pos;
				for (uint32_t r = 0; r < repeatCount; r++) {
					pos = repeatStart;
					for (uint32_t c = 0; c < chunks; c++) {
						uint16_t ri = readBE16(relocData + pos);
						pos += 2;

						uint16_t rt2 = (ri >> 14) & 0x3;
						if (rt2 == 0) {
							uint32_t sk = (ri >> 6) & 0xFF;
							uint32_t rc = ri & 0x3F;
							relocAddr += sk * 4;
							for (uint32_t j = 0; j < rc; j++) relocWord(sectionD);
						} else if (rt2 == 1) {
							uint16_t rt3 = (ri >> 13) & 0x7;
							if (rt3 == 2) {
								uint32_t so = (ri >> 9) & 0xF;
								uint32_t rl = (ri & 0x1FF) + 1;
								switch (so) {
								case 0: for (uint32_t j = 0; j < rl; j++) relocWord(sectionC); break;
								case 1: for (uint32_t j = 0; j < rl; j++) relocWord(sectionD); break;
								case 2: for (uint32_t j = 0; j < rl; j++) { relocWord(sectionC); relocWord(sectionD); relocWord(sectionD); } break;
								case 3: for (uint32_t j = 0; j < rl; j++) { relocWord(sectionC); relocWord(sectionD); } break;
								case 4: for (uint32_t j = 0; j < rl; j++) { relocWord(sectionD); relocWord(sectionD); } break;
								case 5: for (uint32_t j = 0; j < rl; j++) {
									if (importIndex < importAddresses.size()) memoryWriteLong(importAddresses[importIndex], relocAddr);
									importIndex++; relocAddr += 4;
								} break;
								default: break;
								}
							} else if (rt3 == 3) {
								uint32_t so = (ri >> 9) & 0xF;
								uint32_t ix = ri & 0x1FF;
								switch (so) {
								case 0:
									if (ix < importAddresses.size()) memoryWriteLong(importAddresses[ix], relocAddr);
									importIndex = ix + 1; relocAddr += 4; break;
								case 1: if (ix < sections.size()) sectionC = sections[ix].address; break;
								case 2: if (ix < sections.size()) sectionD = sections[ix].address; break;
								case 3:
									if (ix < sections.size()) { uint32_t v = memoryReadLong(relocAddr); memoryWriteLong(v + sections[ix].address, relocAddr); }
									relocAddr += 4; break;
								default: break;
								}
							}
						} else if (rt2 == 2) {
							uint16_t rt4 = (ri >> 12) & 0xF;
							if (rt4 == 8) relocAddr += (ri & 0xFFF) + 1;
						}
					}
				}
				pos = savedPos;

			} else {
				// 2-word instructions: top6 = bits[15:10]
				uint16_t top6 = (instr >> 10) & 0x3F;
				if (pos + 2 > endPos) break;
				uint16_t word2 = readBE16(relocData + pos);
				pos += 2;

				if (top6 == 0x28) {
					// RelocSetPosition
					uint32_t offset = ((uint32_t)(instr & 0x3FF) << 16) | word2;
					relocAddr = sectionAddr + offset;
				} else if (top6 == 0x29) {
					// RelocLgByImport
					uint32_t idx = ((uint32_t)(instr & 0x3FF) << 16) | word2;
					if (idx < importAddresses.size())
						memoryWriteLong(importAddresses[idx], relocAddr);
					importIndex = idx + 1;
					relocAddr += 4;
				} else if (top6 == 0x2C) {
					// RelocLgRepeat
					uint32_t chunks = ((instr >> 6) & 0xF) + 1;
					uint32_t repeatCount = ((uint32_t)(instr & 0x3F) << 16) | word2;
					// Same logic as SmRepeat but with larger counts
					uint32_t repeatStart = pos - 4 - chunks * 2;
					uint32_t savedPos = pos;
					for (uint32_t r = 0; r < repeatCount; r++) {
						pos = repeatStart;
						// Simplified: just re-run the bytes
						for (uint32_t c = 0; c < chunks; c++) {
							uint16_t ri = readBE16(relocData + pos);
							pos += 2;
							// Minimal re-execution (same as SmRepeat inner loop)
							uint16_t rt2 = (ri >> 14) & 0x3;
							if (rt2 == 0) {
								relocAddr += ((ri >> 6) & 0xFF) * 4;
								for (uint32_t j = 0; j < (ri & 0x3Fu); j++) relocWord(sectionD);
							}
							// ... other cases as needed
						}
					}
					pos = savedPos;
				} else if (top6 == 0x2D) {
					// RelocLgSetOrBySection
					uint32_t subOp = (word2 >> 14) & 0x3;
					uint32_t idx = ((uint32_t)(instr & 0x3F) << 16) | (word2 & 0x3FFF);
					switch (subOp) {
					case 0: // LgBySection
						if (idx < sections.size()) {
							uint32_t val = memoryReadLong(relocAddr);
							memoryWriteLong(val + sections[idx].address, relocAddr);
						}
						relocAddr += 4;
						break;
					case 1: // LgSetSectC
						if (idx < sections.size()) sectionC = sections[idx].address;
						break;
					case 2: // LgSetSectD
						if (idx < sections.size()) sectionD = sections[idx].address;
						break;
					default:
						fprintf(stderr, "PEF: unknown LgSetOrBySection subop %u\n", subOp);
						break;
					}
				} else {
					fprintf(stderr, "PEF: unknown 2-word reloc 0x%02X\n", top6);
				}
			}

		} else {
			// top2 == 3: reserved
			fprintf(stderr, "PEF: reserved reloc opcode 0x%04X\n", instr);
		}
	}

	return true;
}

// -- Main loader --

bool LoadPEF(const uint8_t *data, size_t size,
             ImportResolver resolver, LoadResult &result)
{
	if (!IsPEF(data, size)) {
		fprintf(stderr, "PEF: not a valid PEF file\n");
		return false;
	}
	if (size < 40) return false;

	uint32_t architecture = readBE32(data + 8);
	if (architecture != PEF::kPEFArchPPC) {
		fprintf(stderr, "PEF: unsupported architecture 0x%08X\n", architecture);
		return false;
	}

	uint16_t sectionCount = readBE16(data + 32);
	if (size < 40 + (size_t)sectionCount * 28) {
		fprintf(stderr, "PEF: file too small for %d section headers\n", sectionCount);
		return false;
	}

	if (trace) fprintf(stderr, "PEF: %d sections\n", sectionCount);

	// -- Parse section headers and load sections --

	result.sections.resize(sectionCount);
	int loaderSectionIdx = -1;

	for (uint16_t i = 0; i < sectionCount; i++) {
		const uint8_t *sh = data + 40 + i * 28;
		uint32_t totalSize     = readBE32(sh + 8);
		uint32_t unpackedSize  = readBE32(sh + 12);
		uint32_t packedSize    = readBE32(sh + 16);
		uint32_t containerOff  = readBE32(sh + 20);
		uint8_t  sectionKind   = sh[24];

		result.sections[i].size = totalSize;
		result.sections[i].kind = sectionKind;

		if (sectionKind == PEF::kLoader) {
			loaderSectionIdx = i;
			result.sections[i].address = 0;
			if (trace) fprintf(stderr, "PEF:   [%d] %-8s (parsed, not loaded)\n",
			                   i, sectionKindName(sectionKind));
			continue;
		}

		if (sectionKind == PEF::kDebug || sectionKind == PEF::kException ||
		    sectionKind == PEF::kTraceback) {
			result.sections[i].address = 0;
			if (trace) fprintf(stderr, "PEF:   [%d] %-8s (skipped)\n",
			                   i, sectionKindName(sectionKind));
			continue;
		}

		uint32_t addr = 0;
		if (totalSize > 0) {
			uint16_t err = MM::Native::NewPtr(totalSize, true, addr);
			if (err) {
				fprintf(stderr, "PEF: failed to allocate %u bytes for section %d\n",
				        totalSize, i);
				return false;
			}
		}
		result.sections[i].address = addr;

		if (trace) fprintf(stderr, "PEF:   [%d] %-8s @ 0x%08X  size=0x%X",
		                   i, sectionKindName(sectionKind), addr, totalSize);

		if (sectionKind == PEF::kPatternInitData) {
			if (trace) fprintf(stderr, " (packed=0x%X, unpacked=0x%X)", packedSize, unpackedSize);
			if (packedSize > 0 && containerOff + packedSize <= size) {
				decodePatternData(data + containerOff, packedSize, addr, totalSize);
			}
		} else {
			uint32_t copySize = std::min(packedSize, totalSize);
			if (copySize > 0 && containerOff + copySize <= size) {
				uint8_t *dest = memoryPointer(addr);
				if (dest) memcpy(dest, data + containerOff, copySize);
			}
		}

		if (trace) fprintf(stderr, "\n");
	}

	// -- Parse loader section --

	if (loaderSectionIdx < 0) {
		fprintf(stderr, "PEF: no loader section\n");
		return false;
	}

	const uint8_t *lsh = data + 40 + loaderSectionIdx * 28;
	uint32_t loaderOffset = readBE32(lsh + 20);
	uint32_t loaderSize   = readBE32(lsh + 16);
	if (loaderOffset + loaderSize > size || loaderSize < 56) {
		fprintf(stderr, "PEF: invalid loader section\n");
		return false;
	}

	const uint8_t *loader = data + loaderOffset;

	int32_t  mainSection             = readBE32s(loader + 0);
	uint32_t mainOffset              = readBE32(loader + 4);
	int32_t  initSection             = readBE32s(loader + 8);
	uint32_t initOffset              = readBE32(loader + 12);
	int32_t  termSection             = readBE32s(loader + 16);
	uint32_t termOffset              = readBE32(loader + 20);
	uint32_t importedLibraryCount    = readBE32(loader + 24);
	uint32_t totalImportedSymCount   = readBE32(loader + 28);
	uint32_t relocSectionCount       = readBE32(loader + 32);
	uint32_t relocInstrOffset        = readBE32(loader + 36);
	uint32_t loaderStringsOffset     = readBE32(loader + 40);
	uint32_t exportHashOffset        = readBE32(loader + 44);
	uint32_t exportHashTablePower    = readBE32(loader + 48);
	uint32_t exportedSymbolCount     = readBE32(loader + 52);

	const uint8_t *stringTable = loader + loaderStringsOffset;
	uint32_t stringTableSize = loaderSize - loaderStringsOffset;

	auto readString = [&](uint32_t offset, uint32_t len) -> std::string {
		if (offset + len > stringTableSize) return "";
		return std::string((const char *)(stringTable + offset), len);
	};

	auto readCString = [&](uint32_t offset) -> std::string {
		if (offset >= stringTableSize) return "";
		const char *s = (const char *)(stringTable + offset);
		uint32_t maxLen = stringTableSize - offset;
		uint32_t len = 0;
		while (len < maxLen && s[len]) len++;
		return std::string(s, len);
	};

	// -- Resolve imports --

	const uint8_t *libTable = loader + 56;
	const uint8_t *symTable = libTable + importedLibraryCount * 24;

	std::vector<uint32_t> importAddresses(totalImportedSymCount, 0);
	uint32_t resolvedCount = 0;
	uint32_t unresolvedCount = 0;

	for (uint32_t lib = 0; lib < importedLibraryCount; lib++) {
		const uint8_t *le = libTable + lib * 24;
		uint32_t libNameOffset = readBE32(le + 0);
		uint32_t symCount      = readBE32(le + 12);
		uint32_t firstSym      = readBE32(le + 16);
		uint8_t  options       = le[20];
		bool weakLib = (options & PEF::kWeakImport) != 0;

		std::string libName = readCString(libNameOffset);

		if (trace) fprintf(stderr, "PEF:   %s (%d symbols%s):\n",
		                   libName.c_str(), symCount, weakLib ? ", weak" : "");

		for (uint32_t s = 0; s < symCount; s++) {
			uint32_t symIdx = firstSym + s;
			if (symIdx >= totalImportedSymCount) break;

			const uint8_t *se = symTable + symIdx * 4;
			uint32_t classAndName = readBE32(se);
			uint8_t  symFlags = (classAndName >> 28) & 0x0F;
			uint8_t  symClass = (classAndName >> 24) & 0x0F;
			uint32_t nameOffset = classAndName & 0x00FFFFFF;
			bool weakSym = (symFlags & PEF::kWeakSymbol) != 0;

			std::string symName = readCString(nameOffset);

			uint32_t addr = 0;
			if (resolver)
				addr = resolver(libName, symName, symClass);

			importAddresses[symIdx] = addr;

			if (trace) {
				fprintf(stderr, "PEF:     [%d] %s (%s%s) -> ",
				        symIdx, symName.c_str(), symClassName(symClass),
				        weakSym ? ",weak" : "");
				if (addr) fprintf(stderr, "0x%08X\n", addr);
				else fprintf(stderr, "UNRESOLVED\n");
			}

			if (addr) resolvedCount++;
			else unresolvedCount++;
		}
	}

	if (trace)
		fprintf(stderr, "PEF: imports: %d resolved, %d unresolved\n",
		        resolvedCount, unresolvedCount);

	// -- Parse exports --

	if (exportedSymbolCount > 0 && exportHashOffset < loaderSize) {
		uint32_t hashTableSize = 1u << exportHashTablePower;
		const uint8_t *hashTable = loader + exportHashOffset;
		const uint8_t *keyTable = hashTable + hashTableSize * 4;
		const uint8_t *expSymTable = keyTable + exportedSymbolCount * 4;

		for (uint32_t i = 0; i < exportedSymbolCount; i++) {
			if (expSymTable + i * 10 + 10 > loader + loaderSize) break;
			if (keyTable + i * 4 + 4 > loader + loaderSize) break;

			uint32_t keyEntry = readBE32(keyTable + i * 4);
			uint16_t nameLength = (keyEntry >> 16) & 0xFFFF;

			uint32_t classAndName = readBE32(expSymTable + i * 10);
			uint32_t symbolValue  = readBE32(expSymTable + i * 10 + 4);
			int16_t  sectIdx      = readBE16s(expSymTable + i * 10 + 8);

			uint8_t  symClass   = (classAndName >> 24) & 0x0F;
			uint32_t nameOffset = classAndName & 0x00FFFFFF;

			ExportedSymbolInfo esi;
			esi.name = readString(nameOffset, nameLength);
			esi.sectionIndex = (uint32_t)sectIdx;
			esi.offset = symbolValue;
			esi.symbolClass = symClass;
			result.exports.push_back(esi);
		}

		if (trace)
			fprintf(stderr, "PEF: %zu exports\n", result.exports.size());
	}

	// -- Apply relocations --

	if (relocSectionCount > 0 && relocInstrOffset < loaderSize) {
		const uint8_t *relocHeaders = symTable + totalImportedSymCount * 4;

		for (uint32_t r = 0; r < relocSectionCount; r++) {
			const uint8_t *rh = relocHeaders + r * 12;
			if (rh + 12 > loader + loaderSize) break;

			uint16_t sectIdx       = readBE16(rh + 0);
			uint32_t rCount        = readBE32(rh + 4);
			uint32_t firstRelocOff = readBE32(rh + 8);

			if (sectIdx >= result.sections.size() ||
			    result.sections[sectIdx].address == 0)
				continue;

			const uint8_t *rd = loader + relocInstrOffset + firstRelocOff;
			if (rd + rCount * 2 > loader + loaderSize) {
				fprintf(stderr, "PEF: reloc data beyond loader for section %d\n", sectIdx);
				continue;
			}

			if (trace)
				fprintf(stderr, "PEF: relocating section %d (%s), %d instructions\n",
				        sectIdx, sectionKindName(result.sections[sectIdx].kind), rCount);

			relocateSection(rd, rCount, result.sections, importAddresses,
			                result.sections[sectIdx].address, sectIdx);
		}
	}

	// -- Compute entry points and TOC base --

	result.entryPoint = 0;
	result.initPoint = 0;
	result.termPoint = 0;
	result.tocBase = 0;

	if (mainSection >= 0 && (uint32_t)mainSection < result.sections.size()) {
		uint32_t tvec = result.sections[mainSection].address + mainOffset;
		result.entryPoint = tvec;
		result.tocBase = memoryReadLong(tvec + 4);
	}

	if (initSection >= 0 && (uint32_t)initSection < result.sections.size()) {
		uint32_t tvec = result.sections[initSection].address + initOffset;
		result.initPoint = tvec;
		// Use init TVector's TOC if we don't have one from main.
		// This is the critical fix: shared libraries (like StdCLib) have no
		// main entry, only init. The TOC comes from the init TVector.
		if (result.tocBase == 0)
			result.tocBase = memoryReadLong(tvec + 4);
	}

	if (termSection >= 0 && (uint32_t)termSection < result.sections.size()) {
		result.termPoint = result.sections[termSection].address + termOffset;
	}

	// Fallback: TOC = first data section address
	if (result.tocBase == 0) {
		for (auto &s : result.sections) {
			if (s.kind == PEF::kUnpackedData || s.kind == PEF::kPatternInitData) {
				result.tocBase = s.address;
				break;
			}
		}
	}

	if (trace) {
		fprintf(stderr, "PEF: entryPoint=0x%08X initPoint=0x%08X tocBase=0x%08X\n",
		        result.entryPoint, result.initPoint, result.tocBase);
	}

	return true;
}

bool LoadPEFFile(const std::string &path, ImportResolver resolver, LoadResult &result) {
	std::ifstream f(path, std::ios::binary | std::ios::ate);
	if (!f) {
		fprintf(stderr, "PEF: unable to open %s\n", path.c_str());
		return false;
	}

	auto fileSize = f.tellg();
	f.seekg(0);
	std::vector<uint8_t> data(fileSize);
	f.read(reinterpret_cast<char *>(data.data()), fileSize);
	if (!f) {
		fprintf(stderr, "PEF: unable to read %s\n", path.c_str());
		return false;
	}

	return LoadPEF(data.data(), data.size(), resolver, result);
}

uint32_t FindExport(const LoadResult &result, const std::string &name) {
	for (const auto &exp : result.exports) {
		if (exp.name == name && exp.sectionIndex < result.sections.size()) {
			return result.sections[exp.sectionIndex].address + exp.offset;
		}
	}
	return 0;
}

} // namespace PEFLoader
