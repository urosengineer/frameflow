package frameflow.host

import com.sun.jna.Callback
import com.sun.jna.Library
import com.sun.jna.Native
import com.sun.jna.Pointer
import com.sun.jna.StringArray
import com.sun.jna.Structure
import com.sun.jna.ptr.LongByReference
import java.nio.file.Path

enum class FrameflowNativeResult(val code: Int) {
    OK(0),
    INVALID_ARGUMENT(1),
    INVALID_STATE(2),
    NOT_FOUND(3),
    NOT_SUPPORTED(4),
    INTERNAL_ERROR(5);

    companion object {
        fun fromCode(code: Int): FrameflowNativeResult {
            return entries.firstOrNull { it.code == code } ?: INTERNAL_ERROR
        }
    }
}

enum class FrameflowLocationKind(val nativeCode: Int) {
    UNKNOWN(0),
    POINT(1),
    CITY(2),
    REGION(3),
    COUNTRY(4)
}

data class FrameflowRuntimeInfo(
    val bridgeAbiVersion: Int,
    val engineVersionMajor: Int,
    val engineVersionMinor: Int,
    val engineVersionPatch: Int,
    val commandVersionMajor: Int,
    val commandVersionMinor: Int,
)

data class RenderReadyPoint(
    val locationId: Long,
    val label: String,
    val kind: FrameflowLocationKind,
    val countryCode: String?,
    val latitude: Double,
    val longitude: Double,
    val storyCount: Int,
    val latestStoryEpochMillis: Long,
    val topCategories: List<String> = emptyList(),
    val styleKey: String? = null,
)

data class FrameflowFilterSnapshot(
    val query: String? = null,
    val categoryCode: String? = null,
    val locationId: Long? = null,
    val countryCode: String? = null,
    val fromEpochMillis: Long? = null,
    val toEpochMillis: Long? = null,
)

data class FrameflowNativeOptions(
    val theme: String = "dark",
    val tileCachePath: String = "/tmp/frameflow-cache",
    val maxTileCacheBytes: Long = 1_073_741_824L,
    val logLevel: String = "info",
)

data class FrameflowReady(
    val width: Int,
    val height: Int,
    val scaleFactor: Double,
    val runtimeInfo: FrameflowRuntimeInfo,
)

data class FrameflowLocationSelection(
    val locationId: Long,
    val categoryCode: String?,
    val screenX: Double,
    val screenY: Double,
    val interaction: String?,
)

data class FrameflowClusterSelection(
    val clusterId: Long,
    val pointCount: Int,
    val longitude: Double,
    val latitude: Double,
)

data class FrameflowCameraState(
    val longitude: Double,
    val latitude: Double,
    val heightMeters: Double,
    val headingDegrees: Double,
    val pitchDegrees: Double,
    val rollDegrees: Double,
)

data class FrameflowHostError(
    val code: FrameflowNativeResult,
    val message: String?,
    val recoverable: Boolean,
    val runtimeInfo: FrameflowRuntimeInfo?,
)

@Structure.FieldOrder(
    "bridge_abi_version",
    "engine_version_major",
    "engine_version_minor",
    "engine_version_patch",
    "command_version_major",
    "command_version_minor",
)
open class FrameflowRuntimeInfoStruct() : Structure() {
    constructor(pointer: Pointer) : this() {
        useMemory(pointer)
        read()
    }

    @JvmField
    var bridge_abi_version: Int = 0

    @JvmField
    var engine_version_major: Int = 0

    @JvmField
    var engine_version_minor: Int = 0

    @JvmField
    var engine_version_patch: Int = 0

    @JvmField
    var command_version_major: Int = 0

    @JvmField
    var command_version_minor: Int = 0

    fun toModel(): FrameflowRuntimeInfo {
        return FrameflowRuntimeInfo(
            bridgeAbiVersion = bridge_abi_version,
            engineVersionMajor = engine_version_major,
            engineVersionMinor = engine_version_minor,
            engineVersionPatch = engine_version_patch,
            commandVersionMajor = command_version_major,
            commandVersionMinor = command_version_minor,
        )
    }
}

@Structure.FieldOrder("theme", "tile_cache_path", "max_tile_cache_bytes", "log_level")
open class FrameflowOptionsStruct() : Structure() {
    @JvmField
    var theme: String? = null

    @JvmField
    var tile_cache_path: String? = null

    @JvmField
    var max_tile_cache_bytes: Long = 0

    @JvmField
    var log_level: String? = null

    companion object {
        fun from(options: FrameflowNativeOptions): FrameflowOptionsStruct {
            return FrameflowOptionsStruct().apply {
                theme = options.theme
                tile_cache_path = options.tileCachePath
                max_tile_cache_bytes = options.maxTileCacheBytes
                log_level = options.logLevel
                write()
            }
        }
    }
}

