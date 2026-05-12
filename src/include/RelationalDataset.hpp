#pragma once

#include "Schema.hpp"
#include <vector>
#include <memory>

namespace duckdb {

// Forward declaration de TPSubSet (assumindo que será uma estrutura de índices)
class TPSubSet;

/**
 * Interface abstrata para conjuntos de dados relacionais no LIMA.
 * Traduzido de edu.upc.data.dataset.RelationalDataset
 */
class RelationalDataset {
public:
    // Referência ao esquema que define as colunas e predicados
    Schema* schema;
    
    // Indica se uma coluna possui valor constante (otimização de busca)
    std::vector<bool> constColumn;
    
    // Número total de tuplas no dataset
    int32_t size;

    /**
     * Construtor base
     */
    RelationalDataset(Schema* s, int32_t n) : schema(s), size(n) {
        constColumn.assign(s->columns.size(), false);
    }

    /**
     * Destrutor virtual: essencial para classes abstratas em C++.
     * Garante que as classes filhas (ex: DuckDBDataset) sejam limpas corretamente.
     */
    virtual ~RelationalDataset() = default;

    /**
     * Método Abstrato (Pure Virtual): Filtra um conjunto de pares de tuplas (TPs)
     * que satisfazem um predicado específico.
     */
    virtual std::unique_ptr<TPSubSet> filter(TPSubSet& TPs, Predicate* pred) = 0;

    /**
     * Método Abstrato (Pure Virtual): Gera uma amostra aleatória de 'n' tuplas.
     */
    virtual std::unique_ptr<TPSubSet> sample(int32_t n) = 0;
};

} // namespace duckdb