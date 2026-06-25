package frameflow.host

import com.sun.jna.Pointer
import com.sun.jna.ptr.LongByReference
import java.nio.file.Path

private sealed interface FrameflowHostEvent {
    data class Ready(val payload: FrameflowReady) : FrameflowHostEvent
    data class LocationSelected(val payload: FrameflowLocationSelection) : FrameflowHostEvent
    data class ClusterSelected(val payload: FrameflowClusterSelection) : FrameflowHostEvent
    data class CameraChanged(val payload: FrameflowCameraState) : FrameflowHostEvent
    data class Error(val payload: FrameflowHostError) : FrameflowHostEvent
}

private class PendingHostEventDispatcher {
    private val queue = ArrayDeque<FrameflowHostEvent>()

    fun enqueue(event: FrameflowHostEvent) {
        queue.addLast(event)
    }

    fun drainInto(state: FrameflowHostStateStore) {
        while (queue.isNotEmpty()) {
            when (val event = queue.removeFirst()) {
                is FrameflowHostEvent.Ready -> state.recordReady(event.payload)
                is FrameflowHostEvent.LocationSelected -> state.recordLocationSelection(event.payload)
                is FrameflowHostEvent.ClusterSelected -> state.recordClusterSelection(event.payload)
                is FrameflowHostEvent.CameraChanged -> state.recordCameraState(event.payload)
                is FrameflowHostEvent.Error -> state.recordError(event.payload)
            }
        }
    }

    fun clear() {
        queue.clear()
    }
}

class FrameflowHostException(
    val operation: String,
    val result: FrameflowNativeResult,
    val failureMessage: String,
) : IllegalStateException("$operation failed with ${result.name}: $failureMessage")

internal class FrameflowHostCallbackHarness {
    private val dispatcher = PendingHostEventDispatcher()
    private val callbacks = HostCallbacks(dispatcher).toNativeStruct().also { it.write() }

    fun callbacks(): FrameflowCallbacksStruct = callbacks

    fun dispatchInto(stateStore: FrameflowHostStateStore) {
        dispatcher.drainInto(stateStore)
    }
}

