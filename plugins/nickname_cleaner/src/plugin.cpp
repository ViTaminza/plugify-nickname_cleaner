// nickname_cleaner — Plugify port of the Metamod plugin nickname_cleaner (ABKAM).
//
// Behaviour kept identical:
//   * settings.ini ("NicknameCleaner" / "DefaultName", default "Player") and the two list files
//     whitelist.txt / blocklist.txt of <configs>/nickname_cleaner/ are read once at plugin start
//     (the original loaded them in AllPluginsLoaded and never reloaded);
//   * the name of every human player is rewritten with the cleaned nickname on round_prestart, one
//     second after every player_spawn, and when a client authorizes (ApplyCleanName also guards
//     against a slot that changed hands between the authorization and the name write);
//   * a name whose tokens are all filtered becomes DefaultName.
//
// Pure s2sdk (docs/PLUGIN_STANDARD.md §7): no pisexcompat and no guard library. The safety rules
// of handoff/weaponskins/PITFALLS.md live in this file:
//   * B1/B2 map gate: OnMapStart_Register/OnMapEnd_Register in OnPluginStart, unregistered in
//     OnPluginEnd. g_mapReady is latched by OnMapStart and by the first OnPluginUpdate when the
//     listeners are registered and no OnMapEnd was seen (a plugin reloaded into a running map
//     never saw an OnMapStart), and dropped by OnMapEnd. No client query is made while the gate
//     is shut: s2sdk's client list is null before the first map and torn down between OnMapEnd and
//     the next OnMapStart. Map-end handlers only clear state (PITFALLS B2).
//   * C2 slot/controller checks: 0 <= slot < 64, IsClientConnected, IsClientInGame,
//     !IsClientSourceTV, the controller entity pointer and handle valid, and IsFakeClient for the
//     bot skip. Every call is additionally guarded by its __s2sdk_* binding, so a plugin running
//     without s2sdk fails soft instead of calling through a null slot (PITFALLS A1). Two guards are
//     deliberately fail-closed: without the IsFakeClient binding a slot is treated as untouchable
//     (a bot has SteamID 0 just like an unauthenticated human), and controller handles 0 and -1
//     are rejected before IsValidEntHandle (PITFALLS C1; a *negative* valid handle is still
//     accepted, because the serial number sets the sign bit).
//   * C12 event hooks: "round_prestart" and "player_spawn" are hooked with
//     s2sdk::HookEvent(HookMode::Pre) in OnPluginStart and removed with s2sdk::UnhookEvent in
//     OnPluginEnd; the s2sdk callbacks answer ResultType::Continue on every path (a Pre hook must
//     never consume the event of other plugins).
//   * The player_spawn pass is a 64-slot countdown array driven from OnPluginUpdate(dt) instead of
//     a pisexcompat timer: one second after a spawn only that slot is cleaned, and arming the same
//     slot again overwrites the countdown instead of stacking another timer (the original's one
//     timer per spawn). The array is cleared on OnMapEnd and OnPluginEnd.
//   * B6: s2sdk's OnClientAuthenticated is not delivered for every client on a running server, so
//     a once-per-second reconcile applies the cleaner to the connected human players exactly like
//     an authorization would; ApplyCleanName() is a no-op for a name that is already clean.
//
// Port notes:
//   * pisex::KeyValues -> the plugin-local KV1 reader (src/kv1.hpp) through src/config.hpp, which
//     reads settings.ini and the two list files from <configs>/nickname_cleaner/ with a fallback to
//     <configs>/. The production files are read unchanged.
//   * pisex::g_pPlayers->SetPlayerName -> s2sdk::SetClientName behind SetClientName(), which is a
//     no-op unless HasController(slot) is true: s2sdk's Player::SetName dereferences the client and
//     the controller without a null check (a null dereference the old player_spawn timer could hit
//     in the changelevel window; PITFALLS C2).
//   * s2sdk's "<unknown>" name placeholder is treated as an empty name, exactly the "" the
//     original's controller-less getter returned.

#include "config.hpp"
#include "nickname_cleaner_core.hpp"

#include <plg/plugin.hpp>
#include <plg/source_location.hpp>
#include <plg/string.hpp>
#include <plugin_export.h>
#include <s2sdk/s2sdk.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <string_view>

