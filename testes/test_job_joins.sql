.mode csv
.maxrows 999999

.maxrows 999999
LOAD 'lima';

-- Limit CPU threads and memory usage to prevent freezing and OOM kills
SET threads = 1;
SET max_memory = '4GB';

-- Clean up any existing tables to avoid duplicate definitions
DROP TABLE IF EXISTS aka_name;
DROP TABLE IF EXISTS aka_title;
DROP TABLE IF EXISTS cast_info;
DROP TABLE IF EXISTS char_name;
DROP TABLE IF EXISTS comp_cast_type;
DROP TABLE IF EXISTS company_name;
DROP TABLE IF EXISTS company_type;
DROP TABLE IF EXISTS complete_cast;
DROP TABLE IF EXISTS info_type;
DROP TABLE IF EXISTS keyword;
DROP TABLE IF EXISTS kind_type;
DROP TABLE IF EXISTS link_type;
DROP TABLE IF EXISTS movie_companies;
DROP TABLE IF EXISTS movie_info;
DROP TABLE IF EXISTS movie_info_idx;
DROP TABLE IF EXISTS movie_keyword;
DROP TABLE IF EXISTS movie_link;
DROP TABLE IF EXISTS name;
DROP TABLE IF EXISTS person_info;
DROP TABLE IF EXISTS role_type;
DROP TABLE IF EXISTS title;

DROP TABLE IF EXISTS join_char_name_cast_info;
DROP TABLE IF EXISTS join_role_type_cast_info;
DROP TABLE IF EXISTS join_cast_info_name;
DROP TABLE IF EXISTS join_cast_info_title;
DROP TABLE IF EXISTS join_name_aka_name;
DROP TABLE IF EXISTS join_name_person_info;
DROP TABLE IF EXISTS join_person_info_info_type;
DROP TABLE IF EXISTS join_info_type_movie_info;
DROP TABLE IF EXISTS join_movie_info_title;
DROP TABLE IF EXISTS join_movie_info_idx_info_type;
DROP TABLE IF EXISTS join_movie_info_idx_title;
DROP TABLE IF EXISTS join_kind_type_title;
DROP TABLE IF EXISTS join_title_movie_keyword;
DROP TABLE IF EXISTS join_movie_keyword_keyword;
DROP TABLE IF EXISTS join_company_name_movie_companies;
DROP TABLE IF EXISTS join_movie_companies_company_type;
DROP TABLE IF EXISTS join_movie_companies_title;
DROP TABLE IF EXISTS join_complete_cast_comp_cast_type;
DROP TABLE IF EXISTS join_movie_link_link_type;
DROP TABLE IF EXISTS join_movie_link_title;
DROP TABLE IF EXISTS join_aka_title_title;

-- 1. Create the IMDB schema using the existing sql definition
.read /home/thomas/mestrado/JOB/imdb/schematext.sql

-- 2. Load subset of data (to keep the test execution fast and memory-safe)
-- RATIONALE FOR 20k ROW LIMIT:
-- LIMA has O(N log N) to O(N^2) complexity. The JOB dataset is massive (cast_info.csv is 1.4GB, containing 36M rows).
-- Loading the full files and discovering DCs on millions of rows would cause OOM memory crashes on most PCs.
-- To change the data limit, simply increase or decrease the LIMIT values below.