class FrameflowHostPrototype private constructor(
    private val native: FrameflowNativeLibrary,
    private val expectedBridgeAbiVersion: Int,
    private val expectedCommandVersionMajor: Int,
    private val stateStore: FrameflowHostStateStore,
) : AutoCloseable {
    private var engine: Pointer? = null
    private val dispatcher = PendingHostEventDispatcher()
    private val callbackHolder = HostCallbacks(dispatcher)
    private var callbacksStruct: FrameflowCallbacksStruct? = null

    val state: FrameflowHostState
        get() = stateStore.snapshot()

    fun runtimeInfo(): FrameflowRuntimeInfo {
        val runtimeInfo = FrameflowRuntimeInfoStruct()
        requireOkWithoutEngine(
            native.frameflow_bridge_get_runtime_info(runtimeInfo),
            operation = "bridge_get_runtime_info",
        )
        runtimeInfo.read()
        return runtimeInfo.toModel().also(stateStore::recordRuntimeInfo)
    }

    fun checkCompatibility(): FrameflowRuntimeInfo {
        val runtimeInfo = FrameflowRuntimeInfoStruct()
        requireOkWithoutEngine(
            native.frameflow_bridge_check_compatibility(
                expectedBridgeAbiVersion,
                expectedCommandVersionMajor,
                runtimeInfo,
            ),
            operation = "bridge_check_compatibility",
        )
        runtimeInfo.read()
        return runtimeInfo.toModel().also(stateStore::recordRuntimeInfo)
    }

    fun start(
        surfaceHandle: Long = 1L,
        width: Int = 1280,
        height: Int = 720,
        scaleFactor: Double = 1.0,
        options: FrameflowNativeOptions = FrameflowNativeOptions(),
    ) {
        check(engine == null) { "Host prototype has already created an engine handle" }
        stateStore.markLoading()

        val handle = native.frameflow_engine_create()
            ?: throw FrameflowHostException(
                operation = "engine_create",
                result = FrameflowNativeResult.INTERNAL_ERROR,
                failureMessage = "Native bridge returned a null engine handle",
            )
        engine = handle

        val callbacks = callbackHolder.toNativeStruct().also { it.write() }
        callbacksStruct = callbacks
        requireOk(
            native.frameflow_engine_set_callbacks(handle, callbacks),
            operation = "engine_set_callbacks",
        )

        val nativeOptions = FrameflowOptionsStruct.from(options)
        requireOk(
            native.frameflow_engine_initialize(handle, surfaceHandle, width, height, scaleFactor, nativeOptions),
            operation = "engine_initialize",
        )
    }

    fun replacePoints(points: List<RenderReadyPoint>) {
        val handle = requireEngineHandle()
        val preparedPoints = PreparedPointSnapshot.from(points)
        requireOk(
            native.frameflow_engine_set_points(handle, preparedPoints.nativePoints, points.size.toLong()),
            operation = "engine_set_points",
        )
        stateStore.recordPointSnapshot(
            pointCount = native.frameflow_engine_point_count(handle).toInt(),
            selectedLocationId = selectedLocationIdFromNative(handle),
            diagnosticsSummary = diagnosticsSummaryOrNull(handle),
        )
    }

    fun resize(width: Int, height: Int, scaleFactor: Double = 1.0) {
        val handle = requireEngineHandle()
        requireOk(
            native.frameflow_engine_resize(handle, width, height, scaleFactor),
            operation = "engine_resize",
        )
        stateStore.recordDiagnosticsSummary(diagnosticsSummaryOrNull(handle))
    }

    fun pause() {
        val handle = requireEngineHandle()
        requireOk(
            native.frameflow_engine_pause(handle),
            operation = "engine_pause",
        )
        stateStore.recordDiagnosticsSummary(diagnosticsSummaryOrNull(handle))
    }

    fun resume() {
        val handle = requireEngineHandle()
        requireOk(
            native.frameflow_engine_resume(handle),
            operation = "engine_resume",
        )
        stateStore.recordDiagnosticsSummary(diagnosticsSummaryOrNull(handle))
    }

    fun updateFilters(filter: FrameflowFilterSnapshot) {
        val handle = requireEngineHandle()
        requireOk(
            native.frameflow_engine_set_filters(handle, FrameflowFilterStruct.from(filter)),
            operation = "engine_set_filters",
        )
        stateStore.recordFilters(filter, diagnosticsSummaryOrNull(handle))
    }

    fun focusLocation(locationId: Long) {
        requireOk(
            native.frameflow_engine_focus_location(requireEngineHandle(), locationId),
            operation = "engine_focus_location",
        )
    }

    fun selectLocation(
        locationId: Long,
        categoryCode: String? = null,
        screenX: Double,
        screenY: Double,
        interaction: String = "click",
    ) {
        requireOk(
            native.frameflow_engine_select_location(
                requireEngineHandle(),
                locationId,
                categoryCode,
                screenX,
                screenY,
                interaction,
            ),
            operation = "engine_select_location",
        )
    }

    fun selectCluster(
        clusterId: Long,
        pointCount: Int,
        longitude: Double,
        latitude: Double,
    ) {
        requireOk(
            native.frameflow_engine_select_cluster(
                requireEngineHandle(),
                clusterId,
                pointCount,
                longitude,
                latitude,
            ),
            operation = "engine_select_cluster",
        )
    }

    fun reportCameraChanged(cameraState: FrameflowCameraState) {
        requireOk(
            native.frameflow_engine_report_camera_changed(
                requireEngineHandle(),
                cameraState.longitude,
                cameraState.latitude,
                cameraState.heightMeters,
                cameraState.headingDegrees,
                cameraState.pitchDegrees,
                cameraState.rollDegrees,
            ),
            operation = "engine_report_camera_changed",
        )
    }

    fun clearSelection() {
        val handle = requireEngineHandle()
        requireOk(
            native.frameflow_engine_clear_selection(handle),
            operation = "engine_clear_selection",
        )
        stateStore.recordSelectionCleared(diagnosticsSummaryOrNull(handle))
    }

    fun dispatchNativeCallbacks(): Int {
        val handle = requireEngineHandle()
        val dispatched = native.frameflow_engine_dispatch_pending_callbacks(handle).toInt()
        dispatcher.drainInto(stateStore)
        stateStore.recordDiagnosticsSummary(diagnosticsSummaryOrNull(handle))
        return dispatched
    }

    fun hasSelectedLocationId(): Boolean {
        return native.frameflow_engine_has_selected_location_id(requireEngineHandle()).toInt() != 0
    }

    fun selectedLocationId(): Long? {
        return selectedLocationIdFromNative(requireEngineHandle())
    }

    fun diagnosticsSummary(): String {
        val diagnostics = diagnosticsSummaryOrNull(requireEngineHandle()) ?: ""
        stateStore.recordDiagnosticsSummary(diagnostics)
        return diagnostics
    }

    override fun close() {
        val handle = engine
        if (handle != null) {
            native.frameflow_engine_dispose(handle)
            native.frameflow_engine_destroy(handle)
        }
        dispatcher.clear()
        callbacksStruct = null
        engine = null
        stateStore.markDisposed()
    }

    private fun requireEngineHandle(): Pointer {
        return requireNotNull(engine) { "Native engine handle has not been created yet" }
    }

    private fun requireOk(resultCode: Int, operation: String) {
        val result = FrameflowNativeResult.fromCode(resultCode)
        if (result == FrameflowNativeResult.OK) {
            return
        }

        val handle = engine
        val message = if (handle != null) {
            native.frameflow_engine_last_error_message(handle) ?: "Unknown native error"
        } else {
            "No native engine handle available"
        }
        throw FrameflowHostException(operation, result, message)
    }

    private fun requireOkWithoutEngine(resultCode: Int, operation: String) {
        val result = FrameflowNativeResult.fromCode(resultCode)
        if (result == FrameflowNativeResult.OK) {
            return
        }
        throw FrameflowHostException(
            operation = operation,
            result = result,
            failureMessage = "Runtime compatibility check failed before engine creation",
        )
    }

    private fun selectedLocationIdFromNative(handle: Pointer): Long? {
        val selectedLocationId = LongByReference()
        return when (
            FrameflowNativeResult.fromCode(
                native.frameflow_engine_get_selected_location_id(handle, selectedLocationId),
            )
        ) {
            FrameflowNativeResult.OK -> selectedLocationId.value
            FrameflowNativeResult.NOT_FOUND -> null
            else -> throw FrameflowHostException(
                operation = "engine_get_selected_location_id",
                result = FrameflowNativeResult.fromCode(native.frameflow_engine_last_error_code(handle)),
                failureMessage = native.frameflow_engine_last_error_message(handle) ?: "Unknown native error",
            )
        }
    }

    internal fun diagnosticsSummaryOrNull(handle: Pointer): String? {
        return native.frameflow_engine_diagnostics_summary(handle)
    }

    internal fun diagnosticsSummaryOrNull(): String? {
        val handle = engine ?: return null
        return diagnosticsSummaryOrNull(handle)
    }

    internal fun lastNativeError(): FrameflowHostError? {
        val handle = engine ?: return null
        val code = FrameflowNativeResult.fromCode(native.frameflow_engine_last_error_code(handle))
        val message = native.frameflow_engine_last_error_message(handle)
        if (code == FrameflowNativeResult.OK && message == null) {
            return null
        }
        return FrameflowHostError(
            code = code,
            message = message,
            recoverable = native.frameflow_engine_last_error_recoverable(handle).toInt() != 0,
            runtimeInfo = stateStore.snapshot().runtimeInfo,
        )
    }

    companion object {
        fun load(
            libraryPath: Path,
            expectedBridgeAbiVersion: Int = 2,
            expectedCommandVersionMajor: Int = 2,
            stateStore: FrameflowHostStateStore = FrameflowHostStateStore(),
        ): FrameflowHostPrototype {
            return FrameflowHostPrototype(
                native = FrameflowNativeLibrary.load(libraryPath),
                expectedBridgeAbiVersion = expectedBridgeAbiVersion,
                expectedCommandVersionMajor = expectedCommandVersionMajor,
                stateStore = stateStore,
            )
        }

        fun load(
            target: FrameflowNativeLibraryTarget,
            expectedBridgeAbiVersion: Int = 2,
            expectedCommandVersionMajor: Int = 2,
            stateStore: FrameflowHostStateStore = FrameflowHostStateStore(),
        ): FrameflowHostPrototype {
            return FrameflowHostPrototype(
                native = FrameflowNativeLibrary.load(target),
                expectedBridgeAbiVersion = expectedBridgeAbiVersion,
                expectedCommandVersionMajor = expectedCommandVersionMajor,
                stateStore = stateStore,
            )
        }
    }
}

