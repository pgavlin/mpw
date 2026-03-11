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


	// Parse AppleDouble format (magic 0x00051607) to extract the resource fork (entry ID 2).
	std::vector<uint8_t> parseAppleDouble(const std::vector<uint8_t> &data) {
		if (data.size() < 26) return {};

		// Check magic
		uint32_t magic = (uint32_t(data[0]) << 24) | (uint32_t(data[1]) << 16) |
		                 (uint32_t(data[2]) << 8) | data[3];
		if (magic != 0x00051607) return {};

		// Check version
		uint32_t version = (uint32_t(data[4]) << 24) | (uint32_t(data[5]) << 16) |
		                   (uint32_t(data[6]) << 8) | data[7];
		if (version != 0x00020000) return {};

		// Number of entries at offset 24
		uint16_t numEntries = (uint16_t(data[24]) << 8) | data[25];

		// Each entry is 12 bytes starting at offset 26
		for (int i = 0; i < numEntries; ++i) {
			size_t entryOff = 26 + i * 12;
			if (entryOff + 12 > data.size()) break;

			uint32_t entryID = (uint32_t(data[entryOff]) << 24) |
			                   (uint32_t(data[entryOff + 1]) << 16) |
			                   (uint32_t(data[entryOff + 2]) << 8) |
			                   data[entryOff + 3];
			uint32_t offset = (uint32_t(data[entryOff + 4]) << 24) |
			                  (uint32_t(data[entryOff + 5]) << 16) |
			                  (uint32_t(data[entryOff + 6]) << 8) |
			                  data[entryOff + 7];
			uint32_t length = (uint32_t(data[entryOff + 8]) << 24) |
			                  (uint32_t(data[entryOff + 9]) << 16) |
			                  (uint32_t(data[entryOff + 10]) << 8) |
			                  data[entryOff + 11];

			if (entryID == 2) { // Resource fork
				if (offset + length > data.size()) return {};
				return std::vector<uint8_t>(data.begin() + offset,
				                            data.begin() + offset + length);
			}
		}

		return {};
	}


	// Create an AppleDouble file containing a resource fork
	std::vector<uint8_t> createAppleDouble(const std::vector<uint8_t> &rsrcData) {
		// Header: magic(4) + version(4) + filler(16) + numEntries(2) = 26
		// One entry: entryID(4) + offset(4) + length(4) = 12
		// Total header = 38 bytes, then resource fork data

		uint32_t rsrcOffset = 38;
		std::vector<uint8_t> out(rsrcOffset + rsrcData.size(), 0);

		// Magic
		out[0] = 0x00; out[1] = 0x05; out[2] = 0x16; out[3] = 0x07;
		// Version
		out[4] = 0x00; out[5] = 0x02; out[6] = 0x00; out[7] = 0x00;
		// Filler: 16 bytes of zeros (already zeroed)
		// Number of entries: 1
		out[24] = 0x00; out[25] = 0x01;
		// Entry: ID=2 (resource fork)
		out[26] = 0x00; out[27] = 0x00; out[28] = 0x00; out[29] = 0x02;
		// Offset
		out[30] = (rsrcOffset >> 24) & 0xff;
		out[31] = (rsrcOffset >> 16) & 0xff;
		out[32] = (rsrcOffset >> 8) & 0xff;
		out[33] = rsrcOffset & 0xff;
		// Length
		uint32_t len = rsrcData.size();
		out[34] = (len >> 24) & 0xff;
		out[35] = (len >> 16) & 0xff;
		out[36] = (len >> 8) & 0xff;
		out[37] = len & 0xff;

		// Resource fork data
		std::memcpy(out.data() + rsrcOffset, rsrcData.data(), rsrcData.size());

		return out;
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
			auto rsrcData = parseAppleDouble(adData);
			if (!rsrcData.empty()) return rsrcData;
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

	// Fall back to AppleDouble sidecar
	{
		std::string adPath = appleDoublePath(path);
		auto adData = createAppleDouble(data);
		return writeFile(adPath, adData);
	}
}

} // namespace rsrc
