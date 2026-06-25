#include "renderer/cartography/cartography_dataset.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

namespace frameflow::renderer::cartography {

namespace {

constexpr std::string_view kSchemaHeader = "frameflow-boundary-overlay-v1";
constexpr const char* kRuntimeOverrideEnv = "FRAMEFLOW_BOUNDARY_OVERLAY_PATH";

std::string trim(std::string_view value) {
    std::size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        start += 1;
    }

    std::size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        end -= 1;
    }

    return std::string(value.substr(start, end - start));
}

std::vector<std::string> split(const std::string_view value, const char delimiter) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto end = value.find(delimiter, start);
        if (end == std::string_view::npos) {
            parts.emplace_back(value.substr(start));
            break;
        }
        parts.emplace_back(value.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

std::optional<std::string> env_string(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return std::nullopt;
    }
    return std::string(value);
}

bool try_parse_double(const std::string_view value, double* result) {
    try {
        std::size_t parsed = 0;
        const double parsed_value = std::stod(std::string(value), &parsed);
        if (parsed != value.size()) {
            return false;
        }
        *result = parsed_value;
        return true;
    } catch (...) {
        return false;
    }
}

bool try_parse_int(const std::string_view value, int* result) {
    try {
        std::size_t parsed = 0;
        const int parsed_value = std::stoi(std::string(value), &parsed);
        if (parsed != value.size()) {
            return false;
        }
        *result = parsed_value;
        return true;
    } catch (...) {
        return false;
    }
}

std::optional<CartographyBoundaryKind> parse_boundary_kind(const std::string_view value) {
    if (value == "country") {
        return CartographyBoundaryKind::Country;
    }
    return std::nullopt;
}

std::optional<CartographyLabelKind> parse_label_kind(const std::string_view value) {
    if (value == "country") {
        return CartographyLabelKind::Country;
    }
    if (value == "capital") {
        return CartographyLabelKind::Capital;
    }
    if (value == "major_city") {
        return CartographyLabelKind::MajorCity;
    }
    if (value == "city") {
        return CartographyLabelKind::City;
    }
    return std::nullopt;
}

std::optional<std::vector<CartographyCoordinate>> parse_vertices(const std::string_view value) {
    std::vector<CartographyCoordinate> vertices;
    for (const auto& pair : split(value, ';')) {
        if (pair.empty()) {
            continue;
        }
        const auto pieces = split(pair, ',');
        if (pieces.size() != 2u) {
            return std::nullopt;
        }

        CartographyCoordinate coordinate;
        if (!try_parse_double(trim(pieces[0]), &coordinate.longitude) ||
            !try_parse_double(trim(pieces[1]), &coordinate.latitude)) {
            return std::nullopt;
        }
        vertices.push_back(coordinate);
    }

    if (vertices.size() < 2u) {
        return std::nullopt;
    }
    return vertices;
}

CartographyDataset unavailable_dataset(
    const std::filesystem::path& source_path,
    std::string load_status
) {
    CartographyDataset dataset;
    dataset.source = CartographyDatasetSource::Unavailable;
    dataset.source_path = source_path;
    dataset.load_status = std::move(load_status);
    return dataset;
}

} // namespace

bool CartographyDataset::available() const noexcept {
    return source != CartographyDatasetSource::Unavailable;
}

std::size_t CartographyDataset::label_count(const CartographyLabelKind kind) const noexcept {
    return static_cast<std::size_t>(std::count_if(labels.begin(), labels.end(), [kind](const auto& label) {
        return label.kind == kind;
    }));
}

std::string CartographyDataset::diagnostics_summary() const {
    std::ostringstream out;
    out << "source=" << to_string(source)
        << " dataset_id=" << (metadata.dataset_id.empty() ? "none" : metadata.dataset_id)
        << " locale=" << (metadata.locale.empty() ? "und" : metadata.locale)
        << " boundaries=" << boundaries.size()
        << " labels=" << labels.size()
        << " country_labels=" << label_count(CartographyLabelKind::Country)
        << " capital_labels=" << label_count(CartographyLabelKind::Capital)
        << " major_city_labels=" << label_count(CartographyLabelKind::MajorCity)
        << " city_labels=" << label_count(CartographyLabelKind::City)
        << " path=" << (source_path.empty() ? "none" : source_path.string())
        << " status=" << (load_status.empty() ? "ready" : load_status);
    return out.str();
}

CartographyDataset CartographyDatasetLoader::load_default() {
    const auto runtime_override = env_string(kRuntimeOverrideEnv);
    if (runtime_override.has_value()) {
        return load_from_path(*runtime_override, CartographyDatasetSource::RuntimeOverride);
    }

    return unavailable_dataset({}, std::string("missing_env=") + kRuntimeOverrideEnv);
}

