#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace frameflow::renderer::cartography {

enum class CartographyBoundaryKind {
    Country
};

enum class CartographyLabelKind {
    Country,
    Capital,
    MajorCity,
    City
};

enum class CartographyDatasetSource {
    Unavailable,
    RuntimeOverride
};

struct CartographyCoordinate {
    double longitude = 0.0;
    double latitude = 0.0;
};

struct CartographyBoundaryFeature {
    CartographyBoundaryKind kind = CartographyBoundaryKind::Country;
    std::string id;
    std::string label;
    std::string country_code;
    int priority = 0;
    std::vector<CartographyCoordinate> vertices;
};

struct CartographyLabelFeature {
    CartographyLabelKind kind = CartographyLabelKind::Country;
    std::string id;
    std::string text;
    std::string country_code;
    double longitude = 0.0;
    double latitude = 0.0;
    int priority = 0;
    double min_height_meters = 0.0;
    double max_height_meters = 0.0;
};

struct CartographyDatasetMetadata {
    std::string schema = "frameflow-boundary-overlay-v1";
    std::string dataset_id;
    std::string locale = "und";
};

struct CartographyDataset {
    CartographyDatasetMetadata metadata;
    CartographyDatasetSource source = CartographyDatasetSource::Unavailable;
    std::filesystem::path source_path;
    std::string load_status = "uninitialized";
    std::vector<CartographyBoundaryFeature> boundaries;
    std::vector<CartographyLabelFeature> labels;

    [[nodiscard]] bool available() const noexcept;
    [[nodiscard]] std::size_t label_count(CartographyLabelKind kind) const noexcept;
    [[nodiscard]] std::string diagnostics_summary() const;
};

class CartographyDatasetLoader {
public:
    [[nodiscard]] static CartographyDataset load_default();
    [[nodiscard]] static CartographyDataset load_from_path(
        const std::filesystem::path& path,
        CartographyDatasetSource source_hint
    );
};

[[nodiscard]] const char* to_string(CartographyDatasetSource source) noexcept;
[[nodiscard]] const char* to_string(CartographyLabelKind kind) noexcept;

} // namespace frameflow::renderer::cartography
