/*-------------------------------------------------------------------------
 *
 * json_build.c
 *		Helper functions for building up JSON
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "fmgr.h"
#if PG_VERSION_NUM >= 90300
#include "access/htup_details.h"
#endif
#include "access/transam.h"
#include "catalog/pg_cast.h"
#include "catalog/pg_type.h"
#include "parser/parse_coerce.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/json.h"
#include "utils/typcache.h"
#include "utils/syscache.h"
#include "utils/lsyscache.h"

PG_MODULE_MAGIC;

/* functions copied from core PG because they are not exposed there */

static void composite_to_json(Datum composite, StringInfo result,
							  bool use_line_feeds);
static void array_dim_to_json(StringInfo result, int dim, int ndims, int *dims,
				  Datum *vals, bool *nulls, int *valcount,
				  TYPCATEGORY tcategory, Oid typoutputfunc,
				  bool use_line_feeds);
static void array_to_json_internal(Datum array, StringInfo result,
								   bool use_line_feeds);

/* 
 * All the defined  type categories are upper case , so use lower case here
 * so we avoid any possible clash.
 */
/* fake type category for JSON so we can distinguish it in datum_to_json */
#define TYPCATEGORY_JSON 'j'
/* fake category for types that have a cast to json */
#define TYPCATEGORY_JSON_CAST 'c'
/* letters appearing in numeric output that aren't valid in a JSON number */
#define NON_NUMERIC_LETTER "NnAaIiFfTtYy"

/*
 * Turn a scalar Datum into JSON, appending the string to "result".
 *
 * Hand off a non-scalar datum to composite_to_json or array_to_json_internal
 * as appropriate.
 */
static void
datum_to_json(Datum val, bool is_null, StringInfo result,
			  TYPCATEGORY tcategory, Oid typoutputfunc, bool key_scalar)
{
	char	   *outputstr;
	text       *jsontext;

	if (is_null)
	{
		appendStringInfoString(result, "null");
		return;
	}

	switch (tcategory)
	{
		case TYPCATEGORY_ARRAY:
			array_to_json_internal(val, result, false);
			break;
		case TYPCATEGORY_COMPOSITE:
			composite_to_json(val, result, false);
			break;
		case TYPCATEGORY_BOOLEAN:
			if (!key_scalar)
				appendStringInfoString(result, DatumGetBool(val) ? "true" : "false");
			else
				escape_json(result, DatumGetBool(val) ? "true" : "false");
			break;
		case TYPCATEGORY_NUMERIC:
			outputstr = OidOutputFunctionCall(typoutputfunc, val);

			/*
			 * Don't call escape_json here if it's a valid JSON number.
			 * Numeric output should usually be a valid JSON number and JSON
			 * numbers shouldn't be quoted. Quote cases like "Nan" and
			 * "Infinity", however.
			 */
			if (strpbrk(outputstr, NON_NUMERIC_LETTER) == NULL && ! key_scalar)
				appendStringInfoString(result, outputstr);
			else
				escape_json(result, outputstr);
			pfree(outputstr);
			break;
		case TYPCATEGORY_JSON:
			/* JSON will already be escaped */
			outputstr = OidOutputFunctionCall(typoutputfunc, val);
			appendStringInfoString(result, outputstr);
			pfree(outputstr);
			break;
		case TYPCATEGORY_JSON_CAST:
			jsontext = DatumGetTextP(OidFunctionCall1(typoutputfunc, val));
			outputstr = text_to_cstring(jsontext);
			appendStringInfoString(result, outputstr);
			pfree(outputstr);
			pfree(jsontext);
			break;
		default:
			outputstr = OidOutputFunctionCall(typoutputfunc, val);
			if (key_scalar && *outputstr == '\0')
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("key value must not be empty")));
			escape_json(result, outputstr);
			pfree(outputstr);
			break;
	}
}

/*
 * Process a single dimension of an array.
 * If it's the innermost dimension, output the values, otherwise call
 * ourselves recursively to process the next dimension.
 */
