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
#include <cerrno>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace rsrc {

namespace {

	// AppleDouble entry IDs
	enum {
		kEntryResourceFork = 2,
		kEntryFinderInfo = 9,
	};

	// AppleDouble constants
	const uint32_t kAppleDoubleMagic = 0x00051607;
	const uint32_t kAppleDoubleVersion = 0x00020000;
	const size_t kAppleDoubleHeaderSize = 26; // magic(4) + version(4) + filler(16) + numEntries(2)
	const size_t kAppleDoubleEntrySize = 12;  // entryID(4) + offset(4) + length(4)
	const size_t kFinderInfoSize = 32;

	uint32_t read32(const uint8_t *p) {
		return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
		       (uint32_t(p[2]) << 8) | p[3];
	}

	uint16_t read16(const uint8_t *p) {
		return (uint16_t(p[0]) << 8) | p[1];
	}

	void write32(uint8_t *p, uint32_t v) {
		p[0] = (v >> 24) & 0xff;
		p[1] = (v >> 16) & 0xff;
		p[2] = (v >> 8) & 0xff;
		p[3] = v & 0xff;
	}

	void write16(uint8_t *p, uint16_t v) {
		p[0] = (v >> 8) & 0xff;
		p[1] = v & 0xff;
	}


	// Read an entire file into a byte vector. Returns empty on failure.
	std::vector<uint8_t> readFile(const std::string &path) {
		int fd = ::open(path.c_str(), O_RDONLY);
		if (fd < 0) return {};

		struct stat st;
		if (::fstat(fd, &st) < 0 || st.st_size <= 0) {
			::close(fd);
			return {};
		}

		std::vector<uint8_t> data(st.st_size);
		ssize_t total = 0;
		while (total < st.st_size) {
			ssize_t n = ::read(fd, data.data() + total, st.st_size - total);
			if (n <= 0) {
				::close(fd);
				return {};
			}
			total += n;
		}

		::close(fd);
		return data;
	}


	bool writeFile(const std::string &path, const std::vector<uint8_t> &data) {
		int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
		if (fd < 0) return false;

		ssize_t total = 0;
		ssize_t size = data.size();
		while (total < size) {
			ssize_t n = ::write(fd, data.data() + total, size - total);
			if (n <= 0) {
				::close(fd);
				return false;
			}
			total += n;
		}

		::close(fd);
		return true;
	}


	// Extract directory and basename from a path
	void splitPath(const std::string &path, std::string &dir, std::string &base) {
		auto pos = path.rfind('/');
		if (pos == std::string::npos) {
			dir = ".";
			base = path;
		} else {
			dir = path.substr(0, pos);
			base = path.substr(pos + 1);
		}
	}


	std::string appleDoublePath(const std::string &path) {
		std::string dir, base;
		splitPath(path, dir, base);
		return dir + "/._" + base;
	}


	// Parsed AppleDouble entry descriptor
	struct ADEntry {
		uint32_t id;
		uint32_t offset;
		uint32_t length;
	};


	// Parse AppleDouble header and return entry descriptors.
	// Returns false if the data is not valid AppleDouble.
	bool parseAppleDoubleEntries(const std::vector<uint8_t> &data,
	                             std::vector<ADEntry> &entries) {
		entries.clear();
		if (data.size() < kAppleDoubleHeaderSize) return false;

		if (read32(data.data()) != kAppleDoubleMagic) return false;
		if (read32(data.data() + 4) != kAppleDoubleVersion) return false;

		uint16_t numEntries = read16(data.data() + 24);

		for (int i = 0; i < numEntries; ++i) {
			size_t off = kAppleDoubleHeaderSize + i * kAppleDoubleEntrySize;
			if (off + kAppleDoubleEntrySize > data.size()) break;

			ADEntry e;
			e.id = read32(data.data() + off);
			e.offset = read32(data.data() + off + 4);
			e.length = read32(data.data() + off + 8);
			entries.push_back(e);
		}
		return true;
	}


	// Extract a specific entry from parsed AppleDouble data.
	std::vector<uint8_t> extractEntry(const std::vector<uint8_t> &data,
	                                  const std::vector<ADEntry> &entries,
	                                  uint32_t entryID) {
		for (const auto &e : entries) {
			if (e.id == entryID) {
				if (e.offset + e.length > data.size()) return {};
				return std::vector<uint8_t>(data.begin() + e.offset,
				                            data.begin() + e.offset + e.length);
			}
		}
		return {};
	}


