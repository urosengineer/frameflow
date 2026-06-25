package frameflow.host

import kotlin.test.assertEquals
import kotlin.test.assertNull
import kotlin.test.assertTrue
import org.junit.jupiter.api.Test

class FrameflowHostCallbackHandoffTest {
    @Test
    fun clusterCameraAndErrorCallbacksMutateStateOnlyAfterExplicitDrain() {
        val stateStore = FrameflowHostStateStore()
        val harness = FrameflowHostCallbackHarness()
        val callbacks = harness.callbacks()

        stateStore.recordReady(sampleReady())
        stateStore.recordPointSnapshot(pointCount = 3, selectedLocationId = null, diagnosticsSummary = "points=3")
        stateStore.recordLocationSelection(
            FrameflowLocationSelection(
                locationId = 601,
                categoryCode = "POLITICS",
                screenX = 1200.0,
                screenY = 440.0,
                interaction = "click",
            ),
        )

        val clusterEvent = FrameflowClusterSelectionEventStruct().apply {
            cluster_id = 77
            point_count = 3
            longitude = 20.4489
            latitude = 44.7866
            write()
        }
        callbacks.on_cluster_selected!!.invoke(null, clusterEvent.pointer)

        val cameraEvent = FrameflowCameraStateStruct().apply {
            longitude = 21.0
            latitude = 45.0
            height_meters = 1_500_000.0
            heading_degrees = 15.0
            pitch_degrees = -30.0
            roll_degrees = 0.0
            write()
        }
        callbacks.on_camera_changed!!.invoke(null, cameraEvent.pointer)

        val errorEvent = FrameflowErrorEventStruct().apply {
            code = FrameflowNativeResult.INTERNAL_ERROR.code
            message = "recoverable callback error"
            recoverable = 1
            bridge_abi_version = 2
            engine_version_major = 0
            engine_version_minor = 1
            engine_version_patch = 0
            command_version_major = 2
            command_version_minor = 3
            write()
        }
        callbacks.on_engine_error!!.invoke(null, errorEvent.pointer)

        val beforeDrain = stateStore.snapshot()
        assertEquals(FrameflowHostStatus.Ready, beforeDrain.status)
        assertEquals(0, beforeDrain.clusterSelectionEventCount)
        assertEquals(0, beforeDrain.cameraEventCount)
        assertEquals(0, beforeDrain.errorEventCount)
        assertNull(beforeDrain.lastClusterSelection)
        assertNull(beforeDrain.cameraState)

        harness.dispatchInto(stateStore)

        val afterDrain = stateStore.snapshot()
        assertEquals(FrameflowHostStatus.Failed, afterDrain.status)
        assertTrue(afterDrain.canRetry)
        assertEquals(1, afterDrain.clusterSelectionEventCount)
        assertEquals(1, afterDrain.cameraEventCount)
        assertEquals(1, afterDrain.errorEventCount)
        assertNull(afterDrain.selectedLocationId)
        assertNull(afterDrain.lastLocationSelection)
        assertEquals(77L, afterDrain.lastClusterSelection?.clusterId)
        assertEquals(21.0, afterDrain.cameraState?.longitude)
        assertEquals("recoverable callback error", afterDrain.lastError?.message)
    }

    private fun sampleReady(): FrameflowReady {
        return FrameflowReady(
            width = 1280,
            height = 720,
            scaleFactor = 1.0,
            runtimeInfo = FrameflowRuntimeInfo(
                bridgeAbiVersion = 2,
                engineVersionMajor = 0,
                engineVersionMinor = 1,
                engineVersionPatch = 0,
                commandVersionMajor = 2,
                commandVersionMinor = 3,
            ),
        )
    }
}
