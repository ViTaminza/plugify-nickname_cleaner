// nickname_cleaner — engine-independent core: the whitelist/blocklist tables and the nickname
// scrubbing of the original plugin (ref/src/cs2-nickname_cleaner/nickname_cleaner.cpp).
//
// Everything here is pure string work, so it is covered by nickname_cleaner_core_tests without an
// engine (see tests/nickname_cleaner_core_tests.cpp).

#pragma once

#include <string>
#include <string_view>
#include <unordered_set>

namespace nickname_cleaner {

// The two filter tables (g_Whitelist / g_Blocklist of the original).
struct NameLists {
	std::unordered_set<std::string> whitelist;
	std::unordered_set<std::string> blocklist;

	void Clear();
	// Parses a whitelist.txt / blocklist.txt body: one entry per line, trimmed, UTF-8 BOM
	// stripped, "#"/"//" comments and empty lines skipped, lowercased. Exactly what the
	// original LoadList() did with the engine filesystem content.
	void LoadListContent(std::string_view content, bool blocklist);
};

// True when a name read must be treated as "the slot has no name": a null/empty string is what
// the Metamod original's CCSPlayerController::FromSlot()-based GetPlayerName() returned for a slot
// without a controller, and "<unknown>" is the placeholder s2sdk's Player::GetName() answers for a
// slot whose client object is gone (player_manager.cpp). Callers must not touch such a slot.
bool IsPlaceholderName(const char* name);

// std::tolower over the bytes (the original lowercased ASCII; non-ASCII bytes pass through).
std::string ToLower(std::string text);
// Trims " \t\r\n" from both ends (the original Trim()).
std::string Trim(const std::string& text);

// The wildcard/substring membership tests of the original:
//   whitelist - case-insensitive substring match;
//   blocklist - an entry "*.xx" matches a token that *ends* with ".xx", any other entry is a
//               substring match. The whitelist is consulted first by CleanNickname().
bool IsWhitelistedToken(const std::string& token, const NameLists& lists);
bool IsBlockedToken(const std::string& token, const NameLists& lists);

// Removes whitespace inside the token and leading/trailing punctuation (ispunct).
std::string StripPunctuation(std::string token);
// True for "http://", "https://" or "www." anywhere in the token (the original LooksLikeAddress).
bool LooksLikeAddress(const std::string& token);

// The original CleanNickname(): whitespace-split, punctuation-strip every token, keep the
// whitelisted ones as-is, drop blocked ones and plain addresses, join the rest with single
// spaces. An empty result becomes defaultName. The "blockedTokens" bookkeeping of the original
// only built an unused string and is not ported.
std::string CleanNickname(std::string_view rawName, const NameLists& lists, std::string_view defaultName);

} // namespace nickname_cleaner
