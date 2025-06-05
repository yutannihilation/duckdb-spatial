#include "gdal_multi_layer_reader.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/function/table/arrow.hpp"
#include "spatial/spatial_types.hpp"
#include "spatial/geometry/geometry_serialization.hpp"
#include "spatial/geometry/wkb_writer.hpp"

// GDAL includes
#include "ogrsf_frmts.h"
#include "cpl_vsi.h"

namespace duckdb {

//------------------------------------------------------------------------------
// GDALFileReader - Reader for a single GDAL file
//------------------------------------------------------------------------------
class GDALFileReader : public BaseFileReader {
public:
	GDALFileReader(ClientContext &context, const OpenFileInfo &file_info, const GDALMultiLayerReaderOptions &options,
	               vector<LogicalType> expected_types, vector<string> expected_names)
	    : BaseFileReader(file_info), file_info(file_info), options(options),
	      dataset(nullptr, [](GDALDataset *ds) { if (ds) GDALClose(ds); }),
	      expected_types(std::move(expected_types)), expected_names(std::move(expected_names)),
	      current_row(0), finished(false) {
		InitializeReader(context);
	}

	void PrepareReader(ClientContext &context, GlobalTableFunctionState &gstate) override {
		// Reset for scanning
		current_row = 0;
		finished = false;
		if (current_layer) {
			current_layer->ResetReading();
		}
	}

	bool TryInitializeScan(ClientContext &context, GlobalTableFunctionState &gstate, 
	                      LocalTableFunctionState &lstate) override {
		if (!dataset || !current_layer) {
			return false;
		}
		current_layer->ResetReading();
		return true;
	}

	void Scan(ClientContext &context, GlobalTableFunctionState &gstate, LocalTableFunctionState &lstate,
	          DataChunk &output) override {
		if (finished || !current_layer) {
			output.SetCardinality(0);
			return;
		}

		idx_t output_idx = 0;
		const idx_t chunk_size = STANDARD_VECTOR_SIZE;

		while (output_idx < chunk_size) {
			OGRFeature *feature = current_layer->GetNextFeature();
			if (!feature) {
				// End of layer
				finished = true;
				break;
			}

			// Convert feature to DuckDB row
			ConvertFeatureToRow(feature, output, output_idx);
			OGRFeature::DestroyFeature(feature);
			output_idx++;
			current_row++;
		}

		output.SetCardinality(output_idx);
	}

	string GetReaderType() const override {
		return "GDAL";
	}

private:
	void InitializeReader(ClientContext &context) {
		// Open dataset
		dataset = GDALDatasetUniquePtr(
			GDALDataset::Open(file_info.path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
			                  options.options.allowed_drivers, options.options.open_options,
			                  options.options.sibling_files),
			[](GDALDataset *ds) { GDALClose(ds); });

		if (!dataset) {
			throw IOException("Could not open file '%s'", file_info.path);
		}

		// Get the layer
		if (!options.options.layer_name.empty()) {
			current_layer = dataset->GetLayerByName(options.options.layer_name.c_str());
			if (!current_layer) {
				throw IOException("Layer '%s' not found in file '%s'", options.options.layer_name, file_info.path);
			}
		} else if (options.options.layer_idx >= 0) {
			current_layer = dataset->GetLayer(options.options.layer_idx);
			if (!current_layer) {
				throw IOException("Layer index %d not found in file '%s'", options.options.layer_idx, file_info.path);
			}
		} else {
			// Default to first layer
			current_layer = dataset->GetLayer(0);
			if (!current_layer) {
				throw IOException("No layers found in file '%s'", file_info.path);
			}
		}

		// Apply spatial filter if specified
		if (options.options.has_spatial_filter) {
			current_layer->SetSpatialFilterRect(
				options.options.spatial_filter_box.min_x,
				options.options.spatial_filter_box.min_y,
				options.options.spatial_filter_box.max_x,
				options.options.spatial_filter_box.max_y);
		}
	}

