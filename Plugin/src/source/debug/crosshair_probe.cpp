#include "debug/crosshair_probe.h"

#include "config/settings.h"
#include "engine/havok.h"
#include "engine/objects.h"
#include "engine/scene.h"
#include "engine/terrain.h"
#include "engine/ui.h"
#include "occlusion/map.h"
#include "probe/probe.h"
#include "utils/log.h"
#include "utils/timing.h"
#include "utils/vector3.h"

#include <cstdint>
#include <cstdio>

namespace sof::debug {

	namespace {

		// Length of the view ray that finds the point under the crosshair.
		constexpr float kAimRange = 8192.0f;  // Game units

		const char* PointStateName(occlusion::PointState state) {
			switch (state) {
			case occlusion::PointState::Clear:
				return "clear";

			case occlusion::PointState::Occluded:
				return "occluded";

			default:
				return "unknown";
			}
		}

		const char* HitKindName(probe::HitKind kind) {
			switch (kind) {
			case probe::HitKind::Passed:
				return "passed";

			case probe::HitKind::Occluder:
				return "OCCLUDER";

			case probe::HitKind::Terrain:
				return "terrain";

			default:
				return "?";
			}
		}

		// Writes the reference, base form and model of a given reference to the given buffer.
		void DescribeReference(void* reference, char* buffer, std::size_t size) {
			if (!reference) {
				std::snprintf(buffer, size, "no reference");

				return;
			}

			void* baseForm = engine::GetBaseForm(reference);
			const std::uint8_t baseType = engine::GetFormType(baseForm);

			std::snprintf(buffer, size, "Ref=%08X '%s' Base=%s(%02X) %08X '%s' Model=\"%s\"",
				engine::GetFormID(reference), engine::GetEditorID(reference), engine::FormTypeName(baseType), baseType,
				engine::GetFormID(baseForm), engine::GetEditorID(baseForm), engine::GetModelPath(baseForm));
		}

		void LogTraceHit(const probe::TraceHit& traceHit) {
			char referenceText[320];
			DescribeReference(traceHit.hit.reference, referenceText, sizeof(referenceText));

			SOF_LOG("[Probe]   %-4s %+7.1f above terrain, Layer %u (%s), %s, %s", traceHit.upward ? "Up" : "Down",
				traceHit.heightAboveTerrain, traceHit.hit.layer, engine::CollisionLayerName(traceHit.hit.layer),
				HitKindName(traceHit.kind), referenceText);
		}

	}

	// Probes the grid point under the crosshair and logs every hit of the probe rays.
	// Logs the verdict, the deciding occluder, the casts and the time of a probe without logging.
	// Shows the verdict as a corner message.
	//
	// The point is where a view ray from the camera first hits anything.
	// The probe runs at that point's position, as it would for a grid point there.
	//
	// Thread: Main
	void RunCrosshairProbe() {
		Vector3 camera;
		Vector3 forward;

		if (!engine::GetCameraTransform(camera, forward)) {
			SOF_LOG("[Probe] No camera.");

			return;
		}

		const engine::RaycastResult aim = engine::CastRay(camera, camera + forward * kAimRange, engine::kLayer_LineOfSight);

		if (!aim.hit) {
			SOF_LOG("[Probe] Nothing under the crosshair within %.0f units.", kAimRange);
			engine::ShowNotification("Probe: nothing under the crosshair");

			return;
		}

		char aimText[320];
		DescribeReference(aim.reference, aimText, sizeof(aimText));

		const float probeHeight = config::Get().probe.probeHeight;

		SOF_LOG("[Probe] Point (%.1f, %.1f, %.1f), Layer %u (%s), %s. Probe height %.0f:", aim.point.x, aim.point.y,
			aim.point.z, aim.layer, engine::CollisionLayerName(aim.layer), aimText, probeHeight);

		void* cell = engine::GetParentCell(engine::GetPlayer());
		float terrainHeight = 0.0f;

		if (!cell || !engine::GetLandHeight(aim.point, terrainHeight)) {
			SOF_LOG("[Probe] No land under the point.");
			engine::ShowNotification("Probe: NoTerrain");

			return;
		}

		const probe::ProbeResult result = probe::ProbePoint(cell, aim.point.x, aim.point.y, terrainHeight, probeHeight, LogTraceHit);

		// Time a second probe without the trace. Log writes in the trace take longer than the casts.
		const std::int64_t startTime = timing::Now();
		probe::ProbePoint(cell, aim.point.x, aim.point.y, terrainHeight, probeHeight);
		const double microseconds = timing::TicksToMicroseconds(static_cast<double>(timing::Now() - startTime));

		// Compare the land height with the interpolated cache of the map.
		const occlusion::GridPoint gridPoint = occlusion::ToGridPoint(aim.point.x, aim.point.y);
		const occlusion::CellMap* map = occlusion::FindCellMap(occlusion::CellOf(gridPoint.x), occlusion::CellOf(gridPoint.y));
		float cachedHeight = 0.0f;

		if (map && occlusion::LandHeightAt(*map, aim.point.x, aim.point.y, cachedHeight)) {
			SOF_LOG("[Probe] Land height %.1f, cached %.1f (%+.1f).", terrainHeight, cachedHeight, cachedHeight - terrainHeight);
		}

		char occluderText[320];
		DescribeReference(result.occluder, occluderText, sizeof(occluderText));

		SOF_LOG("[Probe] %s%s, terrain at %.1f, %d casts, %.1f us. Map: %s. Occluder: %s", probe::DescriptionForVerdict(result.verdict),
			result.foundByUpRay ? " (up ray)" : "", result.terrainHeight, result.casts, microseconds,
			PointStateName(occlusion::Query(aim.point.x, aim.point.y)), occluderText);

		char notification[96];
		std::snprintf(notification, sizeof(notification), "Probe: %s%s", probe::DescriptionForVerdict(result.verdict),
			result.foundByUpRay ? " (up ray)" : "");
		engine::ShowNotification(notification);
	}

}