	// Build an AppleDouble sidecar from the given entries.
	// Each pair is (entryID, data).
	std::vector<uint8_t> buildAppleDouble(
	    const std::vector<std::pair<uint32_t, std::vector<uint8_t>>> &entries) {

		uint16_t numEntries = entries.size();
		size_t headerSize = kAppleDoubleHeaderSize + numEntries * kAppleDoubleEntrySize;

		// Calculate total size
		size_t totalSize = headerSize;
		for (const auto &e : entries) totalSize += e.second.size();

		std::vector<uint8_t> out(totalSize, 0);

		// Header
		write32(out.data(), kAppleDoubleMagic);
		write32(out.data() + 4, kAppleDoubleVersion);
		// filler (16 bytes of zeros) - already zeroed
		write16(out.data() + 24, numEntries);

		// Entry table and data
		uint32_t dataOffset = headerSize;
		for (int i = 0; i < numEntries; ++i) {
			size_t entryOff = kAppleDoubleHeaderSize + i * kAppleDoubleEntrySize;
			write32(out.data() + entryOff, entries[i].first);
			write32(out.data() + entryOff + 4, dataOffset);
			write32(out.data() + entryOff + 8, entries[i].second.size());

			if (!entries[i].second.empty()) {
				std::memcpy(out.data() + dataOffset,
				            entries[i].second.data(),
				            entries[i].second.size());
			}
			dataOffset += entries[i].second.size();
		}

		return out;
	}


	// Read existing AppleDouble sidecar, update one entry, preserve others.
	bool updateAppleDoubleEntry(const std::string &path,
	                            uint32_t entryID,
	                            const std::vector<uint8_t> &entryData) {
		std::string adPath = appleDoublePath(path);

		// Read existing sidecar (may not exist)
		auto existing = readFile(adPath);
		std::vector<ADEntry> oldEntries;

		// Build new entry list preserving existing entries
		std::vector<std::pair<uint32_t, std::vector<uint8_t>>> newEntries;
		bool replaced = false;

		if (!existing.empty() && parseAppleDoubleEntries(existing, oldEntries)) {
			for (const auto &oe : oldEntries) {
				if (oe.id == entryID) {
					newEntries.push_back({entryID, entryData});
					replaced = true;
				} else {
					auto data = extractEntry(existing, {oe}, oe.id);
					newEntries.push_back({oe.id, std::move(data)});
				}
			}
		}

		if (!replaced) {
			newEntries.push_back({entryID, entryData});
		}

		auto adData = buildAppleDouble(newEntries);
		return writeFile(adPath, adData);
	}

} // anon namespace


std::vector<uint8_t> readResourceFork(const std::string &path) {

#ifdef __APPLE__
	// On macOS, try the named fork first
	{
		std::string forkPath = path + "/..namedfork/rsrc";
		auto data = readFile(forkPath);
		if (!data.empty()) return data;
	}
#endif

	// Try AppleDouble sidecar
	{
		std::string adPath = appleDoublePath(path);
		auto adData = readFile(adPath);
		if (!adData.empty()) {
			std::vector<ADEntry> entries;
			if (parseAppleDoubleEntries(adData, entries)) {
				auto rsrcData = extractEntry(adData, entries, kEntryResourceFork);
				if (!rsrcData.empty()) return rsrcData;
			}
		}
	}

	return {};
}


bool writeResourceFork(const std::string &path, const std::vector<uint8_t> &data) {

#ifdef __APPLE__
	// On macOS, write to the named fork
	{
		std::string forkPath = path + "/..namedfork/rsrc";
		if (writeFile(forkPath, data)) return true;
	}
#endif

	// Fall back to AppleDouble sidecar (preserves existing Finder Info)
	return updateAppleDoubleEntry(path, kEntryResourceFork, data);
}


