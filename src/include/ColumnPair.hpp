#pragma once

#include "Column.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace duckdb {

// Forward declaration para evitar dependência circular,
// já que Predicate também conhece ColumnPair.
class Predicate;

class ColumnPair {
public:
	Column *x;
	Column *y;

	// Vetor de ponteiros para os predicados associados a este par.
	std::vector<Predicate *> preds;

	Column::Type type;
	int32_t cID;

	ColumnPair(Column &col_x, Column &col_y, int32_t id) : x(&col_x), y(&col_y), type(col_x.type), cID(id) {
	}

	std::string ToString() const {
		return x->name + " " + y->name;
	}
};

} // namespace duckdb