CartographyDataset CartographyDatasetLoader::load_from_path(
    const std::filesystem::path& path,
    const CartographyDatasetSource source_hint
) {
    if (path.empty()) {
        return unavailable_dataset(path, "missing_source_path");
    }

    std::ifstream input(path);
    if (!input.is_open()) {
        return unavailable_dataset(path, "open_failed");
    }

    CartographyDataset dataset;
    dataset.source = source_hint;
    dataset.source_path = path;
    dataset.load_status = "ready";

    bool header_seen = false;
    std::size_t line_number = 0;
    for (std::string line; std::getline(input, line);) {
        line_number += 1;
        const std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }

        if (!header_seen) {
            if (trimmed != kSchemaHeader) {
                return unavailable_dataset(path, "invalid_header_line=" + std::to_string(line_number));
            }
            header_seen = true;
            dataset.metadata.schema = trimmed;
            continue;
        }

        const auto fields = split(trimmed, '\t');
        if (fields.empty()) {
            continue;
        }

        if (fields[0] == "meta") {
            for (std::size_t index = 1; index < fields.size(); ++index) {
                const auto separator = fields[index].find('=');
                if (separator == std::string::npos) {
                    return unavailable_dataset(path, "invalid_meta_line=" + std::to_string(line_number));
                }

                const std::string key = trim(std::string_view(fields[index]).substr(0, separator));
                const std::string value = trim(std::string_view(fields[index]).substr(separator + 1));
                if (key == "dataset_id") {
                    dataset.metadata.dataset_id = value;
                } else if (key == "locale") {
                    dataset.metadata.locale = value;
                }
            }
            continue;
        }

        if (fields[0] == "boundary") {
            if (fields.size() != 7u) {
                return unavailable_dataset(path, "invalid_boundary_line=" + std::to_string(line_number));
            }

            auto kind = parse_boundary_kind(fields[1]);
            if (!kind.has_value()) {
                return unavailable_dataset(path, "invalid_boundary_kind_line=" + std::to_string(line_number));
            }

            int priority = 0;
            if (!try_parse_int(fields[5], &priority)) {
                return unavailable_dataset(path, "invalid_boundary_priority_line=" + std::to_string(line_number));
            }

            auto vertices = parse_vertices(fields[6]);
            if (!vertices.has_value()) {
                return unavailable_dataset(path, "invalid_boundary_vertices_line=" + std::to_string(line_number));
            }

            dataset.boundaries.push_back(CartographyBoundaryFeature{
                .kind = *kind,
                .id = fields[2],
                .label = fields[3],
                .country_code = fields[4],
                .priority = priority,
                .vertices = std::move(*vertices),
            });
            continue;
        }

        if (fields[0] == "label") {
            if (fields.size() != 10u) {
                return unavailable_dataset(path, "invalid_label_line=" + std::to_string(line_number));
            }

            auto kind = parse_label_kind(fields[1]);
            if (!kind.has_value()) {
                return unavailable_dataset(path, "invalid_label_kind_line=" + std::to_string(line_number));
            }

            double longitude = 0.0;
            double latitude = 0.0;
            double min_height_meters = 0.0;
            double max_height_meters = 0.0;
            int priority = 0;
            if (!try_parse_double(fields[5], &longitude) ||
                !try_parse_double(fields[6], &latitude) ||
                !try_parse_int(fields[7], &priority) ||
                !try_parse_double(fields[8], &min_height_meters) ||
                !try_parse_double(fields[9], &max_height_meters)) {
                return unavailable_dataset(path, "invalid_label_value_line=" + std::to_string(line_number));
            }

            dataset.labels.push_back(CartographyLabelFeature{
                .kind = *kind,
                .id = fields[2],
                .text = fields[3],
                .country_code = fields[4],
                .longitude = longitude,
                .latitude = latitude,
                .priority = priority,
                .min_height_meters = min_height_meters,
                .max_height_meters = max_height_meters,
            });
            continue;
        }

        return unavailable_dataset(path, "unknown_record_type_line=" + std::to_string(line_number));
    }

    if (!header_seen) {
        return unavailable_dataset(path, "missing_header");
    }

    if (dataset.metadata.dataset_id.empty()) {
        dataset.metadata.dataset_id = path.stem().string();
    }

    return dataset;
}

const char* to_string(const CartographyDatasetSource source) noexcept {
    switch (source) {
        case CartographyDatasetSource::Unavailable:
            return "unavailable";
        case CartographyDatasetSource::RuntimeOverride:
            return "runtime-override";
    }
    return "unavailable";
}

const char* to_string(const CartographyLabelKind kind) noexcept {
    switch (kind) {
        case CartographyLabelKind::Country:
            return "country";
        case CartographyLabelKind::Capital:
            return "capital";
        case CartographyLabelKind::MajorCity:
            return "major_city";
        case CartographyLabelKind::City:
            return "city";
    }
    return "city";
}

} // namespace frameflow::renderer::cartography