INSERT INTO comp_cast_type SELECT * FROM read_csv('/home/thomas/mestrado/JOB/imdb/comp_cast_type.csv', header=false, delim=',', quote='"', escape='\');
INSERT INTO company_type SELECT * FROM read_csv('/home/thomas/mestrado/JOB/imdb/company_type.csv', header=false, delim=',', quote='"', escape='\');
INSERT INTO info_type SELECT * FROM read_csv('/home/thomas/mestrado/JOB/imdb/info_type.csv', header=false, delim=',', quote='"', escape='\');
INSERT INTO kind_type SELECT * FROM read_csv('/home/thomas/mestrado/JOB/imdb/kind_type.csv', header=false, delim=',', quote='"', escape='\');
INSERT INTO link_type SELECT * FROM read_csv('/home/thomas/mestrado/JOB/imdb/link_type.csv', header=false, delim=',', quote='"', escape='\');
INSERT INTO role_type SELECT * FROM read_csv('/home/thomas/mestrado/JOB/imdb/role_type.csv', header=false, delim=',', quote='"', escape='\');

INSERT INTO aka_name SELECT * FROM read_csv('/home/thomas/mestrado/JOB/imdb/aka_name.csv', header=false, delim=',', auto_detect=true) LIMIT 20000;
INSERT INTO aka_title SELECT * FROM read_csv('/home/thomas/mestrado/JOB/imdb/aka_title.csv', header=false, delim=',', auto_detect=true) LIMIT 20000;
INSERT INTO cast_info SELECT * FROM read_csv('/home/thomas/mestrado/JOB/imdb/cast_info.csv', header=false, delim=',', auto_detect=true) LIMIT 20000;
INSERT INTO char_name SELECT * FROM read_csv('/home/thomas/mestrado/JOB/imdb/char_name.csv', header=false, delim=',', auto_detect=true) LIMIT 20000;
INSERT INTO company_name SELECT * FROM read_csv('/home/thomas/mestrado/JOB/imdb/company_name.csv', header=false, delim=',', auto_detect=true) LIMIT 20000;
INSERT INTO complete_cast SELECT * FROM read_csv('/home/thomas/mestrado/JOB/imdb/complete_cast.csv', header=false, delim=',', auto_detect=true) LIMIT 20000;
INSERT INTO keyword SELECT * FROM read_csv('/home/thomas/mestrado/JOB/imdb/keyword.csv', header=false, delim=',', auto_detect=true) LIMIT 20000;
INSERT INTO movie_companies SELECT * FROM read_csv('/home/thomas/mestrado/JOB/imdb/movie_companies.csv', header=false, delim=',', auto_detect=true) LIMIT 20000;
INSERT INTO movie_info SELECT * FROM read_csv('/home/thomas/mestrado/JOB/imdb/movie_info.csv', header=false, delim=',', auto_detect=true) LIMIT 20000;
INSERT INTO movie_info_idx SELECT * FROM read_csv('/home/thomas/mestrado/JOB/imdb/movie_info_idx.csv', header=false, delim=',', auto_detect=true) LIMIT 20000;
INSERT INTO movie_keyword SELECT * FROM read_csv('/home/thomas/mestrado/JOB/imdb/movie_keyword.csv', header=false, delim=',', auto_detect=true) LIMIT 20000;
INSERT INTO movie_link SELECT * FROM read_csv('/home/thomas/mestrado/JOB/imdb/movie_link.csv', header=false, delim=',', auto_detect=true) LIMIT 20000;
INSERT INTO name SELECT * FROM read_csv('/home/thomas/mestrado/JOB/imdb/name.csv', header=false, delim=',', auto_detect=true) LIMIT 20000;
INSERT INTO person_info SELECT * FROM read_csv('/home/thomas/mestrado/JOB/imdb/person_info.csv', header=false, delim=',', auto_detect=true) LIMIT 20000;
INSERT INTO title SELECT * FROM read_csv('/home/thomas/mestrado/JOB/imdb/title.csv', header=false, delim=',', auto_detect=true) LIMIT 20000;

-- 3. Create Joined Tables
-- Columns are explicitly projected and prefixed to avoid duplicate column name errors.

-- 1) char_name + cast_info (person_role_id)
-- OPINION: Useful. Connects character name strings to roles. Since character details (names, phonetic codes)
-- contain rich data, this can reveal if character names correlate with role types or ordering.
CREATE TABLE join_char_name_cast_info AS
SELECT 
    chn.id AS chn_id,
    chn.name AS chn_name,
    chn.imdb_index AS chn_imdb_index,
    chn.imdb_id AS chn_imdb_id,
    chn.name_pcode_nf AS chn_name_pcode_nf,
    chn.surname_pcode AS chn_surname_pcode,
    chn.md5sum AS chn_md5sum,
    ci.id AS ci_id,
    ci.person_id AS ci_person_id,
    ci.movie_id AS ci_movie_id,
    ci.person_role_id AS ci_person_role_id,
    ci.note AS ci_note,
    ci.nr_order AS ci_nr_order,
    ci.role_id AS ci_role_id
FROM char_name chn
JOIN cast_info ci ON chn.id = ci.person_role_id;

-- 2) role_type + cast_info (role_id)
-- OPINION: Useless / Simple lookup. role_type only maps role_id to strings like 'actor' or 'producer'.
-- This join will yield trivial translation FDs (e.g. role_id = 1 -> role = 'actor') and no interesting DCs.
CREATE TABLE join_role_type_cast_info AS
SELECT 
    rt.id AS rt_id,
    rt.role AS rt_role,
    ci.id AS ci_id,
    ci.person_id AS ci_person_id,
    ci.movie_id AS ci_movie_id,
    ci.person_role_id AS ci_person_role_id,
    ci.note AS ci_note,
    ci.nr_order AS ci_nr_order,
    ci.role_id AS ci_role_id
FROM role_type rt
JOIN cast_info ci ON rt.id = ci.role_id;

-- 3) cast_info + name (person_id)
-- OPINION: Very Useful. Connects actors' personal attributes (gender, phonetic codes) to cast transactions.
-- Can find non-trivial FDs/DCs like whether certain roles are constrained by gender (e.g. gender = 'f' -> role != 'actor').
CREATE TABLE join_cast_info_name AS
SELECT 
    ci.id AS ci_id,
    ci.person_id AS ci_person_id,
    ci.movie_id AS ci_movie_id,
    ci.person_role_id AS ci_person_role_id,
    ci.note AS ci_note,
    ci.nr_order AS ci_nr_order,
    ci.role_id AS ci_role_id,
    n.name AS n_name,
    n.imdb_index AS n_imdb_index,
    n.imdb_id AS n_imdb_id,
    n.gender AS n_gender,
    n.name_pcode_cf AS n_name_pcode_cf,
    n.name_pcode_nf AS n_name_pcode_nf,
    n.surname_pcode AS n_surname_pcode,
    n.md5sum AS n_md5sum
FROM cast_info ci
JOIN name n ON ci.person_id = n.id;

-- 4) cast_info + title (movie_id)
-- OPINION: Useful. Connects actors to movies. Allows searching for temporal dependencies (e.g., whether
-- an actor's cast order or note correlates with the production year of the movie).
CREATE TABLE join_cast_info_title AS
SELECT 
    ci.id AS ci_id,
    ci.person_id AS ci_person_id,
    ci.movie_id AS ci_movie_id,
    ci.person_role_id AS ci_person_role_id,
    ci.note AS ci_note,
    ci.nr_order AS ci_nr_order,
    ci.role_id AS ci_role_id,
    t.title AS t_title,
    t.imdb_index AS t_imdb_index,
    t.kind_id AS t_kind_id,
    t.production_year AS t_production_year,
    t.imdb_id AS t_imdb_id,
    t.phonetic_code AS t_phonetic_code,
    t.episode_of_id AS t_episode_of_id,
    t.season_nr AS t_season_nr,
    t.episode_nr AS t_episode_nr,
    t.series_years AS t_series_years,
    t.md5sum AS t_md5sum
FROM cast_info ci
JOIN title t ON ci.movie_id = t.id;

-- 5) name + aka_name (person_id)
-- OPINION: Useless / Mostly redundant. Both tables describe names of the same person.
-- Will only find trivial identity mappings (e.g. n.md5sum = an.md5sum) or phonetic name similarity.
CREATE TABLE join_name_aka_name AS
SELECT 
    n.id AS n_id,
    n.name AS n_name,
    n.imdb_index AS n_imdb_index,
    n.imdb_id AS n_imdb_id,
    n.gender AS n_gender,
    n.name_pcode_cf AS n_name_pcode_cf,
    n.name_pcode_nf AS n_name_pcode_nf,
    n.surname_pcode AS n_surname_pcode,
    n.md5sum AS n_md5sum,
    an.id AS an_id,
    an.person_id AS an_person_id,
    an.name AS an_name,
    an.imdb_index AS an_imdb_index,
    an.name_pcode_cf AS an_name_pcode_cf,
    an.name_pcode_nf AS an_name_pcode_nf,
    an.surname_pcode AS an_surname_pcode,
    an.md5sum AS an_md5sum
FROM name n
JOIN aka_name an ON n.id = an.person_id;

-- 6) name + person_info (person_id)
-- OPINION: Potentially Useful. Links actors to their profile metadata (trivia, bios, birthdate).
-- Might show correlations between gender and certain metadata fields, but text fields are often very diverse.
CREATE TABLE join_name_person_info AS
SELECT 
    n.id AS n_id,
    n.name AS n_name,
    n.imdb_index AS n_imdb_index,
    n.imdb_id AS n_imdb_id,
    n.gender AS n_gender,
    n.name_pcode_cf AS n_name_pcode_cf,
    n.name_pcode_nf AS n_name_pcode_nf,
    n.surname_pcode AS n_surname_pcode,
    n.md5sum AS n_md5sum,
    pi.id AS pi_id,
    pi.person_id AS pi_person_id,
    pi.info_type_id AS pi_info_type_id,
    pi.info AS pi_info,
    pi.note AS pi_note
FROM name n
JOIN person_info pi ON n.id = pi.person_id;

-- 7) person_info + info_type (info_type_id)
-- OPINION: Useless / Simple lookup. Maps info_type_id to label string (e.g. 'birth date').
-- Yields only trivial translation dependencies.
CREATE TABLE join_person_info_info_type AS
SELECT 
    pi.id AS pi_id,
    pi.person_id AS pi_person_id,
    pi.info_type_id AS pi_info_type_id,
    pi.info AS pi_info,
    pi.note AS pi_note,
    it.info AS it_info
