#include "duckdb/main/capi/capi_internal.hpp"
#include "duckdb/common/string_util.hpp"

using duckdb::Connection;
using duckdb::ErrorData;
using duckdb::TableDescription;
using duckdb::TableDescriptionWrapper;

duckdb_state duckdb_table_description_create(duckdb_connection connection, const char *schema, const char *table,
                                             duckdb_table_description *out) {
	return duckdb_table_description_create_ext(connection, INVALID_CATALOG, schema, table, out);
}

duckdb_state duckdb_table_description_create_ext(duckdb_connection connection, const char *catalog, const char *schema,
                                                 const char *table, duckdb_table_description *out) {
	Connection *conn = reinterpret_cast<Connection *>(connection);

	if (!out) {
		return DuckDBError;
	}
	auto wrapper = new TableDescriptionWrapper();
	*out = reinterpret_cast<duckdb_table_description>(wrapper);

	if (!connection || !table) {
		return DuckDBError;
	}
	if (catalog == nullptr) {
		catalog = INVALID_CATALOG;
	}
	if (schema == nullptr) {
		schema = DEFAULT_SCHEMA;
	}

	try {
		wrapper->description = conn->TableInfo(catalog, schema, table);
	} catch (std::exception &ex) {
		ErrorData error(ex);
		wrapper->error = error.RawMessage();
		return DuckDBError;
	} catch (...) { // LCOV_EXCL_START
		wrapper->error = "Unknown Connection::TableInfo error";
		return DuckDBError;
	} // LCOV_EXCL_STOP
	if (!wrapper->description) {
		wrapper->error = "No table with that schema+name could be located";
		return DuckDBError;
	}
	return DuckDBSuccess;
}

void duckdb_table_description_destroy(duckdb_table_description *table) {
	if (!table || !*table) {
		return;
	}
	auto wrapper = reinterpret_cast<TableDescriptionWrapper *>(*table);
	delete wrapper;
	*table = nullptr;
}

const char *duckdb_table_description_error(duckdb_table_description table) {
	if (!table) {
		return nullptr;
	}
	auto wrapper = reinterpret_cast<TableDescriptionWrapper *>(table);
	if (wrapper->error.empty()) {
		return nullptr;
	}
	return wrapper->error.c_str();
}

duckdb_state GetTableDescription(TableDescriptionWrapper *wrapper, idx_t index) {
	if (!wrapper) {
		return DuckDBError;
	}
	auto &table = wrapper->description;
	if (index >= table->columns.size()) {
		wrapper->error = duckdb::StringUtil::Format("Column index %d is out of range, table only has %d columns", index,
		                                            table->columns.size());
		return DuckDBError;
	}
	return DuckDBSuccess;
}

duckdb_state duckdb_column_has_default(duckdb_table_description table_description, idx_t index, bool *out) {
	auto wrapper = reinterpret_cast<TableDescriptionWrapper *>(table_description);
	if (GetTableDescription(wrapper, index) == DuckDBError) {
		return DuckDBError;
	}
	if (!out) {
		wrapper->error = "Please provide a valid (non-null) 'out' variable";
		return DuckDBError;
	}

	auto &table = wrapper->description;
	auto &column = table->columns[index];
	*out = column.HasDefaultValue();
	return DuckDBSuccess;
}

char *duckdb_table_description_get_column_name(duckdb_table_description table_description, idx_t index) {
	auto wrapper = reinterpret_cast<TableDescriptionWrapper *>(table_description);
	if (GetTableDescription(wrapper, index) == DuckDBError) {
		return nullptr;
	}

	auto &table = wrapper->description;
	auto &column = table->columns[index];

	auto name = column.GetName();
	auto result = reinterpret_cast<char *>(malloc(sizeof(char) * (name.size() + 1)));
	memcpy(result, name.c_str(), name.size());
	result[name.size()] = '\0';

	return result;
}
