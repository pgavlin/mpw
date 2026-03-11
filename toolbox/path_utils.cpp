#include "path_utils.h"

#include <sys/stat.h>
#include <dirent.h>
#include <strings.h>
#include <vector>

namespace OS {

	// Split a path into components. Preserves leading "/" for absolute paths.
	static std::vector<std::string> split_path(const std::string &path) {
		std::vector<std::string> components;
		size_t start = 0;
		if (!path.empty() && path[0] == '/') {
			start = 1;
		}
		while (start < path.size()) {
			size_t end = path.find('/', start);
			if (end == std::string::npos) end = path.size();
			if (end > start) {
				components.push_back(path.substr(start, end - start));
			}
			start = end + 1;
		}
		return components;
	}

	// Search a directory for a case-insensitive match of 'name'.
	// Returns the on-disk name if found, or empty string if not.
	static std::string find_entry_ci(const std::string &dir, const std::string &name) {
		DIR *dp = opendir(dir.empty() ? "." : dir.c_str());
		if (!dp) return std::string();

		std::string result;
		struct dirent *entry;
		while ((entry = readdir(dp)) != nullptr) {
			if (strcasecmp(entry->d_name, name.c_str()) == 0) {
				result = entry->d_name;
				break;
			}
		}
		closedir(dp);
		return result;
	}

	std::string resolve_path_ci(const std::string &path, bool resolve_leaf) {
		if (path.empty()) return path;

		// Fast path: try the path as-is.
		struct stat st;
		if (resolve_leaf) {
			if (::stat(path.c_str(), &st) == 0) return path;
			// If the error is not "not found", case-insensitive search won't help.
			if (errno != ENOENT) return path;
		}

		auto components = split_path(path);
		if (components.empty()) return path;

		bool absolute = (!path.empty() && path[0] == '/');
		std::string resolved = absolute ? "/" : "";

		size_t count = resolve_leaf ? components.size() : components.size() - 1;

		for (size_t i = 0; i < count; ++i) {
			const std::string &comp = components[i];
			std::string candidate = resolved + comp;

			if (::stat(candidate.c_str(), &st) == 0) {
				// Exact match exists.
				resolved = candidate + "/";
				continue;
			}

			if (errno != ENOENT) {
				// Permission error or similar — can't recover.
				return path;
			}

			// Case-insensitive scan of the current directory.
			std::string match = find_entry_ci(resolved.empty() ? "." : resolved, comp);
			if (match.empty()) {
				// Truly doesn't exist, even case-insensitively.
				return path;
			}

			resolved += match + "/";
		}

		// Remove trailing slash.
		if (!resolved.empty() && resolved.back() == '/' && resolved.size() > 1) {
			resolved.pop_back();
		}

		// Append the unresolved leaf for create paths.
		if (!resolve_leaf && !components.empty()) {
			if (resolved.empty() || resolved.back() != '/') resolved += "/";
			resolved += components.back();
		}

		return resolved;
	}

}