	void ConvertFeatureToRow(OGRFeature *feature, DataChunk &output, idx_t row_idx) {
		auto layer_def = current_layer->GetLayerDefn();
		idx_t col_idx = 0;

		// Convert regular fields
		for (int field_idx = 0; field_idx < layer_def->GetFieldCount(); field_idx++) {
			if (col_idx >= output.ColumnCount()) break;

			auto field_def = layer_def->GetFieldDefn(field_idx);
			auto &vector = output.data[col_idx];

			if (feature->IsFieldNull(field_idx)) {
				FlatVector::SetNull(vector, row_idx, true);
			} else {
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
			col_idx++;
		}

		// Convert geometry fields
		for (int geom_idx = 0; geom_idx < layer_def->GetGeomFieldCount(); geom_idx++) {
			if (col_idx >= output.ColumnCount()) break;

			auto &vector = output.data[col_idx];
			auto geom = feature->GetGeomFieldRef(geom_idx);

			if (!geom) {
				FlatVector::SetNull(vector, row_idx, true);
			} else {
				if (options.options.keep_wkb) {
					// Export as WKB
					size_t wkb_size = geom->WkbSize();
					std::vector<unsigned char> wkb_data(wkb_size);
					geom->exportToWkb(wkbNDR, wkb_data.data());
					string wkb_string(wkb_data.begin(), wkb_data.end());
					FlatVector::GetData<string_t>(vector)[row_idx] = StringVector::AddString(vector, wkb_string);
				} else {
					// Convert to DuckDB GEOMETRY
					char *wkt = nullptr;
					geom->exportToWkt(&wkt);
					if (wkt) {
						// TODO: Convert WKT to DuckDB GEOMETRY type properly
						FlatVector::GetData<string_t>(vector)[row_idx] = StringVector::AddString(vector, wkt);
						CPLFree(wkt);
					} else {
						FlatVector::SetNull(vector, row_idx, true);
					}
				}
			}
			col_idx++;
		}
	}

	const OpenFileInfo file_info;
	const GDALMultiLayerReaderOptions options;
	GDALDatasetUniquePtr dataset;
	OGRLayer *current_layer = nullptr;
	vector<LogicalType> expected_types;
	vector<string> expected_names;
	idx_t current_row;
	bool finished;
};

//------------------------------------------------------------------------------
// GDALBindData - Bind data for GDAL multi-file reader
//------------------------------------------------------------------------------
struct GDALBindData final : TableFunctionData {
	vector<LogicalType> all_types;
	vector<string> all_names;
	GDALOptions options;
	idx_t total_layers = 0;
	bool combine_schemas = true;

