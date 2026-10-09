// nickname_cleaner — Plugify port of the Metamod plugin nickname_cleaner
// (ref/src/cs2-nickname_cleaner/nickname_cleaner.cpp).
//
// Behaviour kept identical:
//   * settings.ini ("NicknameCleaner" / "DefaultName", default "Player") and the two list files
//     whitelist.txt / blocklist.txt of addons/configs/nickname_cleaner/ are read once at plugin
//     start (the original loaded them in AllPluginsLoaded and never reloaded);
//   * the name of every human player is rewritten with the cleaned nickname on round_prestart, one
//     second after every player_spawn, and when a client authorizes (ApplyCleanName also guards
//     against a slot that changed hands between the authorization and the name write).
//
// Port notes:
//   * IFileSystem reads -> pisex::ReadFile(pisex::ResolvePath(...)): the ported paths
//     ("addons/configs/nickname_cleaner/whitelist.txt") resolve to <configs>/nickname_cleaner/.
//   * s2sdk's OnClientAuthenticated (behind pisex::HookOnClientAuthorized) is not delivered for
//     every client on a live server, so a once-per-second reconcile applies the cleaner to the
//     connected human players exactly like an authorization would; ApplyCleanName() is a no-op for
//     a name that is already clean, so the repeated pass cannot rewrite a name twice.
//   * No client is queried before a map exists: the hooks run from events/timers and the reconcile
//     from OnPluginUpdate (both imply a running map).
//   * ApplyCleanName() only touches a slot that has a connected client *and* a controller entity:
//     s2sdk's Player::SetName dereferences both without a null check (that dereference was the
//     2026-10-08 SIGSEGV, hit by the player_spawn timer in the changelevel window), and the
//     original's CCSPlayerController::FromSlot()-based getter/setter pair was a no-op for such
//     slots anyway. s2sdk's "<unknown>" name placeholder is treated as "no name" for the same
//     reason.

#include "nickname_cleaner_core.hpp"

#include <pisex/pisex.hpp>

#include <plg/plugin.hpp>
#include <plg/source_location.hpp>
#include <plg/string.hpp>
#include <plugin_export.h>
#include <s2sdk/s2sdk.hpp>

#include <chrono>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>

namespace {

constexpr const char* kLogTag = "[nickname_cleaner]";
constexpr int kMaxSlots = 64;
constexpr auto kReconcileInterval = std::chrono::seconds(1);

constexpr const char* kSettingsPath = "addons/configs/nickname_cleaner/settings.ini";
constexpr const char* kWhitelistPath = "addons/configs/nickname_cleaner/whitelist.txt";
constexpr const char* kBlocklistPath = "addons/configs/nickname_cleaner/blocklist.txt";
constexpr const char* kDefaultDefaultName = "Player";

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

private:
	void LoadConfig();
	// Reads one of the two list files (the original LoadWhitelist/LoadBlocklist).
	void LoadListFile(const char* path, bool blocklist);
	void ApplyCleanNameForAllPlayers();
	// returns true when the name was rewritten (the original returned before SetPlayerName when
	// the cleaned name equals the current one).
	bool ApplyCleanName(int iSlot, uint64_t iSteamID64);
	void ReconcileSlots();

	nickname_cleaner::NameLists m_lists;
	std::string m_defaultName = kDefaultDefaultName;
	std::chrono::milliseconds m_sinceReconcile{0};
};

NicknameCleanerPlugin g_nicknameCleanerPlugin;

// --- configuration ---------------------------------------------------------------------------

void NicknameCleanerPlugin::LoadConfig() {
	pisex::KeyValues kv("NicknameCleaner");
	if (!kv.LoadFromFile(kSettingsPath)) {
		LogError(std::string(kLogTag) + " failed to load " + kSettingsPath + " - keeping DefaultName '" +
				 m_defaultName + "'");
		return;
	}
	m_defaultName = kv.GetString("DefaultName", kDefaultDefaultName);
}