FROM person_info pi
JOIN info_type it ON pi.info_type_id = it.id;

-- 8) info_type + movie_info (info_type_id)
-- OPINION: Useless / Simple lookup. Maps movie_info_type_id to metadata label (e.g. 'genres', 'countries').
-- Yields only trivial translation dependencies.
CREATE TABLE join_info_type_movie_info AS
SELECT 
    it.id AS it_id,
    it.info AS it_info,
    mi.id AS mi_id,
    mi.movie_id AS mi_movie_id,
    mi.info_type_id AS mi_info_type_id,
    mi.info AS mi_info,
    mi.note AS mi_note
FROM info_type it
JOIN movie_info mi ON it.id = mi.info_type_id;

-- 9) movie_info + title (movie_id)
-- OPINION: Very Useful. Connects movie metadata (countries, genres) to the production year and title.
-- Can find rich dependencies like "movies of genre X were only produced after year Y" or country/year correlations.
CREATE TABLE join_movie_info_title AS
SELECT 
    mi.id AS mi_id,
    mi.movie_id AS mi_movie_id,
    mi.info_type_id AS mi_info_type_id,
    mi.info AS mi_info,
    mi.note AS mi_note,
    t.title AS t_title,
    t.imdb_index AS t_imdb_index,
    t.kind_id AS t_kind_id,
    t.production_year AS t_production_year,
    t.imdb_id AS t_imdb_id,
    t.phonetic_code AS t_phonetic_code,
    t.episode_of_id AS t_episode_of_id,
    t.season_nr AS t_season_nr,
    t.episode_nr AS t_episode_nr,
    t.series_years AS t_series_years,
    t.md5sum AS t_md5sum
