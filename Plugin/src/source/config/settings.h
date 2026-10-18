#pragma once

#include <cstdint>
#include <string>

namespace sof::config {

	// Library

	struct GeneralSettings {
		bool enabled = true;
	};

	struct ProbeSettings {
		// Height at which to raycast downwards (game units, 1=~7.3 m)
		float probeHeight = 512.0f; 

		// Budget to spend on probes per frame in sync mode (main thread) (ms)
		// Note: Directly adds to frame time as rendering is suspended while probing runs.
		float syncBudget = 0.25f;

		// Defer raycasting to a worker thread in paced bursts.
		// Experimental, uses Havok access lock but may crash unexpectedly.
		bool async = false;
		
		// Budget to spend on probes per frame in async mode (worker thread) (ms).
		// Note: Does not add to frame time, only physics are paused while probing runs.
		// Set to 0.0 to always run full queue without budgeting.
		float asyncBudget = 4.0f;
		
		// Number of probes to perform per allocated burst.
		// Higher batches have less overhead and get more done but may lock for longer.
		// Lower batches are more easily paced but slows overall progress.
		std::uint32_t batchSize = 16;
		
		// Total radius for cells to probe and affect (game units, 4096 = 1 cell).
		float radius = 8192.0f;
	};

	struct DebugSettings {
		// Keybind that reloads the config
		// Virtual-key code (VK_INSERT), 0 = no key
		int reloadKey = 0x2D;

		// Keybind that probes the grid point under the crosshair and logs every hit
		// Virtual-key code (VK_HOME), 0 = no key
		int probeKey = 0x24;

		// Keybind that logs every grass geometry with its instance record
		// Virtual-key code (VK_PRIOR), 0 = no key
		int grassCensusKey = 0x21;

		// Keybind that hides the instances of the nearest grass geometry, and restores them on the next press
		// Virtual-key code (VK_NEXT), 0 = no key
		int grassHideTestKey = 0x22;

		// Log cell probing status individually and a regular status report.
		bool logMap = false;

		// Log threads handling Havok physics ticks, time spent, and interference.
		bool probePhysics = false;

		// Force stall physics worker thread in intervals, for debugging (ms).
		// Use 0.0 to disable.
		float physicsProbeDelay = 0.0f;
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
