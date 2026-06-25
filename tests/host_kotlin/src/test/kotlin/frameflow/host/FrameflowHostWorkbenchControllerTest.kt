package frameflow.host

import kotlin.test.assertEquals
import kotlin.test.assertIs
import kotlin.test.assertNull
import kotlin.test.assertTrue
import org.junit.jupiter.api.Test
import java.nio.file.Path

class FrameflowHostWorkbenchControllerTest {
    @Test
    fun fullscreenHandoffReplaysRememberedPointsFiltersAndCamera() {
        val controller = FrameflowHostWorkbenchController.create(
            FrameflowHostSurfaceConfig(
                packagedNativeDir = resolveNativeLibraryPath().parent,
            ),
        )

        controller.use {
            assertIs<FrameflowHostBootstrapResult.Started>(it.mountEmbedded())
            it.loadSampleDiscoveryData()
            it.moveCamera()
            it.focusLocation()

            assertIs<FrameflowHostBootstrapResult.Started>(it.handoffToFullscreen())

            val state = it.snapshot()
            assertEquals(FrameflowSurfaceMode.Fullscreen, state.activeSurface?.mode)
            assertEquals(2, state.mountSequence)
            assertEquals(3, state.rememberedPointCount)
            assertEquals(3, state.hostState.pointSnapshotCount)
            assertEquals(FrameflowHostWorkbenchController.sampleFilters(), state.hostState.activeFilters)
            assertEquals(21.0, state.hostState.cameraState?.longitude)
            assertNull(state.hostState.selectedLocationId)
            assertTrue(state.lastOperation.contains("fullscreen", ignoreCase = true))
        }
    }

    @Test
    fun returningToEmbeddedKeepsRememberedDatasetAndLifecycleOperationsUsable() {
        val controller = FrameflowHostWorkbenchController.create(
            FrameflowHostSurfaceConfig(
                packagedNativeDir = resolveNativeLibraryPath().parent,
            ),
        )

        controller.use {
            assertIs<FrameflowHostBootstrapResult.Started>(it.mountEmbedded())
            it.loadSampleDiscoveryData()
            it.handoffToFullscreen()
            assertIs<FrameflowHostBootstrapResult.Started>(it.returnToEmbedded())

            it.resizeActive(width = 1500, height = 900, scaleFactor = 1.5)
            it.pauseActive()
            it.resumeActive()

            val state = it.snapshot()
            assertEquals(FrameflowSurfaceMode.Embedded, state.activeSurface?.mode)
            assertEquals(3, state.hostState.pointSnapshotCount)
            assertTrue(state.hostState.diagnosticsSummary?.contains("width=1500") == true)
            assertTrue(state.hostState.diagnosticsSummary?.contains("state=READY") == true)
        }
    }

    private fun resolveNativeLibraryPath(): Path {
        val configured = System.getProperty("frameflow.native.lib")
            ?: error("frameflow.native.lib system property must be set by the test task")
        return Path.of(configured).toAbsolutePath().normalize()
    }
}