static void
array_dim_to_json(StringInfo result, int dim, int ndims, int *dims, Datum *vals,
				  bool *nulls, int *valcount, TYPCATEGORY tcategory,
				  Oid typoutputfunc, bool use_line_feeds)
{
	int			i;
	const char *sep;

	Assert(dim < ndims);

	sep = use_line_feeds ? ",\n " : ",";

	appendStringInfoChar(result, '[');

	for (i = 1; i <= dims[dim]; i++)
	{
		if (i > 1)
			appendStringInfoString(result, sep);

		if (dim + 1 == ndims)
		{
			datum_to_json(vals[*valcount], nulls[*valcount], result, tcategory,
						  typoutputfunc, false);
			(*valcount)++;
		}
		else
		{
			/*
			 * Do we want line feeds on inner dimensions of arrays? For now
			 * we'll say no.
			 */
			array_dim_to_json(result, dim + 1, ndims, dims, vals, nulls,
							  valcount, tcategory, typoutputfunc, false);
		}
	}

	appendStringInfoChar(result, ']');
}

/*
 * Turn an array into JSON.
 */
static void
array_to_json_internal(Datum array, StringInfo result, bool use_line_feeds)
{
	ArrayType  *v = DatumGetArrayTypeP(array);
	Oid			element_type = ARR_ELEMTYPE(v);
	int		   *dim;
	int			ndim;
	int			nitems;
	int			count = 0;
	Datum	   *elements;
	bool	   *nulls;
	int16		typlen;
	bool		typbyval;
	char		typalign,
				typdelim;
	Oid			typioparam;
	Oid			typoutputfunc;
	TYPCATEGORY tcategory;
	Oid         castfunc = InvalidOid;

	ndim = ARR_NDIM(v);
	dim = ARR_DIMS(v);
	nitems = ArrayGetNItems(ndim, dim);

	if (nitems <= 0)
	{
		appendStringInfoString(result, "[]");
		return;
	}

	get_type_io_data(element_type, IOFunc_output,
					 &typlen, &typbyval, &typalign,
					 &typdelim, &typioparam, &typoutputfunc);

	if (element_type > FirstNormalObjectId)
	{
		    HeapTuple   tuple;
			Form_pg_cast castForm;

			tuple = SearchSysCache2(CASTSOURCETARGET,
									ObjectIdGetDatum(element_type),
									ObjectIdGetDatum(JSONOID));
			if (HeapTupleIsValid(tuple))
			{
				castForm = (Form_pg_cast) GETSTRUCT(tuple);

				if (castForm->castmethod == COERCION_METHOD_FUNCTION)
					castfunc = typoutputfunc = castForm->castfunc;

				ReleaseSysCache(tuple);
			}
	}

	deconstruct_array(v, element_type, typlen, typbyval,
					  typalign, &elements, &nulls,
					  &nitems);

	if (castfunc != InvalidOid)
		tcategory = TYPCATEGORY_JSON_CAST;
	else if	(element_type == RECORDOID)
		tcategory = TYPCATEGORY_COMPOSITE;
	else if (element_type == JSONOID)
		tcategory = TYPCATEGORY_JSON;
	else
		tcategory = TypeCategory(element_type);

	array_dim_to_json(result, 0, ndim, dim, elements, nulls, &count, tcategory,
					  typoutputfunc, use_line_feeds);

	pfree(elements);
	pfree(nulls);
}

/*
 * Turn a composite / record into JSON.
 */
