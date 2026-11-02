// nickname_cleaner — config locations and the settings.ini model of the plugin.
//
// The plugin is pure s2sdk (docs/PLUGIN_STANDARD.md §7), so the pisex::ResolvePath /
// pisex::ReadFile / pisex::KeyValues trio of the first port is replaced by this header plus
// src/kv1.hpp:
//
//   <GetConfigsDir()>/nickname_cleaner/settings.ini      the packaged location (PLUGIN_STANDARD §3)
//   <GetConfigsDir()>/nickname_cleaner/whitelist.txt
//   <GetConfigsDir()>/nickname_cleaner/blocklist.txt
//   <GetConfigsDir()>/settings.ini                       a file dropped straight into configs/
//   <GetConfigsDir()>/whitelist.txt
//   <GetConfigsDir()>/blocklist.txt
//
// The first existing candidate wins; a malformed primary file is *not* silently replaced by the
// fallback (the caller logs it and keeps its default), exactly like the advertisement plugin.
// A missing file is what the original's KeyValues load answered: the entry is empty and the
// plugin keeps the built-in default name.
//
// Header-only and engine-free apart from plg::GetConfigsDir: the test drives the path resolution
// with a fake plg::GetConfigsDir.

#pragma once

#include <plg/plugin.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "kv1.hpp"

namespace nickname_cleaner {

// The plugin directory under <GetConfigsDir()> (matches the manifest name).
inline constexpr char kConfigDir[] = "nickname_cleaner";

inline constexpr char kSettingsFile[] = "settings.ini";
inline constexpr char kWhitelistFile[] = "whitelist.txt";
inline constexpr char kBlocklistFile[] = "blocklist.txt";

// "NicknameCleaner" / "DefaultName" of the original settings.ini; "Player" is the Metamod
// default (`m_szDefaultName = "Player"`).
inline constexpr char kSettingsSection[] = "NicknameCleaner";
inline constexpr char kDefaultNameKey[] = "DefaultName";
inline constexpr char kDefaultDefaultName[] = "Player";

inline std::string JoinPath(std::string_view dir, std::string_view file) {
	if (dir.empty()) {
		return std::string(file);
	}
	std::string path(dir);
	if (path.back() != '/') {
		path.push_back('/');
	}
	if (!file.empty() && file.front() == '/') {
		file.remove_prefix(1);
	}
	path.append(file);
	return path;
}

// <plg::GetConfigsDir()>; "" when the host provides no configs dir (the test binary drives it).
inline std::string ConfigsDir() {
	if (plg::GetConfigsDir == nullptr) {
		return {};
	}
	return std::string(plg::GetConfigsDir());
}

// The two locations of a plugin file, in priority order (the packaged plugin dir first, then the
// configs root). Empty when the host exposes no configs dir.
inline std::vector<std::string> ConfigCandidates(std::string_view fileName) {
	std::vector<std::string> candidates;
	const std::string root = ConfigsDir();
	if (root.empty()) {
		return candidates;
	}
	candidates.push_back(JoinPath(JoinPath(root, kConfigDir), fileName));
	candidates.push_back(JoinPath(root, fileName));
	return candidates;
}

// The packaged candidate (used for log lines); the bare file name when no configs dir exists.
inline std::string PrimaryConfigPath(std::string_view fileName) {
	const std::vector<std::string> candidates = ConfigCandidates(fileName);
	return candidates.empty() ? std::string(fileName) : candidates.front();
}

// Reads the first existing candidate. `usedPath` receives the path that was read. False when no
// candidate exists (as opposed to a file that exists but is malformed: that one is returned and
// the caller's ParseKv1 decides).
inline bool ReadConfigFile(std::string_view fileName, std::string& content, std::string& usedPath) {
	for (const std::string& candidate : ConfigCandidates(fileName)) {
		std::optional<std::string> text = ReadTextFile(candidate);
		if (!text) {
			continue;
		}
		content = std::move(*text);
		usedPath = candidate;
		return true;
	}
	return false;
}

// The DefaultName of a parsed settings.ini. ParseKv1() flattens the single "NicknameCleaner" { ... }
// block, so the key is a direct child; a file without the section is accepted too (the original's
// KeyValues::GetString looked at the loaded root). An empty/missing value keeps the built-in
// default.
inline std::string DefaultNameFromKv(const KvNode& root) {
	const KvNode* section = root.Find(kSettingsSection);
	const std::string_view value =
			section != nullptr ? section->GetString(kDefaultNameKey) : root.GetString(kDefaultNameKey);
	if (value.empty()) {
		return std::string(kDefaultDefaultName);
	}
	return std::string(value);
}

} // namespace nickname_cleaner
