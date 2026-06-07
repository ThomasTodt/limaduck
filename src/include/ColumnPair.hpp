#pragma once

#include "Column.hpp"
#include <vector>
#include <string>

namespace duckdb {

// Forward declaration para evitar dependência circular,
// já que Predicate também conhece ColumnPair.
class Predicate;

/**
 * Representa um par de colunas (x, y) que servirá de base para predicados.
 * Traduzido de edu.upc.data.schema.ColumnPair
 */
class ColumnPair {
public:
	Column *x;
	Column *y;

	// Vetor de ponteiros para os predicados associados a este par.
	std::vector<Predicate *> preds;

	Column::Type type;
	int32_t cID;

	/**
	 * Construtor que recebe as referências das colunas e o ID do par.
	 * O tipo do par é herdado da primeira coluna (x).
	 */
	ColumnPair(Column &col_x, Column &col_y, int32_t id) : x(&col_x), y(&col_y), type(col_x.type), cID(id) {
	}

	/**
	 * Equivalente ao toString() do Java.
	 */
	std::string ToString() const {
		return x->name + " " + y->name;
	}
};

} // namespace duckdb
