#pragma once

#include "TPSet.hpp"
#include <vector>
#include <algorithm>
#include <memory>

namespace duckdb {

/**
 * Representa um subconjunto de pares de tuplas.
 * É aqui que a filtragem do LIMA acontece.
 */
class TPSubSet {
public:
	// Mantemos um ponteiro para o set de origem (sharding/sampling)
	std::shared_ptr<TPSet> source;
	std::vector<int32_t> TPs;
	int32_t length;

	// Construtor 1: Cria um subset completo a partir de um TPSet
	TPSubSet(std::shared_ptr<TPSet> source_ptr) : source(source_ptr) {
		this->length = source_ptr->length;
		this->TPs.resize(length);
		// Preenche com 0, 1, 2, ..., length-1
		for (int32_t i = 0; i < length; i++) {
			this->TPs[i] = i;
		}
	}

	// Construtor 2: Cria um subset vazio baseado em outro (para resultados de filtros)
	TPSubSet(const TPSubSet &other) : source(other.source) {
		this->length = 0;
		this->TPs.resize(other.TPs.size());
	}

	/**
	 * Ajusta o tamanho do vetor após uma operação de filtragem.
	 * No C++, o std::vector::resize lida com isso de forma eficiente.
	 */
	void resize(int32_t new_length) {
		this->length = new_length;
		this->TPs.resize(new_length);
		// Opcional: usar shrink_to_fit se a memória for muito crítica,
		// mas no LIMA a performance do allocator geralmente é prioridade.
	}
};

} // namespace duckdb