FROM movie_info mi
JOIN title t ON mi.movie_id = t.id;

-- 10) movie_info_idx + info_type (info_type_id)
-- OPINION: Useless / Simple lookup. Maps index type IDs to labels (e.g., 'rating', 'votes').
-- Trivial translations.
CREATE TABLE join_movie_info_idx_info_type AS
SELECT 
    mii.id AS mii_id,
    mii.movie_id AS mii_movie_id,
    mii.info_type_id AS mii_info_type_id,
    mii.info AS mii_info,
    mii.note AS mii_note,
    it.info AS it_info
FROM movie_info_idx mii
JOIN info_type it ON mii.info_type_id = it.id;

-- 11) movie_info_idx + title (movie_id)
-- OPINION: Very Useful. Connects rating indices directly to movie attributes (year, kind).
-- Can find non-trivial dependencies (e.g., rating range constraints vs production years).
CREATE TABLE join_movie_info_idx_title AS
SELECT 
    mii.id AS mii_id,
    mii.movie_id AS mii_movie_id,
    mii.info_type_id AS mii_info_type_id,
    mii.info AS mii_info,
    mii.note AS mii_note,
    t.title AS t_title,
    t.imdb_index AS t_imdb_index,
    t.kind_id AS t_kind_id,
    t.production_year AS t_production_year,
    t.imdb_id AS t_imdb_id,
    t.phonetic_code AS t_phonetic_code,
    t.episode_of_id AS t_episode_of_id,
    t.season_nr AS t_season_nr,
    t.episode_nr AS t_episode_nr,
    t.series_years AS t_series_years,
    t.md5sum AS t_md5sum
