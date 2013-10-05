-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION json_build" to load this file. \quit

CREATE FUNCTION build_json_object(VARIADIC "any")
RETURNS json
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE;

-- degenerate case - return empty object
CREATE FUNCTION build_json_object()
RETURNS json
AS 'select json $${}$$ '
LANGUAGE SQL IMMUTABLE;

CREATE FUNCTION build_json_array(VARIADIC "any")
RETURNS json
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE;

-- degenerate case - return empty array
CREATE FUNCTION build_json_array()
RETURNS json
AS 'select json $$[]$$ '
LANGUAGE SQL IMMUTABLE;


CREATE FUNCTION json_object_agg_transfn(internal, "any", "any")
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION json_object_agg_finalfn(internal)
RETURNS json
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE;

CREATE AGGREGATE json_object_agg("any", "any") (
    SFUNC = json_object_agg_transfn,
    FINALFUNC = json_object_agg_finalfn,
    STYPE = internal
);
