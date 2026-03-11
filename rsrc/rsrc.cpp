/*
 * Copyright (c) 2026
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "rsrc.h"
#include <cstring>
#include <algorithm>

namespace rsrc {

namespace {

	uint16_t read16(const uint8_t *p) {
		return (uint16_t(p[0]) << 8) | p[1];
	}

	uint32_t read24(const uint8_t *p) {
		return (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2];
	}

	uint32_t read32(const uint8_t *p) {
		return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
		       (uint32_t(p[2]) << 8) | p[3];
	}

	void write16(uint8_t *p, uint16_t v) {
		p[0] = (v >> 8) & 0xff;
		p[1] = v & 0xff;
	}

	void write32(uint8_t *p, uint32_t v) {
		p[0] = (v >> 24) & 0xff;
		p[1] = (v >> 16) & 0xff;
		p[2] = (v >> 8) & 0xff;
		p[3] = v & 0xff;
	}

	void write24(uint8_t *p, uint32_t v) {
		p[0] = (v >> 16) & 0xff;
		p[1] = (v >> 8) & 0xff;
		p[2] = v & 0xff;
	}

} // anon namespace


ResourceFile::ResourceFile() : fileAttrs_(0), dataOffset_(0) {}
ResourceFile::~ResourceFile() {}


std::unique_ptr<ResourceFile> ResourceFile::open(const std::vector<uint8_t> &data) {

	if (data.size() < 256) return nullptr; // minimum header size

	const uint8_t *base = data.data();
	uint32_t dataOffset = read32(base + 0);
	uint32_t mapOffset = read32(base + 4);
	uint32_t dataLength = read32(base + 8);
	uint32_t mapLength = read32(base + 12);

	// basic sanity checks
	if (dataOffset + dataLength > data.size()) return nullptr;
	if (mapOffset + mapLength > data.size()) return nullptr;
	if (mapLength < 30) return nullptr; // minimum map size

	const uint8_t *mapBase = base + mapOffset;

	std::unique_ptr<ResourceFile> rf(new ResourceFile());
	rf->data_ = data;
	rf->dataOffset_ = dataOffset;
	rf->fileAttrs_ = read16(mapBase + 22);

	uint16_t typeListOffset = read16(mapBase + 24);
	uint16_t nameListOffset = read16(mapBase + 26);

	const uint8_t *typeListBase = mapBase + typeListOffset;
	const uint8_t *nameListBase = mapBase + nameListOffset;

	// check type list is within map
	if (typeListOffset + 2 > mapLength) return nullptr;

	uint16_t numTypesMinusOne = read16(typeListBase);
	int numTypes = numTypesMinusOne == 0xFFFF ? 0 : (int)numTypesMinusOne + 1;

	// each type entry is 8 bytes, starting after the count word
	if (numTypes > 0 && typeListOffset + 2 + numTypes * 8 > mapLength) return nullptr;

	rf->types_.resize(numTypes);

	for (int t = 0; t < numTypes; ++t) {
		const uint8_t *te = typeListBase + 2 + t * 8;

		uint32_t resType = read32(te);
		uint16_t numResMinusOne = read16(te + 4);
		uint16_t refListOffset = read16(te + 6);

		int numRes = (int)numResMinusOne + 1;

		rf->types_[t].type = resType;
		rf->types_[t].resources.resize(numRes);

		// reference list is relative to the start of the type list
		const uint8_t *refBase = typeListBase + refListOffset;

		for (int r = 0; r < numRes; ++r) {
			const uint8_t *re = refBase + r * 12;

			// bounds check
			if (re + 12 > base + mapOffset + mapLength) return nullptr;

			ResourceEntry &entry = rf->types_[t].resources[r];
			entry.type = resType;
			entry.id = (int16_t)read16(re);
			entry.attributes = re[4];

			uint32_t resDataOffset = read24(re + 5);
			entry.dataOffset = resDataOffset;

			// read data size from the data section
			uint32_t absDataOffset = dataOffset + resDataOffset;
			if (absDataOffset + 4 <= data.size()) {
				entry.dataSize = read32(base + absDataOffset);
			} else {
				entry.dataSize = 0;
			}

			// read name from name list
			uint16_t nameOffset = read16(re + 2);
			if (nameOffset != 0xffff) {
				const uint8_t *namePtr = nameListBase + nameOffset;
				if (namePtr < base + data.size()) {
					uint8_t nameLen = *namePtr;
					if (namePtr + 1 + nameLen <= base + data.size()) {
						entry.name.assign((const char *)(namePtr + 1), nameLen);
					}
				}
			}
		}
	}

	return rf;
}


int ResourceFile::countTypes() const {
	return (int)types_.size();
}


uint32_t ResourceFile::getIndType(int index) const {
	// 1-based
	if (index < 1 || index > (int)types_.size()) return 0;
	return types_[index - 1].type;
}


int ResourceFile::countResources(uint32_t type) const {
	for (const auto &te : types_) {
		if (te.type == type) return (int)te.resources.size();
	}
	return 0;
}


const ResourceEntry *ResourceFile::findResource(uint32_t type, int16_t id) const {
	for (const auto &te : types_) {
		if (te.type != type) continue;
		for (const auto &re : te.resources) {
			if (re.id == id) return &re;
		}
	}
	return nullptr;
}


const ResourceEntry *ResourceFile::findResource(uint32_t type, const std::string &name) const {
	for (const auto &te : types_) {
		if (te.type != type) continue;
		for (const auto &re : te.resources) {
			// Mac resource names are case-insensitive
			if (re.name.length() == name.length() &&
			    std::equal(re.name.begin(), re.name.end(), name.begin(),
			               [](char a, char b) {
			                   return tolower((unsigned char)a) == tolower((unsigned char)b);
			               }))
			{
				return &re;
			}
		}
	}
	return nullptr;
}


const ResourceEntry *ResourceFile::getIndResource(uint32_t type, int index) const {
	// 1-based
	for (const auto &te : types_) {
		if (te.type != type) continue;
		if (index < 1 || index > (int)te.resources.size()) return nullptr;
		return &te.resources[index - 1];
	}
	return nullptr;
}


std::vector<uint8_t> ResourceFile::loadResource(const ResourceEntry &entry) const {
	uint32_t absOffset = dataOffset_ + entry.dataOffset;
	if (absOffset + 4 > data_.size()) return {};

	uint32_t size = read32(data_.data() + absOffset);
	if (absOffset + 4 + size > data_.size()) return {};

	return std::vector<uint8_t>(
		data_.data() + absOffset + 4,
		data_.data() + absOffset + 4 + size
	);
}


uint16_t ResourceFile::fileAttributes() const {
	return fileAttrs_;
}

void ResourceFile::setFileAttributes(uint16_t attrs) {
	fileAttrs_ = attrs;
}


void ResourceFile::addResource(uint32_t type, int16_t id, const std::string &name,
                                uint8_t attrs, const std::vector<uint8_t> &resData) {
	// find or create type entry
	TypeEntry *te = nullptr;
	for (auto &t : types_) {
		if (t.type == type) { te = &t; break; }
	}
	if (!te) {
		types_.push_back({type, {}});
		te = &types_.back();
	}

	ResourceEntry entry;
	entry.type = type;
	entry.id = id;
	entry.name = name;
	entry.attributes = attrs | resChanged;
	entry.dataSize = resData.size();

	// Store the data in data_ so loadResource() can find it during serialize().
	// dataOffset is relative to dataOffset_ in the raw buffer.
	// We append: 4-byte size + data bytes.
	uint32_t appendPos = data_.size();
	entry.dataOffset = appendPos - dataOffset_;

	uint8_t sizeBuf[4];
	write32(sizeBuf, resData.size());
	data_.insert(data_.end(), sizeBuf, sizeBuf + 4);
	data_.insert(data_.end(), resData.begin(), resData.end());

	te->resources.push_back(std::move(entry));
}


bool ResourceFile::removeResource(uint32_t type, int16_t id) {
	for (auto &te : types_) {
		if (te.type != type) continue;
		for (auto it = te.resources.begin(); it != te.resources.end(); ++it) {
			if (it->id == id) {
				te.resources.erase(it);
				return true;
			}
		}
	}
	return false;
}


bool ResourceFile::updateResource(const ResourceEntry &entry, const std::vector<uint8_t> &newData) {
	return updateResource(entry.type, entry.id, newData);
}

bool ResourceFile::updateResource(uint32_t type, int16_t id, const std::vector<uint8_t> &newData) {
	for (auto &te : types_) {
		if (te.type != type) continue;
		for (auto &re : te.resources) {
			if (re.id == id) {
				re.attributes |= resChanged;
				// Update the data in data_ so serialize/loadResource finds the new data.
				uint32_t appendPos = data_.size();
				re.dataOffset = appendPos - dataOffset_;
				re.dataSize = newData.size();

				uint8_t sizeBuf[4];
				write32(sizeBuf, newData.size());
				data_.insert(data_.end(), sizeBuf, sizeBuf + 4);
				data_.insert(data_.end(), newData.begin(), newData.end());
				return true;
			}
		}
	}
	return false;
}


std::vector<uint8_t> ResourceFile::createEmpty() {
	// Create a minimal empty resource fork
	// Header: 256 bytes
	// Data section: 0 bytes (at offset 256)
	// Map section: 30 bytes (at offset 256)

	uint32_t dataOffset = 256;
	uint32_t mapOffset = 256;
	uint32_t dataLength = 0;
	uint32_t mapLength = 30;

	std::vector<uint8_t> out(dataOffset + dataLength + mapLength, 0);

	// header
	write32(out.data() + 0, dataOffset);
	write32(out.data() + 4, mapOffset);
	write32(out.data() + 8, dataLength);
	write32(out.data() + 12, mapLength);

	// map: copy of header (first 16 bytes)
	uint8_t *map = out.data() + mapOffset;
	std::memcpy(map, out.data(), 16);

	// map: reserved handle (4 bytes) = 0
	// map: reserved fileRef (2 bytes) = 0
	// map: file attributes (2 bytes) = 0
	// map: type list offset (2 bytes) = 28 (relative to map start)
	write16(map + 24, 28);
	// map: name list offset (2 bytes) = 30 (relative to map start, = end of map)
	write16(map + 26, 30);
	// type list: numTypes - 1 = 0xFFFF (meaning 0 types)
	write16(map + 28, 0xffff);

	return out;
}


std::vector<uint8_t> ResourceFile::serialize() const {
	// Rebuild the entire resource fork from the in-memory representation.

	// 1. Build data section
	std::vector<uint8_t> dataSection;

	// We need a parallel structure to track where each resource's data ends up
	struct SerEntry {
		int typeIdx;
		int resIdx;
		uint32_t newDataOffset;
	};
	std::vector<SerEntry> entries;

	for (int t = 0; t < (int)types_.size(); ++t) {
		for (int r = 0; r < (int)types_[t].resources.size(); ++r) {
			const auto &re = types_[t].resources[r];

			SerEntry se;
			se.typeIdx = t;
			se.resIdx = r;
			se.newDataOffset = dataSection.size();

			// load existing data
			auto resData = loadResource(re);

			// write size + data
			uint8_t sizeBuf[4];
			write32(sizeBuf, resData.size());
			dataSection.insert(dataSection.end(), sizeBuf, sizeBuf + 4);
			dataSection.insert(dataSection.end(), resData.begin(), resData.end());

			entries.push_back(se);
		}
	}

	// 2. Build name list
	std::vector<uint8_t> nameList;
	// Map from (typeIdx, resIdx) -> nameOffset
	std::vector<uint16_t> nameOffsets(entries.size(), 0xffff);

	for (size_t i = 0; i < entries.size(); ++i) {
		const auto &re = types_[entries[i].typeIdx].resources[entries[i].resIdx];
		if (!re.name.empty()) {
			nameOffsets[i] = nameList.size();
			nameList.push_back((uint8_t)re.name.size());
			nameList.insert(nameList.end(), re.name.begin(), re.name.end());
		}
	}

	// 3. Build type list and reference lists
	int numTypes = types_.size();

	// type list: 2 bytes (count-1) + numTypes * 8
	uint32_t typeListSize = 2 + numTypes * 8;

	// reference lists: for each type, numResources * 12
	uint32_t refListsSize = 0;
	for (const auto &te : types_) {
		refListsSize += te.resources.size() * 12;
	}

	// 4. Calculate offsets
	uint32_t dataOffset = 256; // header is 256 bytes
	uint32_t mapOffset = dataOffset + dataSection.size();

	// map layout:
	// 0-15: reserved header copy (16 bytes)
	// 16-19: reserved handle (4 bytes)
	// 20-21: reserved fileRef (2 bytes)
	// 22-23: file attributes (2 bytes)
	// 24-25: type list offset (2 bytes) = 28
	// 26-27: name list offset (2 bytes)
	// 28+: type list
	// 28 + typeListSize + refListsSize: name list

	uint32_t typeListOffset = 28;
	uint32_t nameListOffset = typeListOffset + typeListSize + refListsSize;
	uint32_t mapLength = nameListOffset + nameList.size();

	// 5. Build output
	std::vector<uint8_t> out(dataOffset + dataSection.size() + mapLength, 0);

	// header
	write32(out.data() + 0, dataOffset);
	write32(out.data() + 4, mapOffset);
	write32(out.data() + 8, dataSection.size());
	write32(out.data() + 12, mapLength);

	// copy data section
	std::memcpy(out.data() + dataOffset, dataSection.data(), dataSection.size());

	// build map
	uint8_t *map = out.data() + mapOffset;

	// reserved header copy
	std::memcpy(map, out.data(), 16);

	// file attributes
	write16(map + 22, fileAttrs_ & ~(mapChanged)); // clear changed flag on write

	// offsets
	write16(map + 24, typeListOffset);
	write16(map + 26, nameListOffset);

	// type list
	uint8_t *tl = map + typeListOffset;
	write16(tl, numTypes > 0 ? numTypes - 1 : 0xffff);

	uint32_t refOffset = 2 + numTypes * 8; // relative to type list start
	int entryIdx = 0;

	for (int t = 0; t < numTypes; ++t) {
		uint8_t *te = tl + 2 + t * 8;
		write32(te, types_[t].type);
		write16(te + 4, types_[t].resources.size() - 1);
		write16(te + 6, refOffset);

		// reference list
		uint8_t *rl = tl + refOffset;
		for (int r = 0; r < (int)types_[t].resources.size(); ++r) {
			const auto &re = types_[t].resources[r];
			uint8_t *rle = rl + r * 12;

			write16(rle, (uint16_t)re.id);
			write16(rle + 2, nameOffsets[entryIdx]);
			rle[4] = re.attributes & ~resChanged; // clear changed on write
			write24(rle + 5, entries[entryIdx].newDataOffset);
			write32(rle + 8, 0); // reserved handle

			++entryIdx;
		}

		refOffset += types_[t].resources.size() * 12;
	}

	// name list
	if (!nameList.empty()) {
		std::memcpy(map + nameListOffset, nameList.data(), nameList.size());
	}

	return out;
}

} // namespace rsrc
