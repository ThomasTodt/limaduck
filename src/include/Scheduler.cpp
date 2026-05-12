#include "Scheduler.hpp"
#include <iostream>
#include <algorithm>
#include <cmath>
#include <cassert>

namespace duckdb {

// Inicialização da variável estática
bool Scheduler::ar = false;

// --- Auxiliares de Acesso aos Nós e Arestas ---

Scheduler::SchedulerNode* Scheduler::getNode(SchedulerLattice::Node* node) {
    if (!node->n) {
        node->n = std::make_unique<SchedulerNode>();
    }
    return node->n.get();
}

Scheduler::SchedulerEdge* Scheduler::getEdge(SchedulerLattice::Edge* edge) {
    if (!edge->e) {
        edge->e = std::make_unique<SchedulerEdge>();
    }
    return edge->e.get();
}

int32_t Scheduler::getBatchSize(int32_t batch) {
    int32_t maxBatch = 20;
    if (batch > maxBatch) batch = maxBatch;
    int32_t batchSize = 100 * (1 << batch);
    return batchSize;
}

// --- Motor Principal: Avaliação Inicial e Agendamento ---

void Scheduler::populatePredicates() {
    auto& preds = dataset->schema->preds;
    int32_t num_preds = (int32_t)preds.size();
    
    std::vector<SchedulerNode*> predNodes(num_preds);
    std::vector<int32_t> predIDs;
    predIDs.reserve(num_preds);

    // 1. Inicializa o primeiro nível da Lattice
    for (int32_t i = 0; i < num_preds; i++) {
        Predicate* pred = preds[i].get();
        SchedulerLattice::Edge* e = schedulerLattice->fetchRoot()->fetchTo(pred->cID, pred->pID, schedulerLattice.get());
        SchedulerLattice::Node* n = e->to;
        
        SchedulerNode* sn = getNode(n);
        SchedulerEdge* se = getEdge(e);

        predNodes[i] = sn;
        sn->TPs.clear(); // Equivalente ao sn.TPs = new ArrayList<>() do Java
        se->sound = true;

        // Se for coluna constante e o operador for NE, GT ou LT, nós penalizamos a distribuição
        if (dataset->constColumn[pred->cols->x->ID] && 
           (pred->op == Predicate::Operator::NE || pred->op == Predicate::Operator::GT || pred->op == Predicate::Operator::LT)) {
            sn->dist.add(0, 1000000000);
        } else {
            predIDs.push_back(i);
        }
    }

    // 2. Amostragem em Batches para avaliar o Gradiente
    double nn = (double)dataset->size;
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
        
        // Amostra novos pares de tuplas
        auto TPs = dataset->sample(batchSize);

        for (int32_t predID : predIDs) {
            Predicate* pred = preds[predID].get();
            SchedulerNode* predNode = predNodes[predID];
            SchedulerEdge* predEdge = getEdge(schedulerLattice->fetchRoot()->fetchTo(pred->cID, pred->pID, schedulerLattice.get()));

            // O filter retorna unique_ptr, então convertemos para shared_ptr para guardar no Node
            std::shared_ptr<TPSubSet> predTPs = dataset->filter(*TPs, pred);
            predNode->TPs.push_back(predTPs);

            int32_t a = predTPs->length;
            int32_t b = batchSize - a;
            
            predNode->dist.add(a, b);
            predEdge->dist.add(a, b);

            if (predNode->dist.grad > MIN_GRAD) {
                std::cerr << "  [LIMA] Keeping Predicate " << predID << ": " << pred->ToString() 
                          << " (grad: " << predNode->dist.grad << ")\n";
                nextPredIDs.push_back(predID);
            } else {
                std::cerr << "  [LIMA] Pruning Predicate " << predID << ": " << pred->ToString() 
                          << " (grad: " << predNode->dist.grad << ")\n";
            }
        }

        predIDs = std::move(nextPredIDs);
        if (predIDs.empty()) break;
    }

    // 3. Ordenação dos Predicados baseada em MeanLogOdds
    std::vector<int32_t> sortedPreds(num_preds);
    for (int32_t i = 0; i < num_preds; i++) sortedPreds[i] = i;

    // Função lambda capturando predNodes e a flag 'ar'
    std::sort(sortedPreds.begin(), sortedPreds.end(), [&](int32_t a, int32_t b) {
        if (ar && (a < 2 || b < 2)) return false; // Equivalente ao return 0 no Comparator do Java
        return predNodes[a]->dist.meanLogOdds > predNodes[b]->dist.meanLogOdds; // Descendente
    });

