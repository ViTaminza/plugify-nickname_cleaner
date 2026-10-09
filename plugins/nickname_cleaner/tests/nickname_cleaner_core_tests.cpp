// nickname_cleaner unit tests — the engine-independent filter/cleaning logic, including against
// the *production* whitelist.txt/blocklist.txt of ref/prod-27016 (the files the package ships).
//
// Run by ../../test.sh (inside the Steam Runtime image, cwd = build/tests).

#include "nickname_cleaner_core.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const std::string& what) {
	++g_checks;
	if (!condition) {
		++g_failures;
		std::printf("FAIL: %s\n", what.c_str());
	}
}

void CheckEq(const std::string& actual, const std::string& expected, const std::string& what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL: %s\n  expected: [%s]\n  actual:   [%s]\n", what.c_str(), expected.c_str(),
					actual.c_str());
	}
}

std::string ReadWholeFile(const std::string& path, bool& ok) {
	std::ifstream file(path, std::ios::binary);
	ok = file.good();
	if (!ok) {
		return {};
	}
	std::ostringstream buffer;
	buffer << file.rdbuf();
	return buffer.str();
}

void TestListParsing() {
	nickname_cleaner::NameLists lists;
	lists.LoadListContent("\xEF\xBB\xBF*.com\n" // a UTF-8 BOM on the first line
						  "\n"
						  "  *.ru  \n"
						  "# comment\n"
						  "// other comment\n"
						  "Word\n",
						  true);
	Check(lists.blocklist.size() == 3, "BOM/comments/blank lines are skipped");
	Check(lists.blocklist.count("*.com") == 1, "BOM stripped from the first entry");
	Check(lists.blocklist.count("*.ru") == 1, "entries are trimmed");
	Check(lists.blocklist.count("word") == 1, "entries are lowercased");

	nickname_cleaner::NameLists again;
	again.LoadListContent("*.com\n", true);
	Check(again.blocklist.size() == 1, "loading a list clears the previous content");
}

void TestMatching() {
	nickname_cleaner::NameLists lists;
	lists.LoadListContent("*.ru\nbadword\n", true);
	lists.LoadListContent("vcs2.ru\n", false);

	Check(nickname_cleaner::IsBlockedToken("site.ru", lists), "*.ru matches the end of a token");
	Check(nickname_cleaner::IsBlockedToken("SITE.RU", lists), "suffix match is case-insensitive");
	Check(!nickname_cleaner::IsBlockedToken("rusite", lists), "*.ru does not match a prefix");
	Check(nickname_cleaner::IsBlockedToken("aBADWORDb", lists), "plain entries match as substrings");
	Check(!nickname_cleaner::IsBlockedToken("clean", lists), "clean tokens are not blocked");
	Check(nickname_cleaner::IsWhitelistedToken("VCS2.ru", lists), "the whitelist matches substrings");
}

void TestStripPunctuation() {
	CheckEq(nickname_cleaner::StripPunctuation("!!hello!!"), "hello", "leading/trailing punctuation removed");
	CheckEq(nickname_cleaner::StripPunctuation("a.b"), "a.b", "inner punctuation kept");
	CheckEq(nickname_cleaner::StripPunctuation("..."), "", "punctuation-only token becomes empty");
	Check(nickname_cleaner::LooksLikeAddress("http://x"), "http:// is an address");
	Check(nickname_cleaner::LooksLikeAddress("WWW.Site"), "www. is an address");
	Check(!nickname_cleaner::LooksLikeAddress("wow"), "ordinary token is not an address");
}

