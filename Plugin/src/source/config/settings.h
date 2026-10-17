#pragma once

#include <string>

namespace sof::config {

	// Library

	struct GeneralSettings {
		bool enabled = true;
	};

	struct ProbeSettings {
		float probeHeight = 512.0f;  // Game units, ~7.3 m
		float budget = 0.25f;  // Milliseconds per frame
		float radius = 8192.0f;  // Game units around the player, 2 cells
	};

	struct DebugSettings {
		// Key that reloads the config
		// Virtual-key code (VK_INSERT), 0 = no key
		int reloadKey = 0x2D;

		// Key that probes the grid point under the crosshair and logs every hit
		// Virtual-key code (VK_HOME), 0 = no key
		int probeKey = 0x24;

		// Key that logs every grass geometry with its instance record
		// Virtual-key code (VK_PRIOR), 0 = no key
		int grassCensusKey = 0x21;

		// Key that hides the instances of the nearest grass geometry, and restores them on the next press
		// Virtual-key code (VK_NEXT), 0 = no key
		int grassHideTestKey = 0x22;

		// Logs cells as they become ready and complete, and a status line every 10 seconds
		bool logMap = false;
	};

	struct Settings {
		GeneralSettings general;
		ProbeSettings probe;
		DebugSettings debug;
	};

	// Lifecycle

	void Load(const std::string& iniPath);

	void Reload();

	const Settings& Get();

}
