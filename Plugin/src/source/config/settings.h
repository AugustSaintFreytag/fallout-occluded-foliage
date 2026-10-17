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
	};

	struct DebugSettings {
		// Key that reloads the config
		// Virtual-key code (VK_INSERT), 0 = no key
		int reloadKey = 0x2D;

		// Key that probes the grid point under the crosshair and logs every hit
		// Virtual-key code (VK_HOME), 0 = no key
		int probeKey = 0x24;
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
