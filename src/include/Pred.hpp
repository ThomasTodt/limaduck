#pragma once

#include <vector>
#include <cstdint>

namespace duckdb {

struct Pred {
	int32_t cp; // ID do par de colunas (Column Pair)
	int32_t p;  // ID do predicado específico
	int32_t score;

	/**
	 * Construtor que calcula o score de posicionamento na Lattice.
	 * Baseado na fórmula: (p + 1) * predProd[cp]
	 */
	Pred(int32_t cp_idx, int32_t p_idx, const std::vector<int32_t> &predProd) : cp(cp_idx), p(p_idx) {
		// O cálculo do score garante a unicidade da representação na Lattice
		this->score = (p_idx + 1) * predProd[cp_idx];
	}

	/**
	 * Equivalente ao hashCode() do Java.
	 * Utilizado para indexação e comparação rápida.
	 */
	size_t Hash() const {
		return static_cast<size_t>(score);
	}

	bool operator==(const Pred &other) const {
		return cp == other.cp && p == other.p;
	}

	bool operator!=(const Pred &other) const {
		return !(*this == other);
	}
};

} // namespace duckdb
