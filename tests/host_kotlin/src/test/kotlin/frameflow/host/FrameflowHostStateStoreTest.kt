package frameflow.host

import kotlin.test.assertEquals
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertTrue
import org.junit.jupiter.api.Test
import java.nio.file.Path

class FrameflowHostStateStoreTest {
    @Test
    fun transitionsBetweenEmptyReadyAndDisposedWhileKeepingSharedState() {
        val store = FrameflowHostStateStore()
        val filters = FrameflowFilterSnapshot(
            query = "election",
            categoryCode = "POLITICS",
            locationId = 601,
            countryCode = "RS",
        )

        store.markLoading()
        store.recordFilters(filters, "filters=POLITICS")
        store.recordReady(sampleReady())

        val emptyState = store.snapshot()
        assertEquals(FrameflowHostStatus.Empty, emptyState.status)
        assertEquals(filters, emptyState.activeFilters)
        assertEquals(1, emptyState.readyEventCount)

        store.recordPointSnapshot(pointCount = 2, selectedLocationId = null, diagnosticsSummary = "points=2")
        store.recordLocationSelection(
            FrameflowLocationSelection(
                locationId = 601,
                categoryCode = "POLITICS",
                screenX = 1200.0,
                screenY = 440.0,
                interaction = "click",
            ),
        )
        store.recordCameraState(
            FrameflowCameraState(
                longitude = 20.4489,
                latitude = 44.7866,
                heightMeters = 2_500_000.0,
                headingDegrees = 0.0,
                pitchDegrees = -45.0,
                rollDegrees = 0.0,
            ),
        )
        store.markDisposed()

        val disposedState = store.snapshot()
        assertEquals(FrameflowHostStatus.Disposed, disposedState.status)
        assertEquals(601L, disposedState.selectedLocationId)
        assertEquals("click", disposedState.lastLocationSelection?.interaction)
        assertEquals(1200.0, disposedState.lastLocationSelection?.screenX)
        assertEquals(filters, disposedState.activeFilters)
        assertNotNull(disposedState.cameraState)
        assertNull(disposedState.lastClusterSelection)
    }

    @Test
    fun tracksClusterCameraAndRecoverableFailureSignals() {
        val store = FrameflowHostStateStore()
        store.recordReady(sampleReady())
        store.recordPointSnapshot(pointCount = 3, selectedLocationId = null, diagnosticsSummary = "points=3")
        store.recordLocationSelection(
            FrameflowLocationSelection(
                locationId = 601,
                categoryCode = "POLITICS",
                screenX = 1200.0,
                screenY = 440.0,
                interaction = "click",
            ),
        )
        store.recordClusterSelection(
            FrameflowClusterSelection(
                clusterId = 77,
                pointCount = 3,
                longitude = 20.4489,
                latitude = 44.7866,
            ),
        )
        store.recordCameraState(
            FrameflowCameraState(
                longitude = 21.0,
                latitude = 45.0,
                heightMeters = 1_500_000.0,
                headingDegrees = 15.0,
                pitchDegrees = -30.0,
                rollDegrees = 0.0,
            ),
        )
        store.recordError(
            FrameflowHostError(
                code = FrameflowNativeResult.INTERNAL_ERROR,
                message = "render context failed",
                recoverable = true,
                runtimeInfo = sampleRuntimeInfo(),
            ),
        )

        val snapshot = store.snapshot()
        assertEquals(FrameflowHostStatus.Failed, snapshot.status)
        assertTrue(snapshot.canRetry)
        assertEquals(1, snapshot.clusterSelectionEventCount)
        assertEquals(1, snapshot.cameraEventCount)
        assertEquals(1, snapshot.errorEventCount)
        assertNull(snapshot.selectedLocationId)
        assertNull(snapshot.lastLocationSelection)
        assertEquals(77L, snapshot.lastClusterSelection?.clusterId)
        assertEquals("render context failed", snapshot.lastError?.message)
    }

    @Test
    fun markLoadingClearsStaleSelectionButPreservesSharedFiltersAndCamera() {
        val store = FrameflowHostStateStore()
        val filters = FrameflowFilterSnapshot(
            query = "election",
            categoryCode = "POLITICS",
            locationId = 601,
            countryCode = "RS",
        )

        store.recordFilters(filters, "filters=POLITICS")
        store.recordReady(sampleReady())
        store.recordPointSnapshot(pointCount = 1, selectedLocationId = 601, diagnosticsSummary = "points=1")
        store.recordLocationSelection(
            FrameflowLocationSelection(
                locationId = 601,
                categoryCode = "POLITICS",
                screenX = 1200.0,
                screenY = 440.0,
                interaction = "click",
            ),
        )
        store.recordCameraState(
            FrameflowCameraState(
                longitude = 20.4489,
                latitude = 44.7866,
                heightMeters = 2_500_000.0,
                headingDegrees = 0.0,
                pitchDegrees = -45.0,
                rollDegrees = 0.0,
            ),
        )

        store.markLoading()

        val snapshot = store.snapshot()
        assertEquals(FrameflowHostStatus.Loading, snapshot.status)
        assertEquals(0, snapshot.pointSnapshotCount)
        assertNull(snapshot.selectedLocationId)
        assertNull(snapshot.lastLocationSelection)
        assertEquals(filters, snapshot.activeFilters)
        assertNotNull(snapshot.cameraState)
    }

    @Test
    fun marksUnavailableStartupFailuresAsRetryable() {
        val store = FrameflowHostStateStore()
        val resolution = FrameflowNativeLibraryResolution(
            source = FrameflowNativeLibrarySource.EnvironmentOverride,
            target = FrameflowNativeLibraryTarget.AbsolutePath(Path.of("/tmp/frameflow/libframeflow.so")),
        )

        store.markLoading()
        store.recordLibraryResolution(resolution)
        store.recordStartupFailure(
            kind = FrameflowStartupFailureKind.Unavailable,
            error = FrameflowHostError(
                code = FrameflowNativeResult.NOT_FOUND,
                message = "library missing",
                recoverable = true,
                runtimeInfo = null,
            ),
        )

        val snapshot = store.snapshot()
        assertEquals(FrameflowHostStatus.Unavailable, snapshot.status)
        assertEquals(FrameflowStartupFailureKind.Unavailable, snapshot.startupFailureKind)
        assertTrue(snapshot.canRetry)
        assertEquals(resolution, snapshot.libraryResolution)
    }

    private fun sampleReady(): FrameflowReady {
        return FrameflowReady(
            width = 1280,
            height = 720,
            scaleFactor = 1.0,
            runtimeInfo = sampleRuntimeInfo(),
        )
    }

    private fun sampleRuntimeInfo(): FrameflowRuntimeInfo {
        return FrameflowRuntimeInfo(
            bridgeAbiVersion = 2,
            engineVersionMajor = 0,
            engineVersionMinor = 1,
            engineVersionPatch = 0,
            commandVersionMajor = 2,
            commandVersionMinor = 3,
        )
    }
}
