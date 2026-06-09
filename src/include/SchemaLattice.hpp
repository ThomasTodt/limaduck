#pragma once

#include <cstdint>
#include "Lattice.hpp"

namespace duckdb {

class SchemaLattice : public Lattice<void *, void *> {
public:
	SchemaLattice(const std::vector<int32_t> &preds) : Lattice<void *, void *>(preds) {
	}
};

} // namespace duckdb
