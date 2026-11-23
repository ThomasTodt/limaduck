#pragma once

#include <cstdint>
#include "TPSet.hpp"
#include <vector>
#include <algorithm>
#include <memory>

namespace duckdb {

/**
 * Representa um subconjunto de pares de tuplas.
 * Suporta representação em bitvector de 64 bits (uint64_t) para interseção
 * SIMD/POPCNT rápida (64 pares por instrução) e representação esparsa
 * corrigida (TPSubSet(const TPSubSet &other) sem super-alocação).
 */
class TPSubSet {
public:
	std::shared_ptr<TPSet> source;
	std::vector<int32_t> TPs;
	std::vector<uint64_t> bits;
	int32_t length = 0;
	int32_t min_word = 0;
	int32_t max_word = -1;
	bool is_full = false;
	bool has_bits = false;

	// Construtor 1: Cria um subset raiz completo a partir de um TPSet
	// (Sem alocar nem preencher o vetor 0..length-1 de int32_t!)
	TPSubSet(std::shared_ptr<TPSet> source_ptr)
	    : source(source_ptr), length(source_ptr->length), min_word(0),
	      max_word(((source_ptr->length + 63) >> 6) - 1), is_full(true), has_bits(false) {
	}

	// Construtor 2 (1-line fix target): Cria um subset para resultados de filtros
	// dimensionado apenas pelo subconjunto pai `other` (e não por `other.source`).
	TPSubSet(const TPSubSet &other)
	    : source(other.source), length(0), min_word(other.min_word), max_word(other.max_word), is_full(false),
	      has_bits(false) {
		if (!other.is_full && !other.has_bits) {
			this->TPs.resize(other.TPs.size());
		}
	}

	/**
	 * Ajusta o tamanho do vetor esparso após uma operação de filtragem.
	 */
	void resize(int32_t new_length) {
		this->length = new_length;
		this->TPs.resize(new_length);
	}
};

} // namespace duckdb
