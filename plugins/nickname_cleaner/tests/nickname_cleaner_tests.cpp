// nickname_cleaner_tests — the test binary of the pure-s2sdk nickname_cleaner plugin.
//
// Two halves, neither of which links pisexcompat:
//
//   1. engine-free logic: the whitelist/blocklist tables and CleanNickname() of
//      nickname_cleaner_core, the self-contained KV1 reader (src/kv1.hpp), the settings.ini model
//      and the config path candidates (src/config.hpp), and the list files the package ships
//      (tests/data).
//   2. the whole plugin code path: tests/nickname_cleaner_tests.cpp includes src/plugin.cpp (the
//      advertisement/resetscore technique) and every __s2sdk_* binding slot is a recording fake.
//      Covered: the PITFALLS B1/B2 map gate (no client query before a map or in the window between
//      OnMapEnd and the next OnMapStart), the guarded client wrappers of C2 (slot range,
//      IsClientConnected, IsClientInGame, !IsClientSourceTV, controller pointer/handle, bot skip,
//      the "<unknown>" name placeholder and the SteamID guard), the fail-closed IsFakeClient path
//      and the C1 handle edges (0/-1 rejected before IsValidEntHandle, a negative valid handle
//      accepted), the C12 event hooks and their removal, the update-driven player_spawn countdown
//      array (one pass per slot, no accumulation on repeated spawns, cleared on OnMapEnd), the
//      once-per-second reconcile, and fail-soft behaviour when a binding or a config is missing.
//
// No pisexcompat, no Plugify host, no engine. Run by ./test.sh (build/tests/nickname_cleaner_tests).

#include "../src/plugin.cpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifndef NICKNAME_CLEANER_TEST_DATA_DIR
#define NICKNAME_CLEANER_TEST_DATA_DIR "."
#endif

namespace {

int g_checks = 0;
int g_failures = 0;

void Section(const char* name) {
	std::printf("-- %s\n", name);
}

void Check(bool condition, const std::string& what) {
	++g_checks;
	if (!condition) {
		++g_failures;
		std::printf("FAIL: %s\n", what.c_str());
	}
}

void CheckEqInt(int64_t actual, int64_t expected, const std::string& what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL: %s (expected %lld, got %lld)\n", what.c_str(), static_cast<long long>(expected),
					static_cast<long long>(actual));
	}
}

void CheckEqStr(std::string_view actual, std::string_view expected, const std::string& what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL: %s (expected '%s', got '%s')\n", what.c_str(), std::string(expected).c_str(),
					std::string(actual).c_str());
	}
}

std::string ToStd(const plg::string& value) {
	return std::string(value.data(), value.size());
}

// ==============================================================================================
// the fake Plugify host
// ==============================================================================================

std::vector<std::string> g_logLines;
std::vector<plg::Severity> g_logSeverities;

void FakeLog(std::string_view message, plg::Severity severity, const plg::source_location&) {
	g_logLines.emplace_back(message);
	g_logSeverities.push_back(severity);
}

bool HasLogContaining(std::string_view needle) {
	for (const std::string& line : g_logLines) {
		if (line.find(needle) != std::string::npos) {
			return true;
		}
	}
	return false;
}

bool HasErrorLog() {
	for (const plg::Severity severity : g_logSeverities) {
		if (severity == plg::Severity::Error) {
			return true;
		}
	}
	return false;
}

std::string g_configsDir;

plg::string FakeGetConfigsDir() {
	return plg::string(g_configsDir);
}

// ==============================================================================================
// the fake s2sdk bindings
// ==============================================================================================

constexpr int kSlots = 64;
// Controller handles are unique per slot; the fakes never use a real engine handle.
constexpr int32_t kFirstFakeHandle = 1000;

std::array<bool, kSlots> g_connected{};
std::array<bool, kSlots> g_inGame{};
std::array<bool, kSlots> g_sourceTV{};
std::array<bool, kSlots> g_fakeClient{};
std::array<bool, kSlots> g_controllerPresent{};
std::array<bool, kSlots> g_controllerPointerValid{};
std::array<bool, kSlots> g_controllerHandleValid{};
// The handle PlayerSlotToEntHandle answers per slot; AddHuman sets the usual per-slot value and
// the edge-case test overrides it with 0, -1 and a valid negative handle.
std::array<int32_t, kSlots> g_controllerHandles{};
std::array<std::string, kSlots> g_names{};
std::array<uint64_t, kSlots> g_steamIds{};
// A stable, unique non-null pointer per slot; the validity flags above model the entity system.
std::array<char, kSlots> g_controllerStorage{};

// Every client/entity query increments this counter, so a test can prove that no query left the
// plugin while the map gate was shut (PITFALLS B1/B2).
int g_clientQueryCount = 0;
// Number of IsValidEntHandle calls; tests prove that 0 and -1 are rejected before the entity
// system is asked.
int g_isValidHandleQueries = 0;

void NoteClientQuery() {
	++g_clientQueryCount;
}

struct SetNameCall {
	int slot;
	std::string name;
};

std::vector<SetNameCall> g_setNameCalls;
bool g_applySetNames = true;

bool IsSlotInRange(int32_t slot) {
	return slot >= 0 && slot < kSlots;
}

bool FakeIsClientConnected(int32_t slot) {
	NoteClientQuery();
	return IsSlotInRange(slot) && g_connected[static_cast<std::size_t>(slot)];
}

bool FakeIsClientInGame(int32_t slot) {
	NoteClientQuery();
	return IsSlotInRange(slot) && g_inGame[static_cast<std::size_t>(slot)];
}

bool FakeIsClientSourceTV(int32_t slot) {
	NoteClientQuery();
	return IsSlotInRange(slot) && g_sourceTV[static_cast<std::size_t>(slot)];
}

bool FakeIsFakeClient(int32_t slot) {
	NoteClientQuery();
	return IsSlotInRange(slot) && g_fakeClient[static_cast<std::size_t>(slot)];
}

void* FakePlayerSlotToEntPointer(int32_t slot) {
	NoteClientQuery();
	if (!IsSlotInRange(slot) || !g_controllerPresent[static_cast<std::size_t>(slot)]) {
		return nullptr;
	}
	return static_cast<void*>(&g_controllerStorage[static_cast<std::size_t>(slot)]);
}

int32_t FakePlayerSlotToEntHandle(int32_t slot) {
	NoteClientQuery();
	return IsSlotInRange(slot) ? g_controllerHandles[static_cast<std::size_t>(slot)] : 0;
}

bool FakeIsValidEntPointer(void* entity) {
	NoteClientQuery();
	if (entity == nullptr) {
		return false;
	}
	for (std::size_t i = 0; i < static_cast<std::size_t>(kSlots); ++i) {
		if (entity == static_cast<void*>(&g_controllerStorage[i])) {
			return g_controllerPointerValid[i];
		}
	}
	return false;
}

bool FakeIsValidEntHandle(int32_t handle) {
	NoteClientQuery();
	++g_isValidHandleQueries;
	for (std::size_t i = 0; i < static_cast<std::size_t>(kSlots); ++i) {
		if (handle == g_controllerHandles[i]) {
			return g_controllerHandleValid[i];
		}
	}
	return false;
}

uint64_t FakeGetClientSteamID64(int32_t slot) {
	NoteClientQuery();
	return IsSlotInRange(slot) ? g_steamIds[static_cast<std::size_t>(slot)] : 0;
}

plg::string FakeGetClientName(int32_t slot) {
	NoteClientQuery();
	if (!IsSlotInRange(slot)) {
		return plg::string();
	}
	return plg::string(g_names[static_cast<std::size_t>(slot)]);
}

void FakeSetClientName(int32_t slot, const plg::string& name) {
	NoteClientQuery();
	const std::string text = ToStd(name);
	g_setNameCalls.push_back(SetNameCall{slot, text});
	if (g_applySetNames && IsSlotInRange(slot)) {
		g_names[static_cast<std::size_t>(slot)] = text;
	}
}

// --- event hooks (PITFALLS C12) --------------------------------------------------------------

std::map<std::string, s2sdk::EventCallback> g_eventHooks;
std::vector<std::string> g_eventHookLog;
std::string g_hookFailureName;
int g_eventSlot = -1;
int g_dummyEvent = 0;

s2sdk::EventHookError FakeHookEvent(const plg::string& name, s2sdk::EventCallback callback,
									s2sdk::HookMode mode) {
	const std::string eventName = ToStd(name);
	g_eventHookLog.push_back(std::string("hook:") + eventName + ":" +
							 (mode == s2sdk::HookMode::Pre ? "pre" : "post"));
	if (eventName == g_hookFailureName) {
		return s2sdk::EventHookError::InvalidEvent;
	}
	g_eventHooks[eventName] = callback;
	return s2sdk::EventHookError::Okay;
}

