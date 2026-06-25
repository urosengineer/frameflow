package frameflow.host

import java.nio.file.Path

enum class FrameflowSurfaceMode {
    Embedded,
    Fullscreen,
}

data class FrameflowSurfaceMount(
    val surfaceHandle: Long,
    val width: Int,
    val height: Int,
    val scaleFactor: Double = 1.0,
    val mode: FrameflowSurfaceMode = FrameflowSurfaceMode.Embedded,
)

data class FrameflowHostSurfaceConfig(
    val packagedNativeDir: Path? = null,
    val environment: Map<String, String> = System.getenv(),
    val systemLibraryName: String = "frameflow",
    val expectedBridgeAbiVersion: Int = 2,
    val expectedCommandVersionMajor: Int = 2,
    val options: FrameflowNativeOptions = FrameflowNativeOptions(),
    val stateStore: FrameflowHostStateStore = FrameflowHostStateStore(),
)

class FrameflowHostSurfaceController(
    private val config: FrameflowHostSurfaceConfig,
) : AutoCloseable {
    private var activeHost: FrameflowHostPrototype? = null
    private var mountedSurface: FrameflowSurfaceMount? = null
    private var mountSequence: Int = 0

    val state: FrameflowHostState
        get() = config.stateStore.snapshot()

    val activeSurface: FrameflowSurfaceMount?
        get() = mountedSurface

    val activeMountSequence: Int
        get() = mountSequence

    fun mount(surface: FrameflowSurfaceMount): FrameflowHostBootstrapResult {
        activeHost?.close()
        activeHost = null
        mountedSurface = null

        val result = FrameflowHostBootstrapper.bootstrap(
            FrameflowHostBootstrapRequest(
                packagedNativeDir = config.packagedNativeDir,
                environment = config.environment,
                systemLibraryName = config.systemLibraryName,
                expectedBridgeAbiVersion = config.expectedBridgeAbiVersion,
                expectedCommandVersionMajor = config.expectedCommandVersionMajor,
                surfaceHandle = surface.surfaceHandle,
                width = surface.width,
                height = surface.height,
                scaleFactor = surface.scaleFactor,
                options = config.options,
                stateStore = config.stateStore,
            ),
        )
        if (result is FrameflowHostBootstrapResult.Started) {
            activeHost = result.host
            mountedSurface = surface
            mountSequence += 1
        }
        return result
    }

    fun resize(width: Int, height: Int, scaleFactor: Double = requireMountedSurface().scaleFactor) {
        requireMountedHost().resize(width, height, scaleFactor)
        mountedSurface = requireMountedSurface().copy(width = width, height = height, scaleFactor = scaleFactor)
    }

    fun pause() {
        requireMountedHost().pause()
    }

    fun resume() {
        requireMountedHost().resume()
    }

    fun dispatchNativeCallbacks(): Int {
        return requireMountedHost().dispatchNativeCallbacks()
    }

    fun replacePoints(points: List<RenderReadyPoint>) {
        requireMountedHost().replacePoints(points)
    }

    fun updateFilters(filter: FrameflowFilterSnapshot) {
        requireMountedHost().updateFilters(filter)
    }

    fun focusLocation(locationId: Long) {
        requireMountedHost().focusLocation(locationId)
    }

    fun reportCameraChanged(cameraState: FrameflowCameraState) {
        requireMountedHost().reportCameraChanged(cameraState)
    }

    override fun close() {
        activeHost?.close()
        activeHost = null
        mountedSurface = null
    }

    private fun requireMountedHost(): FrameflowHostPrototype {
        return requireNotNull(activeHost) { "No active Frameflow host surface mount" }
    }

    private fun requireMountedSurface(): FrameflowSurfaceMount {
        return requireNotNull(mountedSurface) { "No active Frameflow surface metadata" }
    }
}
