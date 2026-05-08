#pragma once

#include <vector>
#include <cmath>
#include <algorithm>

namespace duckdb {

/**
 * BetaDistribution modela a probabilidade de um predicado ser satisfeito.
 * Utiliza aproximações das funções digama, trigama e tetragama para cálculos de Log-Odds.
 */
class BetaDistribution {
private:
    // Constante de Euler-Mascheroni
    inline static const double yConst = 0.5772156649015328606065120900824024310421;

    // Tabelas de memorização estáticas (Lazy Initialization)
    inline static std::vector<double> y1mem = {-yConst};
    inline static std::vector<double> y2mem = {std::pow(M_PI, 2) / 6.0};
    inline static std::vector<double> y3mem = {-2.404114};

public:
    int a, b;
    double mean;
    double meanLogOdds;
    double sdLogOdds;
    double grad;

    // Aproximação da função Digama psi(n)
    static double y1(int n) {
        if (n >= 1000) return std::log(n);
        while (y1mem.size() <= (size_t)n) {
            double last = y1mem.back();
            double s = static_cast<double>(y1mem.size());
            y1mem.push_back(last + 1.0 / s);
        }
        return y1mem[n - 1];
    }

    // Aproximação da função Trigama psi'(n)
    static double y2(int n) {
        if (n >= 500) return 1.0 / n;
        while (y2mem.size() <= (size_t)n) {
            double last = y2mem.back();
            double s = static_cast<double>(y2mem.size());
            y2mem.push_back(last - (1.0 / (s * s)));
        }
        return y2mem[n - 1];
    }

    // Aproximação da função Tetragama psi''(n)
    static double y3(int n) {
        if (n >= 200) return 1.0 / (static_cast<double>(n) * n);
        while (y3mem.size() <= (size_t)n) {
            double last = y3mem.back();
            double s = static_cast<double>(y3mem.size());
            y3mem.push_back(last + 2.0 / (s * s * s));
        }
        return y3mem[n - 1];
    }

    BetaDistribution(int a_val, int b_val) : a(a_val), b(b_val) {
        updateStats();
    }

    BetaDistribution() : BetaDistribution(1, 1) {}

    /**
     * Atualiza as métricas estatísticas baseadas nos parâmetros alpha (a) e beta (b).
     */
    void updateStats() {
        double sum = static_cast<double>(a) + b;
        this->mean = static_cast<double>(a) / sum;
        
        // meanLogOdds = psi(a) - psi(a + b)
        this->meanLogOdds = y1(a) - y1(a + b); 
        
        // sdLogOdds = psi'(a) - psi'(a + b)
        this->sdLogOdds = y2(a) - y2(a + b); 

        // Cálculo do gradiente para convergência
        this->grad = static_cast<double>(b) / (a * sum * sum);
    }

    void add(int a_inc, int b_inc) {
        this->a += a_inc;
        this->b += b_inc;
        updateStats();
    }
};

} // namespace duckdb