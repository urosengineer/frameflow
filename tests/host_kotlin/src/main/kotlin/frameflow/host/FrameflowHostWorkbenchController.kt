package frameflow.host

data class FrameflowHostWorkbenchState(
    val activeSurface: FrameflowSurfaceMount? = null,
    val mountSequence: Int = 0,
    val hostState: FrameflowHostState = FrameflowHostState(),
    val rememberedPointCount: Int = 0,
    val rememberedFilters: FrameflowFilterSnapshot = FrameflowFilterSnapshot(),
    val rememberedCameraState: FrameflowCameraState? = null,
    val lastOperation: String = "Idle",
)

class FrameflowHostWorkbenchController(
    private val surfaceController: FrameflowHostSurfaceController,
) : AutoCloseable {
    private var rememberedPoints: List<RenderReadyPoint> = emptyList()
    private var rememberedFilters: FrameflowFilterSnapshot = FrameflowFilterSnapshot()
    private var rememberedCameraState: FrameflowCameraState? = null
    private var lastOperation: String = "Idle"

    fun snapshot(): FrameflowHostWorkbenchState {
        return FrameflowHostWorkbenchState(
            activeSurface = surfaceController.activeSurface,
            mountSequence = surfaceController.activeMountSequence,
            hostState = surfaceController.state,
            rememberedPointCount = rememberedPoints.size,
            rememberedFilters = rememberedFilters,
            rememberedCameraState = rememberedCameraState,
            lastOperation = lastOperation,
        )
    }

    fun mountEmbedded(): FrameflowHostBootstrapResult {
        return mount(defaultEmbeddedSurface(), "Mounted embedded surface")
    }

    fun handoffToFullscreen(): FrameflowHostBootstrapResult {
        return mount(defaultFullscreenSurface(), "Handed off to fullscreen surface")
    }

    fun returnToEmbedded(): FrameflowHostBootstrapResult {
        return mount(defaultEmbeddedSurface(), "Returned to embedded surface")
    }

    fun resizeActive(width: Int, height: Int, scaleFactor: Double = surfaceController.activeSurface?.scaleFactor ?: 1.0) {
        surfaceController.resize(width, height, scaleFactor)
        lastOperation = "Resized active surface to ${width}x$height"
    }

    fun pauseActive() {
        surfaceController.pause()
        lastOperation = "Paused active surface"
    }

    fun resumeActive() {
        surfaceController.resume()
        lastOperation = "Resumed active surface"
    }

    fun loadSampleDiscoveryData() {
        rememberedFilters = sampleFilters()
        rememberedPoints = samplePoints()
        applyRememberedDataToMountedSurface()
        lastOperation = "Loaded sample discovery dataset"
    }

    fun focusLocation(locationId: Long = 601L) {
        surfaceController.focusLocation(locationId)
        surfaceController.dispatchNativeCallbacks()
        lastOperation = "Focused location $locationId"
    }

    fun moveCamera(cameraState: FrameflowCameraState = sampleCameraState()) {
        rememberedCameraState = cameraState
        if (surfaceController.activeSurface != null) {
            surfaceController.reportCameraChanged(cameraState)
            surfaceController.dispatchNativeCallbacks()
        }
        lastOperation = "Updated runtime camera snapshot"
    }

    override fun close() {
        surfaceController.close()
    }

    private fun mount(surface: FrameflowSurfaceMount, successMessage: String): FrameflowHostBootstrapResult {
        val result = surfaceController.mount(surface)
        if (result is FrameflowHostBootstrapResult.Started) {
            replayRememberedState()
            lastOperation = successMessage
        } else if (result is FrameflowHostBootstrapResult.Failed) {
            lastOperation = "Mount failed: ${result.failureKind}"
        }
        return result
    }

    private fun replayRememberedState() {
        applyRememberedDataToMountedSurface()
        rememberedCameraState?.let {
            surfaceController.reportCameraChanged(it)
        }
        surfaceController.dispatchNativeCallbacks()
    }

    private fun applyRememberedDataToMountedSurface() {
        if (surfaceController.activeSurface == null) {
            return
        }
        if (rememberedFilters != FrameflowFilterSnapshot()) {
            surfaceController.updateFilters(rememberedFilters)
        }
        if (rememberedPoints.isNotEmpty()) {
            surfaceController.replacePoints(rememberedPoints)
        }
    }

    companion object {
        fun create(config: FrameflowHostSurfaceConfig): FrameflowHostWorkbenchController {
            return FrameflowHostWorkbenchController(FrameflowHostSurfaceController(config))
        }

        fun defaultEmbeddedSurface(): FrameflowSurfaceMount {
            return FrameflowSurfaceMount(
                surfaceHandle = 1001L,
                width = 1280,
                height = 720,
                scaleFactor = 1.0,
                mode = FrameflowSurfaceMode.Embedded,
            )
        }

        fun defaultFullscreenSurface(): FrameflowSurfaceMount {
            return FrameflowSurfaceMount(
                surfaceHandle = 2001L,
                width = 1920,
                height = 1080,
                scaleFactor = 1.25,
                mode = FrameflowSurfaceMode.Fullscreen,
            )
        }

        fun sampleFilters(): FrameflowFilterSnapshot {
            return FrameflowFilterSnapshot(
                query = "election",
                categoryCode = "POLITICS",
                locationId = 601,
                countryCode = "RS",
            )
        }

        fun samplePoints(): List<RenderReadyPoint> {
            return listOf(
                RenderReadyPoint(
                    locationId = 601,
                    label = "Belgrade",
                    kind = FrameflowLocationKind.CITY,
                    countryCode = "RS",
                    latitude = 44.7866,
                    longitude = 20.4489,
                    storyCount = 8,
                    latestStoryEpochMillis = 1_776_335_400_000L,
                    topCategories = listOf("POLITICS", "BREAKING"),
                    styleKey = "category-politics",
                ),
                RenderReadyPoint(
                    locationId = 602,
                    label = "Novi Sad",
                    kind = FrameflowLocationKind.CITY,
                    countryCode = "RS",
                    latitude = 45.2671,
                    longitude = 19.8335,
                    storyCount = 5,
                    latestStoryEpochMillis = 1_776_333_000_000L,
                    topCategories = listOf("SPORT"),
                    styleKey = "category-sport",
                ),
                RenderReadyPoint(
                    locationId = 603,
                    label = "Nis",
                    kind = FrameflowLocationKind.CITY,
                    countryCode = "RS",
                    latitude = 43.3209,
                    longitude = 21.8958,
                    storyCount = 3,
                    latestStoryEpochMillis = 1_776_328_200_000L,
                    topCategories = listOf("ECONOMY"),
                    styleKey = "category-economy",
                ),
            )
        }

        fun sampleCameraState(): FrameflowCameraState {
            return FrameflowCameraState(
                longitude = 21.0,
                latitude = 45.0,
                heightMeters = 1_800_000.0,
                headingDegrees = 15.0,
                pitchDegrees = -30.0,
                rollDegrees = 0.0,
            )
        }
    }
}
