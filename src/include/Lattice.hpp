#pragma once

#include "PredSet.hpp"
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <memory>
#include <functional>
#include <cassert>
#include "duckdb/common/helper.hpp"


namespace duckdb {

/**
 * Lattice genérica para o sistema LIMA.
 * N: Tipo de dado armazenado no Nó (ex: SchedulerNode)
 * E: Tipo de dado armazenado na Aresta (ex: SchedulerEdge)
 */
template <typename N, typename E>
class Lattice {
public:
    struct Edge;

    /**
     * Nó da Lattice: representa uma conjunção de predicados.
     */
    struct Node {
        N n; // Dados específicos (definidos no Scheduler)
        
        // Mapeamentos de arestas (cp = column pair, p = predicate)
        std::unordered_map<int32_t, std::vector<Edge*>> to;
        std::unordered_map<int32_t, Edge*> from;
        
        PredSet preds;
        void* base_node = nullptr; // Referência para o nó correspondente na base lattice

        Node(const PredSet& ps) : preds(ps) {}

        // Métodos de conveniência idênticos ao Java
        Edge* getTo(int cp, int p, Lattice<N, E>* parent) { return parent->getTo(this, cp, p); }
        Edge* fetchTo(int cp, int p, Lattice<N, E>* parent) { return parent->fetchTo(this, cp, p); }
        Edge* getFrom(int cp, int p, Lattice<N, E>* parent) { return parent->getFrom(this, cp, p); }
        Edge* fetchFrom(int cp, int p, Lattice<N, E>* parent) { return parent->fetchFrom(this, cp, p); }
    };

    /**
     * Aresta da Lattice: representa a adição de um predicado.
     */
    struct Edge {
        E e; // Dados específicos (definidos no Scheduler)
        void* base_edge = nullptr;

        Node *from, *to;
        int32_t cp, p;

        Edge(Node* f, int32_t col, int32_t pred, Node* t) 
            : from(f), to(t), cp(col), p(pred) {}
    };

    // Estrutura de suporte para IDs de predicados
    std::vector<int32_t> preds_count;
    std::vector<int32_t> predProd;

    // Contentores de memória (Garantem que os objetos vivam enquanto a Lattice existir)
    std::unordered_map<PredSet, std::unique_ptr<Node>> nodes;
    std::vector<std::unique_ptr<Edge>> all_edges;

    Node* root = nullptr;
    void* base_lattice = nullptr; // Lattice<?, ?> base do Java

    // Construtor principal
    Lattice(const std::vector<int32_t>& p_counts) : preds_count(p_counts) {
        this->base_lattice = this;
        InitializePredProd();
        this->root = fetchNode(PredSet(this->predProd));
    }

    // Construtor baseado em outra Lattice
    Lattice(void* base_lat, const std::vector<int32_t>& p_counts) 
        : preds_count(p_counts), base_lattice(base_lat) {
        InitializePredProd();
        this->root = fetchNode(PredSet(this->predProd));
    }

    void InitializePredProd() {
        predProd.resize(preds_count.size());
        if (predProd.empty()) return;
        predProd[0] = 1;
        for (size_t i = 1; i < preds_count.size(); i++) {
            predProd[i] = (predProd[i - 1] * (preds_count[i - 1] + 1)) % PredSet::MOD;
        }
    }

    Node* fetchRoot() {
        return fetchNode(PredSet(this->predProd));
    }

    Node* fetchNode(const PredSet& ps) {
        // 1. Verificar se o nó já existe no cache
        auto it = nodes.find(ps);
        if (it != nodes.end()) return it->second.get();

        Node* n = nullptr;

        // 2. Garantir unicidade da raiz
        if (ps.Size() == 0) {
            n = this->root;
        } else {
            // Criar novo nó
            auto new_node = make_uniq<Node>(ps);
            n = new_node.get();
            nodes[ps] = std::move(new_node);
        }

        // 3. Lógica de Link Recursivo (Base)
        if (this->base_lattice == (void*)this) {
            n->base_node = (void*)n;
        } else {
            // Como base_lattice é void*, precisamos tratá-lo como a 
            // classe base (SchemaLattice) para chamar fetchNode recursivamente.
            // Assumimos que a base_lattice é uma Lattice<void*, void*>.
            auto* base_ptr = static_cast<Lattice<void*, void*>*>(this->base_lattice);
            n->base_node = (void*)base_ptr->fetchNode(ps);
        }

        return n;
    }