@Structure.FieldOrder(
    "query",
    "category_code",
    "location_id",
    "has_location_id",
    "country_code",
    "from_epoch_millis",
    "has_from_epoch_millis",
    "to_epoch_millis",
    "has_to_epoch_millis",
)
open class FrameflowFilterStruct() : Structure() {
    @JvmField
    var query: String? = null

    @JvmField
    var category_code: String? = null

    @JvmField
    var location_id: Long = 0

    @JvmField
    var has_location_id: Byte = 0

    @JvmField
    var country_code: String? = null

    @JvmField
    var from_epoch_millis: Long = 0

    @JvmField
    var has_from_epoch_millis: Byte = 0

    @JvmField
    var to_epoch_millis: Long = 0

    @JvmField
    var has_to_epoch_millis: Byte = 0

    companion object {
        fun from(filter: FrameflowFilterSnapshot): FrameflowFilterStruct {
            return FrameflowFilterStruct().apply {
                query = filter.query
                category_code = filter.categoryCode
                location_id = filter.locationId ?: 0L
                has_location_id = if (filter.locationId != null) 1 else 0
                country_code = filter.countryCode
                from_epoch_millis = filter.fromEpochMillis ?: 0L
                has_from_epoch_millis = if (filter.fromEpochMillis != null) 1 else 0
                to_epoch_millis = filter.toEpochMillis ?: 0L
                has_to_epoch_millis = if (filter.toEpochMillis != null) 1 else 0
                write()
            }
        }
    }
}

@Structure.FieldOrder(
    "location_id",
    "label",
    "kind",
    "country_code",
    "latitude",
    "longitude",
    "story_count",
    "latest_story_epoch_millis",
    "top_categories",
    "top_category_count",
    "style_key",
)
open class FrameflowPointStruct() : Structure() {
    @JvmField
    var location_id: Long = 0

    @JvmField
    var label: String? = null

    @JvmField
    var kind: Int = 0

    @JvmField
    var country_code: String? = null

    @JvmField
    var latitude: Double = 0.0

    @JvmField
    var longitude: Double = 0.0

    @JvmField
    var story_count: Int = 0

    @JvmField
    var latest_story_epoch_millis: Long = 0

    @JvmField
    var top_categories: Pointer? = null

    @JvmField
    var top_category_count: Long = 0

    @JvmField
    var style_key: String? = null
}

@Structure.FieldOrder(
    "width",
    "height",
    "scale_factor",
    "bridge_abi_version",
    "engine_version_major",
    "engine_version_minor",
    "engine_version_patch",
    "command_version_major",
    "command_version_minor",
)
open class FrameflowReadyEventStruct() : Structure() {
    constructor(pointer: Pointer) : this() {
        useMemory(pointer)
        read()
    }

    @JvmField
    var width: Int = 0

    @JvmField
    var height: Int = 0

    @JvmField
    var scale_factor: Double = 0.0

    @JvmField
    var bridge_abi_version: Int = 0

    @JvmField
    var engine_version_major: Int = 0

    @JvmField
    var engine_version_minor: Int = 0

    @JvmField
    var engine_version_patch: Int = 0

    @JvmField
    var command_version_major: Int = 0

    @JvmField
    var command_version_minor: Int = 0

    fun toModel(): FrameflowReady {
        return FrameflowReady(
            width = width,
            height = height,
            scaleFactor = scale_factor,
            runtimeInfo = FrameflowRuntimeInfo(
                bridgeAbiVersion = bridge_abi_version,
                engineVersionMajor = engine_version_major,
                engineVersionMinor = engine_version_minor,
                engineVersionPatch = engine_version_patch,
                commandVersionMajor = command_version_major,
                commandVersionMinor = command_version_minor,
            ),
        )
    }
}

@Structure.FieldOrder("location_id", "category_code", "screen_x", "screen_y", "interaction")
open class FrameflowLocationSelectionEventStruct() : Structure() {
    constructor(pointer: Pointer) : this() {
        useMemory(pointer)
        read()
    }

    @JvmField
    var location_id: Long = 0

    @JvmField
    var category_code: String? = null

    @JvmField
    var screen_x: Double = 0.0

    @JvmField
    var screen_y: Double = 0.0

    @JvmField
    var interaction: String? = null

    fun toModel(): FrameflowLocationSelection {
        return FrameflowLocationSelection(
            locationId = location_id,
            categoryCode = category_code,
            screenX = screen_x,
            screenY = screen_y,
            interaction = interaction,
        )
    }
}