static void
composite_to_json(Datum composite, StringInfo result, bool use_line_feeds)
{
	HeapTupleHeader td;
	Oid			tupType;
	int32		tupTypmod;
	TupleDesc	tupdesc;
	HeapTupleData tmptup,
			   *tuple;
	int			i;
	bool		needsep = false;
	const char *sep;

	sep = use_line_feeds ? ",\n " : ",";

	td = DatumGetHeapTupleHeader(composite);

	/* Extract rowtype info and find a tupdesc */
	tupType = HeapTupleHeaderGetTypeId(td);
	tupTypmod = HeapTupleHeaderGetTypMod(td);
	tupdesc = lookup_rowtype_tupdesc(tupType, tupTypmod);

	/* Build a temporary HeapTuple control structure */
	tmptup.t_len = HeapTupleHeaderGetDatumLength(td);
	tmptup.t_data = td;
	tuple = &tmptup;

	appendStringInfoChar(result, '{');

	for (i = 0; i < tupdesc->natts; i++)
	{
		Datum		val,
					origval;
		bool		isnull;
		char	   *attname;
		TYPCATEGORY tcategory;
		Oid			typoutput;
		bool		typisvarlena;
		Oid         castfunc = InvalidOid;

		if (tupdesc->attrs[i]->attisdropped)
			continue;

		if (needsep)
			appendStringInfoString(result, sep);
		needsep = true;

		attname = NameStr(tupdesc->attrs[i]->attname);
		escape_json(result, attname);
		appendStringInfoChar(result, ':');

		origval = heap_getattr(tuple, i + 1, tupdesc, &isnull);

		getTypeOutputInfo(tupdesc->attrs[i]->atttypid,
						  &typoutput, &typisvarlena);

		if (tupdesc->attrs[i]->atttypid > FirstNormalObjectId)
		{
		    HeapTuple   cast_tuple;
			Form_pg_cast castForm;
			
			cast_tuple = SearchSysCache2(CASTSOURCETARGET,
										 ObjectIdGetDatum(tupdesc->attrs[i]->atttypid),
										 ObjectIdGetDatum(JSONOID));
			if (HeapTupleIsValid(cast_tuple))
			{
				castForm = (Form_pg_cast) GETSTRUCT(cast_tuple);
				
				if (castForm->castmethod == COERCION_METHOD_FUNCTION)
					castfunc = typoutput = castForm->castfunc;
				
				ReleaseSysCache(cast_tuple);
			}
		}

		if (castfunc != InvalidOid)
			tcategory = TYPCATEGORY_JSON_CAST;
		else if (tupdesc->attrs[i]->atttypid == RECORDARRAYOID)
			tcategory = TYPCATEGORY_ARRAY;
		else if (tupdesc->attrs[i]->atttypid == RECORDOID)
			tcategory = TYPCATEGORY_COMPOSITE;
		else if (tupdesc->attrs[i]->atttypid == JSONOID)
			tcategory = TYPCATEGORY_JSON;
		else
			tcategory = TypeCategory(tupdesc->attrs[i]->atttypid);

		/*
		 * If we have a toasted datum, forcibly detoast it here to avoid
		 * memory leakage inside the type's output routine.
		 */
		if (typisvarlena && !isnull)
			val = PointerGetDatum(PG_DETOAST_DATUM(origval));
		else
			val = origval;

		datum_to_json(val, isnull, result, tcategory, typoutput, false);

		/* Clean up detoasted copy, if any */
		if (val != origval)
			pfree(DatumGetPointer(val));
	}

	appendStringInfoChar(result, '}');
	ReleaseTupleDesc(tupdesc);
}

static void
add_json(Datum orig_val, bool is_null, StringInfo result, Oid val_type, bool key_scalar)
{
    Datum       val;
	TYPCATEGORY tcategory;
	Oid			typoutput;
	bool		typisvarlena;
	Oid         castfunc = InvalidOid;

    if (val_type == InvalidOid)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("could not determine input data type")));


	getTypeOutputInfo(val_type, &typoutput, &typisvarlena);

	if (val_type > FirstNormalObjectId)
	{
		    HeapTuple   tuple;
			Form_pg_cast castForm;

			tuple = SearchSysCache2(CASTSOURCETARGET,
									ObjectIdGetDatum(val_type),
									ObjectIdGetDatum(JSONOID));
			if (HeapTupleIsValid(tuple))
			{
				castForm = (Form_pg_cast) GETSTRUCT(tuple);

				if (castForm->castmethod == COERCION_METHOD_FUNCTION)
					castfunc = typoutput = castForm->castfunc;

				ReleaseSysCache(tuple);
			}
	}

	if (castfunc != InvalidOid)
		tcategory = TYPCATEGORY_JSON_CAST;
	else if (val_type == RECORDARRAYOID)
		tcategory = TYPCATEGORY_ARRAY;
	else if (val_type == RECORDOID)
		tcategory = TYPCATEGORY_COMPOSITE;
	else if (val_type == JSONOID)
		tcategory = TYPCATEGORY_JSON;
	else
		tcategory = TypeCategory(val_type);
	
	if (key_scalar && 
		(tcategory == TYPCATEGORY_ARRAY || 
		 tcategory == TYPCATEGORY_COMPOSITE || 
		 tcategory ==  TYPCATEGORY_JSON || 
		 tcategory == TYPCATEGORY_JSON_CAST))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("key value must be scalar, not array, composite or json")));
		
	/*
	 * If we have a toasted datum, forcibly detoast it here to avoid
	 * memory leakage inside the type's output routine.
	 */
	if (typisvarlena && orig_val != (Datum) 0)
		val = PointerGetDatum(PG_DETOAST_DATUM(orig_val));
	else
		val = orig_val;
	
	datum_to_json(val, is_null, result, tcategory, typoutput, key_scalar);

	/* Clean up detoasted copy, if any */
	if (val != orig_val)
		pfree(DatumGetPointer(val));
}


