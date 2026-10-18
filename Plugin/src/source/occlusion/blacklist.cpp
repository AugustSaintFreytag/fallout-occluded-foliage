// Statics that never occlude, listed by editor ID in `[OccluderBlacklist]` of `OccludedFoliageForms.ini`.
//
// Editor IDs resolve to base form IDs once the game data is loaded. Most need JohnnyGuitar NVSE.
// A changed blacklist on reload clears the occlusion maps and brings back all hidden grass.
// The maps then build again with the new blacklist.

#include "occlusion/blacklist.h"

#include "config/settings.h"
#include "engine/objects.h"
#include "occlusion/builder.h"
#include "occlusion/hider.h"
#include "probe/probe.h"
#include "utils/formatting.h"
#include "utils/log.h"

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace sof::occlusion {

	namespace {

		// Editor IDs without a form listed in the log.
		constexpr std::size_t kMaxLoggedMissing = 16;

		// Thread: Main
		bool g_loaded = false;
		std::unordered_set<std::uint32_t> g_current;

	}

	// Reads and resolves the occluder blacklist and hands it to the probe.
	// Call once the game data is loaded (NVSE `DeferredInit`) and after each config reload.
	// If the resolved set changed after the first load, clears the maps and restores hidden grass first.
	//
	// Thread: Main
	void LoadOccluderBlacklist() {
		const std::vector<std::string> editorIDs = config::ReadOccluderBlacklist();
		std::unordered_set<std::uint32_t> baseFormIDs;
		std::size_t missing = 0;

		for (const std::string& editorID : editorIDs) {
			void* form = engine::GetFormByEditorID(editorID.c_str());

			if (form) {
				baseFormIDs.insert(engine::GetFormID(form));
				continue;
			}

			if (++missing <= kMaxLoggedMissing) {
				SOF_LOG("[Blacklist] No form with editor ID '%s', skipped.", editorID.c_str());
			}
		}

		if (g_loaded && baseFormIDs == g_current) {
			return;
		}

		SOF_LOG("[Blacklist] Resolved %s of %s editor IDs.", format::Count(baseFormIDs.size()).c_str(),
			format::Count(editorIDs.size()).c_str());

		// The reset pauses the async worker before the probe's set changes
		if (g_loaded) {
			ResetBuilder();
			RestoreHiddenGrass();
			SOF_LOG("[Blacklist] Changed, cleared the occlusion maps.");
		}

		g_current = baseFormIDs;
		probe::SetOccluderBlacklist(std::move(baseFormIDs));
		g_loaded = true;
	}

}