namespace {

constexpr const char* kLogTag = "[nickname_cleaner]";
constexpr int kMaxSlots = 64;
constexpr auto kReconcileInterval = std::chrono::seconds(1);
// The original armed its player_spawn timer with pisex::g_pUtils->CreateTimer(1.0f, ...).
constexpr float kSpawnCleanDelay = 1.0f;
constexpr const char* kRoundPrestartEvent = "round_prestart";
constexpr const char* kPlayerSpawnEvent = "player_spawn";

void LogMessage(std::string_view message, plg::Severity severity) {
	if (plg::Log) {
		plg::Log(message, severity, plg::source_location::current());
	}
}

void LogInfo(std::string_view message) {
	LogMessage(message, plg::Severity::Info);
}

void LogError(std::string_view message) {
	LogMessage(message, plg::Severity::Error);
}

class NicknameCleanerPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override;
	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override;
	plg::PluginResult OnPluginEnd() override;

	// Map lifecycle state (called by the s2sdk listeners; no client query may happen here).
	void OnMapStart();
	void OnMapEnd();

	// The three application points of the original AllPluginsLoaded().
	void ApplyCleanNameForAllPlayers();
	bool ApplyCleanName(int iSlot, uint64_t iSteamID64);

	// The update-driven replacement of the original's one-second player_spawn timer.
	void ArmSpawnClean(int iSlot);
	void TickSpawnTimers(float dtSeconds);
	void ClearSpawnTimers();
	void ReconcileSlots();

private:
	void LoadConfig();
	// Reads one of the two list files (the original LoadWhitelist/LoadBlocklist).
	void LoadListFile(const char* szFile, bool blocklist);

	// PITFALLS C2: every wrapper below asks the map gate first and checks its __s2sdk_* binding.
	bool HasClient(int iSlot) const;
	bool HasController(int iSlot) const;
	bool IsBot(int iSlot) const;
	uint64_t ClientSteamID64(int iSlot) const;
	// "" for a slot without a name; s2sdk's "<unknown>" placeholder also means "no name".
	std::string ClientName(int iSlot) const;
	// True only when the write actually went through (the controller existed and the binding was
	// available).
	bool SetClientName(int iSlot, const std::string& name) const;

	nickname_cleaner::NameLists m_lists;
	std::string m_defaultName = nickname_cleaner::kDefaultDefaultName;
	std::chrono::milliseconds m_sinceReconcile{0};
	// One countdown per player slot; 0 means "nothing armed". Arming a slot assigns the delay, it
	// never adds to it, so repeated spawns cannot accumulate timer work.
	std::array<float, kMaxSlots> m_spawnTimers{};
};

NicknameCleanerPlugin g_nicknameCleanerPlugin;

// --- PITFALLS B1/B2: the map gate ------------------------------------------------------------
//
// The state machine pisexcompat used to own (pisex::Init/Poll/Shutdown); a pure-s2sdk plugin keeps
// it in its own source. g_mapReady is the single flag every client call asks before it touches
// s2sdk.
bool g_mapReady = false;               // a map is up: every client call is allowed
bool g_mapStartSeen = false;           // OnMapStart arrived since OnPluginStart / the last OnMapEnd
bool g_mapEndSeen = false;             // OnMapEnd arrived since OnPluginStart
bool g_mapListenersRegistered = false; // OnMapStart/OnMapEnd are registered with s2sdk

// s2sdk listeners are plain function pointers called on the game thread. They must not throw and
// must not touch a client: OnMapEnd runs while the client list is being torn down (PITFALLS B2).

void OnMapStartListener() noexcept {
	// OnMapStart is the map's first live frame; the gate is open for everything that follows.
	// (A plugin loaded into a running map never sees this callback - PollMapGate() covers it.)
	g_mapReady = true;
	g_mapStartSeen = true;
	g_mapEndSeen = false;
	try {
		g_nicknameCleanerPlugin.OnMapStart();
	} catch (...) {
		// State only; nothing left to do.
	}
}

void OnMapEndListener() noexcept {
	// From here until the next OnMapStart every s2sdk client query can crash (PITFALLS B2): only
	// state changes, no client call.
	g_mapReady = false;
	g_mapStartSeen = false;
	g_mapEndSeen = true;
	try {
		g_nicknameCleanerPlugin.OnMapEnd();
	} catch (...) {
		// State only; nothing left to do.
	}
}

