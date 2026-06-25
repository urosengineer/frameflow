package frameflow.host

import kotlin.test.assertEquals
import kotlin.test.assertIs
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertTrue
import org.junit.jupiter.api.Test
import java.nio.file.Path

class FrameflowHostSurfaceControllerTest {
    @Test
    fun remountsFromEmbeddedToFullscreenWhilePreservingSharedFiltersAndCamera() {
        val controller = FrameflowHostSurfaceController(
            FrameflowHostSurfaceConfig(
                packagedNativeDir = resolveNativeLibraryPath().parent,
            ),
        )

        controller.use {
            val embedded = FrameflowSurfaceMount(
                surfaceHandle = 101L,
                width = 1280,
                height = 720,
                scaleFactor = 1.0,
                mode = FrameflowSurfaceMode.Embedded,
            )
            assertIs<FrameflowHostBootstrapResult.Started>(it.mount(embedded))
            assertEquals(embedded, it.activeSurface)
            assertEquals(1, it.activeMountSequence)
            assertEquals(FrameflowHostStatus.Empty, it.state.status)

            val filters = FrameflowFilterSnapshot(
                query = "election",
                categoryCode = "POLITICS",
                locationId = 601,
                countryCode = "RS",
            )
            it.updateFilters(filters)
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
                        topCategories = listOf("POLITICS"),
                        styleKey = "category-politics",
                    ),
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
            it.focusLocation(601)
            assertEquals(1, it.dispatchNativeCallbacks())

            val fullscreen = FrameflowSurfaceMount(
                surfaceHandle = 202L,
                width = 1920,
                height = 1080,
                scaleFactor = 1.25,
                mode = FrameflowSurfaceMode.Fullscreen,
            )
            assertIs<FrameflowHostBootstrapResult.Started>(it.mount(fullscreen))

            val state = it.state
            assertEquals(fullscreen, it.activeSurface)
            assertEquals(2, it.activeMountSequence)
            assertEquals(FrameflowHostStatus.Empty, state.status)
            assertEquals(0, state.pointSnapshotCount)
            assertEquals(filters, state.activeFilters)
            assertNull(state.selectedLocationId)
            assertNull(state.lastLocationSelection)
            assertNotNull(state.cameraState)
            assertEquals(21.0, state.cameraState?.longitude)
        }
    }

    @Test
    fun resizePauseAndResumeFlowThroughMountedSurfaceWrapper() {
        val controller = FrameflowHostSurfaceController(
            FrameflowHostSurfaceConfig(
                packagedNativeDir = resolveNativeLibraryPath().parent,
            ),
        )

        controller.use {
            assertIs<FrameflowHostBootstrapResult.Started>(
                it.mount(
                    FrameflowSurfaceMount(
                        surfaceHandle = 303L,
                        width = 1280,
                        height = 720,
                        scaleFactor = 1.0,
                    ),
                ),
            )

            it.resize(width = 1440, height = 810, scaleFactor = 1.5)
            assertEquals(1440, it.activeSurface?.width)
            assertEquals(810, it.activeSurface?.height)
            assertEquals(1.5, it.activeSurface?.scaleFactor)
            assertTrue(it.state.diagnosticsSummary?.contains("width=1440") == true)
            assertTrue(it.state.diagnosticsSummary?.contains("height=810") == true)

            it.pause()
            assertTrue(it.state.diagnosticsSummary?.contains("state=PAUSED") == true)

            it.resume()
            assertTrue(it.state.diagnosticsSummary?.contains("state=READY") == true)
        }
    }

    private fun resolveNativeLibraryPath(): Path {
        val configured = System.getProperty("frameflow.native.lib")
            ?: error("frameflow.native.lib system property must be set by the test task")
        return Path.of(configured).toAbsolutePath().normalize()
    }
}
