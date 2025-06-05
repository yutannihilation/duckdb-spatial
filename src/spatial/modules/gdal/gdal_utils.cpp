#include "gdal_utils.hpp"
#include "gdal_multi_layer_reader.hpp"  // For SpatialFilterBox definition
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"

namespace duckdb {

//------------------------------------------------------------------------------
// Field Type Conversion
//------------------------------------------------------------------------------
LogicalType GDALUtils::OGRFieldTypeToDuckDB(OGRFieldType field_type) {
	switch (field_type) {
	case OFTInteger:
		return LogicalType::INTEGER;
	case OFTInteger64:
		return LogicalType::BIGINT;
	case OFTReal:
		return LogicalType::DOUBLE;
	case OFTString:
		return LogicalType::VARCHAR;
	case OFTBinary:
		return LogicalType::BLOB;
	case OFTDate:
		return LogicalType::DATE;
	case OFTTime:
		return LogicalType::TIME;
	case OFTDateTime:
		return LogicalType::TIMESTAMP;
	default:
		return LogicalType::VARCHAR;
	}
}

void GDALUtils::ConvertOGRFieldToVector(OGRFeature *feature, int field_idx, OGRFieldDefn *field_def, 
                                       Vector &vector, idx_t row_idx) {
	if (feature->IsFieldNull(field_idx)) {
		FlatVector::SetNull(vector, row_idx, true);
		return;
	}

	switch (field_def->GetType()) {
	case OFTInteger:
		FlatVector::GetData<int32_t>(vector)[row_idx] = feature->GetFieldAsInteger(field_idx);
		break;
	case OFTInteger64:
		FlatVector::GetData<int64_t>(vector)[row_idx] = feature->GetFieldAsInteger64(field_idx);
		break;
	case OFTReal:
		FlatVector::GetData<double>(vector)[row_idx] = feature->GetFieldAsDouble(field_idx);
		break;
	case OFTString:
		FlatVector::GetData<string_t>(vector)[row_idx] = 
			StringVector::AddString(vector, feature->GetFieldAsString(field_idx));
		break;
	case OFTBinary: {
		int size = 0;
		auto data = feature->GetFieldAsBinary(field_idx, &size);
		string_t blob;
		if (size > 0 && data) {
			blob = StringVector::AddStringOrBlob(vector, string((const char*)data, size));
		} else {
			blob = StringVector::EmptyString(vector, 0);
		}
		FlatVector::GetData<string_t>(vector)[row_idx] = blob;
		break;
	}
	case OFTDate:
	case OFTTime:
	case OFTDateTime:
		// TODO: Implement proper date/time conversion
		FlatVector::GetData<string_t>(vector)[row_idx] = 
			StringVector::AddString(vector, feature->GetFieldAsString(field_idx));
		break;
	default:
		FlatVector::GetData<string_t>(vector)[row_idx] = 
			StringVector::AddString(vector, feature->GetFieldAsString(field_idx));
		break;
	}
}

//------------------------------------------------------------------------------
// Geometry Conversion
//------------------------------------------------------------------------------
void GDALUtils::ConvertGeometryToWKB(OGRGeometry *geom, Vector &vector, idx_t row_idx) {
	if (!geom) {
		FlatVector::SetNull(vector, row_idx, true);
		return;
	}

	size_t wkb_size = geom->WkbSize();
	std::vector<unsigned char> wkb_data(wkb_size);
	geom->exportToWkb(wkbNDR, wkb_data.data());
	string wkb_string(wkb_data.begin(), wkb_data.end());
	FlatVector::GetData<string_t>(vector)[row_idx] = StringVector::AddStringOrBlob(vector, wkb_string);
}

void GDALUtils::ConvertGeometryToDuckDB(OGRGeometry *geom, Vector &vector, idx_t row_idx) {
	if (!geom) {
		FlatVector::SetNull(vector, row_idx, true);
		return;
	}

	// For now, convert via WKT (this is temporary until we have proper conversion)
	char *wkt = nullptr;
	geom->exportToWkt(&wkt);
	if (wkt) {
		FlatVector::GetData<string_t>(vector)[row_idx] = StringVector::AddString(vector, wkt);
		CPLFree(wkt);
	} else {
		FlatVector::SetNull(vector, row_idx, true);
	}
}

//------------------------------------------------------------------------------
// Dataset Operations
//------------------------------------------------------------------------------
GDALDatasetUniquePtr GDALUtils::OpenDataset(const string &path, const OpenOptions &options) {
	auto dataset = GDALDatasetUniquePtr(
		GDALDataset::Open(path.c_str(), options.flags,
		                  options.allowed_drivers, options.open_options, options.sibling_files),
		[](GDALDataset *ds) { GDALClose(ds); });

	if (!dataset) {
		throw IOException("Could not open file '%s'", path);
	}

	return dataset;
}

OGRLayer* GDALUtils::GetLayer(GDALDataset *dataset, const string &layer_name, int layer_idx) {
	OGRLayer *layer = nullptr;

	if (!layer_name.empty()) {
		layer = dataset->GetLayerByName(layer_name.c_str());
		if (!layer) {
			throw IOException("Layer '%s' not found", layer_name);
		}
	} else if (layer_idx >= 0) {
		layer = dataset->GetLayer(layer_idx);
		if (!layer) {
			throw IOException("Layer index %d not found", layer_idx);
		}
	} else {
		// Default to first layer
		layer = dataset->GetLayer(0);
		if (!layer) {
			throw IOException("No layers found in dataset");
		}
	}

	return layer;
}

//------------------------------------------------------------------------------
// Spatial Filter Helper
//------------------------------------------------------------------------------
void SpatialFilterHelper::ApplyBoxFilter(OGRLayer *layer, const SpatialFilterBox &box) {
	layer->SetSpatialFilterRect(box.min_x, box.min_y, box.max_x, box.max_y);
}

void SpatialFilterHelper::ApplyWKBFilter(OGRLayer *layer, const string &wkb) {
	OGRGeometryH geom = nullptr;
	auto ok = OGR_G_CreateFromWkb(wkb.c_str(), nullptr, &geom, (int)wkb.size());
	if (ok != OGRERR_NONE) {
		throw InvalidInputException("Could not create geometry from WKB for spatial filter");
	}
	layer->SetSpatialFilter((OGRGeometry*)geom);
	OGR_G_DestroyGeometry(geom);
}

} // namespace duckdb