void TestPlaceholderName() {
	// The names that must never reach SetPlayerName: a slot without a name (the original's
	// controller-less "" answer) and s2sdk's "<unknown>" placeholder for a slot without a client.
	Check(nickname_cleaner::IsPlaceholderName(nullptr), "a null name is a placeholder");
	Check(nickname_cleaner::IsPlaceholderName(""), "an empty name is a placeholder");
	Check(nickname_cleaner::IsPlaceholderName("<unknown>"), "s2sdk's clientless placeholder is skipped");
	Check(!nickname_cleaner::IsPlaceholderName("unknown"), "a real name 'unknown' is cleaned normally");
	Check(!nickname_cleaner::IsPlaceholderName("<unknown>x"), "only the exact placeholder is skipped");
	Check(!nickname_cleaner::IsPlaceholderName("Bob"), "an ordinary name is not a placeholder");
}

void TestCleanNickname() {
	nickname_cleaner::NameLists lists;
	lists.LoadListContent("*.ru\n*.com\nvcs2.ru\n", true);
	lists.LoadListContent("vcs2.ru\n", false);

	// The whitelist wins over the blocklist (the original checked it first).
	CheckEq(nickname_cleaner::CleanNickname("play on vcs2.ru now", lists, "Player"), "play on vcs2.ru now",
			"whitelisted token survives a matching blocklist entry");
	// An address token is dropped, the rest is kept.
	CheckEq(nickname_cleaner::CleanNickname("my http://site.com page", lists, "Player"), "my page",
			"http:// token dropped");
	CheckEq(nickname_cleaner::CleanNickname("a site.ru b", lists, "Player"), "a b",
			"a token matching the '*.ru' rule is dropped");
	// Punctuation is stripped *before* the match, so a literal "*.ru" nickname token becomes "ru"
	// and no longer matches the "*.ru" rule - exactly what the original produced.
	CheckEq(nickname_cleaner::CleanNickname("a *.ru b", lists, "Player"), "a ru b",
			"the original's punctuation stripping quirk is preserved");
	CheckEq(nickname_cleaner::CleanNickname("   ", lists, "Player"), "Player", "empty result -> default name");
	CheckEq(nickname_cleaner::CleanNickname("!!site.com!!", lists, "Player"), "Player",
			"punctuation is stripped before the match");
	CheckEq(nickname_cleaner::CleanNickname("OK  Name", lists, "Player"), "OK Name",
			"tokens are re-joined with single spaces");
}

// The shipped production files: the whitelist must protect vcs2.ru while *.ru still blocks other
// .ru tokens (that is the whole point of the live configuration).
void TestProductionLists() {
	bool ok = false;
	const std::string dir = NICKNAME_CLEANER_TEST_DATA_DIR;
	const std::string whitelist = ReadWholeFile(dir + "/whitelist.txt", ok);
	Check(ok, "production whitelist.txt is readable (" + dir + "/whitelist.txt)");
	const std::string blocklist = ReadWholeFile(dir + "/blocklist.txt", ok);
	Check(ok, "production blocklist.txt is readable (" + dir + "/blocklist.txt)");

	nickname_cleaner::NameLists lists;
	lists.LoadListContent(whitelist, false);
	lists.LoadListContent(blocklist, true);
	Check(lists.whitelist.count("vcs2.ru") == 1, "production whitelist contains vcs2.ru");
	Check(lists.blocklist.count("*.ru") == 1, "production blocklist contains *.ru");

	CheckEq(nickname_cleaner::CleanNickname("Игрок vcs2.ru", lists, "E-boy vcs2.ru"), "Игрок vcs2.ru",
			"a production-style nickname with the whitelisted domain is untouched");
	CheckEq(nickname_cleaner::CleanNickname("Купи на mega-shop.xyz", lists, "E-boy vcs2.ru"), "Купи на",
			"a blocked domain is removed from the nickname");
	CheckEq(nickname_cleaner::CleanNickname("zxc.com", lists, "E-boy vcs2.ru"), "E-boy vcs2.ru",
			"a fully filtered nickname becomes DefaultName");
}

} // namespace

int main() {
	std::printf("nickname_cleaner tests (data dir: %s)\n", NICKNAME_CLEANER_TEST_DATA_DIR);
	TestListParsing();
	TestMatching();
	TestStripPunctuation();
	TestPlaceholderName();
	TestCleanNickname();
	TestProductionLists();
	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
