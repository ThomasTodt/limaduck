#include "Scheduler.hpp"
#include <iostream>
#include <algorithm>
#include <cmath>
#include <cassert>
#include "duckdb/common/helper.hpp"

namespace duckdb {

// Inicialização da variável estática
bool Scheduler::ar = false;

// --- Auxiliares de Acesso aos Nós e Arestas ---

Scheduler::SchedulerNode *Scheduler::getNode(SchedulerLattice::Node *node) {
	if (!node->n) {
		node->n = make_uniq<SchedulerNode>();
	}
	return node->n.get();
}

Scheduler::SchedulerEdge *Scheduler::getEdge(SchedulerLattice::Edge *edge) {
	if (!edge->e) {
		edge->e = make_uniq<SchedulerEdge>();
	}
	return edge->e.get();
}

int32_t Scheduler::getBatchSize(int32_t batch) {
	int32_t maxBatch = 20;
	if (batch > maxBatch)
		batch = maxBatch;
	int32_t batchSize = 100 * (1 << batch);
	return batchSize;
}

// Verifica se um predicado p_cand em um column pair é igual ou logicamente implicado por p_glob
// (Ex.: se no contexto pai `!=` nunca é violado, então `>` e `<` também nunca são violados).
static inline bool predImpliedByParent(int32_t p_cand, int32_t p_parent) {
	if (p_cand == p_parent)
		return true;
	// Em Schema.cpp: 0=EQ, 1=NE, 2=GT, 3=LE, 4=LT, 5=GE.
	if (p_parent == 1 && (p_cand == 2 || p_cand == 4))
		return true;
	if (p_parent == 3 && (p_cand == 0 || p_cand == 4))
		return true;
	if (p_parent == 5 && (p_cand == 0 || p_cand == 2))
		return true;
	return false;
}

bool Scheduler::isSubsumedByParent(const PredSet &candidate, const ContextSummary &parent_summary,
                                   const Schema *schema) {
	// 1. Verifica se algum predicado individual do candidato já possui 0 violações no contexto pai
	for (auto it = candidate.begin(); it != candidate.end(); ++it) {
		int32_t cp = it->first;
		int32_t p = it->second;
		Predicate *pred = schema->columnPairs[cp]->preds[p];
		if ((size_t)pred->ID < parent_summary.l1_zero.size() && parent_summary.l1_zero[pred->ID]) {
			return true;
		}
		// Se NE (1) já possui 0 violações no pai, GT (2) e LT (4) também são trivialmente satisfeitos
		if ((p == 2 || p == 4) && schema->columnPairs[cp]->preds.size() > 1) {
			int32_t ne_id = schema->columnPairs[cp]->preds[1]->ID;
			if ((size_t)ne_id < parent_summary.l1_zero.size() && parent_summary.l1_zero[ne_id]) {
				return true;
			}
		}
	}

	// 2. Verifica se alguma DC descoberta no contexto pai é subconjunto (exato ou por implicação numérica)
	for (const auto &pdc : parent_summary.discovered_dcs) {
		if (pdc.Size() > candidate.Size())
			continue;
		bool all_match = true;
		for (auto it = pdc.begin(); it != pdc.end(); ++it) {
			int32_t p_cp = it->first;
			int32_t p_p = it->second;
			int32_t c_p = candidate.Get(p_cp);
			if (c_p < 0 || !predImpliedByParent(c_p, p_p)) {
				all_match = false;
				break;
			}
		}
		if (all_match)
			return true;
	}
	return false;
}

// --- Motor Estatístico Cross-Cluster (Soundness de Log-Odds para evitar Overfitting de Múltiplas Constantes) ---

static inline bool checkBetaLogOddsSoundness(const BetaDistribution &lower, const BetaDistribution &upper) {
	double combinedSdLogOdds = std::sqrt(lower.sdLogOdds * lower.sdLogOdds + upper.sdLogOdds * upper.sdLogOdds);
	if (combinedSdLogOdds <= 0.0)
		return false;
	double probDist = lower.meanLogOdds - upper.meanLogOdds;
	double normDist = probDist / combinedSdLogOdds;
	double biasCorrection = 0.8;
	if (upper.mean > biasCorrection) {
		normDist = normDist * std::exp(-(1.0 - biasCorrection) / (1.0 - upper.mean));
	}
	return normDist >= 2.0;
}

bool Scheduler::isL1SoundAgainstParentContext(int32_t pred_id, const BetaDistribution &child_dist,
                                              const ContextSummary &parent_summary) {
	if (!parent_summary.ref_tpset)
		return true;
	TPSet &ref = *parent_summary.ref_tpset;
	if ((size_t)pred_id >= ref.pred_popcnt.size() || ref.pred_popcnt[pred_id] < 0) {
		// Avalia sob demanda no ref_tpset do pai se ainda não estava no cache
		TPSubSet full_ref(parent_summary.ref_tpset);
		dataset->filter(full_ref, dataset->schema->preds[pred_id].get());
	}
	int32_t a_parent = ref.pred_popcnt[pred_id];
	int32_t b_parent = ref.length - a_parent;
	BetaDistribution lower(1 + a_parent, 1 + b_parent);
	return checkBetaLogOddsSoundness(lower, child_dist);
}

bool Scheduler::isSoundAgainstParentContext(SchedulerLattice::Edge *edge, const ContextSummary &parent_summary) {
	if (!parent_summary.ref_tpset || !edge || !edge->to || !edge->from)
		return true;
	const PredSet &candidate = edge->to->preds;
	const PredSet &premise = edge->from->preds;
	if (candidate.Size() == 0)
		return true;

	TPSet &ref = *parent_summary.ref_tpset;
	const int32_t num_words = (ref.length + 63) >> 6;
	if (num_words <= 0)
		return true;

	// Garante que todos os predicados de `candidate` possuem seus bitvectors de 64 bits no ref_tpset do pai
	std::vector<const uint64_t *> premise_bvs;
	premise_bvs.reserve(premise.Size());
	for (auto it = premise.begin(); it != premise.end(); ++it) {
		Predicate *pred = dataset->schema->columnPairs[it->first]->preds[it->second];
		if (ref.pred_popcnt.empty() || (size_t)pred->ID >= ref.pred_popcnt.size() || ref.pred_popcnt[pred->ID] < 0) {
			TPSubSet full_ref(parent_summary.ref_tpset);
			dataset->filter(full_ref, pred);
		}
		premise_bvs.push_back(ref.pred_bits[pred->ID].data());
	}

	Predicate *edge_pred = dataset->schema->columnPairs[edge->cp]->preds[edge->p];
	if (ref.pred_popcnt.empty() || (size_t)edge_pred->ID >= ref.pred_popcnt.size() ||
	    ref.pred_popcnt[edge_pred->ID] < 0) {
		TPSubSet full_ref(parent_summary.ref_tpset);
		dataset->filter(full_ref, edge_pred);
	}
	const uint64_t *edge_bv = ref.pred_bits[edge_pred->ID].data();

	// Calcula o número de pares no pai que satisfazem a PREMISSA `fn` (a_parent_fn)
	// e quantos desses satisfazem também a aresta final `tn` (a_parent_tn), comparando a distribuição condicional
	// da aresta `Beta(1 + a_parent_tn, 1 + a_parent_fn - a_parent_tn)` contra `getEdge(edge)->dist`!
	int32_t a_parent_fn = 0;
	int32_t a_parent_tn = 0;
	const size_t k_prem = premise_bvs.size();
	const uint64_t last_mask = (ref.length & 63) ? ((1ULL << (ref.length & 63)) - 1ULL) : ~0ULL;

	for (int32_t w = 0; w < num_words; ++w) {
		uint64_t m_fn = (w == num_words - 1) ? last_mask : ~0ULL;
		for (size_t j = 0; j < k_prem && m_fn != 0ULL; ++j) {
			m_fn &= premise_bvs[j][w];
		}
		if (m_fn) {
			a_parent_fn += __builtin_popcountll(m_fn);
			uint64_t m_tn = m_fn & edge_bv[w];
			if (m_tn) {
				a_parent_tn += __builtin_popcountll(m_tn);
			}
		}
	}

	BetaDistribution lower(1 + a_parent_tn, 1 + (a_parent_fn - a_parent_tn));
	const BetaDistribution &upper = getEdge(edge)->dist;
	return checkBetaLogOddsSoundness(lower, upper);
}

// --- Motor Principal: Avaliação Inicial, Descoberta Global e Constant LIMA (1-Const, 2-Const & Bipartite) ---

void Scheduler::populatePredicates(std::vector<std::string> &output_constraints) {
	ContextSummary global_summary;

	// Estágio 1: Executa a passagem global padrão (Variable DCs) sobre toda a tabela D
	dataset->activeCluster = nullptr;
	dataset->activeClusterY = nullptr;
	runSinglePass(output_constraints, global_summary, nullptr, {});

	if (!enableConstantDCs || ar) {
		return;
	}

	// Estágio 2: Constrói os clusters PLI (1-Constante, 2-Constantes Conjuntivas e Bipartidos Cross-Tuple)
	auto clusters = dataset->buildConstantClusters(minSupport);
	std::vector<ContextSummary> cluster_summaries(clusters.size());

	// Estágios 3 & 4: Amostragem Estratificada Condicional + Soundness Estatística Cross-Cluster + Minimalidade
	for (size_t c_idx = 0; c_idx < clusters.size(); ++c_idx) {
		const auto &cluster = clusters[c_idx];
		dataset->activeCluster = &cluster.row_ids;
		dataset->activeClusterY = cluster.is_bipartite ? &cluster.row_ids_y : nullptr;
		schedulerLattice = make_uniq<SchedulerLattice>(dataset->schema->lattice.get());

		// Monta a lista de contextos ancestrais contra os quais este cluster deve ser minimal e estatisticamente sound:
		// - Todo cluster deve ser sound e não-subsumido contra a tabela Global (`global_summary`).
		// - Clusters de 2-Constantes Conjuntivas também devem ser sound e não-subsumidos contra AMBOS os pais de 1-Constante!
		std::vector<const ContextSummary *> parents;
		parents.push_back(&global_summary);
		if (!cluster.is_bipartite) {
			for (int32_t pid : cluster.parent_cluster_indices) {
				if (pid >= 0 && (size_t)pid < c_idx) {
					parents.push_back(&cluster_summaries[pid]);
				}
			}
		}

		runSinglePass(output_constraints, cluster_summaries[c_idx], &cluster, parents);
	}

	dataset->activeCluster = nullptr;
	dataset->activeClusterY = nullptr;
}

void Scheduler::runSinglePass(std::vector<std::string> &output_constraints, ContextSummary &out_summary,
                              const ConstantCluster *cluster,
                              const std::vector<const ContextSummary *> &parent_summaries) {
	auto &preds = dataset->schema->preds;
	int32_t num_preds = (int32_t)preds.size();
	out_summary.l1_zero.assign(num_preds, false);
	out_summary.discovered_dcs.clear();
	out_summary.ref_tpset = nullptr;

	const bool is_constant_pass = (cluster != nullptr);
	const bool is_bipartite = (cluster != nullptr && cluster->is_bipartite);

	std::vector<SchedulerNode *> predNodes(num_preds);
	std::vector<int32_t> predIDs;
	predIDs.reserve(num_preds);

	// 1. Inicializa o primeiro nível da Lattice
	for (int32_t i = 0; i < num_preds; i++) {
		Predicate *pred = preds[i].get();
		SchedulerLattice::Edge *e =
		    schedulerLattice->fetchRoot()->fetchTo(pred->cID, pred->pID, schedulerLattice.get());
		SchedulerLattice::Node *n = e->to;

		SchedulerNode *sn = getNode(n);
		SchedulerEdge *se = getEdge(e);

		predNodes[i] = sn;
		sn->TPs.clear();
		se->sound = true;

		// Se estivermos condicionados nas colunas de `cluster`, ignoramos predicados dessas colunas
		if (is_constant_pass && cluster->isIgnoredCol(pred->cols->x->ID)) {
			sn->dist.add(0, 1000000000);
			se->sound = false;
			continue;
		}

		// Se for coluna constante e o operador for NE, GT ou LT, nós penalizamos a distribuição
		if (dataset->constColumn[pred->cols->x->ID] &&
		    (pred->op == Predicate::Operator::NE || pred->op == Predicate::Operator::GT ||
		     pred->op == Predicate::Operator::LT)) {
			sn->dist.add(0, 1000000000);
		} else {
			predIDs.push_back(i);
		}
	}

	// 2. Amostragem em Batches para avaliar o Gradiente (escalada para effectiveSize() no C-LIMA)
	double nn = (double)dataset->effectiveSize();
	nn = !ar ? nn * nn * 0.0325 : nn * nn * 0.0325;

	int64_t totalN = 0;
	int32_t batches = 0;
	while (totalN < nn) {
		totalN += getBatchSize(batches);
		batches += 1;
	}

	for (int32_t batch = 0; batch < batches; batch++) {
		std::vector<int32_t> nextPredIDs;
		nextPredIDs.reserve(predIDs.size());

		int32_t batchSize = getBatchSize(batch);

		// Amostra novos pares de tuplas (globalmente, homo-cluster ou bipartido)
		auto TPs = dataset->sample(batchSize);

		// Guarda um batch de referência compacto (até 16.384 pares) para testes de soundness dos filhos
		if (!out_summary.ref_tpset || batchSize <= 16384) {
			out_summary.ref_tpset = TPs->source;
		}

		for (int32_t predID : predIDs) {
			Predicate *pred = preds[predID].get();
			SchedulerNode *predNode = predNodes[predID];
			SchedulerEdge *predEdge =
			    getEdge(schedulerLattice->fetchRoot()->fetchTo(pred->cID, pred->pID, schedulerLattice.get()));

			std::shared_ptr<TPSubSet> predTPs = dataset->filter(*TPs, pred);
			predNode->TPs.push_back(predTPs);

			int32_t a = predTPs->length;
			int32_t b = batchSize - a;

			predNode->dist.add(a, b);
			predEdge->dist.add(a, b);

			if (predNode->dist.grad > minGrad) {
				nextPredIDs.push_back(predID);
			}
		}

		predIDs = std::move(nextPredIDs);
		if (predIDs.empty())
			break;
	}

	// Identifica predicados de Nível 1 com 0 violações (dist.a == 1 e amostrados com dist.b > 1)
	for (int32_t i = 0; i < num_preds; i++) {
		Predicate *pred = preds[i].get();
		if ((is_constant_pass && cluster->isIgnoredCol(pred->cols->x->ID)) || dataset->constColumn[pred->cols->x->ID])
			continue;
		if (predNodes[i]->dist.a == 1 && predNodes[i]->dist.b > 1) {
			out_summary.l1_zero[i] = true;
		}
	}

	// Em modo Constant DC, verifica se um predicado de Nível 1 com 0 violações no cluster forma uma Constant DC
	// minimal E estatisticamente sound contra todos os contextos pais (Global e pais de 1-Constante)!
	if (is_constant_pass) {
		for (int32_t i = 0; i < num_preds; i++) {
			if (!out_summary.l1_zero[i])
				continue;
			Predicate *pred = preds[i].get();

			// Para clusters homo-constantes, pares são simétricos, então apenas EQ/NE podem ter 0 violações não-triviais;
			// Para clusters bipartidos (t1.A = c1 & t2.A = c2), operadores de ordem (GT, LT, GE, LE) também podem valer
			// (ex.: !(t1.Role == 'Manager' & t2.Role == 'Intern' & t1.Salary < t2.Salary))!
			if (!is_bipartite && pred->op != Predicate::Operator::EQ && pred->op != Predicate::Operator::NE) {
				continue;
			}
			// Em clusters bipartidos, se NE já vale (0 violações), GT e LT são redundantes;
			// se GE ou LE vale, EQ/GT/LT implicados também são filtrados.
			int32_t cp = pred->cID;
			const auto &cp_preds = dataset->schema->columnPairs[cp]->preds;
			if (cp_preds.size() > 1) {
				bool l1_implied = false;
				for (size_t op_idx = 0; op_idx < cp_preds.size(); ++op_idx) {
					if ((int32_t)op_idx == pred->pID)
						continue;
					int32_t other_id = cp_preds[op_idx]->ID;
					if (out_summary.l1_zero[other_id] && predImpliedByParent(pred->pID, (int32_t)op_idx)) {
						l1_implied = true;
						break;
					}
				}
				if (l1_implied)
					continue;
			}

			bool valid_across_parents = true;
			for (const ContextSummary *parent : parent_summaries) {
				if ((size_t)i < parent->l1_zero.size() && parent->l1_zero[i]) {
					valid_across_parents = false;
					break;
				}
				// Para clusters bipartidos (t1.A = c1 & t2.A = c2 com c1 != c2), todo par satisfaz t1.A != t2.A.
				// Portanto, se a DC global !(A != A & pred) já existe no pai, a regra bipartida é 100% redundante!
				if (is_bipartite && !cluster->ignored_cols.empty()) {
					int32_t colA = cluster->ignored_cols[0];
					PredSet virtual_bip_cand(schedulerLattice->predProd);
					virtual_bip_cand.Add(colA, 1); // 1 = Predicate::Operator::NE (!=)
					virtual_bip_cand.Add(pred->cID, pred->pID);
					if (isSubsumedByParent(virtual_bip_cand, *parent, dataset->schema)) {
						valid_across_parents = false;
						break;
					}
				}
				if (!isL1SoundAgainstParentContext(i, predNodes[i]->dist, *parent)) {
					valid_across_parents = false;
					break;
				}
			}
			if (valid_across_parents) {
				std::string s = "!(" + cluster->condition_str + " & " + pred->ToString() + " & )";
				output_constraints.push_back(s);
			}
		}
	}

	// Para clusters bipartidos (t1.A = c1 & t2.A = c2), o interesse principal são as regras diretas
	// entre os dois grupos constantes (Nível 1); para clusters homo-constantes (1-Const e 2-Const),
	// executamos também a busca na Lattice de Nível 2+ para descobrir CFDs e DCs multi-atributo!
	if (is_bipartite) {
		return;
	}

	// 3. Ordenação dos Predicados baseada em MeanLogOdds
	std::vector<int32_t> sortedPreds(num_preds);
	for (int32_t i = 0; i < num_preds; i++)
		sortedPreds[i] = i;

	std::sort(sortedPreds.begin(), sortedPreds.end(), [&](int32_t a, int32_t b) {
		if (ar && (a < 2 || b < 2))
			return false;
		return predNodes[a]->dist.meanLogOdds > predNodes[b]->dist.meanLogOdds;
	});

	std::vector<ResultEntry> res;
	std::vector<Predicate *> raw_preds(preds.size());
	for (size_t i = 0; i < preds.size(); i++)
		raw_preds[i] = preds[i].get();

	// 4. Busca em Profundidade (DFS)
	for (int32_t newPredSortedID = 0; newPredSortedID < num_preds; newPredSortedID++) {
		int32_t newPredIDx = sortedPreds[newPredSortedID];
		Predicate *newPred = preds[newPredIDx].get();

		if (is_constant_pass && cluster->isIgnoredCol(newPred->cols->x->ID))
			continue;

		SchedulerLattice::Node *node =
		    schedulerLattice->fetchRoot()->getTo(newPred->cID, newPred->pID, schedulerLattice.get())->to;

		search(node, newPredSortedID, sortedPreds, raw_preds, res, cluster);
	}

	// 5. Exibição / Filtragem de Minimalidade e Soundness Cross-Cluster dos Resultados Finais
	if (ar) {
		int32_t numcols = (int32_t)dataset->schema->columns.size();
		std::vector<std::vector<double>> pairs(numcols, std::vector<double>(numcols, 0.0));

		for (auto &ps : res) {
			for (auto it = ps.e->from->preds.begin(); it != ps.e->from->preds.end(); ++it) {
				int32_t f = it->first;
				int32_t t = ps.e->cp;
				double s = ps.s;
				if (s > pairs[f][t])
					pairs[f][t] = s;
			}
		}
		for (int32_t f = 0; f < numcols; f++) {
			for (int32_t t = 0; t < 1; t++) {
				std::string result_str =
				    std::to_string(f) + " " + std::to_string(t) + " " + std::to_string(pairs[f][t]);
				output_constraints.push_back(result_str);
			}
		}
	} else {
		std::vector<PredSet> raw_cluster_candidates;
		if (is_constant_pass) {
			raw_cluster_candidates.reserve(res.size());
			for (auto &ps : res) {
				raw_cluster_candidates.push_back(ps.e->to->preds);
			}
		}

		for (size_t r_idx = 0; r_idx < res.size(); ++r_idx) {
			auto &ps = res[r_idx];
			const PredSet &cand_preds = ps.e->to->preds;

			if (is_constant_pass) {
				// 1. Verifica que nenhum predicado individual do candidato já era L1 zero no próprio cluster
				bool has_cluster_l1 = false;
				for (auto it = cand_preds.begin(); it != cand_preds.end(); ++it) {
					Predicate *p_ptr = dataset->schema->columnPairs[it->first]->preds[it->second];
					if (out_summary.l1_zero[p_ptr->ID]) {
						has_cluster_l1 = true;
						break;
					}
					if ((p_ptr->op == Predicate::Operator::GT || p_ptr->op == Predicate::Operator::LT) &&
					    dataset->schema->columnPairs[it->first]->preds.size() > 1 &&
					    out_summary.l1_zero[dataset->schema->columnPairs[it->first]->preds[1]->ID]) {
						has_cluster_l1 = true;
						break;
					}
				}
				if (has_cluster_l1)
					continue;

				// 2. Verifica Minimalidade Hierárquica E Soundness Estatística Condicional da Aresta (normDist >= 2.0)
				// contra TODOS os contextos pais (Global e, para 2-constantes, ambos os clusters pais de 1-constante)!
				bool valid_against_parents = true;
				for (const ContextSummary *parent : parent_summaries) {
					if (isSubsumedByParent(cand_preds, *parent, dataset->schema)) {
						valid_against_parents = false;
						break;
					}
					if (!isSoundAgainstParentContext(ps.e, *parent)) {
						valid_against_parents = false;
						break;
					}
				}
				if (!valid_against_parents)
					continue;

				// 3. Filtra variantes numéricas redundantes (`<`, `>`) implicadas por `!=` no mesmo cluster
				bool implied_in_cluster = false;
				for (size_t j = 0; j < raw_cluster_candidates.size(); ++j) {
					if (j == r_idx)
						continue;
					const PredSet &other = raw_cluster_candidates[j];
					if (other.Size() > cand_preds.Size())
						continue;
					bool strictly_stronger = (other.Size() < cand_preds.Size());
					bool all_match = true;
					for (auto it = other.begin(); it != other.end(); ++it) {
						int32_t o_cp = it->first;
						int32_t o_p = it->second;
						int32_t c_p = cand_preds.Get(o_cp);
						if (c_p < 0 || !predImpliedByParent(c_p, o_p)) {
							all_match = false;
							break;
						}
						if (c_p != o_p) {
							strictly_stronger = true;
						}
					}
					if (all_match && strictly_stronger) {
						implied_in_cluster = true;
						break;
					}
				}
				if (implied_in_cluster)
					continue;
			}

			out_summary.discovered_dcs.push_back(cand_preds);

			std::string s = "!(";
			if (is_constant_pass && !cluster->condition_str.empty()) {
				s += cluster->condition_str + " & ";
			}
			for (auto it = cand_preds.begin(); it != cand_preds.end(); ++it) {
				int32_t cp = it->first;
				int32_t p = it->second;
				s += dataset->schema->columnPairs[cp]->preds[p]->ToString() + " & ";
			}
			s += ")";
			output_constraints.push_back(s);
		}
	}
}

// --- DFS: Navegação na Lattice ---

void Scheduler::search(SchedulerLattice::Node *node, int32_t lastPredIdx, const std::vector<int32_t> &predIDXs,
                       const std::vector<Predicate *> &preds, std::vector<ResultEntry> &res,
                       const ConstantCluster *cluster) {

	for (int32_t newPredSortedID = 0; newPredSortedID < lastPredIdx; newPredSortedID++) {
		int32_t newPredIDx = predIDXs[newPredSortedID];
		Predicate *newPred = preds[newPredIDx];

		if (cluster != nullptr && cluster->isIgnoredCol(newPred->cols->x->ID))
			continue;

		if (node->preds.Contains(newPred->cID))
			continue;

		if (exploreNode(node, newPred->cID, newPred->pID)) {
			SchedulerLattice::Edge *toE = node->fetchTo(newPred->cID, newPred->pID, schedulerLattice.get());
			SchedulerLattice::Node *toN = toE->to;

			propagateAcross(toE, res);
			search(toN, newPredSortedID, predIDXs, preds, res, cluster);

			getNode(toN)->TPs.clear();
			getNode(toN)->TPs.shrink_to_fit();
		}
	}
}

// --- Lógica Central de Filtragem e Poda (Propagate) ---

void Scheduler::propagateAcross(SchedulerLattice::Edge *e, std::vector<ResultEntry> &res) {
	SchedulerNode *fn = getNode(e->from);
	SchedulerNode *tn = getNode(e->to);
	SchedulerEdge *se = getEdge(e);

	tn->TPs.clear();
	int32_t a = 0, b = 0;

	Predicate *edge_pred = dataset->schema->columnPairs[e->cp]->preds[e->p];
	for (auto &TPs : fn->TPs) {
		std::shared_ptr<TPSubSet> filteredTPs = dataset->filter(*TPs, edge_pred);

		tn->TPs.push_back(filteredTPs);
		a += filteredTPs->length;
		b += TPs->source->length - filteredTPs->length;
	}

	tn->dist.add(a, b);
	se->dist.add(tn->dist.a - 1, fn->dist.a - tn->dist.a);

	se->sound = true;

	double minlogProbDist = 1e10;

	for (auto it = e->from->preds.begin(); it != e->from->preds.end(); ++it) {
		int32_t curCP = it->first;
		int32_t curP = it->second;

		SchedulerLattice::Edge *fEdge = e->from->fetchFrom(curCP, curP, schedulerLattice.get());
		SchedulerLattice::Node *fNode = fEdge->from;
		SchedulerLattice::Edge *tEdge = fNode->fetchTo(e->cp, e->p, schedulerLattice.get());

		SchedulerEdge *tSEdge = getEdge(tEdge);

		BetaDistribution &lower = tSEdge->dist;
		BetaDistribution &upper = se->dist;

		double combinedSdLogOdds = std::sqrt(lower.sdLogOdds * lower.sdLogOdds + upper.sdLogOdds * upper.sdLogOdds);
		double probDist = lower.meanLogOdds - upper.meanLogOdds;
		double normDist = probDist / combinedSdLogOdds;

		double biasCorrection = 0.8;
		if (upper.mean > biasCorrection) {
			normDist = normDist * std::exp(-(1.0 - biasCorrection) / (1.0 - upper.mean));
		}

		if (normDist < minlogProbDist)
			minlogProbDist = normDist;

		if (normDist < 2.0) {
			se->sound = false;
			break;
		}
	}

	if ((se->sound && se->dist.a == 1) || ar) {
		res.emplace_back(e, minlogProbDist);
	}
}

// --- Estrutura Condicional (Explore) ---

bool Scheduler::exploreNode(SchedulerLattice::Node *n, int32_t cp, int32_t p) {
	if (getNode(n)->dist.a == 1)
		return false;
	if (ar && cp != 0)
		return false;

	for (auto it = n->preds.begin(); it != n->preds.end(); ++it) {
		int32_t curCP = it->first;
		int32_t curP = it->second;

		SchedulerLattice::Edge *fEdge = n->fetchFrom(curCP, curP, schedulerLattice.get());
		SchedulerLattice::Node *fNode = fEdge->from;
		SchedulerLattice::Edge *tEdge = fNode->fetchTo(cp, p, schedulerLattice.get());
		SchedulerLattice::Node *tNode = tEdge->to;

		SchedulerEdge *tSEdge = getEdge(tEdge);

		if (!tSEdge->sound) {
			return false;
		}

		SchedulerNode *tSNode = getNode(tNode);
		if (tSNode->dist.a == 1)
			return false;
	}

	return true;
}

} // namespace duckdb
