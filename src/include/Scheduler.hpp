#pragma once

#include <vector>
#include <memory>
#include <algorithm>
#include <iostream>
#include <cmath>

#include "RelationalDataset.hpp"
#include "SchemaLattice.hpp"
#include "Predicate.hpp"
#include "TPSubSet.hpp"

// Forward declaration if BetaDistribution is in its own header.
// Assuming a simplified structure based on your Java usage.
#include "BetaDistribution.hpp" 

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

    // Especialização da Lattice para o Agendador.
    // Usamos unique_ptr para garantir que o SchedulerLattice seja o dono
    // da memória alocada para os estados estendidos.
    // class SchedulerLattice : public Lattice<std::unique_ptr<SchedulerNode>, std::unique_ptr<SchedulerEdge>> {
    // public:
    //     SchedulerLattice(Lattice<void*, void*>* base) 
    //         : Lattice<std::unique_ptr<SchedulerNode>, std::unique_ptr<SchedulerEdge>>(*base) {}
    // };

    class SchedulerLattice : public Lattice<std::unique_ptr<SchedulerNode>, std::unique_ptr<SchedulerEdge>> {
    public:
        // Passamos o ponteiro 'base' (convertido para void*) 
        // e pegamos o array de contagem 'preds_count' da Lattice original.
        SchedulerLattice(Lattice<void*, void*>* base) 
            : Lattice<std::unique_ptr<SchedulerNode>, std::unique_ptr<SchedulerEdge>>(
                (void*)base, base->preds_count) {}
    };

    struct ResultEntry {
        SchedulerLattice::Edge* e;
        double s;

        ResultEntry(SchedulerLattice::Edge* edge, double score) : e(edge), s(score) {}
    };

    // --- Atributos da Classe ---

    static constexpr double MIN_GRAD = 1e-6;
    static bool ar; // Definido no arquivo .cpp (equivalente a static boolean ar=false;)

    RelationalDataset* dataset;
    std::unique_ptr<SchedulerLattice> schedulerLattice;

    // --- Construtor ---
    
    explicit Scheduler(RelationalDataset* ds) : dataset(ds) {
        schedulerLattice = std::make_unique<SchedulerLattice>(ds->schema->lattice.get());
    }

    // --- Métodos Principais ---

    void populatePredicates();

protected:
    // Métodos auxiliares para garantir a existência dos objetos de estado
    SchedulerNode* getNode(SchedulerLattice::Node* node);
    SchedulerEdge* getEdge(SchedulerLattice::Edge* edge);

    int32_t getBatchSize(int32_t batch);

    // Motor de Busca em Profundidade (DFS)
    void search(SchedulerLattice::Node* node, 
                int32_t lastPredIdx, 
                const std::vector<int32_t>& predIDXs, 
                const std::vector<Predicate*>& preds, 
                std::vector<ResultEntry>& res);

    void propagateAcross(SchedulerLattice::Edge* e, std::vector<ResultEntry>& res);

    bool exploreNode(SchedulerLattice::Node* n, int32_t cp, int32_t p);
};

} // namespace duckdb