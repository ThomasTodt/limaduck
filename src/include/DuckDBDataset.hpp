#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include "RelationalDataset.hpp"
#include "Schema.hpp"
#include "TPSet.hpp"
#include "duckdb.hpp"

namespace duckdb {

/**
 * Xoshiro256++ (Blackman & Vigna): PRNG de 64 bits ultrarrápido
 * inicializado via SplitMix64, substituindo std::mt19937.
 */
struct Xoshiro256PlusPlus {
	uint64_t s[4];

	static inline uint64_t rotl(const uint64_t x, int k) {
		return (x << k) | (x >> (64 - k));
	}

	explicit Xoshiro256PlusPlus(uint64_t seed_val = 42) {
		seed(seed_val);
	}

	void seed(uint64_t seed_val) {
		uint64_t z = seed_val;
		for (int i = 0; i < 4; ++i) {
			z += 0x9e3779b97f4a7c15ULL;
			uint64_t x = z;
			x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
			x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
			s[i] = x ^ (x >> 31);
		}
	}

	inline uint64_t next() {
		const uint64_t result = rotl(s[0] + s[3], 23) + s[0];
		const uint64_t t = s[1] << 17;
		s[2] ^= s[0];
		s[3] ^= s[1];
		s[1] ^= s[2];
		s[0] ^= s[3];
		s[2] ^= t;
		s[3] = rotl(s[3], 45);
		return result;
	}
};

class DuckDBDataset : public RelationalDataset {
public:
	std::vector<std::vector<int32_t>> intData;
	std::vector<std::vector<float>> realData;
	std::vector<std::vector<int32_t>> stringData;

	std::unordered_map<std::string, int32_t> stringMap;
	std::vector<std::string> invStringMap;
	Xoshiro256PlusPlus random_engine;

	// Construtor turbinado que recebe a conexão direta do DuckDB
	DuckDBDataset(ClientContext &context, std::string table_name, int32_t num_lines);
	~DuckDBDataset() override = default;

	const std::vector<uint64_t> &getOrBuildPredBitvector(TPSet &source, Predicate *pred, int32_t &out_popcnt);

	std::unique_ptr<TPSubSet> filter(TPSubSet &TPs, Predicate *pred) override;
	std::unique_ptr<TPSubSet> sample(int32_t n) override;
	std::vector<ConstantCluster> buildConstantClusters(double min_support) override;
};

} // namespace duckdb
