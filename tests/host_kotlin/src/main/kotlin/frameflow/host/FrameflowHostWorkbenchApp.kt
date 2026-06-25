package frameflow.host

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Window
import androidx.compose.ui.window.application
import java.nio.file.Path

fun main() = application {
    val controller = remember {
        FrameflowHostWorkbenchController.create(
            FrameflowHostSurfaceConfig(
                packagedNativeDir = resolvePackagedNativeDir(),
            ),
        )
    }
    var workbenchState by remember { mutableStateOf(controller.snapshot()) }

    fun refresh(after: () -> Unit) {
        after()
        workbenchState = controller.snapshot()
    }

    LaunchedEffect(Unit) {
        if (workbenchState.activeSurface == null) {
            refresh { controller.mountEmbedded() }
        }
    }

    DisposableEffect(Unit) {
        onDispose {
            controller.close()
        }
    }

    Window(
        onCloseRequest = ::exitApplication,
        title = "Frameflow Host Workbench",
    ) {
        MaterialTheme {
            Surface(modifier = Modifier.fillMaxSize()) {
                FrameflowHostWorkbenchScreen(
                    state = workbenchState,
                    onMountEmbedded = { refresh { controller.mountEmbedded() } },
                    onFullscreen = { refresh { controller.handoffToFullscreen() } },
                    onReturnEmbedded = { refresh { controller.returnToEmbedded() } },
                    onLoadSample = { refresh { controller.loadSampleDiscoveryData() } },
                    onFocusBelgrade = { refresh { controller.focusLocation() } },
                    onMoveCamera = { refresh { controller.moveCamera() } },
                    onResize = { refresh { controller.resizeActive(width = 1440, height = 810, scaleFactor = 1.25) } },
                    onPause = { refresh { controller.pauseActive() } },
                    onResume = { refresh { controller.resumeActive() } },
                )
            }
        }
    }
}

@Composable
private fun FrameflowHostWorkbenchScreen(
    state: FrameflowHostWorkbenchState,
    onMountEmbedded: () -> Unit,
    onFullscreen: () -> Unit,
    onReturnEmbedded: () -> Unit,
    onLoadSample: () -> Unit,
    onFocusBelgrade: () -> Unit,
    onMoveCamera: () -> Unit,
    onResize: () -> Unit,
    onPause: () -> Unit,
    onResume: () -> Unit,
) {
    Column(
        modifier = Modifier.fillMaxSize().padding(20.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Text("Frameflow Host Workbench", style = MaterialTheme.typography.headlineSmall)
        Text("Last operation: ${state.lastOperation}")
        Text("Surface: ${state.activeSurface?.mode ?: "none"}")
        Text("Mount sequence: ${state.mountSequence}")
        Text("Host status: ${state.hostState.status}")
        Text("Remembered point count: ${state.rememberedPointCount}")
        Text("Diagnostics: ${state.hostState.diagnosticsSummary ?: "none"}")
        Text("Camera: ${state.hostState.cameraState ?: "none"}")
        Text("Last error: ${state.hostState.lastError?.message ?: "none"}")

        Spacer(Modifier.height(8.dp))

        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Button(onClick = onMountEmbedded) { Text("Mount Embedded") }
            Button(onClick = onFullscreen) { Text("Fullscreen") }
            Button(onClick = onReturnEmbedded) { Text("Back to Embedded") }
        }

        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Button(onClick = onLoadSample) { Text("Load Sample Data") }
            Button(onClick = onFocusBelgrade) { Text("Focus Belgrade") }
            Button(onClick = onMoveCamera) { Text("Move Camera") }
        }

        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Button(onClick = onResize) { Text("Resize 1440x810") }
            Button(onClick = onPause) { Text("Pause") }
            Button(onClick = onResume) { Text("Resume") }
        }
    }
}

private fun resolvePackagedNativeDir(): Path? {
    val explicitDir = System.getProperty("frameflow.host.packagedNativeDir")
    if (!explicitDir.isNullOrBlank()) {
        return Path.of(explicitDir).toAbsolutePath().normalize()
    }

    val nativeLibraryPath = System.getProperty("frameflow.native.lib")
    if (!nativeLibraryPath.isNullOrBlank()) {
        return Path.of(nativeLibraryPath).toAbsolutePath().normalize().parent
    }

    return Path.of("").toAbsolutePath().normalize().resolve("../../build/dev").normalize()
}