s2sdk::EventHookError FakeUnhookEvent(const plg::string& name, s2sdk::EventCallback /*callback*/,
									  s2sdk::HookMode mode) {
	const std::string eventName = ToStd(name);
	g_eventHookLog.push_back(std::string("unhook:") + eventName + ":" +
							 (mode == s2sdk::HookMode::Pre ? "pre" : "post"));
	g_eventHooks.erase(eventName);
	return s2sdk::EventHookError::Okay;
}

int32_t FakeGetEventPlayerSlot(void* event, const plg::string& key) {
	if (event != static_cast<void*>(&g_dummyEvent)) {
		return -1;
	}
	if (ToStd(key) != "userid") {
		return -1;
	}
	return g_eventSlot;
}

void FireEvent(const std::string& name, int slot) {
	const auto it = g_eventHooks.find(name);
	if (it == g_eventHooks.end() || it->second == nullptr) {
		return;
	}
	g_eventSlot = slot;
	it->second(plg::string(name), static_cast<void*>(&g_dummyEvent), false);
}

// --- map listeners (PITFALLS B1/B2) ----------------------------------------------------------

s2sdk::OnMapStartCallback g_mapStartCallback = nullptr;
s2sdk::OnMapEndCallback g_mapEndCallback = nullptr;
int g_mapStartRegistrations = 0;
int g_mapStartUnregistrations = 0;
int g_mapEndRegistrations = 0;
int g_mapEndUnregistrations = 0;

void FakeOnMapStartRegister(s2sdk::OnMapStartCallback callback) {
	g_mapStartCallback = callback;
	++g_mapStartRegistrations;
}

void FakeOnMapStartUnregister(s2sdk::OnMapStartCallback callback) {
	if (g_mapStartCallback == callback) {
		g_mapStartCallback = nullptr;
	}
	++g_mapStartUnregistrations;
}

void FakeOnMapEndRegister(s2sdk::OnMapEndCallback callback) {
	g_mapEndCallback = callback;
	++g_mapEndRegistrations;
}

void FakeOnMapEndUnregister(s2sdk::OnMapEndCallback callback) {
	if (g_mapEndCallback == callback) {
		g_mapEndCallback = nullptr;
	}
	++g_mapEndUnregistrations;
}

void FireMapStart() {
	if (g_mapStartCallback != nullptr) {
		g_mapStartCallback();
	}
}

void FireMapEnd() {
	if (g_mapEndCallback != nullptr) {
		g_mapEndCallback();
	}
}

// --- the authentication listener -------------------------------------------------------------

s2sdk::OnClientAuthenticatedCallback g_authenticatedCallback = nullptr;
int g_authenticatedRegistrations = 0;
int g_authenticatedUnregistrations = 0;

void FakeOnClientAuthenticatedRegister(s2sdk::OnClientAuthenticatedCallback callback) {
	g_authenticatedCallback = callback;
	++g_authenticatedRegistrations;
}

void FakeOnClientAuthenticatedUnregister(s2sdk::OnClientAuthenticatedCallback callback) {
	if (g_authenticatedCallback == callback) {
		g_authenticatedCallback = nullptr;
	}
	++g_authenticatedUnregistrations;
}

void FireAuthenticated(int slot, uint64_t steamId) {
	if (g_authenticatedCallback != nullptr) {
		g_authenticatedCallback(slot, steamId);
	}
}

// ==============================================================================================
// fake world setup
// ==============================================================================================

void InstallFakes() {
	plg::Log = &FakeLog;
	plg::GetConfigsDir = &FakeGetConfigsDir;

	__s2sdk_OnMapStart_Register = &FakeOnMapStartRegister;
	__s2sdk_OnMapStart_Unregister = &FakeOnMapStartUnregister;
	__s2sdk_OnMapEnd_Register = &FakeOnMapEndRegister;
	__s2sdk_OnMapEnd_Unregister = &FakeOnMapEndUnregister;
	__s2sdk_OnClientAuthenticated_Register = &FakeOnClientAuthenticatedRegister;
	__s2sdk_OnClientAuthenticated_Unregister = &FakeOnClientAuthenticatedUnregister;

	__s2sdk_HookEvent = &FakeHookEvent;
	__s2sdk_UnhookEvent = &FakeUnhookEvent;
	__s2sdk_GetEventPlayerSlot = &FakeGetEventPlayerSlot;

	__s2sdk_IsClientConnected = &FakeIsClientConnected;
	__s2sdk_IsClientInGame = &FakeIsClientInGame;
	__s2sdk_IsClientSourceTV = &FakeIsClientSourceTV;
	__s2sdk_IsFakeClient = &FakeIsFakeClient;
	__s2sdk_PlayerSlotToEntPointer = &FakePlayerSlotToEntPointer;
	__s2sdk_PlayerSlotToEntHandle = &FakePlayerSlotToEntHandle;
	__s2sdk_IsValidEntPointer = &FakeIsValidEntPointer;
	__s2sdk_IsValidEntHandle = &FakeIsValidEntHandle;
	__s2sdk_GetClientSteamID64 = &FakeGetClientSteamID64;
	__s2sdk_GetClientName = &FakeGetClientName;
	__s2sdk_SetClientName = &FakeSetClientName;
}

void ResetFakeWorld() {
	g_connected.fill(false);
	g_inGame.fill(true);
	g_sourceTV.fill(false);
	g_fakeClient.fill(false);
	g_controllerPresent.fill(false);
	g_controllerPointerValid.fill(false);
	g_controllerHandleValid.fill(false);
	for (std::size_t i = 0; i < static_cast<std::size_t>(kSlots); ++i) {
		g_controllerHandles[i] = kFirstFakeHandle + static_cast<int32_t>(i);
	}
	g_names.fill("");
	g_steamIds.fill(0);

	g_clientQueryCount = 0;
	g_isValidHandleQueries = 0;
	g_setNameCalls.clear();
	g_applySetNames = true;

	g_eventHooks.clear();
	g_eventHookLog.clear();
	g_hookFailureName.clear();
	g_eventSlot = -1;

	g_mapStartCallback = nullptr;
	g_mapEndCallback = nullptr;
	g_mapStartRegistrations = 0;
	g_mapStartUnregistrations = 0;
	g_mapEndRegistrations = 0;
	g_mapEndUnregistrations = 0;

	g_authenticatedCallback = nullptr;
	g_authenticatedRegistrations = 0;
	g_authenticatedUnregistrations = 0;
}

void AddHuman(int slot, const std::string& name, uint64_t steamId = 0) {
	if (!IsSlotInRange(slot)) {
		return;
	}
	const std::size_t index = static_cast<std::size_t>(slot);
	g_connected[index] = true;
	g_inGame[index] = true;
	g_sourceTV[index] = false;
	g_fakeClient[index] = false;
	g_controllerPresent[index] = true;
	g_controllerPointerValid[index] = true;
	g_controllerHandleValid[index] = true;
	g_controllerHandles[index] = kFirstFakeHandle + static_cast<int32_t>(slot);
	g_names[index] = name;
	g_steamIds[index] = steamId != 0 ? steamId : 76561198000000000ULL + static_cast<uint64_t>(slot);
}

void AddBot(int slot, const std::string& name) {
	AddHuman(slot, name);
	g_fakeClient[static_cast<std::size_t>(slot)] = true;
}

int SetNameCallsFor(int slot) {
	int count = 0;
	for (const SetNameCall& call : g_setNameCalls) {
		if (call.slot == slot) {
			++count;
		}
	}
	return count;
}

std::string LastSetName() {
	return g_setNameCalls.empty() ? std::string() : g_setNameCalls.back().name;
}

// ==============================================================================================
// the temp config directory
// ==============================================================================================

std::filesystem::path TempRoot() {
	return std::filesystem::temp_directory_path() / "nickname_cleaner_tests_work";
}

std::string PathString(const std::filesystem::path& path) {
	return path.string();
}

bool WriteFile(const std::filesystem::path& path, std::string_view content) {
	std::error_code ec;
	std::filesystem::create_directories(path.parent_path(), ec);
	std::ofstream file(path, std::ios::binary | std::ios::trunc);
	file.write(content.data(), static_cast<std::streamsize>(content.size()));
	const bool ok = file.good();
	file.close();
	return ok;
}

bool RemoveFile(const std::filesystem::path& path) {
	std::error_code ec;
	return std::filesystem::remove(path, ec);
}

std::string SettingsText(std::string_view defaultName) {
	return std::string("\"NicknameCleaner\"\n{\n    \"DefaultName\" \"") + std::string(defaultName) +
		   "\" // test value\n}\n";
}

constexpr std::string_view kTestDefaultName = "E-boy example.com";
constexpr std::string_view kTestWhitelist = "example.com\n";
constexpr std::string_view kTestBlocklist = "*.xyz\nbadword\n";

std::filesystem::path PluginConfigDir() {
	return TempRoot() / "nickname_cleaner";
}