// Registers the map listeners and closes the gate. Touches nothing else, so it is safe in
// OnPluginStart (PITFALLS B1). Without the listeners nothing could ever close the gate again, so a
// missing s2sdk binding keeps it shut for good (fail soft, never Failed - PITFALLS A1).
void InitMapGate() {
	g_mapReady = false;
	g_mapStartSeen = false;
	g_mapEndSeen = false;
	if (g_mapListenersRegistered) {
		return;
	}
	if (__s2sdk_OnMapStart_Register != nullptr && __s2sdk_OnMapEnd_Register != nullptr) {
		s2sdk::OnMapStart_Register(&OnMapStartListener);
		s2sdk::OnMapEnd_Register(&OnMapEndListener);
		g_mapListenersRegistered = true;
	} else {
		LogError(std::string(kLogTag) +
				 " the s2sdk map listeners are unavailable - client calls stay disabled");
	}
}

// Per-frame pump (OnPluginUpdate), the replacement of pisex::Poll: a plugin loaded into a running
// map never saw an OnMapStart, so its first update is the proof of a live map (the host pumps
// updates from the server frame). After an OnMapEnd only a new OnMapStart opens the gate again:
// the host keeps pumping updates through a changelevel, so `!g_mapEndSeen` must not be enough once
// one was seen.
void PollMapGate() {
	if (g_mapListenersRegistered && !g_mapReady && (g_mapStartSeen || !g_mapEndSeen)) {
		g_mapReady = true;
		g_mapStartSeen = true;
	}
}

// Removes the map listeners and closes the gate (PITFALLS A7): `plg reload` must leave no pointer
// of this library behind.
void ShutdownMapGate() {
	if (g_mapListenersRegistered) {
		g_mapListenersRegistered = false;
		if (__s2sdk_OnMapStart_Unregister != nullptr) {
			s2sdk::OnMapStart_Unregister(&OnMapStartListener);
		}
		if (__s2sdk_OnMapEnd_Unregister != nullptr) {
			s2sdk::OnMapEnd_Unregister(&OnMapEndListener);
		}
	}
	g_mapReady = false;
	g_mapStartSeen = false;
	g_mapEndSeen = false;
}

// --- PITFALLS C12: the game event hooks ------------------------------------------------------

bool g_roundPrestartHooked = false;
bool g_playerSpawnHooked = false;
bool g_authenticatedListenerRegistered = false;

// A Pre hook must answer Continue on every path: a result >= Handled frees the event for every
// other plugin on the server (the original's listener returned false for the same reason).
s2sdk::ResultType OnRoundPrestartEvent(const plg::string& /*name*/, void* /*event*/,
									   bool /*dontBroadcast*/) noexcept {
	try {
		if (g_mapReady) {
			g_nicknameCleanerPlugin.ApplyCleanNameForAllPlayers();
		}
	} catch (const std::exception& ex) {
		LogError(std::string(kLogTag) + " round_prestart failed: " + ex.what());
	} catch (...) {
		LogError(std::string(kLogTag) + " round_prestart failed with an unknown exception");
	}
	return s2sdk::ResultType::Continue;
}

s2sdk::ResultType OnPlayerSpawnEvent(const plg::string& /*name*/, void* event,
									 bool /*dontBroadcast*/) noexcept {
	try {
		// Events keep arriving during a changelevel (PITFALLS C13) and every client query before
		// the first map crashes (PITFALLS B1/B2): the gate turns both into a no-op first.
		if (!g_mapReady || event == nullptr || __s2sdk_GetEventPlayerSlot == nullptr) {
			return s2sdk::ResultType::Continue;
		}
		// The raw event "userid" is not a slot in CS2; GetEventPlayerSlot resolves it.
		const int32_t iSlot = s2sdk::GetEventPlayerSlot(event, "userid");
		g_nicknameCleanerPlugin.ArmSpawnClean(iSlot);
	} catch (const std::exception& ex) {
		LogError(std::string(kLogTag) + " player_spawn failed: " + ex.what());
	} catch (...) {
		LogError(std::string(kLogTag) + " player_spawn failed with an unknown exception");
	}
	return s2sdk::ResultType::Continue;
}