/*
 * SQL function build_json_object(variadic "any")
 */
extern Datum build_json_object(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(build_json_object);

Datum
build_json_object(PG_FUNCTION_ARGS)
{
	int nargs = PG_NARGS();
	int i;
	Datum arg;
	char *sep = "";
    StringInfo  result;
	Oid val_type;
	

	if (nargs % 2 != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid number or arguments: object must be matched key value pairs")));

    result = makeStringInfo();

	appendStringInfoChar(result,'{');

	for (i = 0; i < nargs; i += 2)
	{

		/* process key */

		if (PG_ARGISNULL(i))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("arg %d: key cannot be null", i+1)));
		val_type = get_fn_expr_argtype(fcinfo->flinfo, i);
		/* 
		 * turn a constant (more or less literal) value that's of unknown
		 * type into text. Unknowns come in as a cstring pointer.
		 */
		if (val_type == UNKNOWNOID && get_fn_expr_arg_stable(fcinfo->flinfo, i))
		{
			val_type = TEXTOID;
			if (PG_ARGISNULL(i))
				arg = (Datum)0;
			else
				arg = CStringGetTextDatum(PG_GETARG_POINTER(i));
		}
		else
		{
			arg = PG_GETARG_DATUM(i);
		}
		if (val_type == InvalidOid || val_type == UNKNOWNOID)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("arg %d: could not determine data type",i+1)));
		appendStringInfoString(result,sep);
		sep = ", ";
		add_json(arg, false, result, val_type, true);

		appendStringInfoString(result," : ");

		/* process value */

		val_type = get_fn_expr_argtype(fcinfo->flinfo, i+1);
		/* see comments above */
		if (val_type == UNKNOWNOID && get_fn_expr_arg_stable(fcinfo->flinfo, i+1))
		{
			val_type = TEXTOID;
			if (PG_ARGISNULL(i+1))
				arg = (Datum)0;
			else
				arg = CStringGetTextDatum(PG_GETARG_POINTER(i+1));
		}
		else
		{
			arg = PG_GETARG_DATUM(i+1);
		}
		if (val_type == InvalidOid || val_type == UNKNOWNOID)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("arg %d: could not determine data type",i+2)));
		add_json(arg, PG_ARGISNULL(i+1), result, val_type, false);

	}
	appendStringInfoChar(result,'}');

	PG_RETURN_TEXT_P(cstring_to_text_with_len(result->data, result->len));
	
}

/*
 * SQL function build_json_array(variadic "any")
 */
