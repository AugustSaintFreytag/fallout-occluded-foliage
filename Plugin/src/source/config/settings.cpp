#include "config/settings.h"

#include "utils/log.h"

#include <Windows.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace sof::config {

	namespace {

		constexpr const char* kFormsFileName = "OccludedFoliageForms.ini";

		// Thread: Main
		Settings g_settings;
		std::string g_iniPath;

		bool ReadBool(const char* iniPath, const char* section, const char* key, bool fallback) {
			const UINT value = GetPrivateProfileIntA(section, key, static_cast<INT>(fallback), iniPath);

			return value != 0;
		}

		float ReadFloat(const char* iniPath, const char* section, const char* key, float fallback) {
			char text[32];
			GetPrivateProfileStringA(section, key, "", text, sizeof(text), iniPath);

			char* parseEnd = nullptr;
			const float value = std::strtof(text, &parseEnd);

			if (parseEnd == text) {
				return fallback;
			}

			return value;
		}

		// Reads an integer, decimal or `0x` hex. Returns the fallback if the key is missing or not a number.
		//
		// `GetPrivateProfileInt` reads only decimal values.
		int ReadInt(const char* iniPath, const char* section, const char* key, int fallback) {
			char text[32];
			GetPrivateProfileStringA(section, key, "", text, sizeof(text), iniPath);

			char* parseEnd = nullptr;
			const long value = std::strtol(text, &parseEnd, 0);

			if (parseEnd == text) {
				return fallback;
			}

			return static_cast<int>(value);
		}

		void LoadGeneral(const char* iniPath, Settings& settings) {
			GeneralSettings& general = settings.general;

			general.enabled = ReadBool(iniPath, "General", "bEnabled", general.enabled);
			general.toggleKey = ReadInt(iniPath, "General", "iToggleKey", general.toggleKey);
		}

		void LoadProbe(const char* iniPath, Settings& settings) {
			ProbeSettings& probe = settings.probe;

			const float probeHeight = ReadFloat(iniPath, "Probe", "fProbeHeight", probe.probeHeight);
			probe.probeHeight = std::clamp(probeHeight, 32.0f, 4096.0f);

			const float syncBudget = ReadFloat(iniPath, "Probe", "fSyncBudget", probe.syncBudget);
			probe.syncBudget = std::clamp(syncBudget, 0.0f, 5.0f);

			probe.async = ReadBool(iniPath, "Probe", "bAsync", probe.async);

			const float asyncBudget = ReadFloat(iniPath, "Probe", "fAsyncBudget", probe.asyncBudget);
			probe.asyncBudget = std::clamp(asyncBudget, 0.0f, 12.0f);

			const int batchSize = ReadInt(iniPath, "Probe", "iBatchSize", static_cast<int>(probe.batchSize));
			probe.batchSize = static_cast<std::uint32_t>(std::clamp(batchSize, 1, 512));

			const float radius = ReadFloat(iniPath, "Probe", "fRadius", probe.radius);
			probe.radius = std::clamp(radius, 512.0f, 16384.0f);
		}

		void LoadDebug(const char* iniPath, Settings& settings) {
			DebugSettings& debug = settings.debug;

			debug.reloadKey = ReadInt(iniPath, "Debug", "iReloadKey", debug.reloadKey);
			debug.probeKey = ReadInt(iniPath, "Debug", "iProbeKey", debug.probeKey);
			debug.grassCensusKey = ReadInt(iniPath, "Debug", "iGrassCensusKey", debug.grassCensusKey);
			debug.grassHideTestKey = ReadInt(iniPath, "Debug", "iGrassHideTestKey", debug.grassHideTestKey);
			debug.logMap = ReadBool(iniPath, "Debug", "bLogMap", debug.logMap);
			debug.probePhysics = ReadBool(iniPath, "Debug", "bProbePhysics", debug.probePhysics);

			const float physicsProbeDelay = ReadFloat(iniPath, "Debug", "fPhysicsProbeDelay", debug.physicsProbeDelay);
			debug.physicsProbeDelay = std::clamp(physicsProbeDelay, 0.0f, 50.0f);
		}

		// Reads the INI from `Load` into fresh settings. A missing key keeps its default.
		Settings ReadSettings() {
			Settings settings;

			LoadGeneral(g_iniPath.c_str(), settings);
			LoadProbe(g_iniPath.c_str(), settings);
			LoadDebug(g_iniPath.c_str(), settings);

			return settings;
		}

	}

	// Reads the INI at the given path, makes its settings current and logs them.
	// Remembers the path for `Reload`.
	//
	// Thread: Main (load time only)
	void Load(const std::string& iniPath) {
		g_iniPath = iniPath;
		g_settings = ReadSettings();
		SOF_LOG("Loaded config from path '%s'.", g_iniPath.c_str());
	}

	// Reads the INI from `Load` again, makes its settings current and logs them.
	//
	// Thread: Main
	void Reload() {
		g_settings = ReadSettings();
		SOF_LOG("Reloaded config from path '%s'.", g_iniPath.c_str());
	}

	// Flips `[General] bEnabled` in the current settings. The next reload sets it from the INI again.
	// Returns the new state.
	//
	// Thread: Main
	bool ToggleEnabled() {
		g_settings.general.enabled = !g_settings.general.enabled;
		SOF_LOG("Toggled %s.", g_settings.general.enabled ? "on" : "off");

		return g_settings.general.enabled;
	}

	// Reads the editor IDs in section `[OccluderBlacklist]` of `OccludedFoliageForms.ini`, next to the main INI.
	// One editor ID per line. Text after `;`, `#` or `=` is ignored.
	//
	// Thread: Main
	std::vector<std::string> ReadOccluderBlacklist() {
		const std::size_t directoryEnd = g_iniPath.find_last_of("\\/");
		const std::string formsPath =
			(directoryEnd == std::string::npos ? std::string() : g_iniPath.substr(0, directoryEnd + 1)) + kFormsFileName;

		std::vector<char> buffer(32767);
		const DWORD length = GetPrivateProfileSectionA("OccluderBlacklist", buffer.data(), static_cast<DWORD>(buffer.size()),
			formsPath.c_str());

		std::vector<std::string> editorIDs;

		for (const char* line = buffer.data(); line < buffer.data() + length && *line; line += std::strlen(line) + 1) {
			std::string entry(line);
			entry = entry.substr(0, entry.find_first_of(";#="));

			const std::size_t first = entry.find_first_not_of(" \t");
			const std::size_t last = entry.find_last_not_of(" \t");

			if (first != std::string::npos) {
				editorIDs.push_back(entry.substr(first, last - first + 1));
			}
		}

		return editorIDs;
	}

	// Returns the current settings. A reload changes them in place.
	//
	// Thread: Main
	const Settings& Get() {
		return g_settings;
	}

}