void WriteConfigSet(std::string_view defaultName) {
	Check(WriteFile(PluginConfigDir() / "settings.ini", SettingsText(defaultName)),
		  "write settings.ini");
	Check(WriteFile(PluginConfigDir() / "whitelist.txt", kTestWhitelist), "write whitelist.txt");
	Check(WriteFile(PluginConfigDir() / "blocklist.txt", kTestBlocklist), "write blocklist.txt");
}

void ResetPluginGlobals() {
	g_mapReady = false;
	g_mapStartSeen = false;
	g_mapEndSeen = false;
	g_mapListenersRegistered = false;
	g_roundPrestartHooked = false;
	g_playerSpawnHooked = false;
	g_authenticatedListenerRegistered = false;
}

void ResetAll() {
	// Tears down whatever the previous test left running, with the fakes still installed.
	try {
		g_nicknameCleanerPlugin.OnPluginEnd();
	} catch (...) {
	}
	ResetPluginGlobals();
	ResetFakeWorld();
	InstallFakes();
	g_logLines.clear();
	g_logSeverities.clear();

	std::error_code ec;
	std::filesystem::remove_all(TempRoot(), ec);
	std::filesystem::create_directories(TempRoot(), ec);
	g_configsDir = PathString(TempRoot());
}

void StartPlugin() {
	ResetAll();
	WriteConfigSet(kTestDefaultName);
	Check(static_cast<bool>(g_nicknameCleanerPlugin.OnPluginStart()), "OnPluginStart never returns Failed");
}

void StartPluginWithoutConfigs() {
	ResetAll();
	Check(static_cast<bool>(g_nicknameCleanerPlugin.OnPluginStart()), "OnPluginStart succeeds without configs");
}

void StartPluginWithSettings(std::string_view settingsText) {
	ResetAll();
	Check(WriteFile(PluginConfigDir() / "settings.ini", settingsText), "write custom settings.ini");
	Check(WriteFile(PluginConfigDir() / "whitelist.txt", kTestWhitelist), "write whitelist.txt");
	Check(WriteFile(PluginConfigDir() / "blocklist.txt", kTestBlocklist), "write blocklist.txt");
	Check(static_cast<bool>(g_nicknameCleanerPlugin.OnPluginStart()), "OnPluginStart never returns Failed");
}

std::string TestDataPath(std::string_view relative) {
	const std::filesystem::path configured =
			std::filesystem::path(NICKNAME_CLEANER_TEST_DATA_DIR) / relative;
	if (std::filesystem::exists(configured)) {
		return PathString(configured);
	}
	// Relative to build/tests (test.sh) and to the repository root (a manual invocation).
	const std::filesystem::path bases[] = {
			std::filesystem::current_path() / "../../plugins/nickname_cleaner/tests/data",
			std::filesystem::current_path() / "plugins/nickname_cleaner/tests/data",
	};
	for (const std::filesystem::path& base : bases) {
		const std::filesystem::path candidate = base / relative;
		if (std::filesystem::exists(candidate)) {
			return PathString(candidate.lexically_normal());
		}
	}
	return PathString(configured);
}

std::optional<std::string> ReadWholeFile(const std::string& path) {
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		return std::nullopt;
	}
	std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	return content;
}

// ==============================================================================================
// 1. the engine-independent core
// ==============================================================================================

void TestListParsing() {
	Section("core: list parsing");
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
	Section("core: matching");
	nickname_cleaner::NameLists lists;
	lists.LoadListContent("*.ru\nbadword\n", true);
	lists.LoadListContent("example.com\n", false);

	Check(nickname_cleaner::IsBlockedToken("site.ru", lists), "*.ru matches the end of a token");
	Check(nickname_cleaner::IsBlockedToken("SITE.RU", lists), "suffix match is case-insensitive");
	Check(!nickname_cleaner::IsBlockedToken("rusite", lists), "*.ru does not match a prefix");
	Check(nickname_cleaner::IsBlockedToken("aBADWORDb", lists), "plain entries match as substrings");
	Check(!nickname_cleaner::IsBlockedToken("clean", lists), "clean tokens are not blocked");
	Check(nickname_cleaner::IsWhitelistedToken("EXAMPLE.com", lists),
		  "the whitelist matches substrings");
}

void TestStripPunctuation() {
	Section("core: punctuation and addresses");
	CheckEqStr(nickname_cleaner::StripPunctuation("!!hello!!"), "hello",
			   "leading/trailing punctuation removed");
	CheckEqStr(nickname_cleaner::StripPunctuation("a.b"), "a.b", "inner punctuation kept");
	CheckEqStr(nickname_cleaner::StripPunctuation("..."), "",
			   "punctuation-only token becomes empty");
	Check(nickname_cleaner::LooksLikeAddress("http://x"), "http:// is an address");
	Check(nickname_cleaner::LooksLikeAddress("WWW.Site"), "www. is an address");
	Check(!nickname_cleaner::LooksLikeAddress("wow"), "ordinary token is not an address");
}

void TestPlaceholderName() {
	Section("core: placeholder names");
	// The names that must never reach SetPlayerName: a slot without a name (the original's
	// controller-less "" answer) and s2sdk's "<unknown>" placeholder for a slot without a client.
	Check(nickname_cleaner::IsPlaceholderName(nullptr), "a null name is a placeholder");
	Check(nickname_cleaner::IsPlaceholderName(""), "an empty name is a placeholder");
	Check(nickname_cleaner::IsPlaceholderName("<unknown>"),
		  "s2sdk's clientless placeholder is skipped");
	Check(!nickname_cleaner::IsPlaceholderName("unknown"),
		  "a real name 'unknown' is cleaned normally");
	Check(!nickname_cleaner::IsPlaceholderName("<unknown>x"),
		  "only the exact placeholder is skipped");
	Check(!nickname_cleaner::IsPlaceholderName("Bob"), "an ordinary name is not a placeholder");
}

void TestCleanNickname() {
	Section("core: CleanNickname");
	nickname_cleaner::NameLists lists;
	lists.LoadListContent("*.ru\n*.com\nexample.com\n", true);
	lists.LoadListContent("example.com\n", false);

	// The whitelist wins over the blocklist (the original checked it first).
	CheckEqStr(nickname_cleaner::CleanNickname("play on example.com now", lists, "Player"),
			   "play on example.com now",
			   "whitelisted token survives a matching blocklist entry");
	// An address token is dropped, the rest is kept.
	CheckEqStr(nickname_cleaner::CleanNickname("my http://site.com page", lists, "Player"),
			   "my page", "http:// token dropped");
	CheckEqStr(nickname_cleaner::CleanNickname("a site.ru b", lists, "Player"), "a b",
			   "a token matching the '*.ru' rule is dropped");
	// Punctuation is stripped *before* the match, so a literal "*.ru" nickname token becomes "ru"
	// and no longer matches the "*.ru" rule - exactly what the original produced.
	CheckEqStr(nickname_cleaner::CleanNickname("a *.ru b", lists, "Player"), "a ru b",
			   "the original's punctuation stripping quirk is preserved");
	CheckEqStr(nickname_cleaner::CleanNickname("   ", lists, "Player"), "Player",
			   "empty result -> default name");
	CheckEqStr(nickname_cleaner::CleanNickname("!!site.com!!", lists, "Player"), "Player",
			   "punctuation is stripped before the match");
	CheckEqStr(nickname_cleaner::CleanNickname("OK  Name", lists, "Player"), "OK Name",
			   "tokens are re-joined with single spaces");
}

// The shipped list files: the whitelist must protect example.com while *.ru still blocks other
// .ru tokens (that is the whole point of the list pair).
void TestProductionLists() {
	Section("core: shipped list files");
	const std::string whitelistPath = TestDataPath("whitelist.txt");
	const std::string blocklistPath = TestDataPath("blocklist.txt");
	const std::optional<std::string> whitelist = ReadWholeFile(whitelistPath);
	const std::optional<std::string> blocklist = ReadWholeFile(blocklistPath);
	Check(whitelist.has_value(), "production whitelist.txt is readable (" + whitelistPath + ")");
	Check(blocklist.has_value(), "production blocklist.txt is readable (" + blocklistPath + ")");
	if (!whitelist || !blocklist) {
		return;
	}

	nickname_cleaner::NameLists lists;
	lists.LoadListContent(*whitelist, false);
	lists.LoadListContent(*blocklist, true);
	Check(lists.whitelist.count("example.com") == 1, "production whitelist contains example.com");
	Check(lists.blocklist.count("*.ru") == 1, "production blocklist contains *.ru");

	CheckEqStr(nickname_cleaner::CleanNickname("Игрок example.com", lists, "E-boy example.com"),
			   "Игрок example.com",
			   "a production-style nickname with the whitelisted domain is untouched");
	CheckEqStr(nickname_cleaner::CleanNickname("Купи на mega-shop.xyz", lists, "E-boy example.com"),
			   "Купи на", "a blocked domain is removed from the nickname");
	CheckEqStr(nickname_cleaner::CleanNickname("zxc.com", lists, "E-boy example.com"),
			   "E-boy example.com", "a fully filtered nickname becomes DefaultName");
}