	GDALBindData() {}
};

//------------------------------------------------------------------------------
// GDAL Global and Local Table Function States
//------------------------------------------------------------------------------
struct GDALGlobalTableFunctionState : public GlobalTableFunctionState {
public:
	GDALGlobalTableFunctionState(ClientContext &context, const MultiFileBindData &bind_data) {
		// Initialize any global state needed for GDAL scanning
	}
};

struct GDALLocalTableFunctionState : public LocalTableFunctionState {
public:
	GDALLocalTableFunctionState(ClientContext &context, GDALGlobalTableFunctionState &gstate) {
		// Initialize any local state needed for GDAL scanning
	}
};

unique_ptr<MultiFileReaderInterface>
GDALMultiLayerInfo::InitializeInterface(ClientContext &context, MultiFileReader &reader, MultiFileList &file_list) {
	return make_uniq<GDALMultiLayerInfo>();
}

unique_ptr<BaseFileReaderOptions> GDALMultiLayerInfo::InitializeOptions(ClientContext &context,
                                                                        optional_ptr<TableFunctionInfo> info) {
	return make_uniq<GDALMultiLayerReaderOptions>();
}

bool GDALMultiLayerInfo::ParseOption(ClientContext &context, const string &original_key, const Value &val,
                                     MultiFileOptions &file_options, BaseFileReaderOptions &base_options) {
	auto &gdal_options = base_options.Cast<GDALMultiLayerReaderOptions>();
	auto &options = gdal_options.options;
	auto key = StringUtil::Upper(original_key);
	if (val.IsNull()) {
		throw BinderException("Cannot use NULL as argument to %s", original_key);
	}

	if (key == "LAYER") {
		if (val.type().id() == LogicalTypeId::VARCHAR) {
			options.layer_name = val.GetValue<string>();
		} else if (val.type().id() == LogicalTypeId::INTEGER) {
			options.layer_idx = val.GetValue<int32_t>();
		} else {
			throw BinderException("Layer must be a string name or integer index");
		}
		return true;
	}

	if (key == "OPEN_OPTIONS") {
		if (val.type().id() == LogicalTypeId::LIST) {
			for (const auto &item : ListValue::GetChildren(val)) {
				options.open_options.AddString(StringValue::Get(item).c_str());
			}
		} else {
			throw BinderException("open_options must be a list of strings");
		}
		return true;
	}

	if (key == "ALLOWED_DRIVERS") {
		if (val.type().id() == LogicalTypeId::LIST) {
			for (const auto &item : ListValue::GetChildren(val)) {
				options.allowed_drivers.AddString(StringValue::Get(item).c_str());
			}
		} else {
			throw BinderException("allowed_drivers must be a list of strings");
		}
		return true;
	}

	if (key == "SIBLING_FILES") {
		if (val.type().id() == LogicalTypeId::LIST) {
			for (const auto &item : ListValue::GetChildren(val)) {
				options.sibling_files.AddString(StringValue::Get(item).c_str());
			}
		} else {
			throw BinderException("sibling_files must be a list of strings");
		}
		return true;
	}

	if (key == "KEEP_WKB") {
		options.keep_wkb = BooleanValue::Get(val);
		return true;
	}

	if (key == "SPATIAL_FILTER_BOX") {
		if (val.type() == GeoTypes::BOX_2D()) {
			auto &children = StructValue::GetChildren(val);
			options.spatial_filter_box.min_x = DoubleValue::Get(children[0]);
			options.spatial_filter_box.min_y = DoubleValue::Get(children[1]);
			options.spatial_filter_box.max_x = DoubleValue::Get(children[2]);
			options.spatial_filter_box.max_y = DoubleValue::Get(children[3]);
			options.has_spatial_filter = true;
		} else {
			throw BinderException("spatial_filter_box must be a BOX_2D");
		}
		return true;
	}

	return false;
}

bool GDALMultiLayerInfo::ParseCopyOption(ClientContext &context, const string &key, const vector<Value> &values,
                                         BaseFileReaderOptions &options, vector<string> &expected_names,
                                         vector<LogicalType> &expected_types) {
	// TODO: anything more needs to be done here?
	return false;
}

void GDALMultiLayerInfo::FinalizeCopyBind(ClientContext &context, BaseFileReaderOptions &options,
                                          const vector<string> &expected_names,
                                          const vector<LogicalType> &expected_types) {
}

unique_ptr<TableFunctionData> GDALMultiLayerInfo::InitializeBindData(MultiFileBindData &multi_file_data,
                                                                     unique_ptr<BaseFileReaderOptions> options) {
	auto &reader_options = options->Cast<GDALMultiLayerReaderOptions>();
	auto bind_data = make_uniq<GDALBindData>();
	bind_data->options = std::move(reader_options.options);
	return std::move(bind_data);
}

void GDALMultiLayerInfo::BindReader(ClientContext &context, vector<LogicalType> &return_types, vector<string> &names,
                                    MultiFileBindData &bind_data) {
	auto &gdal_bind_data = bind_data.bind_data->Cast<GDALBindData>();
	auto &options = gdal_bind_data.options;
	auto &multi_file_list = *bind_data.file_list;

	// We need to scan at least one file to determine the schema
	if (multi_file_list.IsEmpty()) {
		throw IOException("No files found");
	}

	// Get the first file to determine schema
	if (multi_file_list.IsEmpty()) {
		throw IOException("No files found in file list");
	}
	auto first_file = multi_file_list.GetFirstFile();

	// Open the first dataset to get schema information
	auto dataset = GDALDatasetUniquePtr(
		GDALDataset::Open(first_file.path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
		                  options.allowed_drivers, options.open_options, options.sibling_files),
		[](GDALDataset *ds) { GDALClose(ds); });

	if (!dataset) {
		throw IOException("Could not open file '%s'", first_file.path);
	}

	// Get the layer
	OGRLayer *layer = nullptr;
	if (!options.layer_name.empty()) {
		layer = dataset->GetLayerByName(options.layer_name.c_str());
		if (!layer) {
			throw IOException("Layer '%s' not found in file '%s'", options.layer_name, first_file.path);
		}
	} else if (options.layer_idx >= 0) {
		layer = dataset->GetLayer(options.layer_idx);
		if (!layer) {
			throw IOException("Layer index %d not found in file '%s'", options.layer_idx, first_file.path);
		}
	} else {
		// Default to first layer
		layer = dataset->GetLayer(0);
		if (!layer) {
			throw IOException("No layers found in file '%s'", first_file.path);
		}
	}

	// Get the layer definition
	auto layer_def = layer->GetLayerDefn();
	auto field_count = layer_def->GetFieldCount();

	// Build schema from fields
	for (int i = 0; i < field_count; i++) {
		auto field_def = layer_def->GetFieldDefn(i);
		auto field_name = string(field_def->GetNameRef());
		names.push_back(field_name);

		// Map OGR field type to DuckDB type
		auto field_type = field_def->GetType();
		switch (field_type) {
		case OFTInteger:
			return_types.push_back(LogicalType::INTEGER);
			break;
		case OFTInteger64:
			return_types.push_back(LogicalType::BIGINT);
			break;
		case OFTReal:
			return_types.push_back(LogicalType::DOUBLE);
			break;
		case OFTString:
			return_types.push_back(LogicalType::VARCHAR);
			break;
		case OFTBinary:
			return_types.push_back(LogicalType::BLOB);
			break;
		case OFTDate:
			return_types.push_back(LogicalType::DATE);
			break;
		case OFTTime:
			return_types.push_back(LogicalType::TIME);
			break;
		case OFTDateTime:
			return_types.push_back(LogicalType::TIMESTAMP);
			break;
		default:
			return_types.push_back(LogicalType::VARCHAR);
			break;
		}
	}

	// Add geometry column(s)
	auto geom_count = layer_def->GetGeomFieldCount();
	for (int i = 0; i < geom_count; i++) {
		auto geom_field_def = layer_def->GetGeomFieldDefn(i);
		auto geom_name = string(geom_field_def->GetNameRef());
		if (geom_name.empty()) {
			geom_name = (geom_count == 1) ? "geom" : StringUtil::Format("geom_%d", i);
		}
		names.push_back(geom_name);
		
		if (options.keep_wkb) {
			return_types.push_back(GeoTypes::WKB_BLOB());
		} else {
			return_types.push_back(GeoTypes::GEOMETRY());
		}
	}

	// Store the schema in bind data
	gdal_bind_data.all_types = return_types;
	gdal_bind_data.all_names = names;

	// Add file columns
	bind_data.multi_file_reader->BindOptions(bind_data.file_options, multi_file_list, return_types, names,
	                                         bind_data.reader_bind);
}

void GDALMultiLayerInfo::FinalizeBindData(MultiFileBindData &multi_file_data) {
	// TODO: anything more needs to be done here?
}

optional_idx GDALMultiLayerInfo::MaxThreads(const MultiFileBindData &bind_data_p,
                                            const MultiFileGlobalState &global_state, FileExpandResult expand_result) {
	if (expand_result == FileExpandResult::MULTIPLE_FILES) {
		// always launch max threads if we are reading multiple files
		return optional_idx();
	}
	// Otherwise, only one thread
	return 1;
}

unique_ptr<GlobalTableFunctionState> GDALMultiLayerInfo::InitializeGlobalState(ClientContext &context,
                                                                               MultiFileBindData &bind_data,
                                                                               MultiFileGlobalState &global_state) {
	return make_uniq<GDALGlobalTableFunctionState>(context, bind_data);
}

unique_ptr<LocalTableFunctionState> GDALMultiLayerInfo::InitializeLocalState(ExecutionContext &context,
                                                                             GlobalTableFunctionState &global_state) {
	auto &gstate = global_state.Cast<GDALGlobalTableFunctionState>();
	return make_uniq<GDALLocalTableFunctionState>(context.client, gstate);
}

shared_ptr<BaseFileReader> GDALMultiLayerInfo::CreateReader(ClientContext &context, GlobalTableFunctionState &gstate,
                                                           BaseUnionData &union_data, const MultiFileBindData &bind_data_p) {
	auto &bind_data = bind_data_p.bind_data->Cast<GDALBindData>();
	
	GDALMultiLayerReaderOptions options;
	options.options = bind_data.options;
	
	return make_shared_ptr<GDALFileReader>(context, union_data.file, options, 
	                                      bind_data.all_types, bind_data.all_names);
}

shared_ptr<BaseFileReader> GDALMultiLayerInfo::CreateReader(ClientContext &context, GlobalTableFunctionState &gstate,
                                                           const OpenFileInfo &file, idx_t file_idx,
                                                           const MultiFileBindData &bind_data_p) {
	auto &bind_data = bind_data_p.bind_data->Cast<GDALBindData>();
	
	GDALMultiLayerReaderOptions options;
	options.options = bind_data.options;
	
	return make_shared_ptr<GDALFileReader>(context, file, options, 
	                                      bind_data.all_types, bind_data.all_names);
}

shared_ptr<BaseFileReader> GDALMultiLayerInfo::CreateReader(ClientContext &context, const OpenFileInfo &file,
                                                           BaseFileReaderOptions &options,
                                                           const MultiFileOptions &file_options) {
	auto &gdal_options = options.Cast<GDALMultiLayerReaderOptions>();
	vector<LogicalType> empty_types;
	vector<string> empty_names;
	return make_shared_ptr<GDALFileReader>(context, file, gdal_options, empty_types, empty_names);
}

void GDALMultiLayerInfo::FinishReading(ClientContext &context, GlobalTableFunctionState &global_state,
                                      LocalTableFunctionState &local_state) {
	// Nothing to do
}

unique_ptr<NodeStatistics> GDALMultiLayerInfo::GetCardinality(const MultiFileBindData &bind_data, idx_t file_count) {
	// We don't know the exact cardinality for GDAL files
	return nullptr;
}

void GDALMultiLayerInfo::GetVirtualColumns(ClientContext &context, MultiFileBindData &bind_data, 
                                           virtual_column_map_t &result) {
	// No additional virtual columns beyond what MultiFileReader provides
}

unique_ptr<MultiFileReaderInterface> GDALMultiLayerInfo::Copy() {
	return make_uniq<GDALMultiLayerInfo>();
}

} // namespace duckdb