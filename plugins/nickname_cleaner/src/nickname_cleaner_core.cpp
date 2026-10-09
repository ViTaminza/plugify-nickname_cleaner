#include "nickname_cleaner_core.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>

namespace nickname_cleaner {

bool IsPlaceholderName(const char* name) {
	return name == nullptr || name[0] == '\0' || std::strcmp(name, "<unknown>") == 0;
}

namespace {

// True when the line starts with the marker (the original's rfind(marker, 0) == 0).
bool StartsWith(std::string_view text, std::string_view marker) {
	return text.size() >= marker.size() && text.compare(0, marker.size(), marker) == 0;
}

std::string_view Utf8BomStripped(std::string_view line) {
	if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
		static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF) {
		return line.substr(3);
	}
	return line;
}

} // namespace

void NameLists::Clear() {
	whitelist.clear();
	blocklist.clear();
}

void NameLists::LoadListContent(std::string_view content, bool blocklist) {
	std::unordered_set<std::string>& out = blocklist ? this->blocklist : this->whitelist;
	out.clear();
	std::istringstream stream{std::string(content)};
	std::string line;
	while (std::getline(stream, line)) {
		std::string trimmed = Trim(line);
		trimmed = std::string(Utf8BomStripped(trimmed));
		if (trimmed.empty()) {
			continue;
		}
		if (StartsWith(trimmed, "#") || StartsWith(trimmed, "//")) {
			continue;
		}
		out.insert(ToLower(std::move(trimmed)));
	}
}

std::string ToLower(std::string text) {
	std::transform(text.begin(), text.end(), text.begin(),
				   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return text;
}

std::string Trim(const std::string& text) {
	const auto first = text.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) {
		return {};
	}
	const auto last = text.find_last_not_of(" \t\r\n");
	return text.substr(first, last - first + 1);
}

bool IsWhitelistedToken(const std::string& token, const NameLists& lists) {
	const std::string lower = ToLower(token);
	for (const auto& allowed : lists.whitelist) {
		if (lower.find(allowed) != std::string::npos) {
			return true;
		}
	}
	return false;
}

bool IsBlockedToken(const std::string& token, const NameLists& lists) {
	const std::string lower = ToLower(token);
	for (const auto& blocked : lists.blocklist) {
		if (blocked.size() > 2 && blocked[0] == '*' && blocked[1] == '.') {
			// "*.xx": suffix match on ".xx" (the original compared the tail of the token).
			const std::string suffix = blocked.substr(1);
			if (lower.size() >= suffix.size() &&
				lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0) {
				return true;
			}
		} else if (lower.find(blocked) != std::string::npos) {
			return true;
		}
	}
	return false;
}

std::string StripPunctuation(std::string token) {
	token.erase(std::remove_if(token.begin(), token.end(),
							   [](unsigned char c) { return std::isspace(c) != 0; }),
				token.end());
	while (!token.empty() && std::ispunct(static_cast<unsigned char>(token.front())) != 0) {
		token.erase(token.begin());
	}
	while (!token.empty() && std::ispunct(static_cast<unsigned char>(token.back())) != 0) {
		token.pop_back();
	}
	return token;
}

bool LooksLikeAddress(const std::string& token) {
	const std::string lower = ToLower(token);
	return lower.find("http://") != std::string::npos || lower.find("https://") != std::string::npos ||
		   lower.find("www.") != std::string::npos;
}

std::string CleanNickname(std::string_view rawName, const NameLists& lists, std::string_view defaultName) {
	std::istringstream stream{std::string(rawName)};
	std::string kept;
	std::string token;
	while (stream >> token) {
		std::string simplified = StripPunctuation(token);
		if (simplified.empty()) {
			continue;
		}
		if (IsWhitelistedToken(simplified, lists)) {
			// A whitelisted token is kept even when it looks like an address.
			if (!kept.empty()) {
				kept.push_back(' ');
			}
			kept += simplified;
			continue;
		}
		if (IsBlockedToken(simplified, lists) || LooksLikeAddress(simplified)) {
			continue;
		}
		if (!kept.empty()) {
			kept.push_back(' ');
		}
		kept += simplified;
	}
	if (kept.empty()) {
		return std::string(defaultName);
	}
	return kept;
}

} // namespace nickname_cleaner