    std::cerr << "=== Predicados Ordenados para Busca ===\n";
    for (size_t i = 0; i < sortedPreds.size(); i++) {
        int32_t idx = sortedPreds[i];
        std::cerr << "Posicao " << i << " | PredID: " << idx 
                  << " | MeanLogOdds: " << predNodes[idx]->dist.meanLogOdds 
                  << " | Predicado: " << preds[idx]->ToString() << "\n";
    }
    std::cerr << "=======================================\n";

    std::vector<ResultEntry> res;

    // 4. Busca em Profundidade (DFS)
    for (int32_t newPredSortedID = 0; newPredSortedID < num_preds; newPredSortedID++) {
        int32_t newPredIDx = sortedPreds[newPredSortedID];
        Predicate* newPred = preds[newPredIDx].get();
        
        SchedulerLattice::Node* node = schedulerLattice->fetchRoot()->getTo(newPred->cID, newPred->pID, schedulerLattice.get())->to;

        std::cerr << "\n[LOOP PRINCIPAL] Iniciando busca a partir do Predicado Ordenado #" << newPredSortedID << "\n";
        std::cerr << " > No ID (PredSet): " << node->preds.ToString() << "\n";
        std::cerr << " > Coluna: " << newPred->cID << " | Valor/Pred: " << newPred->pID << "\n";
        std::cerr << " > Estatisticas do No: a=" << getNode(node)->dist.a << ", b=" << getNode(node)->dist.b << "\n";
        
        // Passamos ponteiros brutos para preds na busca
        std::vector<Predicate*> raw_preds(preds.size());
        for(size_t i=0; i<preds.size(); i++) raw_preds[i] = preds[i].get();

        search(node, newPredSortedID, sortedPreds, raw_preds, res);
        
        // C++ não precisa de System.gc(). 
        // Os smart pointers serão limpos sozinhos.
    }

    // 5. Exibição dos Resultados Finais
    if (ar) {
        int32_t numcols = (int32_t)dataset->schema->columns.size();
        std::vector<std::vector<double>> pairs(numcols, std::vector<double>(numcols, 0.0));
        
        for (auto& ps : res) {
            for (auto it = ps.e->from->preds.begin(); it != ps.e->from->preds.end(); ++it) {
                int32_t f = it->first;  // curCP
                int32_t t = ps.e->cp;
                if (t != 0) std::cerr << "bad\n";
                double s = ps.s;
                if (s > pairs[f][t]) pairs[f][t] = s;
            }
        }
        for (int32_t f = 0; f < numcols; f++) {
            for (int32_t t = 0; t < 1; t++) {
                std::cout << f << " " << t << " " << pairs[f][t] << "\n";
            }
        }
    } else {
        for (auto& ps : res) {
            std::string s = "!(";
            for (auto it = ps.e->to->preds.begin(); it != ps.e->to->preds.end(); ++it) {
                int32_t cp = it->first;
                int32_t p = it->second;
                s += dataset->schema->columnPairs[cp]->preds[p]->ToString() + " & ";
            }
            s += ")";
            std::cout << s << "\n";
        }
    }
}

// --- DFS: Navegação na Lattice ---

void Scheduler::search(SchedulerLattice::Node* node, int32_t lastPredIdx, 
                       const std::vector<int32_t>& predIDXs, 
                       const std::vector<Predicate*>& preds, 
                       std::vector<ResultEntry>& res) {
    
    std::cerr << "  [LIMA] Searching from node: " << node->preds.ToString() << "\n";
    
    for (int32_t newPredSortedID = 0; newPredSortedID < lastPredIdx; newPredSortedID++) {
        int32_t newPredIDx = predIDXs[newPredSortedID];
        Predicate* newPred = preds[newPredIDx];
        
        if (node->preds.Contains(newPred->cID)) continue;

        if (exploreNode(node, newPred->cID, newPred->pID)) {
            SchedulerLattice::Edge* toE = node->fetchTo(newPred->cID, newPred->pID, schedulerLattice.get());
            SchedulerLattice::Node* toN = toE->to;
            
            propagateAcross(toE, res);
            search(toN, newPredSortedID, predIDXs, preds, res);
            
            // O Java fazia this.getNode(toN).TPs = null; 
            // C++ usa clear() para desalocar a memória controlada pelos shared_ptrs!
            getNode(toN)->TPs.clear(); 
            getNode(toN)->TPs.shrink_to_fit(); 
        }
    }
}

