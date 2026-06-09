#pragma once

#include "duckdb.hpp"
#include <cstdint>
#include <vector>
#include <string>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace duckdb {

class BetaDistribution {
public:
	static constexpr double yConst = 0.577215664901532860606512;

	static double y1(int n) {
		if (n >= 1000)
			return std::log(n);
		static std::vector<double> y1mem = {-yConst};
		while ((int)y1mem.size() <= n) {
			double last = y1mem.back();
			int s = y1mem.size();
			y1mem.push_back(last + 1.0 / s);
		}
		return y1mem[n - 1];
	}

	static double y2(int n) {
		if (n >= 500)
			return 1.0 / n;
		static std::vector<double> y2mem = {M_PI * M_PI / 6.0};
		while ((int)y2mem.size() <= n) {
			double last = y2mem.back();
			int s = y2mem.size();
			y2mem.push_back(last - 1.0 / (double(s) * s));
		}
		return y2mem[n - 1];
	}

	int a, b;
	double mean;
	double meanLogOdds;
	double sdLogOdds;
	double grad;

	BetaDistribution(int a = 1, int b = 1) : a(a), b(b) {
		updateStats();
	}

	void updateStats() {
		this->mean = (double)this->a / (this->a + this->b);
		this->meanLogOdds = y1(this->a) - y1(this->b + this->a);
		this->sdLogOdds = y2(this->a) - y2(this->b + this->a);
		double sum = this->a + this->b;
		this->grad = (double)this->b / (this->a * sum * sum);
	}

	void add(int success, int failure) {
		this->a += success;
		this->b += failure;
		updateStats();
	}
};

enum class LimaOperator : uint8_t { EQ = 0, NE = 1, GT = 2, LE = 3, LT = 4, GE = 5 };

static std::string LimaOperatorToString(LimaOperator op) {
	switch (op) {
	case LimaOperator::EQ:
		return "=";
	case LimaOperator::NE:
		return "!=";
	case LimaOperator::GT:
		return ">";
	case LimaOperator::LE:
		return "<=";
	case LimaOperator::LT:
		return "<";
	case LimaOperator::GE:
		return ">=";
	default:
		return "?";
	}
}

struct LimaPredicate {
	idx_t col_idx;
	LimaOperator op;
	idx_t id;

	std::string ToString(const std::vector<std::string> &col_names) const {
		return "t1." + col_names[col_idx] + " " + LimaOperatorToString(op) + " t2." + col_names[col_idx];
	}
};

struct LimaColumn {
	std::string name;
	LogicalType type;
	std::vector<LimaPredicate> predicates;
};

struct LimaSchema {
	std::vector<LimaColumn> columns;
	std::vector<LimaPredicate> all_predicates;

	void Initialize(const std::vector<std::string> &names, const std::vector<LogicalType> &types) {
		columns.clear();
		all_predicates.clear();
		for (idx_t i = 0; i < names.size(); i++) {
			LimaColumn col;
			col.name = names[i];
			col.type = types[i];

			std::vector<LimaOperator> ops;
			if (types[i].IsNumeric() || types[i].id() == LogicalTypeId::DATE ||
			    types[i].id() == LogicalTypeId::TIMESTAMP) {
				ops = {LimaOperator::EQ, LimaOperator::NE, LimaOperator::GT,
				       LimaOperator::LE, LimaOperator::LT, LimaOperator::GE};
			} else {
				ops = {LimaOperator::EQ, LimaOperator::NE};
			}

			for (auto op : ops) {
				LimaPredicate pred;
				pred.col_idx = i;
				pred.op = op;
				pred.id = all_predicates.size();
				col.predicates.push_back(pred);
				all_predicates.push_back(pred);
			}
			columns.push_back(col);
		}
	}

	void Print(FILE *out) const {
		fprintf(out, "--- LIMA SCHEMA ---\n");
		std::vector<std::string> col_names;
		for (auto &col : columns)
			col_names.push_back(col.name);

		for (auto &col : columns) {
			fprintf(out, "Column: %s (%s)\n", col.name.c_str(), col.type.ToString().c_str());
			for (auto &pred : col.predicates) {
				fprintf(out, "  Predicate ID %llu: %s\n", (unsigned long long)pred.id,
				        pred.ToString(col_names).c_str());
			}
		}
		fprintf(out, "Total Predicates: %llu\n", (unsigned long long)all_predicates.size());
		fprintf(out, "-------------------\n");
		fflush(out);
	}
};

struct LimaNode {
	std::vector<int8_t> predicate_ids; // size = num_columns. -1 means no predicate.
	BetaDistribution dist;
	BetaDistribution edge_dist; // Stats for the "edge" that created this node
	bool sound = false;
	size_t hash_val;

	LimaNode(idx_t num_cols) : predicate_ids(num_cols, -1), hash_val(0) {
	}

	void CalculateHash() {
		hash_val = 0;
		for (size_t i = 0; i < predicate_ids.size(); i++) {
			if (predicate_ids[i] != -1) {
				// Simple hash combine
				hash_val ^= std::hash<size_t> {}(i) + 0x9e3779b9 + (hash_val << 6) + (hash_val >> 2);
				hash_val ^= std::hash<int> {}(predicate_ids[i]) + 0x9e3779b9 + (hash_val << 6) + (hash_val >> 2);
			}
		}
	}

	bool operator==(const LimaNode &other) const {
		return predicate_ids == other.predicate_ids;
	}

	std::string ToString(const LimaSchema &schema) const {
		std::string res = "!(";
		bool first = true;
		std::vector<std::string> col_names;
		for (auto &col : schema.columns)
			col_names.push_back(col.name);

		for (size_t i = 0; i < predicate_ids.size(); i++) {
			if (predicate_ids[i] != -1) {
				if (!first)
					res += " & ";
				// Find the predicate within the columns predicates
				for (auto &p : schema.columns[i].predicates) {
					if ((int8_t)p.op == predicate_ids[i]) {
						res += p.ToString(col_names);
						break;
					}
				}
				first = false;
			}
		}
		res += ")";
		return res;
	}
};

struct LimaNodeHash {
	size_t operator()(const LimaNode &n) const {
		return n.hash_val;
	}
};

} // namespace duckdb