// ==============================================================================================
// 2. the KV1 reader and the settings model
// ==============================================================================================

void TestKv1Reader() {
	Section("KV1 reader: syntax");
	const std::optional<nickname_cleaner::KvNode> root = nickname_cleaner::ParseKv1(R"(
// a line comment before the root
"root"
{
	/* a block
	   comment */
	"quoted"  "value"   // a trailing comment
	unquoted  bare
	"escapes" "a\tb\nc\"d\\e\q"
	"empty"   ""
	"block"
	{
		"nested" "inner"
		"block" { "deep" "42" }
	}
	"both" "value" { "child" "x" }
	"dup" "1"
	"dup" "2"
}
)");
	Check(root.has_value(), "a quoted root block parses");
	if (!root) {
		return;
	}
	CheckEqStr(root->name, "root", "the single top-level block becomes the file root");
	CheckEqStr(root->GetString("quoted"), "value", "quoted value");
	CheckEqStr(root->GetString("unquoted"), "bare", "unquoted value");
	CheckEqStr(root->GetString("escapes"), std::string("a\tb\nc\"d\\e\\q"),
			   "escapes \\t \\n \\\" \\\\ and an unknown escape");
	CheckEqStr(root->GetString("empty"), "", "an empty quoted value");
	CheckEqStr(root->GetString("DUP"), "1", "duplicate keys keep the first value");
	CheckEqStr(root->GetString("missing", "fallback"), "fallback", "missing key -> default");

	const nickname_cleaner::KvNode* block = root->Find("block");
	Check(block != nullptr, "nested block found");
	if (block != nullptr) {
		CheckEqStr(block->GetString("nested"), "inner", "nested value");
		const nickname_cleaner::KvNode* deep = block->Find("block");
		Check(deep != nullptr && deep->GetString("deep") == "42", "doubly nested value");
	}
	const nickname_cleaner::KvNode* both = root->Find("both");
	Check(both != nullptr && both->hasValue && both->GetString("child") == "x",
		  "a key may carry both a value and a block");
	Check(root->Find("QUOTED") == root->Find("quoted"), "lookup is case-insensitive");
	Check(root->Find("") == nullptr, "an empty key never matches");

	// A leading UTF-8 BOM.
	const std::optional<nickname_cleaner::KvNode> bom =
			nickname_cleaner::ParseKv1("\xEF\xBB\xBF\"Root\" { \"k\" \"v\" }");
	Check(bom.has_value() && bom->GetString("k") == "v", "a UTF-8 BOM is skipped");

	// Directives are skipped, not included.
	const std::optional<nickname_cleaner::KvNode> directives = nickname_cleaner::ParseKv1(
			"#base \"other.ini\"\n#define X 1\n\"Root\" { \"k\" \"v\" }\n");
	Check(directives.has_value() && directives->GetString("k") == "v", "# directives are skipped");

	// Multiple top-level keys stay a flat root.
	const std::optional<nickname_cleaner::KvNode> flat =
			nickname_cleaner::ParseKv1("\"a\" \"1\"\n\"b\" \"2\"\n");
	Check(flat.has_value() && flat->GetString("a") == "1" && flat->GetString("b") == "2",
		  "multiple top-level keys are a flat root");

	// An empty file is a valid, empty tree; malformed input is rejected as a whole.
	const std::optional<nickname_cleaner::KvNode> empty = nickname_cleaner::ParseKv1("");
	Check(empty.has_value() && empty->children.empty(), "an empty file parses to an empty root");
	Check(!nickname_cleaner::ParseKv1("\"root\" { \"key\" \"value\"").has_value(),
		  "a missing '}' is malformed");
	Check(!nickname_cleaner::ParseKv1("\"key\" \"value\" }").has_value(),
		  "a stray top-level '}' is malformed");
	Check(!nickname_cleaner::ParseKv1("\"root\" { \"key\" \"unterminated").has_value(),
		  "an unterminated string is malformed");
}

void TestKv1Settings() {
	Section("KV1 reader: settings.ini model");
	const std::optional<nickname_cleaner::KvNode> root =
			nickname_cleaner::ParseKv1(SettingsText("E-boy example.com"));
	Check(root.has_value(), "the shipped settings.ini shape parses");
	if (root) {
		CheckEqStr(nickname_cleaner::DefaultNameFromKv(*root), "E-boy example.com",
				   "DefaultName is read from the NicknameCleaner block");
	}

	const std::optional<nickname_cleaner::KvNode> lower = nickname_cleaner::ParseKv1(
			"\"nicknamecleaner\" { \"defaultname\" \"Lower\" }");
	Check(lower.has_value() && nickname_cleaner::DefaultNameFromKv(*lower) == "Lower",
		  "section and key names are case-insensitive");

	const std::optional<nickname_cleaner::KvNode> flat =
			nickname_cleaner::ParseKv1("\"DefaultName\" \"Flat\"");
	Check(flat.has_value() && nickname_cleaner::DefaultNameFromKv(*flat) == "Flat",
		  "a file without the section still exposes DefaultName");

	const std::optional<nickname_cleaner::KvNode> emptyValue =
			nickname_cleaner::ParseKv1("\"NicknameCleaner\" { \"DefaultName\" \"\" }");
	Check(emptyValue.has_value() &&
				  nickname_cleaner::DefaultNameFromKv(*emptyValue) ==
						  nickname_cleaner::kDefaultDefaultName,
		  "an empty DefaultName keeps the built-in default");

	const std::optional<nickname_cleaner::KvNode> missing =
			nickname_cleaner::ParseKv1("\"NicknameCleaner\" { \"Other\" \"x\" }");
	Check(missing.has_value() &&
				  nickname_cleaner::DefaultNameFromKv(*missing) ==
						  nickname_cleaner::kDefaultDefaultName,
		  "a missing DefaultName keeps the built-in default");

	// The settings.ini the package ships (tests/data/nickname_cleaner/settings.ini).
	const std::string shippedPath = TestDataPath("nickname_cleaner/settings.ini");
	const std::optional<std::string> shippedText = ReadWholeFile(shippedPath);
	Check(shippedText.has_value(), "the shipped settings.ini is readable (" + shippedPath + ")");
	if (shippedText) {
		const std::optional<nickname_cleaner::KvNode> shipped =
				nickname_cleaner::ParseKv1(*shippedText);
		Check(shipped.has_value(), "the shipped settings.ini parses");
		if (shipped) {
			CheckEqStr(nickname_cleaner::DefaultNameFromKv(*shipped), "E-boy example.com",
					   "the shipped settings.ini yields DefaultName");
		}
	}
}

// ==============================================================================================
// 3. config locations and the file reader
// ==============================================================================================

void TestConfigPaths() {
	Section("config: candidates and file reader");
	ResetAll();

	CheckEqStr(nickname_cleaner::ConfigsDir(), g_configsDir, "ConfigsDir answers the host value");
	const std::vector<std::string> candidates =
			nickname_cleaner::ConfigCandidates(nickname_cleaner::kSettingsFile);
	CheckEqInt(static_cast<int64_t>(candidates.size()), 2, "two config candidates");
	if (candidates.size() == 2) {
		CheckEqStr(candidates[0], g_configsDir + "/nickname_cleaner/settings.ini",
				   "the packaged location comes first");
		CheckEqStr(candidates[1], g_configsDir + "/settings.ini", "the configs root comes second");
	}
	CheckEqStr(nickname_cleaner::PrimaryConfigPath(nickname_cleaner::kSettingsFile),
			   g_configsDir + "/nickname_cleaner/settings.ini", "the primary path is the packaged one");

	std::string content;
	std::string usedPath;
	Check(!nickname_cleaner::ReadConfigFile(nickname_cleaner::kSettingsFile, content, usedPath),
		  "a missing file is not read");

	Check(WriteFile(PluginConfigDir() / "settings.ini", "primary"), "write the primary settings.ini");
	Check(nickname_cleaner::ReadConfigFile(nickname_cleaner::kSettingsFile, content, usedPath),
		  "the primary file is read");
	CheckEqStr(content, "primary", "the primary content wins");
	CheckEqStr(usedPath, g_configsDir + "/nickname_cleaner/settings.ini", "the used path is primary");

	Check(WriteFile(TempRoot() / "settings.ini", "fallback"), "write the fallback settings.ini");
	Check(nickname_cleaner::ReadConfigFile(nickname_cleaner::kSettingsFile, content, usedPath),
		  "a present primary is still preferred");
	CheckEqStr(content, "primary", "the fallback does not override the primary");

	Check(RemoveFile(PluginConfigDir() / "settings.ini"), "remove the primary settings.ini");
	Check(nickname_cleaner::ReadConfigFile(nickname_cleaner::kSettingsFile, content, usedPath),
		  "the fallback file is read when the primary is gone");
	CheckEqStr(content, "fallback", "the fallback content is used");
	CheckEqStr(usedPath, g_configsDir + "/settings.ini", "the used path is the fallback");

	// A malformed primary is returned as-is: ReadConfigFile reports "file exists", ParseKv1 then
	// rejects it, and the caller keeps its default instead of silently using the fallback.
	Check(WriteFile(PluginConfigDir() / "settings.ini", "\"NicknameCleaner\" { \"DefaultName\""),
		  "write a malformed primary");
	Check(nickname_cleaner::ReadConfigFile(nickname_cleaner::kSettingsFile, content, usedPath),
		  "the malformed primary is still the file that is read");
	CheckEqStr(usedPath, g_configsDir + "/nickname_cleaner/settings.ini",
			   "the malformed primary is not replaced by the fallback");
	Check(!nickname_cleaner::ParseKv1(content).has_value(), "the malformed primary does not parse");

	// No configs directory at all.
	plg::GetConfigsDir = nullptr;
	CheckEqStr(nickname_cleaner::ConfigsDir(), "", "no host configs dir -> empty path");
	Check(nickname_cleaner::ConfigCandidates(nickname_cleaner::kSettingsFile).empty(),
		  "no host configs dir -> no candidates");
	CheckEqStr(nickname_cleaner::PrimaryConfigPath(nickname_cleaner::kSettingsFile), "settings.ini",
			   "the log path falls back to the bare file name");
	Check(!nickname_cleaner::ReadConfigFile(nickname_cleaner::kSettingsFile, content, usedPath),
		  "no host configs dir -> no read");

	InstallFakes();
}

