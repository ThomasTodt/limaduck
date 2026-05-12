#pragma once

#include <unordered_map>
#include <vector>
#include <string>
#include <sstream>
#include <cassert>

namespace duckdb {

/**
 * PredSet armazena uma conjunção de predicados e mantém um hash incremental.
 * Traduzido de edu.upc.lattice.PredSet
 */
class PredSet {
public:
    static constexpr int32_t MOD = 333333313; //

private:
    // Mapeia Coluna (ID) -> Predicado (ID)
    std::unordered_map<int32_t, int32_t> preds;
    int32_t hash_val;
    std::vector<int32_t> predProd;

public:
    // Construtor inicial
    PredSet(const std::vector<int32_t>& predProd_ref) 
        : hash_val(0), predProd(predProd_ref) {}

    // Construtor de cópia
    PredSet(const PredSet& source) 
        : preds(source.preds), hash_val(source.hash_val), predProd(source.predProd) {}

    // Calcula o score de um predicado individual
    int32_t PredScore(int32_t cp, int32_t p) const {
        return (p + 1) * predProd[cp];
    }

    // Adiciona um predicado e atualiza o hash incrementalmente
    void Add(int32_t cp, int32_t p) {
        assert(preds.find(cp) == preds.end()); //
        preds[cp] = p;
        // Atualiza hash: (hash + score) % MOD
        hash_val = (hash_val + PredScore(cp, p)) % MOD;
    }

    // Remove um predicado e reverte o hash
    void Remove(int32_t cp, int32_t p) {
        assert(preds.find(cp) != preds.end()); //
        assert(preds[cp] == p);
        preds.erase(cp);
        // Reverte hash: (hash - score + MOD) % MOD para evitar números negativos
        hash_val = (hash_val - PredScore(cp, p) + MOD) % MOD;
    }

    bool Contains(int32_t cp) const {
        return preds.find(cp) != preds.end(); //
    }

    int32_t Get(int32_t cp) const {
        auto it = preds.find(cp);
        return (it != preds.end()) ? it->second : -1; //
    }

    size_t Size() const {
        return preds.size(); //
    }

    // Retorna o hash pré-calculado
    int32_t GetHashCode() const {
        return hash_val;
    }

    // Sobrecarga do operador de igualdade para uso em mapas
    bool operator==(const PredSet& other) const {
        return preds == other.preds; // Compara o conteúdo dos mapas
    }

    // Para iterar sobre os predicados (equivalente ao Cursor do Java)
    // auto begin() const { return preds.begin(); }
    // auto end() const { return preds.end(); }

    // ✅ A correção (C++11):
    auto begin() const -> decltype(preds.begin()) { return preds.begin(); }
    auto end() const -> decltype(preds.end()) { return preds.end(); }

    std::string ToString() const {
        std::stringstream ss;
        ss << "{";
        for (auto it = preds.begin(); it != preds.end(); ++it) {
            if (it != preds.begin()) ss << ", ";
            ss << it->first << "=" << it->second;
        }
        ss << "}";
        return ss.str();
    }
};

} // namespace duckdb

/**
 * Especialização de std::hash para permitir que PredSet seja usado como chave
 * em um std::unordered_map (como na Lattice).
 */
namespace std {
    template <>
    struct hash<duckdb::PredSet> {
        size_t operator()(const duckdb::PredSet& ps) const {
            return static_cast<size_t>(ps.GetHashCode());
        }
    };
}