// --- Lógica Central de Filtragem e Poda (Propagate) ---

void Scheduler::propagateAcross(SchedulerLattice::Edge* e, std::vector<ResultEntry>& res) {
    SchedulerNode* fn = getNode(e->from);
    SchedulerNode* tn = getNode(e->to);
    SchedulerEdge* se = getEdge(e);
    
    tn->TPs.clear();
    int32_t a = 0, b = 0;
    
    // Filtro pesado de dados através dos ponteiros compartilhados
    for (auto& TPs : fn->TPs) {
        Predicate* edge_pred = dataset->schema->columnPairs[e->cp]->preds[e->p];
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
        
        SchedulerLattice::Edge* fEdge = e->from->fetchFrom(curCP, curP, schedulerLattice.get());
        SchedulerLattice::Node* fNode = fEdge->from;
        SchedulerLattice::Edge* tEdge = fNode->fetchTo(e->cp, e->p, schedulerLattice.get());
        
        SchedulerEdge* tSEdge = getEdge(tEdge);

        BetaDistribution& lower = tSEdge->dist;
        BetaDistribution& upper = se->dist;

        double devs = 1.0;
        double lowerMinLogProb = lower.meanLogOdds - devs * lower.sdLogOdds;
        double upperMaxLogProb = upper.meanLogOdds + devs * upper.sdLogOdds;

        double combinedSdLogOdds = std::sqrt(lower.sdLogOdds * lower.sdLogOdds + upper.sdLogOdds * upper.sdLogOdds);
        double probDist = lower.meanLogOdds - upper.meanLogOdds;
        double normDist = probDist / combinedSdLogOdds;

        double biasCorrection = 0.8;
        if (upper.mean > biasCorrection) {
            normDist = normDist * std::exp(-(1.0 - biasCorrection) / (1.0 - upper.mean));
        }

        if (normDist < minlogProbDist) minlogProbDist = normDist;

        if (normDist < 2.0) {
            std::cerr << "    [PRUNING STAT] No " << e->to->preds.ToString() 
                      << " | Pred " << e->p << " falhou: normDist=" << normDist << " (Threshold=2.0)\n";
            std::cerr << "      -> MeanLogOdds Pai: " << lower.meanLogOdds << " vs Filho: " << upper.meanLogOdds << "\n";
            se->sound = false;
            break;
        }
    }

    if ((se->sound && se->dist.a == 1) || ar) {
        std::cerr << "  [LIMA] *** DISCOVERY! *** " << e->to->preds.ToString() << "\n";
        res.emplace_back(e, minlogProbDist);
    } else if (se->sound) {
        std::cerr << "  [LIMA] Node " << e->to->preds.ToString() << " is sound (violations: " << a << "). Going deeper...\n";
    } else {
        std::cerr << "  [LIMA] Node " << e->to->preds.ToString() << " is NOT sound. Pruning.\n";
    }
}

// --- Estrutura Condicional (Explore) ---

bool Scheduler::exploreNode(SchedulerLattice::Node* n, int32_t cp, int32_t p) {
    if (getNode(n)->dist.a == 1) return false;
    if (ar && cp != 0) return false;

    for (auto it = n->preds.begin(); it != n->preds.end(); ++it) {
        int32_t curCP = it->first;
        int32_t curP = it->second;
        
        SchedulerLattice::Edge* fEdge = n->fetchFrom(curCP, curP, schedulerLattice.get());
        SchedulerLattice::Node* fNode = fEdge->from;
        SchedulerLattice::Edge* tEdge = fNode->fetchTo(cp, p, schedulerLattice.get());
        SchedulerLattice::Node* tNode = tEdge->to;

        SchedulerEdge* tSEdge = getEdge(tEdge);
        
        if (!tSEdge->sound) {
            std::cerr << "  [PRUNING STRUCT] Abortando " << n->preds.ToString() 
                      << " + {" << cp << "=" << p << "}: Subconjunto ancestral ja e unsound.\n";
            return false;
        }

        SchedulerNode* tSNode = getNode(tNode);
        if (tSNode->dist.a == 1) return false;
    }

    return true;
}

} // namespace duckdb