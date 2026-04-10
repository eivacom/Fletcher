#include "crs_utils.hpp"

#include <charconv>

namespace fletcher {

// ---------------------------------------------------------------------------
// PROJJSON for well-known EPSG codes
// ---------------------------------------------------------------------------

static const char kEpsg4326ProjJson[] = R"({
  "$schema": "https://proj.org/schemas/v0.7/projjson.schema.json",
  "type": "GeographicCRS",
  "name": "WGS 84",
  "datum": {
    "type": "GeodeticReferenceFrame",
    "name": "World Geodetic System 1984",
    "ellipsoid": {
      "name": "WGS 84",
      "semi_major_axis": 6378137,
      "inverse_flattening": 298.257223563
    }
  },
  "coordinate_system": {
    "subtype": "ellipsoidal",
    "axis": [
      {"name": "Geodetic latitude", "abbreviation": "Lat", "direction": "north", "unit": "degree"},
      {"name": "Geodetic longitude", "abbreviation": "Lon", "direction": "east", "unit": "degree"}
    ]
  },
  "id": {"authority": "EPSG", "code": 4326}
})";

static const char kEpsg3857ProjJson[] = R"({
  "$schema": "https://proj.org/schemas/v0.7/projjson.schema.json",
  "type": "ProjectedCRS",
  "name": "WGS 84 / Pseudo-Mercator",
  "base_crs": {
    "name": "WGS 84",
    "datum": {
      "type": "GeodeticReferenceFrame",
      "name": "World Geodetic System 1984",
      "ellipsoid": {
        "name": "WGS 84",
        "semi_major_axis": 6378137,
        "inverse_flattening": 298.257223563
      }
    },
    "coordinate_system": {
      "subtype": "ellipsoidal",
      "axis": [
        {"name": "Geodetic latitude", "abbreviation": "Lat", "direction": "north", "unit": "degree"},
        {"name": "Geodetic longitude", "abbreviation": "Lon", "direction": "east", "unit": "degree"}
      ]
    }
  },
  "conversion": {
    "name": "Popular Visualisation Pseudo-Mercator",
    "method": {"name": "Popular Visualisation Pseudo Mercator", "id": {"authority": "EPSG", "code": 1024}},
    "parameters": [
      {"name": "Latitude of natural origin", "value": 0, "unit": "degree", "id": {"authority": "EPSG", "code": 8801}},
      {"name": "Longitude of natural origin", "value": 0, "unit": "degree", "id": {"authority": "EPSG", "code": 8802}},
      {"name": "False easting", "value": 0, "unit": "metre", "id": {"authority": "EPSG", "code": 8806}},
      {"name": "False northing", "value": 0, "unit": "metre", "id": {"authority": "EPSG", "code": 8807}}
    ]
  },
  "coordinate_system": {
    "subtype": "Cartesian",
    "axis": [
      {"name": "Easting", "abbreviation": "X", "direction": "east", "unit": "metre"},
      {"name": "Northing", "abbreviation": "Y", "direction": "north", "unit": "metre"}
    ]
  },
  "id": {"authority": "EPSG", "code": 3857}
})";

// ---------------------------------------------------------------------------

std::string EpsgToProjJson(int code) {
    switch (code) {
        case 4326: return kEpsg4326ProjJson;
        case 3857: return kEpsg3857ProjJson;
        default:   return {};
    }
}

std::string ResolveCrs(std::string_view crs) {
    if (crs.empty()) return {};
    if (crs.front() == '{') return std::string(crs);

    // Try "EPSG:<code>"
    constexpr std::string_view prefix = "EPSG:";
    if (crs.size() > prefix.size() &&
        crs.substr(0, prefix.size()) == prefix) {
        int code = 0;
        auto [ptr, ec] = std::from_chars(
            crs.data() + prefix.size(),
            crs.data() + crs.size(),
            code);
        if (ec == std::errc{} && ptr == crs.data() + crs.size())
            return EpsgToProjJson(code);
    }
    return {};
}

std::string BuildExtensionMetadata(std::string_view resolved_crs) {
    if (resolved_crs.empty()) return "{}";
    std::string result;
    result.reserve(8 + resolved_crs.size());
    result += "{\"crs\":";
    result += resolved_crs;
    result += '}';
    return result;
}

}  // namespace fletcher
