#pragma once

#include <vector>
#include <cstdint>

namespace duckdb {

/**
 * TPSet armazena os pares de índices (x, y) que representam 
 * as combinações de tuplas sendo analisadas pelo LIMA.
 */
struct TPSet {
    std::vector<int32_t> x;
    std::vector<int32_t> y;
    int32_t length;

    // Construtor equivalente ao Java: inicializa os arrays com tamanho n
    TPSet(int32_t n) : length(n) {
        x.resize(n);
        y.resize(n);
    }
};

} // namespace duckdb