uint32_t resourceForkSize(const std::string &path) {

#ifdef __APPLE__
	{
		std::string forkPath = path + "/..namedfork/rsrc";
		struct stat st;
		if (::stat(forkPath.c_str(), &st) == 0)
			return st.st_size;
	}
#endif

	// Try AppleDouble sidecar
	{
		std::string adPath = appleDoublePath(path);
		auto adData = readFile(adPath);
		if (!adData.empty()) {
			std::vector<ADEntry> entries;
			if (parseAppleDoubleEntries(adData, entries)) {
				for (const auto &e : entries) {
					if (e.id == kEntryResourceFork)
						return e.length;
				}
			}
		}
	}

	return 0;
}


bool readFinderInfo(const std::string &path, void *buffer, size_t bufferSize) {
	if (bufferSize > kFinderInfoSize) bufferSize = kFinderInfoSize;

	std::string adPath = appleDoublePath(path);
	auto adData = readFile(adPath);
	if (adData.empty()) return false;

	std::vector<ADEntry> entries;
	if (!parseAppleDoubleEntries(adData, entries)) return false;

	auto fiData = extractEntry(adData, entries, kEntryFinderInfo);
	if (fiData.empty()) return false;

	size_t copySize = std::min(bufferSize, fiData.size());
	std::memcpy(buffer, fiData.data(), copySize);
	return true;
}


bool writeFinderInfo(const std::string &path, const void *info, size_t infoSize) {
	if (infoSize > kFinderInfoSize) infoSize = kFinderInfoSize;

	std::vector<uint8_t> fiData(kFinderInfoSize, 0);
	std::memcpy(fiData.data(), info, infoSize);

	return updateAppleDoubleEntry(path, kEntryFinderInfo, fiData);
}


int openResourceFork(const std::string &path, int nativeFlags) {

#ifdef __APPLE__
	{
		std::string forkPath = path + "/..namedfork/rsrc";
		int fd = ::open(forkPath.c_str(), nativeFlags, 0666);
		if (fd >= 0) return fd;
		// If O_CREAT was requested, try creating
		if ((nativeFlags & O_CREAT) && errno == ENOENT) {
			fd = ::open(forkPath.c_str(), nativeFlags | O_CREAT, 0666);
			if (fd >= 0) return fd;
		}
	}
#endif

	// On non-macOS: extract resource fork to a temp file.
	// Caller is responsible for writing back via writeResourceFork on close.
	auto rsrcData = readResourceFork(path);

	char tmpPath[] = "/tmp/mpw_rsrc_XXXXXX";
	int fd = ::mkstemp(tmpPath);
	if (fd < 0) return -1;

	// Immediately unlink so the temp file is cleaned up if we crash.
	// We'll read it back via the fd before closing.
	// Actually, we need the path for the FDEntry to track it.
	// Don't unlink — we'll clean up in close.

	if (!rsrcData.empty()) {
		ssize_t total = 0;
		ssize_t size = rsrcData.size();
		while (total < size) {
			ssize_t n = ::write(fd, rsrcData.data() + total, size - total);
			if (n <= 0) {
				::close(fd);
				::unlink(tmpPath);
				return -1;
			}
			total += n;
		}
		::lseek(fd, 0, SEEK_SET);
	}

	if (nativeFlags & O_TRUNC) {
		::ftruncate(fd, 0);
	}

	// Store the temp path in a way the caller can retrieve.
	// We use a static to pass it back (not thread-safe, but matches
	// the single-threaded nature of this emulator).
	lastTempPath() = tmpPath;

	return fd;
}


std::string &lastTempPath() {
	static std::string path;
	return path;
}


bool closeResourceFork(int fd, const std::string &originalPath,
                       const std::string &tempPath) {
	if (tempPath.empty()) {
		// macOS path — just close
		::close(fd);
		return true;
	}

	// Read temp file contents and write back to AppleDouble sidecar
	struct stat st;
	bool ok = true;
	if (::fstat(fd, &st) == 0 && st.st_size > 0) {
		std::vector<uint8_t> data(st.st_size);
		::lseek(fd, 0, SEEK_SET);
		ssize_t total = 0;
		while (total < st.st_size) {
			ssize_t n = ::read(fd, data.data() + total, st.st_size - total);
			if (n <= 0) break;
			total += n;
		}
		if (total == st.st_size) {
			ok = writeResourceFork(originalPath, data);
		}
	}

	::close(fd);
	::unlink(tempPath.c_str());
	return ok;
}

} // namespace rsrc