FROM movie_info_idx mii
JOIN title t ON mii.movie_id = t.id;

-- 12) kind_type + title (kind_id)
-- NOTE: Corrected the join key from user's 'movie_id' to 'kind_id' since title.kind_id references kind_type.id.
-- OPINION: Useless / Simple lookup. Maps kind IDs (e.g. 1, 2) to strings ('movie', 'tv series').
-- Only yields trivial translations.
CREATE TABLE join_kind_type_title AS
SELECT 
    kt.id AS kt_id,
    kt.kind AS kt_kind,
    t.id AS t_id,
    t.title AS t_title,
    t.imdb_index AS t_imdb_index,
    t.production_year AS t_production_year,
    t.imdb_id AS t_imdb_id,
    t.phonetic_code AS t_phonetic_code,
    t.episode_of_id AS t_episode_of_id,
    t.season_nr AS t_season_nr,
    t.episode_nr AS t_episode_nr,
    t.series_years AS t_series_years,
    t.md5sum AS t_md5sum
FROM kind_type kt
JOIN title t ON kt.id = t.kind_id;

-- 13) title + movie_keyword (movie_id)
-- OPINION: Useful. Associates keywords with movie titles and years.
-- Can help find constraints on keyword association over time.
CREATE TABLE join_title_movie_keyword AS
SELECT 
    t.id AS t_id,
    t.title AS t_title,
    t.imdb_index AS t_imdb_index,
    t.kind_id AS t_kind_id,
    t.production_year AS t_production_year,
    t.imdb_id AS t_imdb_id,
    t.phonetic_code AS t_phonetic_code,
    t.episode_of_id AS t_episode_of_id,
    t.season_nr AS t_season_nr,
    t.episode_nr AS t_episode_nr,
    t.series_years AS t_series_years,
    t.md5sum AS t_md5sum,
    mk.id AS mk_id,
    mk.movie_id AS mk_movie_id,
    mk.keyword_id AS mk_keyword_id
FROM title t
JOIN movie_keyword mk ON t.id = mk.movie_id;

-- 14) movie_keyword + keyword (keyword_id)
-- OPINION: Useless / Simple lookup. Maps keyword IDs to actual string values.
-- Trivial translations.
CREATE TABLE join_movie_keyword_keyword AS
SELECT 
    mk.id AS mk_id,
    mk.movie_id AS mk_movie_id,
    mk.keyword_id AS mk_keyword_id,
    k.keyword AS k_keyword,
    k.phonetic_code AS k_phonetic_code
FROM movie_keyword mk
JOIN keyword k ON mk.keyword_id = k.id;