// The s2sdk OnClientAuthenticated listener; must not throw into the engine.
void OnClientAuthenticatedListener(int32_t iSlot, uint64_t iSteamID64) noexcept {
	try {
		if (g_mapReady) {
			g_nicknameCleanerPlugin.ApplyCleanName(iSlot, iSteamID64);
		}
	} catch (const std::exception& ex) {
		LogError(std::string(kLogTag) + " OnClientAuthenticated failed: " + ex.what());
	} catch (...) {
		LogError(std::string(kLogTag) + " OnClientAuthenticated failed with an unknown exception");
	}
}

// --- configuration ---------------------------------------------------------------------------

void NicknameCleanerPlugin::LoadConfig() {
	std::string content;
	std::string path;
	if (!nickname_cleaner::ReadConfigFile(nickname_cleaner::kSettingsFile, content, path)) {
		LogError(std::string(kLogTag) + " failed to load " +
				 nickname_cleaner::PrimaryConfigPath(nickname_cleaner::kSettingsFile) +
				 " - keeping DefaultName '" + m_defaultName + "'");
		return;
	}
	const std::optional<nickname_cleaner::KvNode> root = nickname_cleaner::ParseKv1(content);
	if (!root) {
		LogError(std::string(kLogTag) + " failed to parse " + path + " - keeping DefaultName '" +
				 m_defaultName + "'");
		return;
	}
	m_defaultName = nickname_cleaner::DefaultNameFromKv(*root);
}

void NicknameCleanerPlugin::LoadListFile(const char* szFile, bool blocklist) {
	std::string content;
	std::string path;
	if (!nickname_cleaner::ReadConfigFile(szFile, content, path)) {
		LogError(std::string(kLogTag) + " failed to open " +
				 nickname_cleaner::PrimaryConfigPath(szFile) + ", list empty");
		// The original cleared the set before returning, so a missing file means "no entries".
		if (blocklist) {
			m_lists.blocklist.clear();
		} else {
			m_lists.whitelist.clear();
		}
		return;
	}
	m_lists.LoadListContent(content, blocklist);
}

// --- PITFALLS C2: the guarded client wrappers ------------------------------------------------

bool NicknameCleanerPlugin::HasClient(int iSlot) const {
	if (!g_mapReady || iSlot < 0 || iSlot >= kMaxSlots) {
		return false;
	}
	if (__s2sdk_IsClientConnected == nullptr) {
		return false;
	}
	try {
		return s2sdk::IsClientConnected(iSlot);
	} catch (...) {
		return false;
	}
}

bool NicknameCleanerPlugin::HasController(int iSlot) const {
	if (!HasClient(iSlot)) {
		return false;
	}
	if (__s2sdk_IsClientInGame == nullptr || __s2sdk_IsClientSourceTV == nullptr ||
		__s2sdk_PlayerSlotToEntPointer == nullptr || __s2sdk_PlayerSlotToEntHandle == nullptr ||
		__s2sdk_IsValidEntPointer == nullptr || __s2sdk_IsValidEntHandle == nullptr) {
		return false;
	}
	try {
		if (!s2sdk::IsClientInGame(iSlot) || s2sdk::IsClientSourceTV(iSlot)) {
			return false;
		}
		// The controller entity: without it s2sdk's name write dereferences null (PITFALLS C2).
		void* pController = s2sdk::PlayerSlotToEntPointer(iSlot);
		if (pController == nullptr || !s2sdk::IsValidEntPointer(pController)) {
			return false;
		}
		// PITFALLS C1: entity handles are index | serial << 15. 0 and -1 are never a controller,
		// so they are rejected before the entity-system call; a *negative* handle can still be a
		// valid one (the serial number sets the sign bit) and must go through.
		const int32_t iHandle = s2sdk::PlayerSlotToEntHandle(iSlot);
		if (iHandle == 0 || iHandle == -1) {
			return false;
		}
		return s2sdk::IsValidEntHandle(iHandle);
	} catch (...) {
		return false;
	}
}

