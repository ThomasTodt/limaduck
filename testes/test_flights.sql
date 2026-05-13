.timer on

-- INSTALL '/home/thomas/limaduck/mockdb/build/release/extension/duckteste/duckteste.duckdb_extension';
LOAD 'duckteste';

CREATE TABLE flights_20 AS SELECT * FROM read_csv_auto('/home/thomas/mestrado/DCValidity/datasets/flights.csv');

SELECT * FROM lima_discover('flights_20', num_lines=10000, threshold=0.00000001);