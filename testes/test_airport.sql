LOAD 'lima';

CREATE TABLE airport AS SELECT * FROM read_csv_auto('/home/thomas/limaduck/DCValidity/datasets/airport.csv');

SELECT * FROM lima_discover('airport');

