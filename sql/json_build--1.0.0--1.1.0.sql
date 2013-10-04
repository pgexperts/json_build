-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION json_build UPDATE TO '1.1.0'" to load this file. \quit


CREATE FUNCTION json_object_agg_transfn(internal, text, anyelement)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION json_object_agg_finalfn(internal)
RETURNS json
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE;

CREATE AGGREGATE json_object_agg(text, anyelement) (
    SFUNC = json_object_agg_transfn,
    FINALFUNC = json_object_agg_finalfn,
    STYPE = internal
);


