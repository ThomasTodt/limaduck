#pragma once

#include "Lattice.hpp"

namespace duckdb {

/**
 * SchemaLattice representa a estrutura base da grade de predicados.
 * Equivale ao Lattice<Object, Object> do Java.
 */
class SchemaLattice : public Lattice<void *, void *> {
public:
	/**
	 * O construtor recebe o número de predicados por coluna e
	 * inicializa a estrutura base (Lattice).
	 */
	SchemaLattice(const std::vector<int32_t> &preds) : Lattice<void *, void *>(preds) {
		// O trecho ": Lattice<void*, void*>(preds)" é o equivalente
		// exato do "super(preds)" no Java.
	}
};

} // namespace duckdb