-- 15) company_name + movie_companies (company_id)
-- OPINION: Useful. Connects movie company records with company names/countries.
-- Can find constraints related to company countries, phonetic codes, and active movie companies.
CREATE TABLE join_company_name_movie_companies AS
SELECT 
    cn.id AS cn_id,
    cn.name AS cn_name,
    cn.country_code AS cn_country_code,
    cn.imdb_id AS cn_imdb_id,
    cn.name_pcode_nf AS cn_name_pcode_nf,
    cn.name_pcode_sf AS cn_name_pcode_sf,
    cn.md5sum AS cn_md5sum,
    mc.id AS mc_id,
    mc.movie_id AS mc_movie_id,
    mc.company_id AS mc_company_id,
    mc.company_type_id AS mc_company_type_id,
    mc.note AS mc_note
FROM company_name cn
JOIN movie_companies mc ON cn.id = mc.company_id;

-- 16) movie_companies + company_type (company_type_id)
-- OPINION: Useless / Simple lookup. Maps company type IDs to descriptions ('distributor', 'production co').
-- Trivial translations.
CREATE TABLE join_movie_companies_company_type AS
SELECT 
    mc.id AS mc_id,
    mc.movie_id AS mc_movie_id,
    mc.company_id AS mc_company_id,
    mc.company_type_id AS mc_company_type_id,
    mc.note AS mc_note,
    ct.kind AS ct_kind
FROM movie_companies mc
JOIN company_type ct ON mc.company_type_id = ct.id;

-- 17) movie_companies + title (movie_id)
-- OPINION: Useful. Connects movie releases to the producing/distributing companies.
-- Can show patterns of company activity over years.
CREATE TABLE join_movie_companies_title AS
SELECT 
    mc.id AS mc_id,
    mc.movie_id AS mc_movie_id,
    mc.company_id AS mc_company_id,
    mc.company_type_id AS mc_company_type_id,
    mc.note AS mc_note,
    t.title AS t_title,
    t.imdb_index AS t_imdb_index,
    t.kind_id AS t_kind_id,
    t.production_year AS t_production_year,
    t.imdb_id AS t_imdb_id,
    t.phonetic_code AS t_phonetic_code,
    t.episode_of_id AS t_episode_of_id,
    t.season_nr AS t_season_nr,
    t.episode_nr AS t_episode_nr,
    t.series_years AS t_series_years,
    t.md5sum AS t_md5sum
FROM movie_companies mc
JOIN title t ON mc.movie_id = t.id;

-- 18) complete_cast + comp_cast_type (subject_id, status_id)
-- DESIGN NOTE: complete_cast has TWO foreign keys pointing to comp_cast_type (subject_id and status_id).
-- We join comp_cast_type twice (aliased as cct_sub and cct_stat) to capture both lookups in a single join.
-- OPINION: Useless / Simple lookup. Only maps complete cast identifiers to verification/completion descriptions.
CREATE TABLE join_complete_cast_comp_cast_type AS
SELECT 
    cc.id AS cc_id,
    cc.movie_id AS cc_movie_id,
    cc.subject_id AS cc_subject_id,
    cc.status_id AS cc_status_id,
    cct_sub.kind AS cc_subject_kind,
    cct_stat.kind AS cc_status_kind
FROM complete_cast cc
JOIN comp_cast_type cct_sub ON cc.subject_id = cct_sub.id
JOIN comp_cast_type cct_stat ON cc.status_id = cct_stat.id;

-- 19) movie_link + link_type (link_type_id)
-- OPINION: Useless / Simple lookup. Maps link type IDs to relationship labels ('sequel', 'remake').
-- Trivial translations.
CREATE TABLE join_movie_link_link_type AS
SELECT 
    ml.id AS ml_id,
    ml.movie_id AS ml_movie_id,
    ml.linked_movie_id AS ml_linked_movie_id,
    ml.link_type_id AS ml_link_type_id,
    lt.link AS lt_link
FROM movie_link ml
JOIN link_type lt ON ml.link_type_id = lt.id;