bool NicknameCleanerPlugin::IsBot(int iSlot) const {
	// IsFakeClient answers false for a slot whose client object does not exist (s2sdk:
	// "client && client->IsFakeClient()"), so HasClient() has to come first.
	if (!HasClient(iSlot)) {
		return false;
	}
	// Fail-closed: when the binding is unavailable the plugin cannot tell a bot from a human, so
	// it treats every slot as untouchable instead of risking a rename of a bot (both sides can
	// answer SteamID 0 there).
	if (__s2sdk_IsFakeClient == nullptr) {
		return true;
	}
	try {
		return s2sdk::IsFakeClient(iSlot);
	} catch (...) {
		return true; // treat as unavailable rather than as a real player
	}
}

uint64_t NicknameCleanerPlugin::ClientSteamID64(int iSlot) const {
	// For a slot that is not connected s2sdk answers the previous occupant's id; HasClient() keeps
	// the original's 0 answer for an empty slot.
	if (!HasClient(iSlot) || __s2sdk_GetClientSteamID64 == nullptr) {
		return 0;
	}
	try {
		return s2sdk::GetClientSteamID64(iSlot);
	} catch (...) {
		return 0;
	}
}

std::string NicknameCleanerPlugin::ClientName(int iSlot) const {
	if (!HasController(iSlot) || __s2sdk_GetClientName == nullptr) {
		return {};
	}
	try {
		const plg::string name = s2sdk::GetClientName(iSlot);
		std::string out(name.data(), name.size());
		// s2sdk answers "<unknown>" when it cannot resolve the client; the Pisex API returned ""
		// there and callers treat "" as "no name".
		if (out == "<unknown>") {
			out.clear();
		}
		return out;
	} catch (...) {
		return {};
	}
}

bool NicknameCleanerPlugin::SetClientName(int iSlot, const std::string& name) const {
	// Player::SetName crashes for a slot without a client or without a controller (both are
	// dereferenced unchecked) - never call SetClientName unless the slot is fully in place.
	if (!HasController(iSlot) || __s2sdk_SetClientName == nullptr) {
		return false;
	}
	try {
		s2sdk::SetClientName(iSlot, plg::string(name));
		return true;
	} catch (...) {
		// A name write must never escape into the engine.
		return false;
	}
}

// --- name cleaning ---------------------------------------------------------------------------

bool NicknameCleanerPlugin::ApplyCleanName(int iSlot, uint64_t iSteamID64) {
	if (iSlot < 0 || iSlot >= kMaxSlots) {
		return false;
	}

	// The slot must hold a connected, in-game human client whose controller entity exists at this
	// instant. HasController() is silent and is exactly the original's guard: with no
	// CCSPlayerController::FromSlot(slot) its GetPlayerName returned "" and its SetPlayerName did
	// nothing.
	if (!HasController(iSlot)) {
		return false;
	}
	if (IsBot(iSlot)) {
		return false;
	}

	const uint64_t currentSteamId = ClientSteamID64(iSlot);
	if (currentSteamId != 0 && iSteamID64 != 0 && currentSteamId != iSteamID64) {
		return false;
	}

	const std::string currentName = ClientName(iSlot);
	// A null/empty name is the original's "no controller" answer; "<unknown>" (ClientName turns it
	// into "") is s2sdk's placeholder for a slot whose client is gone. Both mean "nothing to clean".
	if (nickname_cleaner::IsPlaceholderName(currentName.c_str())) {
		return false;
	}

	const std::string cleaned = nickname_cleaner::CleanNickname(currentName, m_lists, m_defaultName);
	if (cleaned == currentName) {
		return false;
	}
	return SetClientName(iSlot, cleaned);
}

void NicknameCleanerPlugin::ApplyCleanNameForAllPlayers() {
	for (int iSlot = 0; iSlot < kMaxSlots; ++iSlot) {
		try {
			ApplyCleanName(iSlot, 0);
		} catch (const std::exception& ex) {
			LogError(std::string(kLogTag) + " cleaning slot " + std::to_string(iSlot) +
					 " failed: " + ex.what());
		} catch (...) {
			LogError(std::string(kLogTag) + " cleaning slot " + std::to_string(iSlot) + " failed");
		}
	}
}

// --- the player_spawn countdown --------------------------------------------------------------

void NicknameCleanerPlugin::ArmSpawnClean(int iSlot) {
	if (!g_mapReady || iSlot < 0 || iSlot >= kMaxSlots) {
		return;
	}
	// Assignment, not accumulation: a player spawning again during the countdown keeps one pending
	// pass (the original started a second one-second timer per spawn; one pass covers the slot).
	m_spawnTimers[static_cast<std::size_t>(iSlot)] = kSpawnCleanDelay;
}

