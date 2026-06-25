package frameflow.host

import kotlin.test.assertEquals
import kotlin.test.assertIs
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertTrue
import org.junit.jupiter.api.Test
import java.nio.file.Path

class FrameflowHostBootstrapTest {
    @Test
    fun resolverPrefersEnvironmentOverrideOverPackagedDirectory() {
        val packagedDir = resolveNativeLibraryPath().parent
        val resolution = FrameflowNativeLibraryResolver(
            packagedNativeDir = packagedDir,
            environment = mapOf("FRAMEFLOW_LIBRARY_PATH" to "/tmp/custom/libframeflow.so"),
            systemLibraryName = "frameflow",
        ).resolve()

        assertEquals(FrameflowNativeLibrarySource.EnvironmentOverride, resolution.source)
        assertEquals(
            Path.of("/tmp/custom/libframeflow.so").toAbsolutePath().normalize(),
            assertIs<FrameflowNativeLibraryTarget.AbsolutePath>(resolution.target).path.toAbsolutePath().normalize(),
        )
    }

    @Test
    fun resolverFallsBackToPackagedDirectoryBeforeSystemLoader() {
        val nativeLibraryPath = resolveNativeLibraryPath()
        val resolution = FrameflowNativeLibraryResolver(
            packagedNativeDir = nativeLibraryPath.parent,
            environment = emptyMap(),
            systemLibraryName = "frameflow",
        ).resolve()

        assertEquals(FrameflowNativeLibrarySource.PackagedDirectory, resolution.source)
        assertEquals(
            nativeLibraryPath.toAbsolutePath().normalize(),
            assertIs<FrameflowNativeLibraryTarget.AbsolutePath>(resolution.target).path.toAbsolutePath().normalize(),
        )
    }

    @Test
    fun bootstrapReturnsUnavailableWhenEnvironmentOverrideDoesNotExist() {
        val result = FrameflowHostBootstrapper.bootstrap(
            FrameflowHostBootstrapRequest(
                packagedNativeDir = resolveNativeLibraryPath().parent,
                environment = mapOf("FRAMEFLOW_LIBRARY_PATH" to "/tmp/does-not-exist/libframeflow.so"),
            ),
        )

        val failure = assertIs<FrameflowHostBootstrapResult.Failed>(result)
        assertEquals(FrameflowStartupFailureKind.Unavailable, failure.failureKind)
        assertEquals(FrameflowHostStatus.Unavailable, failure.state.status)
        assertTrue(failure.state.canRetry)
        assertEquals(FrameflowStartupFailureKind.Unavailable, failure.state.startupFailureKind)
        assertNull(failure.state.runtimeInfo)
    }

    @Test
    fun bootstrapReturnsIncompatibleWhenAbiExpectationDoesNotMatch() {
        val result = FrameflowHostBootstrapper.bootstrap(
            FrameflowHostBootstrapRequest(
                packagedNativeDir = resolveNativeLibraryPath().parent,
                environment = emptyMap(),
                expectedBridgeAbiVersion = 999,
            ),
        )

        val failure = assertIs<FrameflowHostBootstrapResult.Failed>(result)
        assertEquals(FrameflowStartupFailureKind.Incompatible, failure.failureKind)
        assertEquals(FrameflowHostStatus.Failed, failure.state.status)
        assertEquals(FrameflowStartupFailureKind.Incompatible, failure.state.startupFailureKind)
        assertNotNull(failure.state.runtimeInfo)
        assertEquals(2, failure.state.runtimeInfo?.bridgeAbiVersion)
        assertTrue(!failure.state.canRetry)
    }

    @Test
    fun bootstrapReturnsInitFailedForInvalidStartupArguments() {
        val result = FrameflowHostBootstrapper.bootstrap(
            FrameflowHostBootstrapRequest(
                packagedNativeDir = resolveNativeLibraryPath().parent,
                environment = emptyMap(),
                width = 0,
            ),
        )

        val failure = assertIs<FrameflowHostBootstrapResult.Failed>(result)
        assertEquals(FrameflowStartupFailureKind.InitFailed, failure.failureKind)
        assertEquals(FrameflowHostStatus.Failed, failure.state.status)
        assertEquals(FrameflowStartupFailureKind.InitFailed, failure.state.startupFailureKind)
        assertNotNull(failure.state.lastError)
        assertTrue(failure.state.lastError?.message?.contains("positive size") == true)
    }

    @Test
    fun bootstrapStartsHostThroughPackagedDirectoryAndLeavesItEmptyUntilPointsArrive() {
        val result = FrameflowHostBootstrapper.bootstrap(
            FrameflowHostBootstrapRequest(
                packagedNativeDir = resolveNativeLibraryPath().parent,
                environment = emptyMap(),
            ),
        )

        val started = assertIs<FrameflowHostBootstrapResult.Started>(result)
        started.host.use {
            assertEquals(FrameflowNativeLibrarySource.PackagedDirectory, started.resolution.source)
            assertEquals(FrameflowHostStatus.Empty, it.state.status)
            assertEquals(1, it.state.readyEventCount)
            assertEquals(0, it.state.pointSnapshotCount)
            assertNull(it.state.startupFailureKind)
        }
    }

    @Test
    fun repeatedMountUnmountWithSharedStoreDoesNotCarryOldNativeSnapshotState() {
        val sharedStore = FrameflowHostStateStore()
        val filters = FrameflowFilterSnapshot(
            query = "election",
            categoryCode = "POLITICS",
            locationId = 601,
            countryCode = "RS",
        )

        val firstStart = assertIs<FrameflowHostBootstrapResult.Started>(
            FrameflowHostBootstrapper.bootstrap(
                FrameflowHostBootstrapRequest(
                    packagedNativeDir = resolveNativeLibraryPath().parent,
                    environment = emptyMap(),
                    stateStore = sharedStore,
                ),
            ),
        )
        firstStart.host.use {
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
            it.focusLocation(601)
            assertEquals(1, it.dispatchNativeCallbacks())
        }

        val secondStart = assertIs<FrameflowHostBootstrapResult.Started>(
            FrameflowHostBootstrapper.bootstrap(
                FrameflowHostBootstrapRequest(
                    packagedNativeDir = resolveNativeLibraryPath().parent,
                    environment = emptyMap(),
                    stateStore = sharedStore,
                ),
            ),
        )
        secondStart.host.use {
            val state = it.state
            assertEquals(FrameflowHostStatus.Empty, state.status)
            assertEquals(0, state.pointSnapshotCount)
            assertEquals(filters, state.activeFilters)
            assertNull(state.selectedLocationId)
            assertNull(state.lastLocationSelection)
        }
    }

    private fun resolveNativeLibraryPath(): Path {
        val configured = System.getProperty("frameflow.native.lib")
            ?: error("frameflow.native.lib system property must be set by the test task")
        return Path.of(configured).toAbsolutePath().normalize()
    }
}
