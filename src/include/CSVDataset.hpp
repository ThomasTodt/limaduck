#pragma once

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "RelationalDataset.hpp"
#include "TPSubSet.hpp"
#include "TPSet.hpp"
#include <unordered_map>
#include <random>
#include "duckdb/common/helper.hpp"

namespace duckdb {

class CSVDataset : public RelationalDataset {
public:
	// Dados organizados por coluna (Columnar Storage)
	std::vector<std::vector<int32_t>> intData;
	std::vector<std::vector<float>> realData;
	std::vector<std::vector<int32_t>> stringData;

	// Mapeamento para Dictionary Encoding de Strings
	std::unordered_map<std::string, int32_t> stringMap;
	std::vector<std::string> invStringMap;

	std::mt19937 random_engine;

	CSVDataset(const std::string &path, int32_t n_size, uint64_t seed);
	~CSVDataset() override = default;

	std::vector<std::vector<std::string>> get(int32_t n);

	// Implementação dos métodos virtuais puros
	std::unique_ptr<TPSubSet> filter(TPSubSet &TPs, Predicate *pred) override;
	std::unique_ptr<TPSubSet> sample(int32_t n) override;

private:
	void buildColumns(const std::string &path);
	std::ifstream file_stream;
};

} // namespace duckdb