void NicknameCleanerPlugin::TickSpawnTimers(float dtSeconds) {
	// OnPluginUpdate already asked the gate; the second check keeps a direct caller (the test) from
	// running a countdown between OnMapEnd and the next OnMapStart.
	if (!g_mapReady) {
		return;
	}
	const float delta = dtSeconds > 0.0f ? dtSeconds : 0.0f;
	if (delta <= 0.0f) {
		return;
	}
	for (int iSlot = 0; iSlot < kMaxSlots; ++iSlot) {
		float& remaining = m_spawnTimers[static_cast<std::size_t>(iSlot)];
		if (remaining <= 0.0f) {
			continue;
		}
		remaining -= delta;
		if (remaining > 0.0f) {
			continue;
		}
		remaining = 0.0f;
		try {
			// The slot is re-validated inside ApplyCleanName: a player can disconnect or a map can
			// change during the one-second wait (PITFALLS B4/C2).
			ApplyCleanName(iSlot, 0);
		} catch (const std::exception& ex) {
			LogError(std::string(kLogTag) + " spawn clean of slot " + std::to_string(iSlot) +
					 " failed: " + ex.what());
		} catch (...) {
			LogError(std::string(kLogTag) + " spawn clean of slot " + std::to_string(iSlot) +
					 " failed");
		}
	}
}

void NicknameCleanerPlugin::ClearSpawnTimers() {
	m_spawnTimers.fill(0.0f);
}

void NicknameCleanerPlugin::ReconcileSlots() {
	for (int iSlot = 0; iSlot < kMaxSlots; ++iSlot) {
		try {
			// ApplyCleanName re-checks everything; the bot skip here is only the original's early
			// exit and keeps the pass quiet for the bot-only slots.
			if (IsBot(iSlot)) {
				continue;
			}
			// The same call the authorization hook makes; the steamid guard inside is skipped
			// (0 = "any slot occupant"), which is what the original's round_prestart pass did.
			ApplyCleanName(iSlot, 0);
		} catch (const std::exception& ex) {
			LogError(std::string(kLogTag) + " reconcile of slot " + std::to_string(iSlot) +
					 " failed: " + ex.what());
		} catch (...) {
			LogError(std::string(kLogTag) + " reconcile of slot " + std::to_string(iSlot) +
					 " failed");
		}
	}
}

// --- lifecycle --------------------------------------------------------------------------------

void NicknameCleanerPlugin::OnMapStart() {
	// No client query here (PITFALLS B1/B2): the next update is the first frame that may touch a
	// client, and the reconcile starts counting from a clean second.
	m_sinceReconcile = std::chrono::milliseconds(0);
}

void NicknameCleanerPlugin::OnMapEnd() {
	// Map shutdown: state only. A countdown that was running must not survive into the next map.
	ClearSpawnTimers();
	m_sinceReconcile = std::chrono::milliseconds(0);
}

