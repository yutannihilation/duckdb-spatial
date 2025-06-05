#pragma once

#include "duckdb/common/types.hpp"
#include "duckdb/common/vector.hpp"
#include "spatial/spatial_types.hpp"

// GDAL includes
#include "ogrsf_frmts.h"
#include "cpl_string.h"

namespace duckdb {

// Forward declaration for GDALDatasetUniquePtr (defined in gdal_multi_layer_reader.hpp)
using GDALDatasetUniquePtr = unique_ptr<GDALDataset, void (*)(GDALDataset *)>;

//------------------------------------------------------------------------------
// GDAL Utility Functions
//------------------------------------------------------------------------------
class GDALUtils {
public:
	// Convert OGR field type to DuckDB LogicalType
	static LogicalType OGRFieldTypeToDuckDB(OGRFieldType field_type);
	
	// Convert OGR field value to DuckDB value
	static void ConvertOGRFieldToVector(OGRFeature *feature, int field_idx, OGRFieldDefn *field_def, 
	                                   Vector &vector, idx_t row_idx);
	
	// Convert OGR geometry to WKB blob
	static void ConvertGeometryToWKB(OGRGeometry *geom, Vector &vector, idx_t row_idx);
	
	// Convert OGR geometry to DuckDB GEOMETRY (via WKT for now)
	static void ConvertGeometryToDuckDB(OGRGeometry *geom, Vector &vector, idx_t row_idx);
	
	// Open GDAL dataset with common options
	struct OpenOptions {
		CSLConstList allowed_drivers = nullptr;
		CSLConstList open_options = nullptr;
		CSLConstList sibling_files = nullptr;
		unsigned int flags = GDAL_OF_VECTOR | GDAL_OF_READONLY;
	};
	
	static GDALDatasetUniquePtr OpenDataset(const string &path, const OpenOptions &options);
	
	// Get layer from dataset with common logic
	static OGRLayer* GetLayer(GDALDataset *dataset, const string &layer_name, int layer_idx);
};

//------------------------------------------------------------------------------
// Spatial Filter Helper
//------------------------------------------------------------------------------
// Forward declaration from gdal_multi_layer_reader.hpp
struct SpatialFilterBox;

class SpatialFilterHelper {
public:
	static void ApplyBoxFilter(OGRLayer *layer, const SpatialFilterBox &box);
	static void ApplyWKBFilter(OGRLayer *layer, const string &wkb);
};

} // namespace duckdb