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

#ifndef __rsrc_h__
#define __rsrc_h__

#include <cstdint>
#include <string>
#include <vector>
#include <memory>

namespace rsrc {

	// resource attributes (bit flags)
	enum {
		resSysHeap   = 0x40,
		resPurgeable = 0x20,
		resLocked    = 0x10,
		resProtected = 0x08,
		resPreload   = 0x04,
		resChanged   = 0x02,
	};

	// file attributes (bit flags on the map)
	enum {
		mapReadOnly  = 0x80,
		mapCompact   = 0x40,
		mapChanged   = 0x20,
	};

	struct ResourceEntry {
		uint32_t type;
		int16_t  id;
		std::string name;
		uint8_t  attributes;
		uint32_t dataOffset; // offset into data section (relative to data section start)
		uint32_t dataSize;
	};

	class ResourceFile {
	public:
		~ResourceFile();
		ResourceFile(ResourceFile &&) = default;
		ResourceFile &operator=(ResourceFile &&) = default;

		// Parse a resource fork from raw bytes. Returns nullptr on parse failure.
		static std::unique_ptr<ResourceFile> open(const std::vector<uint8_t> &data);

		// Resource type queries
		int countTypes() const;
		uint32_t getIndType(int index) const; // 1-based index

		// Resource queries within this file
		int countResources(uint32_t type) const;

		// Resource retrieval
		const ResourceEntry *findResource(uint32_t type, int16_t id) const;
		const ResourceEntry *findResource(uint32_t type, const std::string &name) const;
		const ResourceEntry *getIndResource(uint32_t type, int index) const; // 1-based

		// Load resource data
		std::vector<uint8_t> loadResource(const ResourceEntry &entry) const;

		// File attributes
		uint16_t fileAttributes() const;
		void setFileAttributes(uint16_t attrs);

		// Write support
		void addResource(uint32_t type, int16_t id, const std::string &name,
		                 uint8_t attrs, const std::vector<uint8_t> &data);
		bool removeResource(uint32_t type, int16_t id);
		bool updateResource(const ResourceEntry &entry, const std::vector<uint8_t> &data);
		bool updateResource(uint32_t type, int16_t id, const std::vector<uint8_t> &data);

		// Serialize the resource file to bytes (for writing back to disk)
		std::vector<uint8_t> serialize() const;

		// Create an empty resource file
		static std::vector<uint8_t> createEmpty();

	private:
		ResourceFile();

		struct TypeEntry {
			uint32_t type;
			std::vector<ResourceEntry> resources;
		};

		std::vector<uint8_t> data_; // raw fork data (for lazy loading)
		std::vector<TypeEntry> types_;
		uint16_t fileAttrs_;
		uint32_t dataOffset_;
	};


	// Platform-specific resource fork access

	// Read the resource fork bytes for a given file path.
	// On macOS: tries path/..namedfork/rsrc, falls back to AppleDouble sidecar
	// On other platforms: reads from AppleDouble sidecar (._basename)
	std::vector<uint8_t> readResourceFork(const std::string &path);

	// Write resource fork bytes for a given file path.
	// On macOS: writes to path/..namedfork/rsrc
	// On other platforms: writes to AppleDouble sidecar (preserves other entries)
	bool writeResourceFork(const std::string &path, const std::vector<uint8_t> &data);

	// Get the resource fork size without reading the entire fork.
	uint32_t resourceForkSize(const std::string &path);

	// Read Finder Info (up to 32 bytes) from AppleDouble sidecar.
	// Returns true if found, false otherwise. Buffer should be at least 32 bytes.
	bool readFinderInfo(const std::string &path, void *buffer, size_t bufferSize);

	// Write Finder Info (up to 32 bytes) to AppleDouble sidecar.
	// Preserves other entries (e.g. resource fork) in the sidecar.
	bool writeFinderInfo(const std::string &path, const void *info, size_t infoSize);

	// Open a resource fork for fd-based I/O.
	// On macOS: opens path/..namedfork/rsrc directly.
	// On other platforms: extracts resource fork to a temp file and opens that.
	// Returns the fd, or -1 on failure.
	// After calling this, use lastTempPath() to get the temp file path (empty on macOS).
	int openResourceFork(const std::string &path, int nativeFlags);

	// Returns the temp file path from the last openResourceFork call.
	// Empty string if the resource fork was opened natively (macOS).
	std::string &lastTempPath();

	// Close a resource fork fd. If tempPath is non-empty, reads the temp file
	// contents back and writes them to the AppleDouble sidecar, then removes
	// the temp file.
	bool closeResourceFork(int fd, const std::string &originalPath,
	                       const std::string &tempPath);

} // namespace rsrc

#endif
