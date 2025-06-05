#pragma once

#include "gdal_multi_layer_reader.hpp"

namespace duckdb {

struct GDALBindData final : TableFunctionData {

	string file_path;
	vector<LogicalType> field_sql_types;
	vector<string> field_names;
	string layer_name;
	OGRwkbGeometryType geometry_type = wkbUnknown;
	GDALOptions options;

	GDALBindData(string file_path, vector<LogicalType> field_sql_types, vector<string> field_names)
	    : file_path(std::move(file_path)), field_sql_types(std::move(field_sql_types)),
	      field_names(std::move(field_names)) {
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

	if (key == "DRIVER") {
		if (val.type().id() == LogicalTypeId::VARCHAR) {
			options.driver_name = val.GetValue<string>();
		} else {
			throw BinderException("Driver name must be a string");
		}
		return true;
	}

	if (key == "SRS") {
		if (val.type().id() == LogicalTypeId::VARCHAR) {
			options.target_srs = val.GetValue<string>();
		} else {
			throw BinderException("SRS must be a string");
		}
		return true;
	}

	if (key == "LAYER_CREATION_OPTIONS") {
		if (val.type().id() != LogicalTypeId::VARCHAR) {
			throw BinderException("Layer creation options must be strings");
		}
		auto str = val.GetValue<string>();
		options.layer_creation_options.AddString(str.c_str());
	}
	if (key == "DATASET_CREATION_OPTIONS") {
		if (val.type().id() != LogicalTypeId::VARCHAR) {
			throw BinderException("Dataset creation options must be strings");
		}
		auto str = val.GetValue<string>();
		options.dataset_creation_options.AddString(str.c_str());
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

	bind_data.multi_file_reader->BindOptions(bind_data.file_options, multi_file_list, return_types, names,
	                                         bind_data.reader_bind);
}

void GDALMultiLayerInfo::FinalizeBindData(MultiFileBindData &multi_file_data) {
	auto &gdal_bind_data = multi_file_data.bind_data->Cast<GDALBindData>();
	auto &options = gdal_bind_data.options;

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
}

unique_ptr<LocalTableFunctionState> GDALMultiLayerInfo::InitializeLocalState(ExecutionContext &,
                                                                             GlobalTableFunctionState &) {
}

} // namespace duckdb