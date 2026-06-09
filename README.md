# LIMA Extension for DuckDB

This is a native C++ extension for DuckDB that implements the **LIMA** algorithm for automatic Denial Constraint (DC) discovery directly within the database engine.

This repository is based on https://github.com/duckdb/extension-template

## Prerequisites

*   **CMake** (3.20 or higher)
*   **C++ Compiler** (with C++11 support)
*   **Make**

---

## Compilation Guide

### 1. Clone the Repository

Since the extension depends on the DuckDB source code as a submodule, you must clone recursively:

```bash
git clone --recursive https://github.com/ThomasTodt/mockdb limaduck
cd limaduck/mockdb
```

*Alternatively, if cloned without submodules, initialize them manually:*
```bash
git submodule update --init --recursive
```

### 2. Build the Extension (Release Mode)

Build in `release` mode for maximum performance.

In the `mockdb` directory:

```bash
make release
```

The compiled extension file will be generated at:
`build/release/extension/lima/lima.duckdb_extension`

---

### Usage

The `lima` extension provides the `lima_discover` table function. 

> **IMPORTANT:** The `lima_discover` function accepts either a direct local file path (a CSV file) or the name of a standard, physical database table. It **does not work with DuckDB views**.

#### Arguments

| Argument | Type | Required? | Default Value | Description |
| :--- | :--- | :---: | :--- | :--- |
| `file_name` | `VARCHAR` | Yes (Positional) | - | The path to the dataset file to be analyzed. |
| `num_lines` | `INTEGER` | No (Named) | `-1` | Number of lines to process. Leave as `-1` to read the entire file. |
| `threshold` | `DOUBLE` | No (Named) | `0.0000001` | The threshold parameter for the scheduler's heuristic search. |

#### Example Query

```sql
-- Using a local CSV file
SELECT * FROM lima_discover('my_dataset.csv');

-- Using an existing physical database table
SELECT * FROM lima_discover('my_physical_table');

-- Advanced usage with optional named parameters
SELECT * FROM lima_discover('my_physical_table', num_lines=1000, threshold=0.00005);
```

---

## Usage Guide

### Using with an External DuckDB (Python, CLI, or GUI)

If you already have DuckDB installed (e.g., via `pip install duckdb` or a system package), you can load this extension without using the DuckDB binary built by the `make` command.

**Important: Version Matching**
DuckDB extensions are tied to specific versions. For this extension to load, your installed DuckDB version must match the version in the `mockdb/duckdb` submodule (currently **v1.5.3**).

In your DuckDB environment (CLI or Python), run:

```sql
-- 1. Enable loading of unsigned/local extensions
SET allow_unsigned_extensions=true;

-- 2. Install using the ABSOLUTE path to the .duckdb_extension file
INSTALL '/path/to/limaduck/mockdb/build/release/extension/lima/lima.duckdb_extension';

-- 3. Load the extension
LOAD 'lima';

-- 4. Run discovery
SELECT * FROM lima_discover('my_table');
```

### Using via Python Example

```python
import duckdb

# Connect and allow local extensions
con = duckdb.connect(config={"allow_unsigned_extensions": "true"})

# Path to the extension file you compiled
EXT_PATH = '/home/user/limaduck/mockdb/build/release/extension/lima/lima.duckdb_extension'

con.execute(f"INSTALL '{EXT_PATH}';")
con.execute("LOAD 'lima';")

# The extension works on both Tables and raw CSV files
results = con.execute("SELECT * FROM lima_discover('data.csv', threshold=0.01);").fetchall()
print(results)
```

---