plg::PluginResult NicknameCleanerPlugin::OnPluginStart() {
	try {
		// The map listeners come first: they gate every s2sdk client call this plugin makes
		// (PITFALLS B1/B2). Registering them touches no client.
		InitMapGate();

		// File reads only - legal before the first map.
		LoadConfig();
		LoadListFile(nickname_cleaner::kWhitelistFile, false);
		LoadListFile(nickname_cleaner::kBlocklistFile, true);
		LogInfo(std::string(kLogTag) + " loaded " + std::to_string(m_lists.whitelist.size()) +
				" whitelist / " + std::to_string(m_lists.blocklist.size()) +
				" blocklist entries, DefaultName '" + m_defaultName + "'");

		// PITFALLS C12: one Pre hook per event, the plain function pointers s2sdk requires. A hook
		// is removed in OnPluginEnd only when its registration succeeded.
		if (__s2sdk_HookEvent != nullptr) {
			if (s2sdk::HookEvent(kRoundPrestartEvent, &OnRoundPrestartEvent, s2sdk::HookMode::Pre) ==
				s2sdk::EventHookError::Okay) {
				g_roundPrestartHooked = true;
			} else {
				LogError(std::string(kLogTag) + " hooking '" + kRoundPrestartEvent +
						 "' failed - the round pass is disabled");
			}
			if (s2sdk::HookEvent(kPlayerSpawnEvent, &OnPlayerSpawnEvent, s2sdk::HookMode::Pre) ==
				s2sdk::EventHookError::Okay) {
				g_playerSpawnHooked = true;
			} else {
				LogError(std::string(kLogTag) + " hooking '" + kPlayerSpawnEvent +
						 "' failed - the spawn pass is disabled");
			}
		} else {
			LogError(std::string(kLogTag) +
					 " the s2sdk event bindings are unavailable - events are not hooked");
		}

		// PITFALLS B6: the listener is unreliable in this stack; the reconcile loop below covers
		// every client it misses, so a missing binding is an error line, not a failure.
		if (__s2sdk_OnClientAuthenticated_Register != nullptr) {
			s2sdk::OnClientAuthenticated_Register(&OnClientAuthenticatedListener);
			g_authenticatedListenerRegistered = true;
		} else {
			LogError(std::string(kLogTag) +
					 " the s2sdk OnClientAuthenticated binding is unavailable - new clients are only "
					 "cleaned by the reconcile loop");
		}
	} catch (const std::exception& ex) {
		LogError(std::string(kLogTag) + " OnPluginStart failed: " + ex.what());
	} catch (...) {
		LogError(std::string(kLogTag) + " OnPluginStart failed with an unknown exception");
	}
	return {}; // never Failed: that stops every plugify plugin (PITFALLS A1)
}

plg::PluginResult NicknameCleanerPlugin::OnPluginUpdate(std::chrono::milliseconds dt) {
	try {
		// A plugin loaded into a running map never saw an OnMapStart: the first update latches
		// readiness (no-op while a map is already up, never after an OnMapEnd).
		PollMapGate();
		if (!g_mapReady) {
			return {};
		}

		const float dtSeconds = dt.count() > 0 ? static_cast<float>(dt.count()) / 1000.0f : 0.0f;
		TickSpawnTimers(dtSeconds);

		m_sinceReconcile += dt;
		if (m_sinceReconcile >= kReconcileInterval) {
			m_sinceReconcile = std::chrono::milliseconds(0);
			ReconcileSlots();
		}
	} catch (const std::exception& ex) {
		LogError(std::string(kLogTag) + " update failed: " + ex.what());
	} catch (...) {
		LogError(std::string(kLogTag) + " update failed with an unknown exception");
	}
	return {};
}

plg::PluginResult NicknameCleanerPlugin::OnPluginEnd() {
	try {
		// PITFALLS A7/C12: every registration of this library is removed again, so a `plg reload`
		// leaves no callback pointing into the unloaded .so.
		if (g_authenticatedListenerRegistered) {
			g_authenticatedListenerRegistered = false;
			if (__s2sdk_OnClientAuthenticated_Unregister != nullptr) {
				s2sdk::OnClientAuthenticated_Unregister(&OnClientAuthenticatedListener);
			}
		}
		if (g_roundPrestartHooked) {
			g_roundPrestartHooked = false;
			if (__s2sdk_UnhookEvent != nullptr) {
				s2sdk::UnhookEvent(kRoundPrestartEvent, &OnRoundPrestartEvent, s2sdk::HookMode::Pre);
			}
		}
		if (g_playerSpawnHooked) {
			g_playerSpawnHooked = false;
			if (__s2sdk_UnhookEvent != nullptr) {
				s2sdk::UnhookEvent(kPlayerSpawnEvent, &OnPlayerSpawnEvent, s2sdk::HookMode::Pre);
			}
		}
	} catch (const std::exception& ex) {
		LogError(std::string(kLogTag) + " OnPluginEnd failed: " + ex.what());
	} catch (...) {
		LogError(std::string(kLogTag) + " OnPluginEnd failed with an unknown exception");
	}

	try {
		ShutdownMapGate();
		// Pending counts of a map that is already gone must not fire after a reload.
		ClearSpawnTimers();
		m_sinceReconcile = std::chrono::milliseconds(0);
		m_lists.Clear();
		m_defaultName = nickname_cleaner::kDefaultDefaultName;
	} catch (...) {
		// nothing left to do at unload
	}
	return {};
}

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_nicknameCleanerPlugin)