@Structure.FieldOrder("cluster_id", "point_count", "longitude", "latitude")
open class FrameflowClusterSelectionEventStruct() : Structure() {
    constructor(pointer: Pointer) : this() {
        useMemory(pointer)
        read()
    }

    @JvmField
    var cluster_id: Long = 0

    @JvmField
    var point_count: Int = 0

    @JvmField
    var longitude: Double = 0.0

    @JvmField
    var latitude: Double = 0.0

    fun toModel(): FrameflowClusterSelection {
        return FrameflowClusterSelection(
            clusterId = cluster_id,
            pointCount = point_count,
            longitude = longitude,
            latitude = latitude,
        )
    }
}

@Structure.FieldOrder(
    "longitude",
    "latitude",
    "height_meters",
    "heading_degrees",
    "pitch_degrees",
    "roll_degrees",
)
open class FrameflowCameraStateStruct() : Structure() {
    constructor(pointer: Pointer) : this() {
        useMemory(pointer)
        read()
    }

    @JvmField
    var longitude: Double = 0.0

    @JvmField
    var latitude: Double = 0.0

    @JvmField
    var height_meters: Double = 0.0

    @JvmField
    var heading_degrees: Double = 0.0

    @JvmField
    var pitch_degrees: Double = 0.0

    @JvmField
    var roll_degrees: Double = 0.0

    fun toModel(): FrameflowCameraState {
        return FrameflowCameraState(
            longitude = longitude,
            latitude = latitude,
            heightMeters = height_meters,
            headingDegrees = heading_degrees,
            pitchDegrees = pitch_degrees,
            rollDegrees = roll_degrees,
        )
    }
}

@Structure.FieldOrder(
    "code",
    "message",
    "recoverable",
    "bridge_abi_version",
    "engine_version_major",
    "engine_version_minor",
    "engine_version_patch",
    "command_version_major",
    "command_version_minor",
)
open class FrameflowErrorEventStruct() : Structure() {
    constructor(pointer: Pointer) : this() {
        useMemory(pointer)
        read()
    }

    @JvmField
    var code: Int = 0

    @JvmField
    var message: String? = null

    @JvmField
    var recoverable: Byte = 0

    @JvmField
    var bridge_abi_version: Int = 0

    @JvmField
    var engine_version_major: Int = 0

    @JvmField
    var engine_version_minor: Int = 0

    @JvmField
    var engine_version_patch: Int = 0

    @JvmField
    var command_version_major: Int = 0

    @JvmField
    var command_version_minor: Int = 0

    fun toModel(): FrameflowHostError {
        return FrameflowHostError(
            code = FrameflowNativeResult.fromCode(code),
            message = message,
            recoverable = recoverable.toInt() != 0,
            runtimeInfo = FrameflowRuntimeInfo(
                bridgeAbiVersion = bridge_abi_version,
                engineVersionMajor = engine_version_major,
                engineVersionMinor = engine_version_minor,
                engineVersionPatch = engine_version_patch,
                commandVersionMajor = command_version_major,
                commandVersionMinor = command_version_minor,
            ),
        )
    }
}

fun interface FrameflowReadyCallback : Callback {
    fun invoke(user: Pointer?, event: Pointer?)
}

fun interface FrameflowLocationSelectionCallback : Callback {
    fun invoke(user: Pointer?, event: Pointer?)
}

fun interface FrameflowClusterSelectionCallback : Callback {
    fun invoke(user: Pointer?, event: Pointer?)
}

fun interface FrameflowCameraChangedCallback : Callback {
    fun invoke(user: Pointer?, event: Pointer?)
}

fun interface FrameflowErrorCallback : Callback {
    fun invoke(user: Pointer?, event: Pointer?)
}

@Structure.FieldOrder(
    "user",
    "on_ready",
    "on_location_selected",
    "on_cluster_selected",
    "on_camera_changed",
    "on_engine_error",
)
open class FrameflowCallbacksStruct() : Structure() {
    @JvmField
    var user: Pointer? = null

    @JvmField
    var on_ready: FrameflowReadyCallback? = null

    @JvmField
    var on_location_selected: FrameflowLocationSelectionCallback? = null

    @JvmField
    var on_cluster_selected: FrameflowClusterSelectionCallback? = null

    @JvmField
    var on_camera_changed: FrameflowCameraChangedCallback? = null

    @JvmField
    var on_engine_error: FrameflowErrorCallback? = null
}

internal interface FrameflowNativeLibrary : Library {
    fun frameflow_bridge_get_runtime_info(outInfo: FrameflowRuntimeInfoStruct): Int
    fun frameflow_bridge_check_compatibility(
        requiredBridgeAbiVersion: Int,
        requiredCommandVersionMajor: Int,
        outInfo: FrameflowRuntimeInfoStruct,
    ): Int