private class HostCallbacks(
    private val dispatcher: PendingHostEventDispatcher,
) {
    private val onReady = FrameflowReadyCallback { _, eventPointer ->
        if (eventPointer != null) {
            dispatcher.enqueue(FrameflowHostEvent.Ready(FrameflowReadyEventStruct(eventPointer).toModel()))
        }
    }

    private val onLocationSelected = FrameflowLocationSelectionCallback { _, eventPointer ->
        if (eventPointer != null) {
            dispatcher.enqueue(
                FrameflowHostEvent.LocationSelected(
                    FrameflowLocationSelectionEventStruct(eventPointer).toModel(),
                ),
            )
        }
    }

    private val onClusterSelected = FrameflowClusterSelectionCallback { _, eventPointer ->
        if (eventPointer != null) {
            dispatcher.enqueue(
                FrameflowHostEvent.ClusterSelected(
                    FrameflowClusterSelectionEventStruct(eventPointer).toModel(),
                ),
            )
        }
    }

    private val onCameraChanged = FrameflowCameraChangedCallback { _, eventPointer ->
        if (eventPointer != null) {
            dispatcher.enqueue(
                FrameflowHostEvent.CameraChanged(
                    FrameflowCameraStateStruct(eventPointer).toModel(),
                ),
            )
        }
    }

    private val onEngineError = FrameflowErrorCallback { _, eventPointer ->
        if (eventPointer != null) {
            dispatcher.enqueue(FrameflowHostEvent.Error(FrameflowErrorEventStruct(eventPointer).toModel()))
        }
    }

    fun toNativeStruct(): FrameflowCallbacksStruct {
        return FrameflowCallbacksStruct().apply {
            user = null
            on_ready = onReady
            on_location_selected = onLocationSelected
            on_cluster_selected = onClusterSelected
            on_camera_changed = onCameraChanged
            on_engine_error = onEngineError
        }
    }
}