// ==============================================================================================
// 4. the guarded client wrappers (PITFALLS C2)
// ==============================================================================================

void TestClientGuards() {
	Section("guards: slot/controller/name");
	StartPlugin();
	FireMapStart();
	Check(g_mapReady, "map start opens the gate");

	AddHuman(1, "mega-shop.xyz");
	Check(g_nicknameCleanerPlugin.ApplyCleanName(1, 0), "a dirty name is rewritten");
	CheckEqInt(SetNameCallsFor(1), 1, "one name write for the dirty slot");
	CheckEqStr(LastSetName(), std::string(kTestDefaultName),
			   "a fully filtered name becomes DefaultName");
	Check(!g_nicknameCleanerPlugin.ApplyCleanName(1, 0), "an already clean name is not rewritten");
	CheckEqInt(SetNameCallsFor(1), 1, "the second pass wrote nothing");

	AddHuman(2, "badword player");
	Check(g_nicknameCleanerPlugin.ApplyCleanName(2, 0), "a blocked word is removed");
	CheckEqStr(LastSetName(), "player", "the remaining token survives");
	Check(!g_nicknameCleanerPlugin.ApplyCleanName(2, 0), "the cleaned name is stable");

	AddHuman(3, "example.com shop.xyz"); // whitelisted token kept, the .xyz token dropped
	Check(g_nicknameCleanerPlugin.ApplyCleanName(3, 0), "the whitelist protects example.com while .xyz is blocked");
	CheckEqStr(LastSetName(), "example.com", "the whitelisted token stays, the blocked one goes");

	// Out-of-range slots never reach a binding.
	const int queriesBeforeRange = g_clientQueryCount;
	Check(!g_nicknameCleanerPlugin.ApplyCleanName(-1, 0), "slot -1 is rejected");
	Check(!g_nicknameCleanerPlugin.ApplyCleanName(kSlots, 0), "slot 64 is rejected");
	CheckEqInt(g_clientQueryCount - queriesBeforeRange, 0, "out-of-range slots make no s2sdk call");

	// Disconnected / not-in-game / SourceTV slots.
	AddHuman(4, "badword");
	g_connected[4] = false;
	Check(!g_nicknameCleanerPlugin.ApplyCleanName(4, 0), "a disconnected slot is skipped");

	AddHuman(5, "badword");
	g_inGame[5] = false;
	Check(!g_nicknameCleanerPlugin.ApplyCleanName(5, 0), "a client not in game is skipped");

	AddHuman(6, "badword");
	g_sourceTV[6] = true;
	Check(!g_nicknameCleanerPlugin.ApplyCleanName(6, 0), "SourceTV is skipped");

	// Controller entity checks.
	AddHuman(7, "badword");
	g_controllerPresent[7] = false;
	Check(!g_nicknameCleanerPlugin.ApplyCleanName(7, 0), "a slot without a controller entity is skipped");

	AddHuman(8, "badword");
	g_controllerPointerValid[8] = false;
	Check(!g_nicknameCleanerPlugin.ApplyCleanName(8, 0), "an invalid controller pointer is skipped");

	AddHuman(9, "badword");
	g_controllerHandleValid[9] = false;
	Check(!g_nicknameCleanerPlugin.ApplyCleanName(9, 0), "an invalid controller handle is skipped");

	// Bot.
	AddBot(10, "badword bot");
	Check(!g_nicknameCleanerPlugin.ApplyCleanName(10, 0), "a bot is skipped");
	CheckEqInt(SetNameCallsFor(10), 0, "no name write for a bot");

	// s2sdk's "<unknown>" placeholder and an empty name.
	AddHuman(11, "<unknown>");
	Check(!g_nicknameCleanerPlugin.ApplyCleanName(11, 0), "the <unknown> placeholder is treated as no name");
	AddHuman(12, "");
	Check(!g_nicknameCleanerPlugin.ApplyCleanName(12, 0), "an empty name is treated as no name");

	// SteamID guard: the module only writes the slot that still has the authenticated client.
	AddHuman(13, "badword", 111);
	Check(!g_nicknameCleanerPlugin.ApplyCleanName(13, 999), "a changed slot occupant is rejected");
	Check(g_nicknameCleanerPlugin.ApplyCleanName(13, 111), "the matching SteamID is accepted");
	AddHuman(14, "badword");
	g_steamIds[14] = 0; // s2sdk answers 0 for a slot that never authenticated
	Check(g_nicknameCleanerPlugin.ApplyCleanName(14, 999), "0 means 'any slot occupant'");

	// Missing bindings fail soft.
	AddHuman(15, "badword");
	__s2sdk_SetClientName = nullptr;
	Check(!g_nicknameCleanerPlugin.ApplyCleanName(15, 0), "without the SetClientName binding nothing is rewritten");
	__s2sdk_SetClientName = &FakeSetClientName;
	Check(g_nicknameCleanerPlugin.ApplyCleanName(15, 0), "the binding restored, the name is rewritten");

	AddHuman(16, "badword");
	__s2sdk_GetClientName = nullptr;
	Check(!g_nicknameCleanerPlugin.ApplyCleanName(16, 0), "without GetClientName nothing is rewritten");
	__s2sdk_GetClientName = &FakeGetClientName;

	AddHuman(17, "badword");
	__s2sdk_IsClientConnected = nullptr;
	Check(!g_nicknameCleanerPlugin.ApplyCleanName(17, 0), "without IsClientConnected nothing is rewritten");
	__s2sdk_IsClientConnected = &FakeIsClientConnected;

	AddHuman(18, "badword");
	__s2sdk_GetClientSteamID64 = nullptr;
	Check(g_nicknameCleanerPlugin.ApplyCleanName(18, 123), "without GetClientSteamID64 the SteamID guard is inert");
	__s2sdk_GetClientSteamID64 = &FakeGetClientSteamID64;

	AddHuman(19, "badword");
	__s2sdk_PlayerSlotToEntPointer = nullptr;
	Check(!g_nicknameCleanerPlugin.ApplyCleanName(19, 0), "without the controller pointer binding nothing is written");
	__s2sdk_PlayerSlotToEntPointer = &FakePlayerSlotToEntPointer;
}

// ==============================================================================================
// 4b. fail-closed bot detection and the entity-handle edges (PITFALLS C1)
// ==============================================================================================