void NicknameCleanerPlugin::LoadListFile(const char* path, bool blocklist) {
	const std::string resolved = pisex::ResolvePath(path);
	bool ok = false;
	const std::string content = pisex::ReadFile(resolved, &ok);
	if (!ok) {
		LogError(std::string(kLogTag) + " failed to open " + path + ", list empty");
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

// --- name cleaning ---------------------------------------------------------------------------

bool NicknameCleanerPlugin::ApplyCleanName(int iSlot, uint64_t iSteamID64) {
	if (iSlot < 0 || iSlot >= kMaxSlots) {
		return false;
	}

	// The slot must hold a *connected client* whose controller entity exists at this instant.
	// s2sdk's name write (Player::SetName, player_manager.cpp) dereferences both - the client to
	// get its slot and the controller to write m_iszPlayerName - without a null check, so a slot
	// without a client or without a controller (an empty slot of the server, a client still
	// connecting, a slot in the changelevel window when the map-scoped objects are re-created)
	// crashes the server inside s2sdk. Both checks are silent and are exactly the original's guard:
	// with no CCSPlayerController::FromSlot(slot) its GetPlayerName returned "" and its
	// SetPlayerName did nothing.
	if (!pisex::g_pPlayers->IsConnected(iSlot) || pisex::GetControllerEntity(iSlot) == 0) {
		return false;
	}

	// IsFakeClient answers *false* for a slot whose client object does not exist (s2sdk:
	// "client && client->IsFakeClient()"), so the connectivity check above has to come first or a
	// bot slot in that state would walk into the name write.
	if (pisex::g_pPlayers->IsFakeClient(iSlot)) {
		return false;
	}

	const uint64_t currentSteamId = pisex::g_pPlayers->GetSteamID64(iSlot);
	if (currentSteamId != 0 && iSteamID64 != 0 && currentSteamId != iSteamID64) {
		return false;
	}

	const char* szName = pisex::g_pPlayers->GetPlayerName(iSlot);
	// A null/empty name is the original's "no controller" answer; "<unknown>" is the placeholder
	// s2sdk's Player::GetName() returns for a slot whose client is gone, which the original would
	// have reported as "" - both mean "this slot has no name to clean".
	if (nickname_cleaner::IsPlaceholderName(szName)) {
		return false;
	}

	const std::string cleaned = nickname_cleaner::CleanNickname(szName, m_lists, m_defaultName);
	if (cleaned == szName) {
		return false;
	}
	pisex::g_pPlayers->SetPlayerName(iSlot, cleaned.c_str());
	return true;
}

void NicknameCleanerPlugin::ApplyCleanNameForAllPlayers() {
	for (int iSlot = 0; iSlot < kMaxSlots; ++iSlot) {
		try {
			// ApplyCleanName uses the neutral player wrappers outside of a map (they return 0/"")
			// and skips bots, exactly like the original's IsFakeClient guard.
			ApplyCleanName(iSlot, 0);
		} catch (const std::exception& ex) {
			LogError(std::string(kLogTag) + " cleaning slot " + std::to_string(iSlot) + " failed: " + ex.what());
		} catch (...) {
			LogError(std::string(kLogTag) + " cleaning slot " + std::to_string(iSlot) + " failed");
		}
	}
}

void NicknameCleanerPlugin::ReconcileSlots() {
	if (!pisex::IsGameReady()) {
		return;
	}
	for (int iSlot = 0; iSlot < kMaxSlots; ++iSlot) {
		try {
			if (!pisex::g_pPlayers->IsInGame(iSlot) || pisex::g_pPlayers->IsFakeClient(iSlot)) {
				continue;
			}
			// The same call the authorization hook makes; the steamid guard inside is skipped
			// (0 = "any slot occupant"), which is what the original's round_prestart pass did.
			ApplyCleanName(iSlot, 0);
		} catch (const std::exception& ex) {
			LogError(std::string(kLogTag) + " reconcile of slot " + std::to_string(iSlot) + " failed: " + ex.what());
		} catch (...) {
			LogError(std::string(kLogTag) + " reconcile of slot " + std::to_string(iSlot) + " failed");
		}
	}
}

// --- lifecycle --------------------------------------------------------------------------------

plg::PluginResult NicknameCleanerPlugin::OnPluginStart() {
	try {
		pisex::Init("nickname_cleaner");

		LoadConfig();
		LoadListFile(kWhitelistPath, false);
		LoadListFile(kBlocklistPath, true);
		LogInfo(std::string(kLogTag) + " loaded " + std::to_string(m_lists.whitelist.size()) + " whitelist / " +
				std::to_string(m_lists.blocklist.size()) + " blocklist entries");

		// The three application points of the original AllPluginsLoaded().
		pisex::g_pUtils->HookEvent("round_prestart", [](const char*, pisex::IGameEvent*, bool) {
			g_nicknameCleanerPlugin.ApplyCleanNameForAllPlayers();
		});
		pisex::g_pUtils->HookEvent("player_spawn", [](const char*, pisex::IGameEvent*, bool) {
			pisex::g_pUtils->CreateTimer(1.0f, []() -> float {
				g_nicknameCleanerPlugin.ApplyCleanNameForAllPlayers();
				return -1.0f; // one shot, like the original
			});
		});
		pisex::g_pPlayers->HookOnClientAuthorized([](int iSlot, uint64_t iSteamID64) {
			g_nicknameCleanerPlugin.ApplyCleanName(iSlot, iSteamID64);
		});
	} catch (const std::exception& ex) {
		LogError(std::string(kLogTag) + " OnPluginStart failed: " + ex.what());
	} catch (...) {
		LogError(std::string(kLogTag) + " OnPluginStart failed with an unknown exception");
	}
	return {}; // never Failed: that stops every plugify plugin
}

plg::PluginResult NicknameCleanerPlugin::OnPluginUpdate(std::chrono::milliseconds dt) {
	try {
		pisex::Poll();
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
		pisex::Shutdown();
	} catch (const std::exception& ex) {
		LogError(std::string(kLogTag) + " OnPluginEnd failed: " + ex.what());
	} catch (...) {
		LogError(std::string(kLogTag) + " OnPluginEnd failed with an unknown exception");
	}
	return {};
}

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_nicknameCleanerPlugin)
