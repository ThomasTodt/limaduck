#include "Schema.hpp"

namespace duckdb {

Schema::Schema(const std::vector<std::string>& colNames) : colnames(colNames) {
    BuildColumns(colNames);     //
    BuildColumnPairs();        //
    BuildPredicates();         //
    BuildLattice();           //
}

void Schema::BuildColumns(const std::vector<std::string>& colNames) {
    columns.reserve(colNames.size());
    for (size_t i = 0; i < colNames.size(); i++) {
        auto col = std::make_unique<Column>(colNames[i]);
        col->ID = (int)i;
        
        // Mapeamento por tipo similar ao EnumMap
        auto& typeVec = typeColumns[col->type];
        col->typeID = (int)typeVec.size();
        typeVec.push_back(col.get());
        
        columns.push_back(std::move(col));
    }
}

void Schema::BuildColumnPairs() {
    columnPairs.reserve(columns.size());
    for (auto& col : columns) {
        int cID = (int)columnPairs.size();
        // O Java cria pares (col, col) inicialmente
        auto colPair = std::make_unique<ColumnPair>(*col, *col, cID);
        typeColumnPairs[colPair->type].push_back(colPair.get());
        columnPairs.push_back(std::move(colPair));
    }
}

void Schema::BuildPredicates() {
    int numPreds = 0;
    // Operadores seguem a ordem do Enum no Java
    std::vector<Predicate::Operator> ops = {
        Predicate::Operator::EQ, Predicate::Operator::NE,
        Predicate::Operator::GT, Predicate::Operator::LE,
        Predicate::Operator::LT, Predicate::Operator::GE
    };

    for (auto& colPair : columnPairs) {
        // Strings recebem 2 predicados (EQ, NE), outros recebem 6
        int limit = (colPair->type == Column::Type::STRING) ? 2 : 6;
        colPair->preds.resize(limit);

        for (int i = 0; i < limit; i++) {
            auto pred = std::make_unique<Predicate>(*colPair, ops[i], numPreds++, colPair->cID, i);
            colPair->preds[i] = pred.get();
            // No LIMA, o Schema mantém um repositório central de predicados
            preds.push_back(pred.get());
            
            // Nota: Em uma implementação completa, você precisaria gerenciar a 
            // posse desses predicados, possivelmente movendo para um container
            // de unique_ptr dentro do Schema.
        }
    }
}

void Schema::BuildLattice() {
    std::vector<int> predCounts(columnPairs.size());
    for (size_t cp = 0; cp < columnPairs.size(); cp++) {
        predCounts[cp] = (int)columnPairs[cp]->preds.size(); //
    }
    lattice = std::make_unique<SchemaLattice>(predCounts); //
}

bool Schema::Equals(const Schema& other) const {
    if (columns.size() != other.columns.size()) return false; //
    for (size_t i = 0; i < columns.size(); i++) {
        if (!columns[i]->Equals(*other.columns[i])) return false; //
    }
    return true;
}

} // namespace duckdb