void TestBotAndHandleEdges() {
	Section("guards: fail-closed IsFakeClient and controller handles");
	StartPlugin();
	FireMapStart();

	// Baseline with the binding present: a human is cleaned, a bot is skipped.
	AddHuman(1, "badword human");
	Check(g_nicknameCleanerPlugin.ApplyCleanName(1, 0), "a human is cleaned when IsFakeClient works");
	CheckEqInt(SetNameCallsFor(1), 1, "one write for the human");
	AddBot(2, "badword bot");
	Check(!g_nicknameCleanerPlugin.ApplyCleanName(2, 0), "a bot is skipped when IsFakeClient works");
	CheckEqInt(SetNameCallsFor(2), 0, "no write for the bot");

	// Without the binding the plugin cannot tell a bot from a human (both can answer SteamID 0),
	// so it must fail closed and write nothing at all.
	AddHuman(3, "badword human");
	AddBot(4, "badword bot");
	__s2sdk_IsFakeClient = nullptr;
	Check(!g_nicknameCleanerPlugin.ApplyCleanName(3, 0),
		  "without IsFakeClient a human slot is left untouched (fail-closed)");
	Check(!g_nicknameCleanerPlugin.ApplyCleanName(4, 0),
		  "without IsFakeClient a bot slot is left untouched (fail-closed)");
	CheckEqInt(SetNameCallsFor(3), 0, "no write for the human slot without IsFakeClient");
	CheckEqInt(SetNameCallsFor(4), 0, "no write for the bot slot without IsFakeClient");

	// The round pass goes through the same guard.
	FireEvent("round_prestart", 0);
	CheckEqInt(SetNameCallsFor(3), 0, "round_prestart also fails closed without IsFakeClient");
	CheckEqInt(SetNameCallsFor(4), 0, "round_prestart leaves the bot alone without IsFakeClient");

	// Restoring the binding keeps the normal behaviour.
	__s2sdk_IsFakeClient = &FakeIsFakeClient;
	Check(g_nicknameCleanerPlugin.ApplyCleanName(3, 0), "the human is cleaned after the binding returns");
	Check(!g_nicknameCleanerPlugin.ApplyCleanName(4, 0), "the bot stays skipped after the binding returns");
	CheckEqInt(SetNameCallsFor(4), 0, "the restored binding still skips the bot");

	// PITFALLS C1: handles 0 and -1 are never a controller and are rejected before
	// IsValidEntHandle; a negative handle can still be a valid controller (serial sets the sign).
	AddHuman(5, "badword");
	g_controllerHandles[5] = 0;
	g_controllerHandleValid[5] = true; // prove the early rejection wins over a "valid" fake
	int queriesBefore = g_isValidHandleQueries;
	Check(!g_nicknameCleanerPlugin.ApplyCleanName(5, 0), "controller handle 0 is rejected");
	CheckEqInt(g_isValidHandleQueries - queriesBefore, 0,
			   "handle 0 never reaches IsValidEntHandle");
	CheckEqInt(SetNameCallsFor(5), 0, "no write for handle 0");

	AddHuman(6, "badword");
	g_controllerHandles[6] = -1;
	g_controllerHandleValid[6] = true;
	queriesBefore = g_isValidHandleQueries;
	Check(!g_nicknameCleanerPlugin.ApplyCleanName(6, 0), "controller handle -1 is rejected");
	CheckEqInt(g_isValidHandleQueries - queriesBefore, 0,
			   "handle -1 never reaches IsValidEntHandle");
	CheckEqInt(SetNameCallsFor(6), 0, "no write for handle -1");

	AddHuman(7, "badword");
	g_controllerHandles[7] = -54321; // negative, but a valid controller handle (C1)
	g_controllerHandleValid[7] = true;
	queriesBefore = g_isValidHandleQueries;
	Check(g_nicknameCleanerPlugin.ApplyCleanName(7, 0),
		  "a negative but valid controller handle is accepted");
	Check(g_isValidHandleQueries > queriesBefore,
		  "the negative valid handle reaches IsValidEntHandle (each guard pass validates it)");
	CheckEqInt(SetNameCallsFor(7), 1, "the name of the negative-handle slot is written");
	CheckEqStr(LastSetName(), std::string(kTestDefaultName), "the negative-handle slot got the default name");
}

// ==============================================================================================
// 5. the map gate (PITFALLS B1/B2)
// ==============================================================================================

void TestMapGate() {
	Section("map gate: no client query outside a live map");
	ResetAll();
	WriteConfigSet(kTestDefaultName);
	AddHuman(1, "badword");

	const int queriesBeforeStart = g_clientQueryCount;
	Check(static_cast<bool>(g_nicknameCleanerPlugin.OnPluginStart()), "OnPluginStart never returns Failed");
	CheckEqInt(g_clientQueryCount - queriesBeforeStart, 0, "OnPluginStart makes no client query");
	Check(g_mapListenersRegistered, "the map listeners are registered");
	Check(!g_mapReady, "the gate starts shut");
	CheckEqInt(g_mapStartRegistrations, 1, "OnMapStart registered once");
	CheckEqInt(g_mapEndRegistrations, 1, "OnMapEnd registered once");

	// Events and a direct clean pass before a map is ready are no-ops.
	const int queriesBeforeGate = g_clientQueryCount;
	FireEvent("round_prestart", 0);
	FireEvent("player_spawn", 1);
	Check(!g_nicknameCleanerPlugin.ApplyCleanName(1, 0), "no clean pass while the gate is shut");
	g_nicknameCleanerPlugin.TickSpawnTimers(5.0f);
	CheckEqInt(g_clientQueryCount - queriesBeforeGate, 0, "no client query while the gate is shut");
	CheckEqInt(SetNameCallsFor(1), 0, "nothing written while the gate is shut");

	// A plugin loaded into a running map: the first update is the proof of a live map.
	g_nicknameCleanerPlugin.OnPluginUpdate(std::chrono::milliseconds(16));
	Check(g_mapReady, "the first update latches a running map");
	Check(g_nicknameCleanerPlugin.ApplyCleanName(1, 0), "after the latch the name is cleaned");

	// OnMapEnd closes the gate and only state may change there.
	g_setNameCalls.clear();
	const int queriesBeforeEnd = g_clientQueryCount;
	FireMapEnd();
	Check(!g_mapReady, "OnMapEnd closes the gate");
	Check(!g_nicknameCleanerPlugin.ApplyCleanName(2, 0), "no clean pass in the changelevel window");
	g_nicknameCleanerPlugin.TickSpawnTimers(5.0f);
	FireEvent("round_prestart", 0);
	FireEvent("player_spawn", 2);
	g_nicknameCleanerPlugin.OnPluginUpdate(std::chrono::milliseconds(5000));
	CheckEqInt(g_clientQueryCount - queriesBeforeEnd, 0,
			   "no client query between OnMapEnd and the next OnMapStart");
	CheckEqInt(static_cast<int64_t>(g_setNameCalls.size()), 0, "nothing written after OnMapEnd");

	// The host keeps pumping updates through a changelevel: only a new OnMapStart re-opens the gate.
	AddHuman(2, "badword");
	FireMapStart();
	Check(g_mapReady, "OnMapStart re-opens the gate");
	Check(g_nicknameCleanerPlugin.ApplyCleanName(2, 0), "the next map cleans again");

	// An OnMapEnd seen before the first update keeps the latch shut.
	ResetAll();
	WriteConfigSet(kTestDefaultName);
	Check(static_cast<bool>(g_nicknameCleanerPlugin.OnPluginStart()), "start for the late-map gate test");
	FireMapEnd();
	g_nicknameCleanerPlugin.OnPluginUpdate(std::chrono::milliseconds(16));
	Check(!g_mapReady, "an OnMapEnd before the first update keeps the gate shut");
	const int queriesBeforeLate = g_clientQueryCount;
	Check(!g_nicknameCleanerPlugin.ApplyCleanName(1, 0), "still no client call");
	CheckEqInt(g_clientQueryCount - queriesBeforeLate, 0, "no binding reached");
	FireMapStart();
	Check(g_mapReady, "a new OnMapStart opens the gate again");

	// Without the map listeners nothing can ever open the gate (fail soft).
	ResetAll();
	WriteConfigSet(kTestDefaultName);
	__s2sdk_OnMapStart_Register = nullptr;
	Check(static_cast<bool>(g_nicknameCleanerPlugin.OnPluginStart()), "OnPluginStart succeeds without map listeners");
	Check(!g_mapListenersRegistered, "no map listeners registered");
	Check(HasLogContaining("map listeners are unavailable"), "the missing listeners are logged");
	g_nicknameCleanerPlugin.OnPluginUpdate(std::chrono::milliseconds(16));
	Check(!g_mapReady, "without listeners the update cannot latch readiness");
	const int queriesBeforeNoListeners = g_clientQueryCount;
	Check(!g_nicknameCleanerPlugin.ApplyCleanName(1, 0), "no client call without the map listeners");
	CheckEqInt(g_clientQueryCount - queriesBeforeNoListeners, 0, "no binding reached");

	InstallFakes();
}

// ==============================================================================================
// 6. the player_spawn countdown array
// ==============================================================================================

