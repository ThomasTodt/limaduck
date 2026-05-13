#include "Schema.hpp"
#include "duckdb/common/helper.hpp"
#include <iostream>



namespace duckdb {

Schema::Schema(const std::vector<std::string>& colNames, const std::vector<Column::Type>& colTypes) : colnames(colNames) {
    buildColumns(colNames, colTypes);
    buildColumnPairs();
    buildPredicates();
    buildLattice();
}

void Schema::buildColumns(const std::vector<std::string>& colNames, const std::vector<Column::Type>& colTypes) {
    std::cerr << "[RADAR] buildcolumns\n";
    columns.reserve(colNames.size());
    for (size_t i = 0; i < colNames.size(); i++) {
        auto col = make_uniq<Column>(colNames[i]);
        col->ID = (int32_t)i;
        
        col->type = colTypes[i];
        
        Column* col_ptr = col.get();
        columns.push_back(std::move(col));
        
        auto& typeVec = typeColumns[col_ptr->type];
        col_ptr->typeID = (int32_t)typeVec.size();
        typeVec.push_back(col_ptr);
    }
    std::cerr << "[RADAR] buildcolumns 2\n";
}

void Schema::buildColumnPairs() {
    std::cerr << "[RADAR] buildcolumnpairs\n";
    columnPairs.reserve(columns.size());
    for (auto& col : columns) {
        int32_t cID = (int32_t)columnPairs.size();
        // O LIMA cria pares reflexivos (col, col) nesta fase
        auto colPair = make_uniq<ColumnPair>(*col, *col, cID);
        
        ColumnPair* pair_ptr = colPair.get();
        typeColumnPairs[pair_ptr->type].push_back(pair_ptr);
        columnPairs.push_back(std::move(colPair));
    }
    std::cerr << "[RADAR] buildcolumnpairs 2\n";
}

void Schema::buildPredicates() {
    std::cerr << "[RADAR] buildpredicates\n";

    // Ordem dos operadores deve bater com o enum no Predicate.hpp
    std::vector<Predicate::Operator> ops = {
        Predicate::Operator::EQ, Predicate::Operator::NE,
        Predicate::Operator::GT, Predicate::Operator::LE,
        Predicate::Operator::LT, Predicate::Operator::GE
    };

    int32_t numPreds = 0;
    for (auto& colPair : columnPairs) {
        // Strings = 2 predicados (EQ, NE), Numéricos = 6
        int limit = (colPair->type == Column::Type::STRING) ? 2 : 6;
        colPair->preds.reserve(limit);

        for (int i = 0; i < limit; i++) {
            auto pred = make_uniq<Predicate>(colPair.get(), ops[i], numPreds++, colPair->cID, i);
            
            // O ColumnPair guarda o ponteiro para consulta
            colPair->preds.push_back(pred.get());
            
            // O Schema guarda a posse do objeto
            preds.push_back(std::move(pred));
        }
    }
    std::cerr << "[RADAR] buildpredicates 2\n";
}

void Schema::buildLattice() {
    std::cerr << "[RADAR] buildlattice\n";
    std::vector<int32_t> predCounts;
    predCounts.reserve(columnPairs.size());
    for (auto& cp : columnPairs) {
        predCounts.push_back((int32_t)cp->preds.size());
    }
    lattice = make_uniq<SchemaLattice>(predCounts);
    std::cerr << "[RADAR] buildlattice 2\n";
}

bool Schema::equals(const Schema& other) const {
    if (columns.size() != other.columns.size()) return false;
    for (size_t i = 0; i < columns.size(); i++) {
        if (!(columns[i]->operator==(*other.columns[i]))) return false;
    }
    return true;
}

} // namespace duckdb