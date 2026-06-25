package frameflow.host

import kotlin.test.assertEquals
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertTrue
import org.junit.jupiter.api.Test
import java.nio.file.Files
import java.nio.file.Path

class FrameflowHostPrototypeIntegrationTest {
    @Test
    fun loadsNativeLibraryChecksAbiCreatesEngineDispatchesCallbacksAndDisposes() {
        val nativeLibraryPath = resolveNativeLibraryPath()
        assertTrue(Files.isRegularFile(nativeLibraryPath), "Native library must exist at $nativeLibraryPath")

        val host = FrameflowHostPrototype.load(nativeLibraryPath)
        host.use {
            val runtimeInfo = it.runtimeInfo()
            assertEquals(2, runtimeInfo.bridgeAbiVersion)
            assertEquals(2, runtimeInfo.commandVersionMajor)

            val compatibilityInfo = it.checkCompatibility()
            assertEquals(2, compatibilityInfo.bridgeAbiVersion)
            assertEquals(2, compatibilityInfo.commandVersionMajor)

            it.start()
            assertEquals(1, it.dispatchNativeCallbacks())

            val readyState = it.state
            assertEquals(FrameflowHostStatus.Empty, readyState.status)
            assertEquals(1, readyState.readyEventCount)
            assertEquals(0, readyState.pointSnapshotCount)
            assertNotNull(readyState.lastReady)
            assertEquals(2, readyState.lastReady.runtimeInfo.bridgeAbiVersion)
            assertEquals(2, readyState.lastReady.runtimeInfo.commandVersionMajor)

            it.updateFilters(
                FrameflowFilterSnapshot(
                    query = "election",
                    categoryCode = "POLITICS",
                    locationId = 601,
                    countryCode = "RS",
                ),
            )
            it.replacePoints(
                listOf(
                    RenderReadyPoint(
                        locationId = 601,
                        label = "Belgrade",
                        kind = FrameflowLocationKind.CITY,
                        countryCode = "RS",
                        latitude = 44.7866,
                        longitude = 20.4489,
                        storyCount = 8,
                        latestStoryEpochMillis = 1_776_335_400_000L,
                        topCategories = listOf("POLITICS", "CRIME"),
                        styleKey = "category-politics",
                    ),
                ),
            )
            val populatedState = it.state
            assertEquals(FrameflowHostStatus.Ready, populatedState.status)
            assertEquals(1, populatedState.pointSnapshotCount)
            assertEquals("POLITICS", populatedState.activeFilters.categoryCode)
            assertEquals(601L, populatedState.activeFilters.locationId)

            it.focusLocation(601)
            assertEquals(1, it.dispatchNativeCallbacks())
            assertTrue(it.hasSelectedLocationId())
            assertEquals(601L, it.selectedLocationId())

            it.selectLocation(
                locationId = 601,
                categoryCode = "POLITICS",
                screenX = 1200.0,
                screenY = 440.0,
                interaction = "click",
            )
            assertEquals(1, it.dispatchNativeCallbacks())

            val selectionState = it.state
            assertEquals(2, selectionState.selectionEventCount)
            assertEquals(601L, selectionState.selectedLocationId)
            assertEquals("click", selectionState.lastLocationSelection?.interaction)
            assertEquals("POLITICS", selectionState.lastLocationSelection?.categoryCode)
            assertEquals(1200.0, selectionState.lastLocationSelection?.screenX)
            assertEquals(440.0, selectionState.lastLocationSelection?.screenY)
            assertTrue(it.diagnosticsSummary().contains("first_style_key=category-politics"))
            assertTrue(it.diagnosticsSummary().contains("first_top_category=POLITICS"))

            it.selectCluster(
                clusterId = 77,
                pointCount = 2,
                longitude = 20.1412,
                latitude = 45.02685,
            )
            assertEquals(1, it.dispatchNativeCallbacks())

            val clusterState = it.state
            assertEquals(1, clusterState.clusterSelectionEventCount)
            assertNull(clusterState.selectedLocationId)
            assertNull(clusterState.lastLocationSelection)
            assertEquals(77L, clusterState.lastClusterSelection?.clusterId)
            assertEquals(2, clusterState.lastClusterSelection?.pointCount)

            it.reportCameraChanged(
                FrameflowCameraState(
                    longitude = 20.8,
                    latitude = 44.9,
                    heightMeters = 2_200_000.0,
                    headingDegrees = 5.0,
                    pitchDegrees = -40.0,
                    rollDegrees = 0.0,
                ),
            )
            it.reportCameraChanged(
                FrameflowCameraState(
                    longitude = 20.9,
                    latitude = 44.95,
                    heightMeters = 2_000_000.0,
                    headingDegrees = 10.0,
                    pitchDegrees = -35.0,
                    rollDegrees = 0.0,
                ),
            )
            it.reportCameraChanged(
                FrameflowCameraState(
                    longitude = 21.0,
                    latitude = 45.0,
                    heightMeters = 1_800_000.0,
                    headingDegrees = 15.0,
                    pitchDegrees = -30.0,
                    rollDegrees = 0.0,
                ),
            )
            assertEquals(1, it.dispatchNativeCallbacks())

            val cameraState = it.state
            assertEquals(1, cameraState.cameraEventCount)
            assertEquals(21.0, cameraState.cameraState?.longitude)
            assertEquals(1_800_000.0, cameraState.cameraState?.heightMeters)
            assertTrue(it.diagnosticsSummary().contains("camera_longitude_degrees=21"))
            assertTrue(it.diagnosticsSummary().contains("coalesced_camera_events=2"))

            it.clearSelection()
            val clearedSelectionState = it.state
            assertEquals(FrameflowHostStatus.Ready, clearedSelectionState.status)
            assertNull(clearedSelectionState.selectedLocationId)

            it.replacePoints(emptyList())
            val emptyState = it.state
            assertEquals(FrameflowHostStatus.Empty, emptyState.status)
            assertEquals(0, emptyState.pointSnapshotCount)
            assertNull(emptyState.selectedLocationId)
        }

        assertEquals(FrameflowHostStatus.Disposed, host.state.status)
    }

    private fun resolveNativeLibraryPath(): Path {
        val configured = System.getProperty("frameflow.native.lib")
            ?: error("frameflow.native.lib system property must be set by the test task")
        return Path.of(configured).toAbsolutePath().normalize()
    }
}
