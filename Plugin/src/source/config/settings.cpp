#include "config/settings.h"

#include "utils/log.h"

#include <Windows.h>

#include <algorithm>
#include <cstdlib>

namespace sof::config {

	namespace {

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
		}

		void LoadProbe(const char* iniPath, Settings& settings) {
			ProbeSettings& probe = settings.probe;

			const float probeHeight = ReadFloat(iniPath, "Probe", "fProbeHeight", probe.probeHeight);
			probe.probeHeight = std::clamp(probeHeight, 32.0f, 4096.0f);

			const float budget = ReadFloat(iniPath, "Probe", "fBudget", probe.budget);
			probe.budget = std::clamp(budget, 0.0f, 5.0f);
		}

		void LoadDebug(const char* iniPath, Settings& settings) {
			DebugSettings& debug = settings.debug;

			debug.reloadKey = ReadInt(iniPath, "Debug", "iReloadKey", debug.reloadKey);
		}

		void LogSettings(const Settings& settings) {
			SOF_LOG("Config general: Enabled=%d", settings.general.enabled);
			SOF_LOG("Config probe: ProbeHeight=%.0f Budget=%.2fms", settings.probe.probeHeight, settings.probe.budget);
			SOF_LOG("Config debug: ReloadKey=0x%X", settings.debug.reloadKey);
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

		SOF_LOG("Config loaded from '%s'.", g_iniPath.c_str());
		LogSettings(g_settings);
	}

	// Reads the INI from `Load` again, makes its settings current and logs them.
	//
	// Thread: Main
	void Reload() {
		g_settings = ReadSettings();

		SOF_LOG("Config reloaded from '%s'.", g_iniPath.c_str());
		LogSettings(g_settings);
	}

	// Returns the current settings. A reload changes them in place.
	//
	// Thread: Main
	const Settings& Get() {
		return g_settings;
	}

}
