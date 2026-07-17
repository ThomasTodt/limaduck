-- systemd-run --user --scope -p MemoryMax=6G ./build/release/duckdb -unsigned -cmd ".mode csv" < testes/test_tpch_joins.sql > testes/resultados/test_tpch_joins_00000001.txt
-- GEN=ninja PROC=4 make release -j 4 -l 6

.mode csv
.maxrows 999999
INSTALL tpch;
LOAD tpch;
LOAD 'lima';

-- Limit CPU threads and memory usage to prevent freezing and OOM kills
SET threads = 1;
SET max_memory = '4GB';


-- Clean up any previous tables
DROP TABLE IF EXISTS customer;
DROP TABLE IF EXISTS lineitem;
DROP TABLE IF EXISTS nation;
DROP TABLE IF EXISTS orders;
DROP TABLE IF EXISTS part;
DROP TABLE IF EXISTS partsupp;
DROP TABLE IF EXISTS region;
DROP TABLE IF EXISTS supplier;

DROP TABLE IF EXISTS join_lineitem_orders;
DROP TABLE IF EXISTS join_lineitem_partsupp;
DROP TABLE IF EXISTS join_customer_orders;
DROP TABLE IF EXISTS join_partsupp_part;
DROP TABLE IF EXISTS join_partsupp_supplier;
DROP TABLE IF EXISTS join_customer_nation;
DROP TABLE IF EXISTS join_nation_supplier;
DROP TABLE IF EXISTS join_nation_region;
DROP TABLE IF EXISTS join_lineitem_part;
DROP TABLE IF EXISTS join_lineitem_supplier;
DROP TABLE IF EXISTS join_lineitem_part_supplier;

-- Generate TPC-H data (Scale Factor 1)
CALL dbgen(sf = 0.01);

-- Create joined physical tables (physical tables are required by lima_discover)
CREATE TABLE join_lineitem_orders AS 
SELECT * EXCLUDE (o_orderkey) FROM lineitem JOIN orders ON l_orderkey = o_orderkey;

CREATE TABLE join_lineitem_partsupp AS 
SELECT * EXCLUDE (ps_partkey, ps_suppkey) FROM lineitem JOIN partsupp ON l_partkey = ps_partkey AND l_suppkey = ps_suppkey;

CREATE TABLE join_customer_orders AS 
SELECT * EXCLUDE (o_custkey) FROM customer JOIN orders ON c_custkey = o_custkey;

CREATE TABLE join_partsupp_part AS 
SELECT * EXCLUDE (p_partkey) FROM partsupp JOIN part ON ps_partkey = p_partkey;

CREATE TABLE join_partsupp_supplier AS 
SELECT * EXCLUDE (s_suppkey) FROM partsupp JOIN supplier ON ps_suppkey = s_suppkey;

CREATE TABLE join_customer_nation AS 
SELECT * EXCLUDE (n_nationkey) FROM customer JOIN nation ON c_nationkey = n_nationkey;

-- Join nation + supplier on nationkey
CREATE TABLE join_nation_supplier AS 
SELECT * EXCLUDE (s_nationkey) FROM nation JOIN supplier ON n_nationkey = s_nationkey;

-- Join nation + region on regionkey
CREATE TABLE join_nation_region AS 
SELECT * EXCLUDE (r_regionkey) FROM nation JOIN region ON n_regionkey = r_regionkey;

CREATE TABLE join_lineitem_part AS 
SELECT * EXCLUDE (p_partkey) FROM lineitem JOIN part ON l_partkey = p_partkey;

CREATE TABLE join_lineitem_supplier AS 
SELECT * EXCLUDE (s_suppkey) FROM lineitem JOIN supplier ON l_suppkey = s_suppkey;

-- 3-way join: lineitem + part + supplier
CREATE TABLE join_lineitem_part_supplier AS 
SELECT * EXCLUDE (p_partkey, s_suppkey) FROM lineitem 
JOIN part ON l_partkey = p_partkey 
JOIN supplier ON l_suppkey = s_suppkey;

-- Run discovery on the original individual tables
SELECT '--- customer ---' AS section;
SELECT * FROM lima_discover('customer', threshold=0.00000001);

SELECT '--- lineitem ---' AS section;
SELECT * FROM lima_discover('lineitem', threshold=0.00000001);

SELECT '--- nation ---' AS section;
SELECT * FROM lima_discover('nation', threshold=0.00000001);

SELECT '--- orders ---' AS section;
SELECT * FROM lima_discover('orders', threshold=0.00000001);

SELECT '--- part ---' AS section;
SELECT * FROM lima_discover('part', threshold=0.00000001);

SELECT '--- partsupp ---' AS section;
SELECT * FROM lima_discover('partsupp', threshold=0.00000001);

SELECT '--- region ---' AS section;
SELECT * FROM lima_discover('region', threshold=0.00000001);

SELECT '--- supplier ---' AS section;
SELECT * FROM lima_discover('supplier', threshold=0.00000001);

-- Run discovery on the joined tables
SELECT '--- join_lineitem_orders ---' AS section;
SELECT * FROM lima_discover('join_lineitem_orders', threshold=0.00000001);

SELECT '--- join_lineitem_partsupp ---' AS section;
SELECT * FROM lima_discover('join_lineitem_partsupp', threshold=0.00000001);

SELECT '--- join_customer_orders ---' AS section;
SELECT * FROM lima_discover('join_customer_orders', threshold=0.00000001);

SELECT '--- join_partsupp_part ---' AS section;
SELECT * FROM lima_discover('join_partsupp_part', threshold=0.00000001);

SELECT '--- join_partsupp_supplier ---' AS section;
SELECT * FROM lima_discover('join_partsupp_supplier', threshold=0.00000001);

SELECT '--- join_customer_nation ---' AS section;
SELECT * FROM lima_discover('join_customer_nation', threshold=0.00000001);

SELECT '--- join_nation_supplier ---' AS section;
SELECT * FROM lima_discover('join_nation_supplier', threshold=0.00000001);

SELECT '--- join_nation_region ---' AS section;
SELECT * FROM lima_discover('join_nation_region', threshold=0.00000001);

SELECT '--- join_lineitem_part ---' AS section;
SELECT * FROM lima_discover('join_lineitem_part', threshold=0.00000001);

SELECT '--- join_lineitem_supplier ---' AS section;
SELECT * FROM lima_discover('join_lineitem_supplier', threshold=0.00000001);

-- Run discovery on the 3-way join
SELECT '--- join_lineitem_part_supplier ---' AS section;
SELECT * FROM lima_discover('join_lineitem_part_supplier', threshold=0.00000001);