extern Datum build_json_array(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(build_json_array);

Datum
build_json_array(PG_FUNCTION_ARGS)
{
	int nargs = PG_NARGS();
	int i;
	Datum arg;
	char *sep = "";
    StringInfo  result;
	Oid val_type;
	

    result = makeStringInfo();

	appendStringInfoChar(result,'[');

	for (i = 0; i < nargs; i ++)
	{
		val_type = get_fn_expr_argtype(fcinfo->flinfo, i);
		arg = PG_GETARG_DATUM(i+1);
		/* see comments in build_json_object above */
		if (val_type == UNKNOWNOID && get_fn_expr_arg_stable(fcinfo->flinfo, i))
		{
			val_type = TEXTOID;
			if (PG_ARGISNULL(i))
				arg = (Datum)0;
			else
				arg = CStringGetTextDatum(PG_GETARG_POINTER(i));
		}
		else
		{
			arg = PG_GETARG_DATUM(i);
		}
		if (val_type == InvalidOid || val_type == UNKNOWNOID)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("arg %d: could not determine data type",i+1)));
		appendStringInfoString(result,sep);
		sep = ", ";
		add_json(arg, PG_ARGISNULL(i), result, val_type, false);
	}
	appendStringInfoChar(result,']');

	PG_RETURN_TEXT_P(cstring_to_text_with_len(result->data, result->len));
	
}

extern Datum json_object_agg_transfn(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(json_object_agg_transfn);

Datum
json_object_agg_transfn(PG_FUNCTION_ARGS)
{
    Oid         val_type;
    MemoryContext aggcontext,
                oldcontext;
    StringInfo  state;
    Datum       arg;

    if (!AggCheckCallContext(fcinfo, &aggcontext))
    {
        /* cannot be called directly because of internal-type argument */
        elog(ERROR, "json_agg_transfn called in non-aggregate context");
    }

    if (PG_ARGISNULL(0))
    {
        /*
         * Make this StringInfo in a context where it will persist for the
         * duration off the aggregate call. It's only needed for this initial
         * piece, as the StringInfo routines make sure they use the right
         * context to enlarge the object if necessary.
         */
        oldcontext = MemoryContextSwitchTo(aggcontext);
        state = makeStringInfo();
        MemoryContextSwitchTo(oldcontext);

        appendStringInfoString(state, "{ ");
    }
    else
    {
        state = (StringInfo) PG_GETARG_POINTER(0);
        appendStringInfoString(state, ", ");
    }

	if (PG_ARGISNULL(1))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("field name must not be null")));


	val_type = get_fn_expr_argtype(fcinfo->flinfo, 1);
	/* 
	 * turn a constant (more or less literal) value that's of unknown
	 * type into text. Unknowns come in as a cstring pointer.
	 */
	if (val_type == UNKNOWNOID && get_fn_expr_arg_stable(fcinfo->flinfo, 1))
	{
		val_type = TEXTOID;
		arg = CStringGetTextDatum(PG_GETARG_POINTER(1));
	}
	else
	{
		arg = PG_GETARG_DATUM(1);
	}

	if (val_type == InvalidOid || val_type == UNKNOWNOID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("arg 1: could not determine data type")));

	add_json(arg, false, state, val_type, true);
	
	appendStringInfoString(state," : ");

	val_type = get_fn_expr_argtype(fcinfo->flinfo, 2);
	/* see comments above */
	if (val_type == UNKNOWNOID && get_fn_expr_arg_stable(fcinfo->flinfo, 2))
	{
		val_type = TEXTOID;
		if (PG_ARGISNULL(2))
			arg = (Datum)0;
		else
			arg = CStringGetTextDatum(PG_GETARG_POINTER(2));
	}
	else
	{
		arg = PG_GETARG_DATUM(2);
	}

	if (val_type == InvalidOid || val_type == UNKNOWNOID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("arg 2: could not determine data type")));

	add_json(arg, PG_ARGISNULL(2), state, val_type, false);

	PG_RETURN_POINTER(state);
}

extern Datum json_object_agg_finalfn(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(json_object_agg_finalfn);

Datum
json_object_agg_finalfn(PG_FUNCTION_ARGS)
{
    StringInfo  state;

    /* cannot be called directly because of internal-type argument */
    Assert(AggCheckCallContext(fcinfo, NULL));

    state = PG_ARGISNULL(0) ? NULL : (StringInfo) PG_GETARG_POINTER(0);

    if (state == NULL)
        PG_RETURN_TEXT_P(cstring_to_text("{}"));

    appendStringInfoString(state, " }");

    PG_RETURN_TEXT_P(cstring_to_text(state->data));
}


