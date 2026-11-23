#pragma once

#include "Schema.hpp"
#include <cstdint>
#include <vector>
#include <string>
#include <memory>

namespace duckdb {

class TPSubSet;

struct ConstantCluster {
	// Colunas condicionadas por constantes (1 coluna para 1-constant/bipartite, 2 colunas para 2-constant)
	std::vector<int32_t> ignored_cols;
	std::string condition_str;
	// Linhas para t_x (e também t_y quando !is_bipartite)
	std::vector<int32_t> row_ids;
	// Linhas para t_y apenas quando is_bipartite == true (t_x.A = c1 & t_y.A = c2, c1 != c2)
	std::vector<int32_t> row_ids_y;
	bool is_bipartite = false;
	// Índices dos clusters pais de 1-constante (para clusters de 2-constantes verificarem soundness contra os pais)
	std::vector<int32_t> parent_cluster_indices;

	bool isIgnoredCol(int32_t col_id) const {
		for (int32_t c : ignored_cols) {
			if (c == col_id)
				return true;
		}
		return false;
	}
};

class RelationalDataset {
public:
	// Referência ao esquema que define as colunas e predicados
	Schema *schema;

	// Indica se uma coluna possui valor constante (otimização de busca)
	std::vector<bool> constColumn;

	// Número total de tuplas no dataset
	int32_t size;

	// Ponteiros opcionais para o cluster PLI ativo durante a descoberta de Constant DCs (C-LIMA)
	const std::vector<int32_t> *activeCluster = nullptr;
	const std::vector<int32_t> *activeClusterY = nullptr;

	RelationalDataset(Schema *s, int32_t n) : schema(s), size(n), activeCluster(nullptr), activeClusterY(nullptr) {
		constColumn.assign(s->columns.size(), false);
	}

	int32_t effectiveSize() const {
		if (activeCluster != nullptr && activeClusterY != nullptr) {
			double geom = std::sqrt(static_cast<double>(activeCluster->size()) *
			                        static_cast<double>(activeClusterY->size()));
			return static_cast<int32_t>(geom);
		}
		return (activeCluster != nullptr) ? static_cast<int32_t>(activeCluster->size()) : size;
	}

	virtual ~RelationalDataset() = default;

	virtual std::unique_ptr<TPSubSet> filter(TPSubSet &TPs, Predicate *pred) = 0;

	virtual std::unique_ptr<TPSubSet> sample(int32_t n) = 0;

	virtual std::vector<ConstantCluster> buildConstantClusters(double min_support) {
		return {};
	}
};

} // namespace duckdb