-- 20) movie_link + title (movie_id)
-- OPINION: Extremely Useful. Connects linked movies together (e.g. sequels, spin-offs).
-- Can find non-trivial temporal DCs such as "a sequel's production year must be greater than or equal
-- to the original movie's production year".
CREATE TABLE join_movie_link_title AS
SELECT 
    ml.id AS ml_id,
    ml.movie_id AS ml_movie_id,
    ml.linked_movie_id AS ml_linked_movie_id,
    ml.link_type_id AS ml_link_type_id,
    t.title AS t_title,
    t.imdb_index AS t_imdb_index,
    t.kind_id AS t_kind_id,
    t.production_year AS t_production_year,
    t.imdb_id AS t_imdb_id,
    t.phonetic_code AS t_phonetic_code,
    t.episode_of_id AS t_episode_of_id,
    t.season_nr AS t_season_nr,
    t.episode_nr AS t_episode_nr,
    t.series_years AS t_series_years,
    t.md5sum AS t_md5sum
FROM movie_link ml
JOIN title t ON ml.movie_id = t.id;

-- 21) aka_title + title (movie_id)
-- OPINION: Useless / Redundant. Connects movies to alternative titles.
-- Only finds identity mappings.
CREATE TABLE join_aka_title_title AS
SELECT 
    akt.id AS at_id,
    akt.movie_id AS at_movie_id,
    akt.title AS at_title,
    akt.imdb_index AS at_imdb_index,
    akt.kind_id AS at_kind_id,
    akt.production_year AS at_production_year,
    akt.phonetic_code AS at_phonetic_code,
    akt.episode_of_id AS at_episode_of_id,
    akt.season_nr AS at_season_nr,
    akt.episode_nr AS at_episode_nr,
    akt.note AS at_note,
    akt.md5sum AS at_md5sum,
    t.title AS t_title,
    t.imdb_index AS t_imdb_index,
    t.kind_id AS t_kind_id,
    t.production_year AS t_production_year,
    t.imdb_id AS t_imdb_id,
    t.phonetic_code AS t_phonetic_code,
    t.episode_of_id AS t_episode_of_id,
    t.season_nr AS t_season_nr,
    t.episode_nr AS t_episode_nr,
    t.series_years AS t_series_years,
    t.md5sum AS t_md5sum
FROM aka_title akt
JOIN title t ON akt.movie_id = t.id;


-- 4. Run LIMA discovery on the individual tables
SELECT '--- aka_name ---' AS section;
SELECT * FROM lima_discover('aka_name', threshold=0.01);

SELECT '--- aka_title ---' AS section;
SELECT * FROM lima_discover('aka_title', threshold=0.01);

SELECT '--- cast_info ---' AS section;
SELECT * FROM lima_discover('cast_info', threshold=0.01);

SELECT '--- char_name ---' AS section;
SELECT * FROM lima_discover('char_name', threshold=0.01);

SELECT '--- comp_cast_type ---' AS section;
SELECT * FROM lima_discover('comp_cast_type', threshold=0.01);

SELECT '--- company_name ---' AS section;
SELECT * FROM lima_discover('company_name', threshold=0.01);

SELECT '--- company_type ---' AS section;
SELECT * FROM lima_discover('company_type', threshold=0.01);

SELECT '--- complete_cast ---' AS section;
SELECT * FROM lima_discover('complete_cast', threshold=0.01);

SELECT '--- info_type ---' AS section;
SELECT * FROM lima_discover('info_type', threshold=0.01);

SELECT '--- keyword ---' AS section;
SELECT * FROM lima_discover('keyword', threshold=0.01);

SELECT '--- kind_type ---' AS section;
SELECT * FROM lima_discover('kind_type', threshold=0.01);

SELECT '--- link_type ---' AS section;
SELECT * FROM lima_discover('link_type', threshold=0.01);

SELECT '--- movie_companies ---' AS section;
SELECT * FROM lima_discover('movie_companies', threshold=0.01);

SELECT '--- movie_info ---' AS section;
SELECT * FROM lima_discover('movie_info', threshold=0.01);

