#pragma once

#include "ColumnPair.hpp"
#include <string>
#include <vector>

namespace duckdb {

/**
 * Representa um predicado lógico (ex: col1 == col2) dentro de uma DC.
 * Traduzido de edu.upc.data.schema.Predicate
 */
class Predicate {
public:
	// Enum class para evitar colisões e garantir tipagem forte
	enum class Operator { EQ, NE, GT, LE, LT, GE };

	Operator op;
	ColumnPair *cols; // Ponteiro para o par de colunas associado
	int32_t ID;       // ID global do predicado
	int32_t cID;      // ID do par de colunas (Column Pair ID)
	int32_t pID;      // ID do operador dentro do par

	Predicate(ColumnPair *column_pair, Operator operation, int32_t id, int32_t column_pair_id, int32_t pred_id)
	    : op(operation), cols(column_pair), ID(id), cID(column_pair_id), pID(pred_id) {
	}

	/**
	 * Converte o enum Operator para uma string legível.
	 */

	//  std::string OperatorToString() const {
	//     static const std::vector<std::string> op_strings = {
	//         "EQ", "NE", "GT", "LE", "LT", "GE"
	//     };
	//     return op_strings[static_cast<int>(op)];
	// }

	std::string OperatorToString() const {
		static const std::vector<std::string> op_strings = {"==", "!=", ">", "<=", "<", ">="};
		return op_strings[static_cast<int>(op)];
	}

	/**
	 * Equivalente ao toString() do Java.
	 */
	std::string ToString() const {
		if (!cols || !cols->x || !cols->y)
			return "Invalid Predicate";
		return cols->x->name + " " + OperatorToString() + " " + cols->y->name;
	}
};

} // namespace duckdb