    // --- Métodos de Navegação (Getters) ---

    Edge* getTo(Node* n, int cp, int p) {
        auto it = n->to.find(cp);
        if (it != n->to.end() && (size_t)p < it->second.size()) {
            return it->second[p];
        }
        return nullptr;
    }

    Edge* getFrom(Node* n, int cp, int p) {
        auto it = n->from.find(cp);
        if (it != n->from.end()) {
            assert(it->second->p == p);
            return it->second;
        }
        return nullptr;
    }

    // --- Métodos de Construção (Fetchers) ---

    std::vector<Edge*>& fetchToEdges(Node* n, int cp) {
        auto& ecps = n->to[cp];
        if (ecps.empty()) {
            ecps.resize(preds_count[cp], nullptr);
        }
        return ecps;
    }

    Edge* fetchTo(Node* n, int cp, int p) {
        Edge* e = getTo(n, cp, p);
        if (e == nullptr) {
            PredSet newPreds(n->preds);
            assert(!newPreds.Contains(cp));
            newPreds.Add(cp, p);
            
            Node* toNode = fetchNode(newPreds);
            auto new_edge = make_uniq<Edge>(n, cp, p, toNode);
            e = new_edge.get();
            linkEdge(e);
            all_edges.push_back(std::move(new_edge));
        }
        return e;
    }

    Edge* fetchFrom(Node* n, int cp, int p) {
        Edge* e = n->from[cp];
        if (e == nullptr) {
            PredSet newPreds(n->preds);
            assert(newPreds.Contains(cp));
            newPreds.Remove(cp, p);
            
            Node* fromNode = fetchNode(newPreds);
            auto new_edge = make_uniq<Edge>(fromNode, cp, p, n);
            e = new_edge.get();
            linkEdge(e);
            all_edges.push_back(std::move(new_edge));
        }
        return e;
    }

    void linkEdge(Edge* e) {
        e->to->from[e->cp] = e;
        auto& toEdges = fetchToEdges(e->from, e->cp);
        assert(toEdges[e->p] == nullptr);
        toEdges[e->p] = e;
    }

    // --- Métodos de Travessia (BFS) ---

    void supersets(Node* n, std::function<bool(Node*)> f) {
        traverse(n, f, true, false);
    }

    void subsets(Node* n, std::function<bool(Node*)> f) {
        traverse(n, f, false, false);
    }

    void fetchSupersets(Node* n, std::function<bool(Node*)> f) {
        traverse(n, f, true, true);
    }

private:
    void traverse(Node* n, std::function<bool(Node*)>& f, bool up, bool force_fetch) {
        std::queue<Node*> q;
        std::unordered_set<Node*> visited;
        q.push(n);
        visited.insert(n);

        while (!q.empty()) {
            Node* curr = q.front();
            q.pop();

            if (f(curr)) continue;

            if (up) {
                if (force_fetch) {
                    for (int cp = 0; cp < (int)preds_count.size(); cp++) {
                        if (curr->preds.Contains(cp)) continue;
                        for (int p = 0; p < preds_count[cp]; p++) {
                            Node* next = fetchTo(curr, cp, p)->to;
                            if (visited.insert(next).second) q.push(next);
                        }
                    }
                } else {
                    for (auto& entry : curr->to) {
                        for (Edge* e : entry.second) {
                            if (e && visited.insert(e->to).second) q.push(e->to);
                        }
                    }
                }
            } else {
                for (auto& entry : curr->from) {
                    Node* next = entry.second->from;
                    if (visited.insert(next).second) q.push(next);
                }
            }
        }
    }
};

} // namespace duckdb