    fun frameflow_engine_create(): Pointer?
    fun frameflow_engine_destroy(engine: Pointer?)
    fun frameflow_engine_set_callbacks(engine: Pointer?, callbacks: FrameflowCallbacksStruct): Int
    fun frameflow_engine_initialize(
        engine: Pointer?,
        surfaceHandle: Long,
        width: Int,
        height: Int,
        scaleFactor: Double,
        options: FrameflowOptionsStruct,
    ): Int
    fun frameflow_engine_resize(
        engine: Pointer?,
        width: Int,
        height: Int,
        scaleFactor: Double,
    ): Int
    fun frameflow_engine_pause(engine: Pointer?): Int
    fun frameflow_engine_resume(engine: Pointer?): Int

    fun frameflow_engine_set_points(
        engine: Pointer?,
        points: Array<FrameflowPointStruct>?,
        pointCount: Long,
    ): Int
    fun frameflow_engine_set_filters(engine: Pointer?, filter: FrameflowFilterStruct): Int
    fun frameflow_engine_point_count(engine: Pointer?): Long
    fun frameflow_engine_focus_location(engine: Pointer?, locationId: Long): Int
    fun frameflow_engine_select_location(
        engine: Pointer?,
        locationId: Long,
        categoryCode: String?,
        screenX: Double,
        screenY: Double,
        interaction: String?,
    ): Int
    fun frameflow_engine_select_cluster(
        engine: Pointer?,
        clusterId: Long,
        pointCount: Int,
        longitude: Double,
        latitude: Double,
    ): Int
    fun frameflow_engine_report_camera_changed(
        engine: Pointer?,
        longitude: Double,
        latitude: Double,
        heightMeters: Double,
        headingDegrees: Double,
        pitchDegrees: Double,
        rollDegrees: Double,
    ): Int
    fun frameflow_engine_clear_selection(engine: Pointer?): Int
    fun frameflow_engine_dispatch_pending_callbacks(engine: Pointer?): Long
    fun frameflow_engine_dispose(engine: Pointer?): Int
    fun frameflow_engine_has_selected_location_id(engine: Pointer?): Byte
    fun frameflow_engine_get_selected_location_id(engine: Pointer?, outLocationId: LongByReference): Int
    fun frameflow_engine_diagnostics_summary(engine: Pointer?): String?
    fun frameflow_engine_last_error_code(engine: Pointer?): Int
    fun frameflow_engine_last_error_message(engine: Pointer?): String?
    fun frameflow_engine_last_error_recoverable(engine: Pointer?): Byte

    companion object {
        fun load(libraryPath: Path): FrameflowNativeLibrary {
            return load(FrameflowNativeLibraryTarget.AbsolutePath(libraryPath))
        }

        fun load(target: FrameflowNativeLibraryTarget): FrameflowNativeLibrary {
            return when (target) {
                is FrameflowNativeLibraryTarget.AbsolutePath ->
                    Native.load(target.path.toAbsolutePath().toString(), FrameflowNativeLibrary::class.java)
                is FrameflowNativeLibraryTarget.LibraryName ->
                    Native.load(target.value, FrameflowNativeLibrary::class.java)
            }
        }
    }
}

internal class PreparedPointSnapshot private constructor(
    val nativePoints: Array<FrameflowPointStruct>?,
    @Suppress("unused")
    private val keepAlive: List<Any>,
) {
    companion object {
        fun from(points: List<RenderReadyPoint>): PreparedPointSnapshot {
            if (points.isEmpty()) {
                return PreparedPointSnapshot(nativePoints = null, keepAlive = emptyList())
            }

            @Suppress("UNCHECKED_CAST")
            val nativePoints = FrameflowPointStruct().toArray(points.size) as Array<FrameflowPointStruct>
            val keepAlive = mutableListOf<Any>()

            points.forEachIndexed { index, point ->
                val nativePoint = nativePoints[index]
                nativePoint.location_id = point.locationId
                nativePoint.label = point.label
                nativePoint.kind = point.kind.nativeCode
                nativePoint.country_code = point.countryCode
                nativePoint.latitude = point.latitude
                nativePoint.longitude = point.longitude
                nativePoint.story_count = point.storyCount
                nativePoint.latest_story_epoch_millis = point.latestStoryEpochMillis
                nativePoint.style_key = point.styleKey
                if (point.topCategories.isNotEmpty()) {
                    val categories = StringArray(point.topCategories.toTypedArray())
                    nativePoint.top_categories = categories
                    nativePoint.top_category_count = point.topCategories.size.toLong()
                    keepAlive += categories
                }
                nativePoint.write()
            }

            return PreparedPointSnapshot(nativePoints, keepAlive)
        }
    }
}