SELECT '--- movie_info_idx ---' AS section;
SELECT * FROM lima_discover('movie_info_idx', threshold=0.01);

SELECT '--- movie_keyword ---' AS section;
SELECT * FROM lima_discover('movie_keyword', threshold=0.01);

SELECT '--- movie_link ---' AS section;
SELECT * FROM lima_discover('movie_link', threshold=0.01);

SELECT '--- name ---' AS section;
SELECT * FROM lima_discover('name', threshold=0.01);

SELECT '--- person_info ---' AS section;
SELECT * FROM lima_discover('person_info', threshold=0.01);

SELECT '--- role_type ---' AS section;
SELECT * FROM lima_discover('role_type', threshold=0.01);

SELECT '--- title ---' AS section;
SELECT * FROM lima_discover('title', threshold=0.01);


-- 5. Run LIMA discovery on the joined tables
SELECT '--- join_char_name_cast_info ---' AS section;
SELECT * FROM lima_discover('join_char_name_cast_info', threshold=0.01);

SELECT '--- join_role_type_cast_info ---' AS section;
SELECT * FROM lima_discover('join_role_type_cast_info', threshold=0.01);

SELECT '--- join_cast_info_name ---' AS section;
SELECT * FROM lima_discover('join_cast_info_name', threshold=0.01);

SELECT '--- join_cast_info_title ---' AS section;
SELECT * FROM lima_discover('join_cast_info_title', threshold=0.01);

SELECT '--- join_name_aka_name ---' AS section;
SELECT * FROM lima_discover('join_name_aka_name', threshold=0.01);

SELECT '--- join_name_person_info ---' AS section;
SELECT * FROM lima_discover('join_name_person_info', threshold=0.01);

SELECT '--- join_person_info_info_type ---' AS section;
SELECT * FROM lima_discover('join_person_info_info_type', threshold=0.01);

SELECT '--- join_info_type_movie_info ---' AS section;
SELECT * FROM lima_discover('join_info_type_movie_info', threshold=0.01);

SELECT '--- join_movie_info_title ---' AS section;
SELECT * FROM lima_discover('join_movie_info_title', threshold=0.01);

SELECT '--- join_movie_info_idx_info_type ---' AS section;
SELECT * FROM lima_discover('join_movie_info_idx_info_type', threshold=0.01);

SELECT '--- join_movie_info_idx_title ---' AS section;
SELECT * FROM lima_discover('join_movie_info_idx_title', threshold=0.01);

SELECT '--- join_kind_type_title ---' AS section;
SELECT * FROM lima_discover('join_kind_type_title', threshold=0.01);

SELECT '--- join_title_movie_keyword ---' AS section;
SELECT * FROM lima_discover('join_title_movie_keyword', threshold=0.01);

SELECT '--- join_movie_keyword_keyword ---' AS section;
SELECT * FROM lima_discover('join_movie_keyword_keyword', threshold=0.01);

SELECT '--- join_company_name_movie_companies ---' AS section;
SELECT * FROM lima_discover('join_company_name_movie_companies', threshold=0.01);

SELECT '--- join_movie_companies_company_type ---' AS section;
SELECT * FROM lima_discover('join_movie_companies_company_type', threshold=0.01);

SELECT '--- join_movie_companies_title ---' AS section;
SELECT * FROM lima_discover('join_movie_companies_title', threshold=0.01);

SELECT '--- join_complete_cast_comp_cast_type ---' AS section;
SELECT * FROM lima_discover('join_complete_cast_comp_cast_type', threshold=0.01);

SELECT '--- join_movie_link_link_type ---' AS section;
SELECT * FROM lima_discover('join_movie_link_link_type', threshold=0.01);

SELECT '--- join_movie_link_title ---' AS section;
SELECT * FROM lima_discover('join_movie_link_title', threshold=0.01);

SELECT '--- join_aka_title_title ---' AS section;
SELECT * FROM lima_discover('join_aka_title_title', threshold=0.01);