void TestSpawnTimers() {
	Section("spawn: the update-driven countdown array");
	StartPlugin();
	FireMapStart();

	AddHuman(1, "badword");
	FireEvent("player_spawn", 1);
	g_nicknameCleanerPlugin.TickSpawnTimers(0.6f);
	CheckEqInt(SetNameCallsFor(1), 0, "nothing happens 0.6 s after the spawn");
	g_nicknameCleanerPlugin.TickSpawnTimers(0.4f);
	CheckEqInt(SetNameCallsFor(1), 1, "the pass runs one second after the spawn");
	CheckEqStr(LastSetName(), std::string(kTestDefaultName), "the spawn pass cleans the name");

	// Repeated spawns overwrite the countdown instead of stacking another one: an accumulating
	// implementation would hold 2.0 s here and miss the 1.0 s mark completely.
	AddHuman(2, "badword");
	FireEvent("player_spawn", 2);
	FireEvent("player_spawn", 2);
	g_nicknameCleanerPlugin.TickSpawnTimers(0.6f);
	CheckEqInt(SetNameCallsFor(2), 0, "the second arming did not run early");
	g_nicknameCleanerPlugin.TickSpawnTimers(0.4f);
	CheckEqInt(SetNameCallsFor(2), 1, "repeated spawns keep one pending pass, not two");

	// Invalid slots in the event are ignored.
	FireEvent("player_spawn", -1);
	FireEvent("player_spawn", kSlots);
	g_nicknameCleanerPlugin.TickSpawnTimers(5.0f);
	CheckEqInt(static_cast<int64_t>(g_setNameCalls.size()), 2, "invalid spawn slots arm nothing");

	// The same pass is driven by OnPluginUpdate(dt).
	AddHuman(3, "badword");
	g_setNameCalls.clear();
	FireEvent("player_spawn", 3);
	g_nicknameCleanerPlugin.OnPluginUpdate(std::chrono::milliseconds(999));
	CheckEqInt(SetNameCallsFor(3), 0, "OnPluginUpdate(999 ms) is not enough");
	g_nicknameCleanerPlugin.OnPluginUpdate(std::chrono::milliseconds(1));
	Check(g_nicknameCleanerPlugin.ApplyCleanName(3, 0) == false, "the spawn pass already cleaned slot 3");
	CheckEqInt(SetNameCallsFor(3), 1, "OnPluginUpdate(1 ms) fires the countdown");
	CheckEqStr(LastSetName(), std::string(kTestDefaultName), "the update-driven pass cleaned the name");

	// OnMapEnd drops the pending countdown: it must not fire on the next map.
	AddHuman(4, "badword");
	g_setNameCalls.clear();
	FireEvent("player_spawn", 4);
	FireMapEnd();
	g_nicknameCleanerPlugin.TickSpawnTimers(5.0f);
	CheckEqInt(SetNameCallsFor(4), 0, "OnMapEnd clears the pending spawn pass");

	// A slot that disappears during the wait is re-validated when the countdown fires.
	FireMapStart();
	AddHuman(5, "badword");
	g_setNameCalls.clear();
	FireEvent("player_spawn", 5);
	g_connected[5] = false;
	g_nicknameCleanerPlugin.TickSpawnTimers(1.0f);
	CheckEqInt(SetNameCallsFor(5), 0, "a disconnected slot is skipped by the armed pass");

	// A clean name produces no write when the countdown fires.
	FireMapStart();
	AddHuman(6, "clean name");
	g_setNameCalls.clear();
	FireEvent("player_spawn", 6);
	g_nicknameCleanerPlugin.TickSpawnTimers(1.0f);
	CheckEqInt(SetNameCallsFor(6), 0, "an already clean name is not rewritten");
}

// ==============================================================================================
// 7. authorization, round_prestart and the reconcile loop
// ==============================================================================================

void TestAuthAndRoundPrestart() {
	Section("events: authorization and round_prestart");
	StartPlugin();
	FireMapStart();

	// Both callbacks answer Continue on every path: a Pre hook that answers Handled would consume
	// the event for every other plugin (PITFALLS C12).
	const auto roundHook = g_eventHooks.find("round_prestart");
	Check(roundHook != g_eventHooks.end() &&
				  roundHook->second(plg::string("round_prestart"), static_cast<void*>(&g_dummyEvent),
									false) == s2sdk::ResultType::Continue,
		  "round_prestart answers Continue");
	const auto spawnHook = g_eventHooks.find("player_spawn");
	Check(spawnHook != g_eventHooks.end() &&
				  spawnHook->second(plg::string("player_spawn"), static_cast<void*>(&g_dummyEvent),
									false) == s2sdk::ResultType::Continue,
		  "player_spawn answers Continue");

	AddHuman(1, "badword one", 111);
	AddHuman(2, "badword two", 222);
	FireAuthenticated(1, 111);
	CheckEqInt(SetNameCallsFor(1), 1, "OnClientAuthenticated cleans the authorizing client");
	CheckEqInt(SetNameCallsFor(2), 0, "the other slot is untouched by the authorization");

	FireAuthenticated(2, 999);
	CheckEqInt(SetNameCallsFor(2), 0, "an authorization with a mismatched SteamID is ignored");
	FireAuthenticated(2, 0);
	CheckEqInt(SetNameCallsFor(2), 1, "an authorization without a SteamID is accepted");

	AddHuman(3, "badword three");
	AddHuman(4, "badword four");
	g_setNameCalls.clear();
	FireEvent("round_prestart", 0);
	CheckEqInt(SetNameCallsFor(3), 1, "round_prestart cleans every dirty slot");
	CheckEqInt(SetNameCallsFor(4), 1, "round_prestart covers all slots");

	// The event callback is a Pre hook and answers Continue, never consuming the event.
	Check(g_eventHooks.count("round_prestart") == 1, "round_prestart is hooked");
	Check(g_eventHooks.count("player_spawn") == 1, "player_spawn is hooked");
	int preHooks = 0;
	for (const std::string& line : g_eventHookLog) {
		if (line == "hook:round_prestart:pre" || line == "hook:player_spawn:pre") {
			++preHooks;
		}
	}
	CheckEqInt(preHooks, 2, "both hooks are registered in Pre mode");
}

void TestReconcile() {
	Section("events: the once-per-second reconcile");
	StartPlugin();
	FireMapStart();

	AddHuman(1, "badword");
	g_nicknameCleanerPlugin.OnPluginUpdate(std::chrono::milliseconds(500));
	CheckEqInt(SetNameCallsFor(1), 0, "the reconcile waits for the full second");
	g_nicknameCleanerPlugin.OnPluginUpdate(std::chrono::milliseconds(500));
	CheckEqInt(SetNameCallsFor(1), 1, "the reconcile cleans a connected human slot");
	g_nicknameCleanerPlugin.OnPluginUpdate(std::chrono::milliseconds(1000));
	CheckEqInt(SetNameCallsFor(1), 1, "the next reconcile does not rewrite a clean name");

	AddBot(2, "badword bot");
	AddHuman(3, "badword human");
	g_nicknameCleanerPlugin.OnPluginUpdate(std::chrono::milliseconds(1000));
	CheckEqInt(SetNameCallsFor(2), 0, "the reconcile skips bots");
	CheckEqInt(SetNameCallsFor(3), 1, "the reconcile picks up a human player");
}

// ==============================================================================================
// 8. hook registration, teardown and fail-soft paths
// ==============================================================================================

void TestTeardown() {
	Section("lifecycle: OnPluginEnd removes every registration");
	StartPlugin();
	FireMapStart();
	AddHuman(1, "badword");

	Check(g_roundPrestartHooked && g_playerSpawnHooked, "both event hooks are active");
	Check(g_authenticatedListenerRegistered, "the authentication listener is active");
	Check(g_mapListenersRegistered, "the map listeners are active");

	g_nicknameCleanerPlugin.OnPluginEnd();
	Check(!g_roundPrestartHooked && !g_playerSpawnHooked, "the event hooks are dropped");
	Check(!g_authenticatedListenerRegistered, "the authentication listener is dropped");
	Check(!g_mapListenersRegistered, "the map listeners are dropped");
	Check(g_eventHooks.empty(), "the fake hook table is empty");
	CheckEqInt(g_mapStartUnregistrations, 1, "OnMapStart unregistered once");
	CheckEqInt(g_mapEndUnregistrations, 1, "OnMapEnd unregistered once");
	CheckEqInt(g_authenticatedUnregistrations, 1, "OnClientAuthenticated unregistered once");
	Check(!g_mapReady, "the gate is shut after the unload");

	const int queriesAfterEnd = g_clientQueryCount;
	AddHuman(2, "badword");
	g_nicknameCleanerPlugin.OnPluginUpdate(std::chrono::milliseconds(2000));
	Check(!g_nicknameCleanerPlugin.ApplyCleanName(2, 0), "no client call after the unload");
	CheckEqInt(g_clientQueryCount - queriesAfterEnd, 0, "no binding reached after the unload");
	CheckEqInt(SetNameCallsFor(2), 0, "nothing written after the unload");

	// A second OnPluginEnd is harmless (the host does not call it twice, but a reload can).
	g_nicknameCleanerPlugin.OnPluginEnd();
	CheckEqInt(g_mapStartUnregistrations, 1, "no duplicate unregistration");
}

