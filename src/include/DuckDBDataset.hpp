#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <random>
#include "RelationalDataset.hpp"
#include "Schema.hpp"
#include "duckdb.hpp"

namespace duckdb {

class DuckDBDataset : public RelationalDataset {
public:
	std::vector<std::vector<int32_t>> intData;
	std::vector<std::vector<float>> realData;
	std::vector<std::vector<int32_t>> stringData;

	std::unordered_map<std::string, int32_t> stringMap;
	std::vector<std::string> invStringMap;
	std::mt19937 random_engine;

	// Construtor turbinado que recebe a conexão direta do DuckDB
	DuckDBDataset(ClientContext &context, std::string table_name, int32_t num_lines);
	~DuckDBDataset() override = default;

	std::unique_ptr<TPSubSet> filter(TPSubSet &TPs, Predicate *pred) override;
	std::unique_ptr<TPSubSet> sample(int32_t n) override;
};

} // namespace duckdb
