# Changelog & Systems Engineering Guide: `cpu-optimizations` Branch

This document details the four core C++ systems optimizations implemented on the `cpu-optimizations` branch of `limaduck`, explaining from first principles what each change does in memory and CPU registers, why the original implementation was bottlenecked, and the measured single-core performance gains across the LIMA benchmark suite.

To make every optimization concrete, keep **one running example** in mind:
> Suppose our database table has **50,000 rows** of flights, and LIMA is currently working on a sample batch of **$N = 1,000,000$ random pairs of rows** (`pair #0`, `pair #1`, ..., `pair #999,999`).

---

## 1. The 1-Line `TPSubSet` Fix (Understanding C++ Constructors & Heap Allocation)

**Modified Files:** [`src/DuckDBDataset.cpp`](file:///home/thomas/Mestrado/papers/limaduck/src/DuckDBDataset.cpp#L346), [`src/include/TPSubSet.hpp`](file:///home/thomas/Mestrado/papers/limaduck/src/include/TPSubSet.hpp)

### 1.1 What is `TPSet` vs. `TPSubSet`?
In LIMA, we have two classes that represent tuple pairs:
- **`TPSet` ([`TPSet.hpp`](file:///home/thomas/Mestrado/papers/limaduck/src/include/TPSet.hpp)):** Holds the actual **root batch** of $N = 1,000,000$ randomly sampled pairs of row IDs:
  - `x = [row 14, row 802, row 5, ...]` ($1,000,000$ integers)
  - `y = [row 99, row 12,  row 41, ...]` ($1,000,000$ integers)
- **`TPSubSet` ([`TPSubSet.hpp`](file:///home/thomas/Mestrado/papers/limaduck/src/include/TPSubSet.hpp)):** Holds the **subset of pair indices** from that batch that survived a predicate filter.
  - For example, if only 3 pairs out of the $1,000,000$ had `FlightNum_x == FlightNum_y` (say, `pair #12`, `pair #504`, and `pair #9810`), then that node’s `TPSubSet` only needs to remember those **3 integers**: `[12, 504, 9810]`.
  - Every `TPSubSet` also keeps a pointer `source` pointing back to the original 1,000,000-pair `TPSet`, so it knows which batch those pair IDs belong to.

### 1.2 What the Original Code Did (And Why It Was a Bottleneck)
Look at the two constructors that existed in the original `TPSubSet.hpp`:

```cpp
// Constructor A: Takes the ROOT batch (source_ptr)
TPSubSet(std::shared_ptr<TPSet> source_ptr) : source(source_ptr) {
    this->length = source_ptr->length;       // e.g., 1,000,000
    this->TPs.resize(length);                // Allocates 1,000,000 ints (4 MB) in RAM!
    for (int32_t i = 0; i < length; i++) {   // Writes 0, 1, 2, ..., 999,999 into RAM!
        this->TPs[i] = i;
    }
}

// Constructor B: Takes the PARENT subset (other)
TPSubSet(const TPSubSet &other) : source(other.source) {
    this->length = 0;
    this->TPs.resize(other.TPs.size());      // Allocates ONLY the parent's surviving size (e.g., 3 ints)!
}
```

Now look at what `DuckDBDataset::filter(TPSubSet &TPs, Predicate *pred)` did on **every single edge** of the lattice search:
- Suppose we are at Level 2 of the tree. The parent node `TPs` is `"Same FlightNum"`, which only has **3 surviving pairs** (`TPs.TPs = [12, 504, 9810]`).
- We want to filter those **3 pairs** by `"Same Carrier"`. Since we start with 3 pairs, the output can have **at most 3 pairs**!
- Yet line 215 of `DuckDBDataset.cpp` had this bug (ported from Java `CSVDataset.java:135`):
  ```cpp
  // BUG: Passed `TPs.source` (the 1,000,000-pair root TPSet) instead of `TPs` (the 3-pair parent)!
  auto newTPSubset = make_uniq<TPSubSet>(TPs.source);
  ```
- Because it passed `TPs.source`, C++ called **Constructor A** instead of **Constructor B**!
- **What happened inside the computer?**
  1. Just to check **3 pairs**, C++ asked the operating system for space for **1,000,000 integers** (or in Batch 19, **52,428,800 integers = 209 Megabytes**!).
  2. Then it ran a `for` loop writing `0, 1, 2, ..., 52,428,799` into that 209 MB array.
  3. Then it checked the 3 pairs, overwrote index `0` with the 1 matching pair, and immediately threw away the other 52,428,799 integers (`newTPSubset->resize(1)`)!
  4. And it did this **146,000 times** during the DFS search (**58% to 87% of total runtime**)!

### 1.3 How the Fix Works
By changing that single argument from `TPs.source` to `TPs` ([`DuckDBDataset.cpp:L346`](file:///home/thomas/Mestrado/papers/limaduck/src/DuckDBDataset.cpp#L346)):
```cpp
auto newTPSubset = make_uniq<TPSubSet>(TPs);
```
C++ now calls **Constructor B**. When a sparse parent has 3 surviving pairs, it allocates space for **3 integers (12 bytes)** instead of **52 million integers (209 MB)**, and skips the `0..N-1` initialization loop completely. Furthermore, on the root Constructor A (`TPSubSet(std::shared_ptr<TPSet> source_ptr)`), we set `is_full = true` so Level 1 does not allocate or fill the redundant `0..N-1` vector either.

---

## 2. CPU 64-Bit `uint64_t` Bitvectors (Doing 64 Comparisons in 1 CPU Instruction)

**Modified Files:** [`src/include/TPSet.hpp`](file:///home/thomas/Mestrado/papers/limaduck/src/include/TPSet.hpp), [`src/include/TPSubSet.hpp`](file:///home/thomas/Mestrado/papers/limaduck/src/include/TPSubSet.hpp), [`src/DuckDBDataset.cpp`](file:///home/thomas/Mestrado/papers/limaduck/src/DuckDBDataset.cpp#L170-L445)

Even after fixing the bug in Section 1, wide tables like `flights.csv` (20 columns, 88 predicates) still spent **73% to 86% of their runtime** in DFS predicate comparisons. Here is why—and how 64-bit bitvectors eliminate that bottleneck.

### 2.1 The "List of Integer IDs" Problem
Suppose our batch has $N = 64$ sampled tuple pairs (`pair #0` through `pair #63`), and:
- Predicate $P_1$ (`Origin == Origin`) is satisfied by **20 pairs**:
  $$\text{List for } P_1 = [0, 2, 3, 7, 10, 11, 15, \dots] \quad (\text{20 integers, } 20 \times 4 = 80\text{ bytes})$$
- Now in the DFS search, we want to test the 2-predicate candidate $\{P_1, P_2\}$ (`Origin == Origin` AND `State == State`), and then $\{P_1, P_3\}$, $\{P_1, P_4\}$, ..., $\{P_1, P_{88}\}$.
- In the old code, for every single branch $\{P_1, P_k\}$, `filter()` looped over the 20 pair IDs in $P_1$'s list, looked up which two database rows `tpx[tp]` and `tpy[tp]` belonged to that pair, jumped out to the 500,000-row database column in RAM, and compared `State[row_x] == State[row_y]`.
- Jumping to random positions in a 500,000-row array causes a **CPU Cache Miss** (the CPU stalls for ~14 nanoseconds waiting for RAM).

### 2.2 The Bitvector Idea: 1 Bit per Tuple Pair
For any predicate $P$, a tuple pair either satisfies $P$ (`true = 1`) or does not satisfy $P$ (`false = 0`).
Why store a 32-bit integer (`4 bytes = 32 bits`) for every pair that satisfies $P$, when a single **`1` or `0` bit** is enough?

In C++, the type `uint64_t` is an **unsigned 64-bit integer**. It fits inside **one single CPU register** and has **64 individual bits** (numbered bit 0 to bit 63):

```text
Pair index:   63 62 61 ...  7  6  5  4  3  2  1  0
              -------------------------------------
B_P1 (64b): [  0  1  0 ...  1  0  0  0  1  1  0  1 ]   <-- One single uint64_t variable!
B_P2 (64b): [  0  1  1 ...  1  0  1  0  0  1  0  1 ]   <-- One single uint64_t variable!
```

When we store each predicate's result on the sampled tuple pairs as a vector of `uint64_t` words (`pred_bits` in [`TPSet.hpp:L19`](file:///home/thomas/Mestrado/papers/limaduck/src/include/TPSet.hpp#L19)), we unlock three hardware superpowers:

#### Superpower A: Intersecting 64 Tuple Pairs in 1 Clock Cycle (`&` and `__builtin_popcountll`)
How do we find which tuple pairs satisfy **BOTH** $P_1$ AND $P_2$?
In C++, the bitwise `AND` operator (`&`) compares **all 64 bits of two `uint64_t` variables in parallel in 1 single CPU clock cycle (~0.25 nanoseconds)**:

```cpp
uint64_t m = B_P1 & B_P2; // Intersects 64 tuple pairs at once!
```
```text
B_P1:         [ 0  1  0 ... 1  0  0  0  1  1  0  1 ]
B_P2:       & [ 0  1  1 ... 1  0  1  0  0  1  0  1 ]
              --------------------------------------
m = B_P1&B_P2:[ 0  1  0 ... 1  0  0  0  0  1  0  1 ]  <-- 1 CPU instruction!
```
And how do we count how many pairs survived (how many `1` bits are in `m`)?
Modern x86 CPUs have a dedicated hardware instruction called `POPCNT` (Population Count), accessed in GCC/Clang as `__builtin_popcountll(m)`, which counts the number of `1` bits inside a 64-bit integer in **1 clock cycle**:
```cpp
newLength += __builtin_popcountll(m); // Counts all surviving pairs out of 64 in 1 instruction!
```
During the DFS search, we **never look up `tpx[tp]`, `tpy[tp]`, or the database columns `intData`/`stringData` again**!

#### Superpower B: Getting Half of the Level-1 Predicates for Free with Bitwise `NOT` (`~`)
In Level 1, for every integer column $A$, LIMA tests 6 predicates:
- `==` (`EQ`) and `!=` (`NE`)
- `>` (`GT`) and `<=` (`LE`)
- `<` (`LT`) and `>=` (`GE`)

If `t_x.A == t_y.A` is `1` (true), then `t_x.A != t_y.A` is guaranteed to be `0` (false), and vice versa!
In the old code, `limaduck` scanned the entire batch of tuple pairs 6 separate times—once for `==`, once for `!=`, once for `>`, once for `<=`, etc.
With 64-bit bitvectors ([`DuckDBDataset.cpp:L192-L237`](file:///home/thomas/Mestrado/papers/limaduck/src/DuckDBDataset.cpp#L192-L237)):
- Once we compute the bitvector for `==` (`EQ`), we get the bitvector for `!=` (`NE`) by simply flipping all 64 bits at once using the C++ bitwise `NOT` operator (`~`):
  ```cpp
  bv_NE[w] = ~bv_EQ[w]; // 64 tuple pairs inverted in 1 clock cycle!
  ```
- Same for `LE = ~GT` and `GE = ~LT`! That cuts Level-1 column scans in half.

#### Superpower C: Adaptive Slicing & Sparse Transition (`__builtin_ctzll`)
What if a batch has $N = 640,000$ pairs ($10,000$ `uint64_t` words), and deep down in the DFS tree (say at Level 4), a candidate node only has **2 surviving pairs** (only two `1` bits out of $640,000$ bits)?
If we kept scanning all $10,000$ `uint64_t` words just to find two `1` bits, we would waste time scanning $9,998$ words that are `0`.
So in [`DuckDBDataset.cpp:L368-L445`](file:///home/thomas/Mestrado/papers/limaduck/src/DuckDBDataset.cpp#L368-L445), we do two smart things:
1. **Active Word Slicing (`[min_word, max_word]`):** We only store and scan the words between the first non-zero 64-bit word (`min_word`) and the last non-zero 64-bit word (`max_word`).
2. **Sparse Transition (`newLength < new_span`):** As soon as there is less than **1 surviving pair per 64-bit word on average**, we extract the exact pair indices of those few surviving `1` bits into the compact list `TPs` using `__builtin_ctzll(m)` ("Count Trailing Zeros", a 1-cycle CPU instruction that tells you the position `0..63` of the lowest `1` bit in `m`):
   ```cpp
   while (m) {
       int bit = __builtin_ctzll(m);          // Finds the index (0..63) of the next '1' bit in 1 cycle!
       newTPSubset->TPs[idx++] = base_tp + bit;
       m &= (m - 1);                          // Clears that '1' bit so the next iteration finds the next one
   }
   ```
   And when filtering those few surviving indices `tp`, we still don't touch the database columns—we just check bit `tp` inside the target predicate's cached bitvector:
   ```cpp
   if ((e_bits[tp >> 6] >> (tp & 63)) & 1ULL) // tp >> 6 is (tp / 64), tp & 63 is (tp % 64)
   ```

---

## 3. `Xoshiro256++` & Lemire's Multiply-Shift (Fast Random Sampling)

**Modified Files:** [`src/include/DuckDBDataset.hpp`](file:///home/thomas/Mestrado/papers/limaduck/src/include/DuckDBDataset.hpp#L16-L49), [`src/DuckDBDataset.cpp`](file:///home/thomas/Mestrado/papers/limaduck/src/DuckDBDataset.cpp#L547-L564)

### 3.1 Why `std::mt19937` + `std::uniform_int_distribution` Took 35%–59% of Runtime
In Level 1, LIMA samples millions of random row pairs `(x, y)` where $0 \le x < \text{size}$ and $0 \le y < \text{size}$ ($x \neq y$).
The original code in `DuckDBDataset::sample()` used C++'s standard Mersenne Twister (`std::mt19937`) and `std::uniform_int_distribution<int32_t>`:
```cpp
std::uniform_int_distribution<int32_t> dist_x(0, this->size - 1);
std::uniform_int_distribution<int32_t> dist_y(0, this->size - 2);
for (int i = 0; i < n; i++) {
    int32_t x = dist_x(random_engine);
    int32_t y = dist_y(random_engine);
    ...
}
```
Why is this slow in C++?
1. **`std::mt19937` has a large state:** It maintains an array of **624 integers (2.5 KB)** of state and does complex bit-twisting across that array.
2. **Two PRNG calls per pair:** It called the generator at least twice per pair (once for `x`, once for `y`).
3. **Integer Division inside `uniform_int_distribution`:** To turn a random 32-bit number into a number in `[0, size - 1]` without bias, `std::uniform_int_distribution` performs **integer division / modulo (`%`)** and rejection loops. On a modern CPU, a 64-bit addition or multiplication takes **1–3 clock cycles**, whereas an integer division (`div`) takes **20 to 40 clock cycles**!

### 3.2 How `Xoshiro256++` + Lemire's Multiply-Shift Works
In [`DuckDBDataset.hpp:L16-L49`](file:///home/thomas/Mestrado/papers/limaduck/src/include/DuckDBDataset.hpp#L16-L49) and [`DuckDBDataset.cpp:L547-L564`](file:///home/thomas/Mestrado/papers/limaduck/src/DuckDBDataset.cpp#L547-L564), we replaced that with two modern techniques:

1. **`Xoshiro256++` (by Blackman & Vigna):**
   - Its entire state is just **four `uint64_t` variables (`s[0], s[1], s[2], s[3]` = 32 bytes)**, which stay permanently inside **4 CPU registers**!
   - Generating a 64-bit random number `uint64_t r = random_engine.next()` takes only **a few register `XOR`, shift, and rotate instructions (~1 nanosecond)**.
2. **Two 32-Bit Random Numbers from One 64-Bit Draw + Lemire's Division-Free Range Trick:**
   - Since `r` has **64 random bits**, we split `r` into two independent 32-bit random numbers:
     - `low32  = static_cast<uint32_t>(r)` (lower 32 bits, a number in $[0, 2^{32}-1]$)
     - `high32 = static_cast<uint32_t>(r >> 32)` (upper 32 bits, a number in $[0, 2^{32}-1]$)
   - To map a 32-bit random number $u \in [0, 2^{32}-1]$ into the range $[0, \text{size}-1]$ **without using division (`%`)**, think of $\frac{u}{2^{32}}$ as a random fraction in $[0, 1)$. Multiplying that fraction by `size` gives a uniform integer in $[0, \text{size})$:
     $$x = \left\lfloor \frac{u \times \text{size}}{2^{32}} \right\rfloor = \texttt{(uint64\_t(u) * uint64\_t(size)) >> 32}$$
   - Multiplying two 32-bit numbers as 64-bit integers and shifting right by 32 bits (`>> 32` is dividing by $2^{32}$) uses **zero division instructions**—just 1 multiply (`MUL`) and 1 shift (`SHR`)!

---

## 4. Flat `PredSet` (Eliminating `std::unordered_map` & Vector Heap Copies in the Lattice)

**Modified File:** [`src/include/PredSet.hpp`](file:///home/thomas/Mestrado/papers/limaduck/src/include/PredSet.hpp)

### 4.1 What is `PredSet`?
In LIMA's lattice, every node represents a set of predicates (a candidate DC), like:
$$\varphi = \{\text{ColumnPair } 2 \text{ has Operator } 0, \quad \text{ColumnPair } 7 \text{ has Operator } 1\}$$
In the code, that set of `(ColumnPair_ID, Operator_ID)` pairs is stored in `PredSet` ([`PredSet.hpp`](file:///home/thomas/Mestrado/papers/limaduck/src/include/PredSet.hpp)).

During the lattice search (`exploreNode`, `fetchTo`, `fetchFrom` in [`Lattice.hpp`](file:///home/thomas/Mestrado/papers/limaduck/src/include/Lattice.hpp#L185-L215)), LIMA constantly asks questions like:
- *"If I take node `n`'s `PredSet` and add `(cp = 9, p = 0)`, does that child node already exist in the lattice?"*
- *"If I take node `n`'s `PredSet` and remove one predicate, was that parent subset sound?"*

To ask those questions, `Lattice.hpp` makes a **temporary copy** of `n->preds`:
```cpp
PredSet newPreds(n->preds); // Copy constructor called hundreds of thousands of times!
newPreds.Add(cp, p);
Node *toNode = fetchNode(newPreds);
```

### 4.2 Why the Original `PredSet` Was Slow
Look at the member variables inside the original `PredSet`:
```cpp
class PredSet {
private:
    std::unordered_map<int32_t, int32_t> preds; // Hash table on the heap!
    int32_t hash_val;
    std::vector<int32_t> predProd;              // Vector of all column multipliers on the heap!
};
```
In C++, every single time `PredSet newPreds(n->preds);` was executed:
1. **It copied `std::unordered_map<int32_t, int32_t> preds`:**
   In C++, `std::unordered_map` is a **chained hash table** where every single `(key, value)` pair is a separately allocated node on the heap connected by pointers. Even if a candidate DC only had **3 predicates**, copying `std::unordered_map` allocated a bucket array on the heap plus 3 separate heap nodes (`malloc` / `new`), and destroying `newPreds` a microsecond later called `free` / `delete` 4 times.
2. **It copied `std::vector<int32_t> predProd`:**
   `predProd` is a constant table of hash multipliers (one integer per column in the database) created once when the lattice starts. It **never changes**. Yet the original `PredSet` stored a full copy of `std::vector<int32_t> predProd` inside *every single `PredSet`*, allocating and copying that vector on the heap every time a `PredSet` was copied.

At $\epsilon = 10^{-2}$ and $10^{-4}$, **65% to 80% of the entire program's runtime** was spent just allocating and freeing `PredSet` copies!

### 4.3 How "Flat `PredSet`" Fixes All Three Issues
In [`PredSet.hpp`](file:///home/thomas/Mestrado/papers/limaduck/src/include/PredSet.hpp), `PredSet` is redesigned around three C++ systems principles:

```text
OLD PredSet (Heap Pointers Everywhere):
[ PredSet ] ---> heap bucket array ---> heap node (2, 0) ---> heap node (7, 1)
            ---> heap vector copy of predProd [1, 7, 49, 343, ...]

NEW Flat PredSet (100% Inside the Object — Zero Heap Allocations!):
[ inline_preds_: [(2, 0), (7, 1), ... up to 16 pairs] | size_: 2 | cp_mask_: 0b10000100 | predProd_: *ptr ]
```

1. **Inline Fixed Array (`Entry inline_preds_[16]`):**
   - A Denial Constraint in LIMA almost never has more than 16 predicates in a single rule.
   - By storing `std::pair<int32_t, int32_t> inline_preds_[16]` directly inside the `PredSet` struct (and keeping the active `size_` pairs sorted by `cp`), copying a `PredSet` with 3 predicates just copies 3 pairs of integers on the CPU stack—**zero heap allocations (`new`/`delete`)**! *(If a DC ever exceeds 16 predicates, it automatically falls back to `overflow_preds_`).*
2. **Pointer to Shared `predProd` (`const std::vector<int32_t> *predProd_`):**
   - Instead of copying the `predProd` vector into every `PredSet`, we store an 8-byte pointer (`&predProd_ref`) pointing to the single `predProd` vector owned by the `Lattice`.
3. **64-Bit Presence Bitmask (`uint64_t cp_mask_`):**
   - Whenever column pair `cp` (say `cp = 7`) is added to `PredSet`, we set bit #7 of `cp_mask_` to `1`:
     ```cpp
     cp_mask_ |= (1ULL << cp);
     ```
   - Whenever LIMA asks `node->preds.Contains(cp)` ([`Scheduler.cpp:L225`](file:///home/thomas/Mestrado/papers/limaduck/src/Scheduler.cpp#L225)), instead of hashing `cp` and chasing pointers in an `unordered_map`, the CPU checks if bit `cp` of `cp_mask_` is `1`:
     ```cpp
     return ((cp_mask_ >> cp) & 1ULL) != 0; // 1 single CPU instruction!
     ```
   - And when comparing two `PredSet`s for equality (`operator==`), we first check `if (hash_val_ != other.hash_val_ || size_ != other.size_ || cp_mask_ != other.cp_mask_) return false;`—rejecting non-matching sets in a single instruction.

---

## 5. Summary Table & Measured Single-Core Speedups

| Optimization | Old C++ Construct | New C++ Construct | Why the CPU Loves It |
| :--- | :--- | :--- | :--- |
| **1. `TPSubSet(TPs)` Fix** | `TPSubSet(TPs.source)` (allocated $N \le 10^8$ ints on every DFS edge) | `TPSubSet(TPs)` (sizes to parent subset, no $0..N-1$ fill loop) | Eliminates **47× to 481×** wasted RAM allocation traffic. |
| **2. 64-Bit `uint64_t` Bitvectors** | `std::vector<int32_t>` list of pair IDs + random column lookups in DFS | `std::vector<uint64_t>` bitmasks (`1` bit/pair) + `&`, `~`, `__builtin_popcountll`, `__builtin_ctzll` | Compares **64 tuple pairs in 1 CPU cycle** and eliminates L3 cache misses during DFS. |
| **3. `Xoshiro256++` + Multiply-Shift** | `std::mt19937` (2.5 KB state) + 2× `std::uniform_int_distribution` (integer division) | `Xoshiro256++` (4 registers) + `(uint64_t(u) * size) >> 32` | Generates both `x` and `y` from **one 64-bit random draw** with **zero division instructions**. |
| **4. Flat `PredSet`** | `std::unordered_map` + deep-copied `std::vector<int32_t> predProd` | Inline sorted `inline_preds_[16]` + `uint64_t cp_mask_` + `const vector*` pointer | **Zero heap allocations** on copy; 1-cycle `Contains(cp)` bit test. |

### Measured Single-Core Benchmark Results (`taskset -c 0`)

| Dataset & Setting | Unmodified `limaduck` (`BASE`) | 1-Line Fix Only (`FIX`) | **All 4 Fixes (`cpu-optimizations`)** | **Total Speedup vs. `BASE`** | **Speedup vs. `FIX`** |
| :--- | ---: | ---: | ---: | :---: | :---: |
| **`flights.csv` (499,308 rows, $\epsilon = 10^{-8}$)** | `12,990.3 ms` | `4,804.4 ms` | **`760.2 ms`** | **$17.1\times$** | **$6.3\times$** |
| **`flights.csv` (499,308 rows, $\epsilon = 10^{-4}$)** | `260.5 ms` | `191.4 ms` | **`126.4 ms`** | **$2.1\times$** | **$1.5\times$** |
| **`flights.csv` (499,308 rows, $\epsilon = 10^{-2}$)** | `103.1 ms` | `101.8 ms` | **`69.3 ms`** | **$1.5\times$** | **$1.5\times$** |
| **`flights.csv` (32,768 rows, $\epsilon = 10^{-8}$)** | `10,079.4 ms` | `2,163.9 ms` | **`606.5 ms`** | **$16.6\times$** | **$3.6\times$** |
| **`ncvoter.csv` (32,768 rows, $\epsilon = 10^{-8}$)** | `34,559.3 ms` | `1,752.3 ms` | **`589.8 ms`** | **$58.6\times$** | **$3.0\times$** |
| **`tax500k.csv` (32,768 rows, $\epsilon = 10^{-8}$)** | `17,744.2 ms` | `1,163.1 ms` | **`347.9 ms`** | **$51.0\times$** | **$3.3\times$** |
| **`Hospital.csv` (32,768 rows, $\epsilon = 10^{-8}$)** | `2,223.1 ms` | `293.1 ms` | **`119.7 ms`** | **$18.6\times$** | **$2.4\times$** |
| **`airport.csv` (32,768 rows, $\epsilon = 10^{-8}$)** | `9,043.8 ms` | `1,411.4 ms` | **`533.3 ms`** | **$17.0\times$** | **$2.6\times$** |

---

## 6. Constant Denial Constraint Discovery Mode (`C-LIMA`: 1-Constant, 2-Constant Conjunctive & Bipartite Cross-Tuple Constants)

**Modified Files:** [`src/include/RelationalDataset.hpp`](file:///home/thomas/Mestrado/papers/limaduck/src/include/RelationalDataset.hpp), [`src/include/DuckDBDataset.hpp`](file:///home/thomas/Mestrado/papers/limaduck/src/include/DuckDBDataset.hpp), [`src/DuckDBDataset.cpp`](file:///home/thomas/Mestrado/papers/limaduck/src/DuckDBDataset.cpp#L547-L725), [`src/include/Scheduler.hpp`](file:///home/thomas/Mestrado/papers/limaduck/src/include/Scheduler.hpp), [`src/Scheduler.cpp`](file:///home/thomas/Mestrado/papers/limaduck/src/Scheduler.cpp), [`src/lima_extension.cpp`](file:///home/thomas/Mestrado/papers/limaduck/src/lima_extension.cpp)

### 6.1 SQL / DuckDB Extension Usage
`lima_discover` now supports two optional named parameters for Constant DC discovery:
- `constant_dcs` (`BOOLEAN`, default `false`): Enables the `C-LIMA` discovery pipeline (1-Constant, 2-Constant Conjunctive, and Bipartite Cross-Tuple Constant DCs).
- `min_support` (`DOUBLE`, default `0.01`): Minimum row support threshold $\tau \in (0, 1)$ required for a constant cluster to be evaluated.

```sql
-- Standard Variable-DC LIMA (default):
SELECT * FROM lima_discover('Hospital.csv', threshold=1e-6);

-- Constant-DC LIMA (C-LIMA) with 5% minimum support:
SELECT * FROM lima_discover('Hospital.csv', threshold=1e-6, constant_dcs=true, min_support=0.05);
```

### 6.2 The Three Classes of Constant Clusters Built in `buildConstantClusters`
1. **1-Constant Homo-Clusters ($d_C = 1$):**
   $$t_x.A = c \ \wedge \ t_y.A = c$$
   Groups row IDs by value $c$ for each non-constant `STRING` and `INTEGER` column, keeping clusters with row count $\max(2, \lceil \tau \cdot |D| \rceil) \le k < |D|$.
2. **2-Constant Conjunctive Homo-Clusters ($d_C = 2$, Distinct Columns $A \neq B$):**
   $$t_x.A = c_1 \ \wedge \ t_y.A = c_1 \ \wedge \ t_x.B = c_2 \ \wedge \ t_y.B = c_2$$
   Because each 1-constant cluster's `row_ids` vector is sorted in ascending order (`0 .. n-1`), 2-constant clusters are constructed in $O(|C_1| + |C_2|)$ time via `std::set_intersection`. Intersections are kept only if they satisfy $\ge \lceil \tau \cdot |D| \rceil$ rows **and** are strictly smaller than both parent clusters ($|C_1 \cap C_2| < \min(|C_1|, |C_2|)$, skipping trivial aliases where one constant deterministically implies the other).
3. **Bipartite Cross-Tuple Constant Clusters ($t_1.A = c_1 \ \wedge \ t_2.A = c_2$, $c_1 \neq c_2$ on the Same Column $A$):**
   For pairs of frequent constants $(c_1, c_2)$ on the same column $A$ whose geometric-mean support satisfies $\sqrt{|C_1| \cdot |C_2|} \ge \lceil \tau \cdot |D| \rceil$, `activeCluster` points to $C_1$'s rows (`row_ids`) and `activeClusterY` points to $C_2$'s rows (`row_ids_y`). `DuckDBDataset::sample(n)` draws `t_x` uniformly from $C_1$ and `t_y` uniformly from $C_2$ via `Xoshiro256++`, enabling discovery of asymmetric ordering and partitioning rules across distinct constant groups.

### 6.3 Cross-Cluster Statistical Soundness (`normDist >= 2.0`) & Hierarchical Minimality
To prevent multi-constant rules from overfitting small slices of the data, `Scheduler.cpp` extends LIMA's own Bayesian Beta log-odds soundness motor across the constant cluster hierarchy:
- **Reference Bitvector Summary (`ContextSummary::ref_tpset`):** Each context (the Global Table and each 1-Constant parent cluster) retains a reference `TPSet` batch along with its cached 64-bit `uint64_t` predicate bitvectors.
- **Cross-Cluster Beta Log-Odds Soundness (`isSoundAgainstParentContext` & `isL1SoundAgainstParentContext`):**
  Whenever a candidate variable rule $\varphi_V$ achieves `0` sample violations inside a child constant cluster $C$ (`upper = child_dist`), `Scheduler` computes the exact violation count $(a_{\text{parent}}, b_{\text{parent}})$ of $\varphi_V$ on each parent context's `ref_tpset` by bitwise-`AND`ing the 64-bit bitvectors of the predicates in $\varphi_V$, forms `BetaDistribution lower(1 + a_parent, 1 + b_parent)`, and checks LIMA's log-odds distance:
  $$\text{normDist}(\text{lower}_{\text{parent}}, \ \text{upper}_{\text{child}}) = \frac{\mu_{\text{parent}} - \mu_{\text{child}}}{\sqrt{\sigma_{\text{parent}}^2 + \sigma_{\text{child}}^2}} \ge 2.0$$
  - A **1-Constant** or **Bipartite** DC must be statistically sound (`normDist >= 2.0`) and non-subsumed against the **Global Table**.
  - A **2-Constant Conjunctive** DC ($C_1 \cap C_2$) must be statistically sound (`normDist >= 2.0`) and non-subsumed against the **Global Table**, **Parent Cluster $C_1$**, **and Parent Cluster $C_2$**. If adding the second constant does not produce a $>2\sigma$ drop in violation log-odds compared to the single-constant parents, LIMA rejects it as unsound/overfitted.

### 6.4 Examples of Discovered 1-Constant, 2-Constant, and Bipartite DCs
- **Bipartite Cross-Tuple Constant DCs (`tax500k.csv` & `Hospital.csv`):**
  - `!(t1.MaritalStatus(String) == 'S' & t2.MaritalStatus(String) == 'M' & SingleExemp(Integer) < SingleExemp(Integer) & )`
  - `!(t1.MaritalStatus(String) == 'S' & t2.MaritalStatus(String) == 'M' & MarriedExemp(Integer) > MarriedExemp(Integer) & )`
  - `!(t1.HasChild(String) == 'Y' & t2.HasChild(String) == 'N' & ChildExemp(Integer) < ChildExemp(Integer) & )`
  - `!(t1.Condition(String) == 'Surgical Infection Prevention' & t2.Condition(String) == 'Heart Attack' & Measure Code(String) == Measure Code(String) & )`
- **2-Constant Conjunctive DCs (`Hospital.csv`):**
  - `!(Emergency Service(String) == 'Yes' & State(String) == 'GA' & County Name(String) == County Name(String) & Hospital Type(String) != Hospital Type(String) & )`
  - `!(Hospital Type(String) == 'Acute Care Hospitals' & Hospital Owner(String) == 'Voluntary non-profit - Other' & City(String) == City(String) & County Name(String) != County Name(String) & )`
- **1-Constant DCs (`flights.csv`):**
  - `!(Month(Integer) == 12 & DayofMonth(Integer) == DayofMonth(Integer) & FlightDate(String) != FlightDate(String) & )`
