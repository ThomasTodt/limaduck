#pragma once

#include <vector>
#include <cstdint>

namespace duckdb {

/**
 * TPSet armazena os pares de índices (x, y) que representam
 * as combinações de tuplas sendo analisadas pelo LIMA, além do cache
 * de bitvectors de 64 bits (uint64_t) por predicado para este batch.
 */
struct TPSet {
	std::vector<int32_t> x;
	std::vector<int32_t> y;
	int32_t length;

	// Cache de bitvectors de 64 bits por ID de predicado: pred_bits[predID][word]
	std::vector<std::vector<uint64_t>> pred_bits;
	std::vector<int32_t> pred_popcnt;

	TPSet(int32_t n) : length(n) {
		x.resize(n);
		y.resize(n);
	}
};

} // namespace duckdb
