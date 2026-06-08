LOAD 'lima';

CREATE TABLE food AS SELECT * FROM read_csv_auto('/home/thomas/limaduck/DCValidity/datasets/food.csv');

SELECT * FROM lima_discover('food');

