package frameflow.host

enum class FrameflowHostStatus {
    Loading,
    Ready,
    Empty,
    Unavailable,
    Failed,
    Disposed,
}

data class FrameflowHostState(
    val status: FrameflowHostStatus = FrameflowHostStatus.Loading,
    val runtimeInfo: FrameflowRuntimeInfo? = null,
    val startupFailureKind: FrameflowStartupFailureKind? = null,
    val libraryResolution: FrameflowNativeLibraryResolution? = null,
    val lastReady: FrameflowReady? = null,
    val selectedLocationId: Long? = null,
    val lastLocationSelection: FrameflowLocationSelection? = null,
    val activeFilters: FrameflowFilterSnapshot = FrameflowFilterSnapshot(),
    val pointSnapshotCount: Int = 0,
    val lastClusterSelection: FrameflowClusterSelection? = null,
    val cameraState: FrameflowCameraState? = null,
    val lastError: FrameflowHostError? = null,
    val diagnosticsSummary: String? = null,
    val canRetry: Boolean = false,
    val readyEventCount: Int = 0,
    val selectionEventCount: Int = 0,
    val clusterSelectionEventCount: Int = 0,
    val cameraEventCount: Int = 0,
    val errorEventCount: Int = 0,
)

class FrameflowHostStateStore {
    private var status: FrameflowHostStatus = FrameflowHostStatus.Loading
    private var runtimeInfo: FrameflowRuntimeInfo? = null
    private var startupFailureKind: FrameflowStartupFailureKind? = null
    private var libraryResolution: FrameflowNativeLibraryResolution? = null
    private var lastReady: FrameflowReady? = null
    private var selectedLocationId: Long? = null
    private var lastLocationSelection: FrameflowLocationSelection? = null
    private var activeFilters: FrameflowFilterSnapshot = FrameflowFilterSnapshot()
    private var pointSnapshotCount: Int = 0
    private var lastClusterSelection: FrameflowClusterSelection? = null
    private var cameraState: FrameflowCameraState? = null
    private var lastError: FrameflowHostError? = null
    private var diagnosticsSummary: String? = null
    private var canRetry: Boolean = false
    private var readyEventCount: Int = 0
    private var selectionEventCount: Int = 0
    private var clusterSelectionEventCount: Int = 0
    private var cameraEventCount: Int = 0
    private var errorEventCount: Int = 0

    fun snapshot(): FrameflowHostState {
        return FrameflowHostState(
            status = status,
            runtimeInfo = runtimeInfo,
            startupFailureKind = startupFailureKind,
            libraryResolution = libraryResolution,
            lastReady = lastReady,
            selectedLocationId = selectedLocationId,
            lastLocationSelection = lastLocationSelection,
            activeFilters = activeFilters,
            pointSnapshotCount = pointSnapshotCount,
            lastClusterSelection = lastClusterSelection,
            cameraState = cameraState,
            lastError = lastError,
            diagnosticsSummary = diagnosticsSummary,
            canRetry = canRetry,
            readyEventCount = readyEventCount,
            selectionEventCount = selectionEventCount,
            clusterSelectionEventCount = clusterSelectionEventCount,
            cameraEventCount = cameraEventCount,
            errorEventCount = errorEventCount,
        )
    }

    internal fun markLoading() {
        status = FrameflowHostStatus.Loading
        pointSnapshotCount = 0
        selectedLocationId = null
        startupFailureKind = null
        lastError = null
        lastLocationSelection = null
        lastClusterSelection = null
        diagnosticsSummary = null
        canRetry = false
    }

    internal fun recordLibraryResolution(resolution: FrameflowNativeLibraryResolution) {
        libraryResolution = resolution
    }

    internal fun recordRuntimeInfo(runtimeInfo: FrameflowRuntimeInfo) {
        this.runtimeInfo = runtimeInfo
    }

    internal fun recordReady(payload: FrameflowReady) {
        lastReady = payload
        runtimeInfo = payload.runtimeInfo
        startupFailureKind = null
        lastError = null
        canRetry = false
        readyEventCount += 1
        refreshDataStatus()
    }

    internal fun recordPointSnapshot(pointCount: Int, selectedLocationId: Long?, diagnosticsSummary: String?) {
        pointSnapshotCount = pointCount
        this.selectedLocationId = selectedLocationId
        if (selectedLocationId == null) {
            lastLocationSelection = null
            lastClusterSelection = null
        }
        this.diagnosticsSummary = diagnosticsSummary
        refreshDataStatus()
    }

    internal fun recordFilters(filter: FrameflowFilterSnapshot, diagnosticsSummary: String?) {
        activeFilters = filter
        if (diagnosticsSummary != null) {
            this.diagnosticsSummary = diagnosticsSummary
        }
    }

    internal fun recordLocationSelection(payload: FrameflowLocationSelection) {
        selectedLocationId = payload.locationId
        lastLocationSelection = payload
        lastClusterSelection = null
        selectionEventCount += 1
        refreshDataStatus()
    }

    internal fun recordClusterSelection(payload: FrameflowClusterSelection) {
        selectedLocationId = null
        lastLocationSelection = null
        lastClusterSelection = payload
        clusterSelectionEventCount += 1
        refreshDataStatus()
    }

    internal fun recordCameraState(payload: FrameflowCameraState) {
        cameraState = payload
        cameraEventCount += 1
    }

    internal fun recordError(payload: FrameflowHostError) {
        lastError = payload
        payload.runtimeInfo?.let { runtimeInfo = it }
        canRetry = payload.recoverable
        errorEventCount += 1
        status = FrameflowHostStatus.Failed
    }

    internal fun recordStartupFailure(
        kind: FrameflowStartupFailureKind,
        error: FrameflowHostError,
        diagnosticsSummary: String? = null,
    ) {
        startupFailureKind = kind
        lastError = error
        error.runtimeInfo?.let { runtimeInfo = it }
        canRetry = kind == FrameflowStartupFailureKind.Unavailable || error.recoverable
        errorEventCount += 1
        status = if (kind == FrameflowStartupFailureKind.Unavailable) {
            FrameflowHostStatus.Unavailable
        } else {
            FrameflowHostStatus.Failed
        }
        if (diagnosticsSummary != null) {
            this.diagnosticsSummary = diagnosticsSummary
        }
    }

    internal fun recordSelectionCleared(diagnosticsSummary: String?) {
        selectedLocationId = null
        lastLocationSelection = null
        lastClusterSelection = null
        if (diagnosticsSummary != null) {
            this.diagnosticsSummary = diagnosticsSummary
        }
        refreshDataStatus()
    }

    internal fun recordDiagnosticsSummary(diagnosticsSummary: String?) {
        this.diagnosticsSummary = diagnosticsSummary
    }

    internal fun markDisposed() {
        status = FrameflowHostStatus.Disposed
        lastClusterSelection = null
        canRetry = false
    }

    private fun refreshDataStatus() {
        if (lastReady == null || status == FrameflowHostStatus.Failed || status == FrameflowHostStatus.Unavailable ||
            status == FrameflowHostStatus.Disposed
        ) {
            return
        }
        status = if (pointSnapshotCount == 0) FrameflowHostStatus.Empty else FrameflowHostStatus.Ready
    }
}
