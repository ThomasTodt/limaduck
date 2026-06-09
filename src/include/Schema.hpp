#pragma once

#include <vector>
#include <string>
#include <map>
#include <memory>
#include "Column.hpp"
#include "ColumnPair.hpp"
#include "Predicate.hpp"
#include "SchemaLattice.hpp"

namespace duckdb {

class Schema {
public:
	std::vector<std::string> colnames;

	std::vector<std::unique_ptr<Column>> columns;
	std::vector<std::unique_ptr<ColumnPair>> columnPairs;
	std::vector<std::unique_ptr<Predicate>> preds;
	std::unique_ptr<SchemaLattice> lattice;

	// Mapas para acesso rápido por tipo (equivalente ao EnumMap do Java)
	std::map<Column::Type, std::vector<Column *>> typeColumns;
	std::map<Column::Type, std::vector<ColumnPair *>> typeColumnPairs;

	// Schema(const std::vector<std::string>& colNames);
	Schema(const std::vector<std::string> &colNames, const std::vector<Column::Type> &colTypes);

	// void buildColumns(const std::vector<std::string>& colNames);
	void buildColumns(const std::vector<std::string> &colNames, const std::vector<Column::Type> &colTypes);
	void buildColumnPairs();
	void buildPredicates();
	void buildLattice();

	bool equals(const Schema &other) const;
};

} // namespace duckdb