void TestHookFailures() {
	Section("lifecycle: a failed or missing HookEvent binding is soft");
	ResetAll();
	WriteConfigSet(kTestDefaultName);
	__s2sdk_HookEvent = nullptr;
	Check(static_cast<bool>(g_nicknameCleanerPlugin.OnPluginStart()), "OnPluginStart succeeds without HookEvent");
	Check(!g_roundPrestartHooked && !g_playerSpawnHooked, "no hook is recorded");
	Check(HasLogContaining("event bindings are unavailable"), "the missing binding is logged");
	g_nicknameCleanerPlugin.OnPluginEnd();

	ResetAll();
	WriteConfigSet(kTestDefaultName);
	g_hookFailureName = "player_spawn";
	Check(static_cast<bool>(g_nicknameCleanerPlugin.OnPluginStart()), "OnPluginStart survives a failed hook");
	Check(g_roundPrestartHooked, "the successful hook is kept");
	Check(!g_playerSpawnHooked, "the failed hook is not recorded");
	Check(HasLogContaining("hooking 'player_spawn' failed"), "the failed hook is logged");
	g_nicknameCleanerPlugin.OnPluginEnd();
	bool unhookedRound = false;
	bool unhookedSpawn = false;
	for (const std::string& line : g_eventHookLog) {
		unhookedRound = unhookedRound || line == "unhook:round_prestart:pre";
		unhookedSpawn = unhookedSpawn || line == "unhook:player_spawn:pre";
	}
	Check(unhookedRound, "the successful hook is unhooked in OnPluginEnd");
	Check(!unhookedSpawn, "the hook that never registered is not unhooked");
}

void TestConfigFailSoft() {
	Section("config: missing and malformed files");
	// An empty configs directory: no settings.ini and no list files. The built-in DefaultName and
	// an empty blocklist are used, exactly like the original's failed KeyValues load.
	StartPluginWithoutConfigs();
	Check(HasLogContaining("failed to load"), "the missing settings.ini is logged");
	FireMapStart();
	AddHuman(1, "http://promo");
	Check(g_nicknameCleanerPlugin.ApplyCleanName(1, 0), "the built-in DefaultName still cleans a name");
	CheckEqStr(LastSetName(), nickname_cleaner::kDefaultDefaultName,
			   "the built-in default is Player");
	g_nicknameCleanerPlugin.OnPluginEnd();

	// No configs directory at all: the built-in DefaultName is used.
	ResetAll();
	plg::GetConfigsDir = nullptr;
	Check(static_cast<bool>(g_nicknameCleanerPlugin.OnPluginStart()), "OnPluginStart succeeds without a configs dir");
	Check(HasLogContaining("failed to load"), "the missing settings.ini is logged");
	FireMapStart();
	AddHuman(1, "http://promo");
	Check(g_nicknameCleanerPlugin.ApplyCleanName(1, 0), "the built-in DefaultName still cleans a name");
	CheckEqStr(LastSetName(), nickname_cleaner::kDefaultDefaultName,
			   "the built-in default is Player");
	g_nicknameCleanerPlugin.OnPluginEnd();

	InstallFakes();

	// A malformed settings.ini keeps the default and does not use the fallback.
	StartPluginWithSettings("\"NicknameCleaner\" { \"DefaultName\"");
	Check(HasLogContaining("failed to parse"), "the malformed settings.ini is logged");
	FireMapStart();
	AddHuman(1, "mega-shop.xyz");
	Check(g_nicknameCleanerPlugin.ApplyCleanName(1, 0), "the default still cleans a name");
	CheckEqStr(LastSetName(), nickname_cleaner::kDefaultDefaultName,
			   "a malformed settings.ini keeps DefaultName 'Player'");
	g_nicknameCleanerPlugin.OnPluginEnd();

	// A missing whitelist/blocklist pair means "no entries", like the original's cleared sets.
	ResetAll();
	Check(WriteFile(PluginConfigDir() / "settings.ini", SettingsText(kTestDefaultName)),
		  "write settings.ini only");
	Check(static_cast<bool>(g_nicknameCleanerPlugin.OnPluginStart()), "OnPluginStart succeeds without the lists");
	Check(HasLogContaining("failed to open"), "the missing list files are logged");
	FireMapStart();
	AddHuman(1, "site.ru");
	Check(!g_nicknameCleanerPlugin.ApplyCleanName(1, 0), "with an empty blocklist the name is clean");
	CheckEqInt(SetNameCallsFor(1), 0, "nothing is written with an empty blocklist");
	g_nicknameCleanerPlugin.OnPluginEnd();

	// The configs-root fallback is read when the plugin directory is not installed.
	ResetAll();
	Check(WriteFile(TempRoot() / "settings.ini", SettingsText("Fallback")),
		  "write the fallback settings.ini");
	Check(WriteFile(TempRoot() / "whitelist.txt", kTestWhitelist), "write the fallback whitelist");
	Check(WriteFile(TempRoot() / "blocklist.txt", kTestBlocklist), "write the fallback blocklist");
	Check(static_cast<bool>(g_nicknameCleanerPlugin.OnPluginStart()), "OnPluginStart reads the configs root");
	Check(!HasLogContaining("failed to load"), "no load error for the fallback");
	FireMapStart();
	AddHuman(1, "mega-shop.xyz");
	Check(g_nicknameCleanerPlugin.ApplyCleanName(1, 0), "the fallback config cleans a name");
	CheckEqStr(LastSetName(), "Fallback", "the fallback DefaultName is used");
	g_nicknameCleanerPlugin.OnPluginEnd();
}

void TestMissingBindingsFailSoft() {
	Section("lifecycle: every missing binding stays soft");
	ResetAll();
	WriteConfigSet(kTestDefaultName);

	__s2sdk_OnClientAuthenticated_Register = nullptr;
	__s2sdk_OnClientAuthenticated_Unregister = nullptr;
	__s2sdk_HookEvent = nullptr;
	__s2sdk_UnhookEvent = nullptr;
	__s2sdk_IsClientConnected = nullptr;
	__s2sdk_IsClientInGame = nullptr;
	__s2sdk_IsClientSourceTV = nullptr;
	__s2sdk_IsFakeClient = nullptr;
	__s2sdk_PlayerSlotToEntPointer = nullptr;
	__s2sdk_PlayerSlotToEntHandle = nullptr;
	__s2sdk_IsValidEntPointer = nullptr;
	__s2sdk_IsValidEntHandle = nullptr;
	__s2sdk_GetClientSteamID64 = nullptr;
	__s2sdk_GetClientName = nullptr;
	__s2sdk_SetClientName = nullptr;
	__s2sdk_GetEventPlayerSlot = nullptr;

	Check(static_cast<bool>(g_nicknameCleanerPlugin.OnPluginStart()),
		  "OnPluginStart returns success with every binding missing");
	FireMapStart();
	AddHuman(1, "badword");
	Check(!g_nicknameCleanerPlugin.ApplyCleanName(1, 0), "no name is written without the bindings");
	g_nicknameCleanerPlugin.TickSpawnTimers(2.0f);
	g_nicknameCleanerPlugin.OnPluginUpdate(std::chrono::milliseconds(2000));
	FireAuthenticated(1, 0);
	g_nicknameCleanerPlugin.OnPluginEnd();
	Check(HasErrorLog(), "the missing bindings are reported and the plugin stays soft");
}

void TestUnloadAfterMapEnd() {
	Section("lifecycle: unload while the map is still up");
	StartPlugin();
	FireMapStart();
	AddHuman(1, "badword");
	FireEvent("player_spawn", 1);
	g_nicknameCleanerPlugin.OnPluginEnd();

	const int queries = g_clientQueryCount;
	g_nicknameCleanerPlugin.OnPluginUpdate(std::chrono::milliseconds(2000));
	CheckEqInt(g_clientQueryCount - queries, 0, "an unloaded plugin makes no client query");
	CheckEqInt(SetNameCallsFor(1), 0, "the armed spawn pass did not survive the unload");
}

// ==============================================================================================

} // namespace

int main() {
	std::printf("nickname_cleaner_tests (data dir: %s)\n", NICKNAME_CLEANER_TEST_DATA_DIR);

	TestListParsing();
	TestMatching();
	TestStripPunctuation();
	TestPlaceholderName();
	TestCleanNickname();
	TestProductionLists();
	TestKv1Reader();
	TestKv1Settings();
	TestConfigPaths();
	TestClientGuards();
	TestBotAndHandleEdges();
	TestMapGate();
	TestSpawnTimers();
	TestAuthAndRoundPrestart();
	TestReconcile();
	TestTeardown();
	TestHookFailures();
	TestConfigFailSoft();
	TestMissingBindingsFailSoft();
	TestUnloadAfterMapEnd();

	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
