//===----------------------------------------------------------------------===//
//                         DuckDB
//
// sqllogic_command.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"
#include "duckdb/common/virtual_file_system.hpp"

namespace duckdb {
class SQLLogicTestRunner;

enum class SortStyle : uint8_t { NO_SORT, ROW_SORT, VALUE_SORT };
enum class ExpectedResult : uint8_t { RESULT_SUCCESS, RESULT_ERROR, RESULT_UNKNOWN };

struct LoopDefinition {
	string loop_iterator_name;
	int loop_idx;
	int loop_start;
	int loop_end;
	bool is_parallel;
	vector<string> tokens;
};

struct ExecuteContext {
	ExecuteContext() : con(nullptr), is_parallel(false) {
	}
	ExecuteContext(Connection *con, vector<LoopDefinition> running_loops_p)
	    : con(con), running_loops(std::move(running_loops_p)), is_parallel(true) {
	}

	Connection *con;
	vector<LoopDefinition> running_loops;
	bool is_parallel;
	string sql_query;
	string error_file;
	int error_line;
};

struct Condition {
	string keyword;
	string value;
	ExpressionType comparison;
	bool skip_if;
};

class Command {
public:
	Command(SQLLogicTestRunner &runner);
	virtual ~Command();

	SQLLogicTestRunner &runner;
	string connection_name;
	int query_line;
	string base_sql_query;
	string file_name;
	vector<Condition> conditions;

public:
	Connection *CommandConnection(ExecuteContext &context) const;

	duckdb::unique_ptr<MaterializedQueryResult> ExecuteQuery(ExecuteContext &context, Connection *connection,
	                                                         string file_name, idx_t query_line) const;

	virtual void ExecuteInternal(ExecuteContext &context) const = 0;
	void Execute(ExecuteContext &context) const;

	virtual bool SupportsConcurrent() const {
		return false;
	}

private:
	void RestartDatabase(ExecuteContext &context, Connection *&connection, string sql_query) const;
};

class Statement : public Command {
public:
	Statement(SQLLogicTestRunner &runner);

	ExpectedResult expected_result;
	string expected_error;

public:
	void ExecuteInternal(ExecuteContext &context) const override;

	bool SupportsConcurrent() const override {
		return true;
	}
};

class ResetLabel : public Command {
public:
	ResetLabel(SQLLogicTestRunner &runner);

public:
	void ExecuteInternal(ExecuteContext &context) const override;

	bool SupportsConcurrent() const override {
		return true;
	}

public:
	string query_label;
};

class Query : public Command {
public:
	Query(SQLLogicTestRunner &runner);

	idx_t expected_column_count = 0;
	SortStyle sort_style = SortStyle::NO_SORT;
	vector<string> values;
	bool query_has_label = false;
	string query_label;

public:
	void ExecuteInternal(ExecuteContext &context) const override;

	bool SupportsConcurrent() const override {
		return true;
	}
};

class RestartCommand : public Command {
public:
	bool load_extensions;
	RestartCommand(SQLLogicTestRunner &runner, bool load_extensions);

public:
	void ExecuteInternal(ExecuteContext &context) const override;
};

class ReconnectCommand : public Command {
public:
	ReconnectCommand(SQLLogicTestRunner &runner);

public:
	void ExecuteInternal(ExecuteContext &context) const override;
};

class LoopCommand : public Command {
public:
	LoopCommand(SQLLogicTestRunner &runner, LoopDefinition definition_p);

public:
	LoopDefinition definition;
	vector<duckdb::unique_ptr<Command>> loop_commands;

	void ExecuteInternal(ExecuteContext &context) const override;

	bool SupportsConcurrent() const override;
};

class ModeCommand : public Command {
public:
	ModeCommand(SQLLogicTestRunner &runner, string parameter);

public:
	string parameter;

	void ExecuteInternal(ExecuteContext &context) const override;
};

enum class SleepUnit : uint8_t { NANOSECOND, MICROSECOND, MILLISECOND, SECOND };

class SleepCommand : public Command {
public:
	SleepCommand(SQLLogicTestRunner &runner, idx_t duration, SleepUnit unit);

public:
	void ExecuteInternal(ExecuteContext &context) const override;

	static SleepUnit ParseUnit(const string &unit);

private:
	idx_t duration;
	SleepUnit unit;
};

class UnzipCommand : public Command {
public:
	// 1 MB
	static constexpr const int64_t BUFFER_SIZE = 1u << 20;

public:
	UnzipCommand(SQLLogicTestRunner &runner, string &input, string &output);

	void ExecuteInternal(ExecuteContext &context) const override;

private:
	string input_path;
	string extraction_path;
};

class LoadCommand : public Command {
public:
	LoadCommand(SQLLogicTestRunner &runner, string dbpath, bool readonly, const string &version = "");

	string dbpath;
	bool readonly;
	string version;

public:
	void ExecuteInternal(ExecuteContext &context) const override;
};

} // namespace duckdb
