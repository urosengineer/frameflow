package frameflow.host

import java.nio.file.Files
import java.nio.file.Path

private const val FrameflowLibraryPathEnv = "FRAMEFLOW_LIBRARY_PATH"

enum class FrameflowStartupFailureKind {
    Unavailable,
    Incompatible,
    InitFailed,
}

enum class FrameflowNativeLibrarySource {
    EnvironmentOverride,
    PackagedDirectory,
    SystemLoaderFallback,
}

sealed interface FrameflowNativeLibraryTarget {
    data class AbsolutePath(val path: Path) : FrameflowNativeLibraryTarget
    data class LibraryName(val value: String) : FrameflowNativeLibraryTarget
}

data class FrameflowNativeLibraryResolution(
    val source: FrameflowNativeLibrarySource,
    val target: FrameflowNativeLibraryTarget,
) {
    val description: String
        get() = when (target) {
            is FrameflowNativeLibraryTarget.AbsolutePath -> target.path.toAbsolutePath().normalize().toString()
            is FrameflowNativeLibraryTarget.LibraryName -> target.value
        }
}

data class FrameflowHostBootstrapRequest(
    val packagedNativeDir: Path? = null,
    val environment: Map<String, String> = System.getenv(),
    val systemLibraryName: String = "frameflow",
    val expectedBridgeAbiVersion: Int = 2,
    val expectedCommandVersionMajor: Int = 2,
    val surfaceHandle: Long = 1L,
    val width: Int = 1280,
    val height: Int = 720,
    val scaleFactor: Double = 1.0,
    val options: FrameflowNativeOptions = FrameflowNativeOptions(),
    val stateStore: FrameflowHostStateStore = FrameflowHostStateStore(),
)

sealed interface FrameflowHostBootstrapResult {
    data class Started(
        val host: FrameflowHostPrototype,
        val resolution: FrameflowNativeLibraryResolution,
    ) : FrameflowHostBootstrapResult

    data class Failed(
        val failureKind: FrameflowStartupFailureKind,
        val state: FrameflowHostState,
        val resolution: FrameflowNativeLibraryResolution?,
    ) : FrameflowHostBootstrapResult
}

class FrameflowNativeLibraryResolver(
    private val packagedNativeDir: Path?,
    private val environment: Map<String, String>,
    private val systemLibraryName: String,
) {
    fun resolve(): FrameflowNativeLibraryResolution {
        val environmentOverride = environment[FrameflowLibraryPathEnv]?.trim()
        if (!environmentOverride.isNullOrEmpty()) {
            return FrameflowNativeLibraryResolution(
                source = FrameflowNativeLibrarySource.EnvironmentOverride,
                target = FrameflowNativeLibraryTarget.AbsolutePath(Path.of(environmentOverride)),
            )
        }

        val packagedNativeLibrary = packagedNativeDir
            ?.resolve(System.mapLibraryName(systemLibraryName))
            ?.takeIf(Files::isRegularFile)
        if (packagedNativeLibrary != null) {
            return FrameflowNativeLibraryResolution(
                source = FrameflowNativeLibrarySource.PackagedDirectory,
                target = FrameflowNativeLibraryTarget.AbsolutePath(packagedNativeLibrary),
            )
        }

        return FrameflowNativeLibraryResolution(
            source = FrameflowNativeLibrarySource.SystemLoaderFallback,
            target = FrameflowNativeLibraryTarget.LibraryName(systemLibraryName),
        )
    }
}

object FrameflowHostBootstrapper {
    fun bootstrap(request: FrameflowHostBootstrapRequest): FrameflowHostBootstrapResult {
        val stateStore = request.stateStore
        stateStore.markLoading()

        val resolution = FrameflowNativeLibraryResolver(
            packagedNativeDir = request.packagedNativeDir,
            environment = request.environment,
            systemLibraryName = request.systemLibraryName,
        ).resolve()
        stateStore.recordLibraryResolution(resolution)

        val host = try {
            FrameflowHostPrototype.load(
                target = resolution.target,
                expectedBridgeAbiVersion = request.expectedBridgeAbiVersion,
                expectedCommandVersionMajor = request.expectedCommandVersionMajor,
                stateStore = stateStore,
            )
        } catch (error: UnsatisfiedLinkError) {
            stateStore.recordStartupFailure(
                kind = FrameflowStartupFailureKind.Unavailable,
                error = FrameflowHostError(
                    code = FrameflowNativeResult.NOT_FOUND,
                    message = error.message ?: "Failed to load native frameflow library",
                    recoverable = true,
                    runtimeInfo = null,
                ),
            )
            return FrameflowHostBootstrapResult.Failed(
                failureKind = FrameflowStartupFailureKind.Unavailable,
                state = stateStore.snapshot(),
                resolution = resolution,
            )
        }

        try {
            host.runtimeInfo()
            host.checkCompatibility()
        } catch (error: FrameflowHostException) {
            host.close()
            stateStore.recordStartupFailure(
                kind = FrameflowStartupFailureKind.Incompatible,
                error = FrameflowHostError(
                    code = error.result,
                    message = error.failureMessage,
                    recoverable = false,
                    runtimeInfo = stateStore.snapshot().runtimeInfo,
                ),
            )
            return FrameflowHostBootstrapResult.Failed(
                failureKind = FrameflowStartupFailureKind.Incompatible,
                state = stateStore.snapshot(),
                resolution = resolution,
            )
        }

        try {
            host.start(
                surfaceHandle = request.surfaceHandle,
                width = request.width,
                height = request.height,
                scaleFactor = request.scaleFactor,
                options = request.options,
            )
            host.dispatchNativeCallbacks()
            return FrameflowHostBootstrapResult.Started(host, resolution)
        } catch (error: FrameflowHostException) {
            val nativeError = host.lastNativeError()
            val diagnosticsSummary = host.diagnosticsSummaryOrNull()
            host.close()
            stateStore.recordStartupFailure(
                kind = FrameflowStartupFailureKind.InitFailed,
                error = nativeError ?: FrameflowHostError(
                    code = error.result,
                    message = error.failureMessage,
                    recoverable = false,
                    runtimeInfo = stateStore.snapshot().runtimeInfo,
                ),
                diagnosticsSummary = diagnosticsSummary,
            )
            return FrameflowHostBootstrapResult.Failed(
                failureKind = FrameflowStartupFailureKind.InitFailed,
                state = stateStore.snapshot(),
                resolution = resolution,
            )
        }
    }
}
