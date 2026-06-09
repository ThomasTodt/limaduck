#pragma once

#include "Schema.hpp"
#include <cstdint>
#include <vector>
#include <memory>

namespace duckdb {

class TPSubSet;

class RelationalDataset {
public:
	// Referência ao esquema que define as colunas e predicados
	Schema *schema;

	// Indica se uma coluna possui valor constante (otimização de busca)
	std::vector<bool> constColumn;

	// Número total de tuplas no dataset
	int32_t size;

	RelationalDataset(Schema *s, int32_t n) : schema(s), size(n) {
		constColumn.assign(s->columns.size(), false);
	}

	/**
	 * Destrutor virtual
	 * Garante que as classes filhas (DuckDBDataset) sejam limpas corretamente.
	 */
	virtual ~RelationalDataset() = default;

	virtual std::unique_ptr<TPSubSet> filter(TPSubSet &TPs, Predicate *pred) = 0;

	virtual std::unique_ptr<TPSubSet> sample(int32_t n) = 0;
};

} // namespace duckdb
