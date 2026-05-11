#pragma once

#include <string>
#include <vector>
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
    std::map<Column::Type, std::vector<Column*>> typeColumns;
    
    std::vector<std::unique_ptr<ColumnPair>> columnPairs;
    std::map<Column::Type, std::vector<ColumnPair*>> typeColumnPairs;

    std::vector<Predicate*> preds; // Referências para facilitar o acesso
    std::unique_ptr<SchemaLattice> lattice;

    Schema(const std::vector<std::string>& colNames);

    void BuildColumns(const std::vector<std::string>& colNames);
    void BuildColumnPairs();
    void BuildPredicates();
    void BuildLattice();

    bool Equals(const Schema& other) const;
};

} // namespace duckdb