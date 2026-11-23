#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <memory>
#include <algorithm>
#include <iostream>
#include <cmath>

#include "RelationalDataset.hpp"
#include "SchemaLattice.hpp"
#include "Predicate.hpp"
#include "TPSubSet.hpp"

#include "BetaDistribution.hpp"
#include "duckdb/common/helper.hpp"

namespace duckdb {

class Scheduler {
public:
	// --- Estruturas Internas ---

	struct SchedulerNode {
		BetaDistribution dist;
		bool satisfied = false;
		// Vector of shared_ptr to avoid deep copies during search propagation
		std::vector<std::shared_ptr<TPSubSet>> TPs;
	};

	struct SchedulerEdge {
		BetaDistribution dist;
		bool sound = false;
	};

	class SchedulerLattice : public Lattice<std::unique_ptr<SchedulerNode>, std::unique_ptr<SchedulerEdge>> {
	public:
		SchedulerLattice(Lattice<void *, void *> *base)
		    : Lattice<std::unique_ptr<SchedulerNode>, std::unique_ptr<SchedulerEdge>>((void *)base, base->preds_count) {
		}
	};

	struct ResultEntry {
		SchedulerLattice::Edge *e;
		double s;

		ResultEntry(SchedulerLattice::Edge *edge, double score) : e(edge), s(score) {
		}
	};

	struct ContextSummary {
		std::vector<PredSet> discovered_dcs;
		std::vector<bool> l1_zero;
		std::shared_ptr<TPSet> ref_tpset;
	};

	// --- Atributos da Classe ---

	double minGrad = 1e-6;
	bool enableConstantDCs = false;
	double minSupport = 0.01;
	static bool ar;

	RelationalDataset *dataset;
	std::unique_ptr<SchedulerLattice> schedulerLattice;

	explicit Scheduler(RelationalDataset *ds, double threshold_val, bool constant_dcs = false,
	                   double min_support = 0.01)
	    : minGrad(threshold_val), enableConstantDCs(constant_dcs), minSupport(min_support), dataset(ds) {
		schedulerLattice = make_uniq<SchedulerLattice>(ds->schema->lattice.get());
	}

	void populatePredicates(std::vector<std::string> &output_constraints);

protected:
	SchedulerNode *getNode(SchedulerLattice::Node *node);
	SchedulerEdge *getEdge(SchedulerLattice::Edge *edge);

	int32_t getBatchSize(int32_t batch);

	void runSinglePass(std::vector<std::string> &output_constraints, ContextSummary &out_summary,
	                   const ConstantCluster *cluster = nullptr,
	                   const std::vector<const ContextSummary *> &parent_summaries = {});

	static bool isSubsumedByParent(const PredSet &candidate, const ContextSummary &parent_summary,
	                               const Schema *schema);

	bool isSoundAgainstParentContext(SchedulerLattice::Edge *edge, const ContextSummary &parent_summary);

	bool isL1SoundAgainstParentContext(int32_t pred_id, const BetaDistribution &child_dist,
	                                   const ContextSummary &parent_summary);

	// Motor de Busca em Profundidade (DFS)
	void search(SchedulerLattice::Node *node, int32_t lastPredIdx, const std::vector<int32_t> &predIDXs,
	            const std::vector<Predicate *> &preds, std::vector<ResultEntry> &res,
	            const ConstantCluster *cluster = nullptr);

	void propagateAcross(SchedulerLattice::Edge *e, std::vector<ResultEntry> &res);

	bool exploreNode(SchedulerLattice::Node *n, int32_t cp, int32_t p);
};

} // namespace duckdb
