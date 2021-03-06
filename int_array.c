/*
 * int_array.c - routines for arrays of integer indices.
 */

/*
 * Copyright (C) 1986, 1988, 1989, 1991-2013, 2016, 2017, 2019, 2020,
 * the Free Software Foundation, Inc.
 *
 * This file is part of GAWK, the GNU implementation of the
 * AWK Programming Language.
 *
 * GAWK is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * GAWK is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */

#include "awk.h"

static awk_ulong_t INT_CHAIN_MAX = 2u;

static NODE **int_array_init(NODE *symbol, NODE *subs);
static NODE **int_lookup(NODE *symbol, NODE *subs);
static NODE **int_exists(NODE *symbol, NODE *subs);
static NODE **int_clear(NODE *symbol, NODE *subs);
static NODE **int_remove(NODE *symbol, NODE *subs);
static NODE **int_list(NODE *symbol, NODE *t);
static NODE **int_copy(NODE *symbol, NODE *newsymb);
static NODE **int_dump(NODE *symbol, NODE *ndump);

static uint32_t int_hash(awk_long_t k, size_t hsize);
static inline NODE **int_find(NODE *symbol, awk_long_t k, uint32_t hash1);
static NODE **int_insert(NODE *symbol, awk_long_t k, uint32_t hash1);
static void grow_int_table(NODE *symbol);

const array_funcs_t int_array_func = {
	"int",
	int_array_init,
	is_integer,
	int_lookup,
	int_exists,
	int_clear,
	int_remove,
	int_list,
	int_copy,
	int_dump,
	(afunc_t) 0,
};


/* int_array_init --- array initialization routine */

static NODE **
int_array_init(NODE *symbol, NODE *subs)
{
	(void) subs;

	if (symbol == NULL) {	/* first time */
		awk_long_t newval;

		/* check relevant environment variables */
		if ((newval = getenv_long("INT_CHAIN_MAX")) > 0)
			INT_CHAIN_MAX = (awk_ulong_t) newval;
	} else
		null_array(symbol);

	return & success_node;
}

/*
 * standard_integer_string -- check whether the string matches what
 * sprintf("%ld", <value>) would produce. This is accomplished by accepting
 * only strings that look like /^0$/ or /^-?[1-9][0-9]*$/. This should be
 * faster than comparing vs. the results of actually calling sprintf.
 */

static bool
standard_integer_string(const char *s, size_t len)
{
	const char *end;

	if (len == 0)
		return false;
	if (*s == '0' && len == 1)
		return true;
	end = s + len;
	/* ignore leading minus sign */
	if (*s == '-' && ++s == end)
		return false;
	/* check first char is [1-9] */
	if (char_digit_value((unsigned char) *s) < 1)
		return false;
	while (++s < end) {
		if (!char_is_digit((unsigned char) *s))
			return false;
	}
	return true;
}

/* is_integer --- check if subscript is an integer */

NODE **
is_integer(NODE *symbol, NODE *subs)
{
	AWKNUM d;
	(void) symbol;

	if ((subs->flags & NUMINT) != 0)
		/* quick exit */
		return & success_node;

	if (subs == Nnull_string || do_mpfr)
		return NULL;

#ifdef CHECK_INTEGER_USING_FORCE_NUMBER
	/*
	 * This approach is much simpler, because we remove all of the strtol
	 * logic below. But this may be slower in some usage cases.
	 */
	if ((subs->flags & NUMCUR) == 0) {
		str2number(subs);

		/* check again in case force_number set NUMINT */
		if ((subs->flags & NUMINT) != 0)
			return & success_node;
	}
#else /* CHECK_INTEGER_USING_FORCE_NUMBER */
	if ((subs->flags & NUMCUR) != 0) {
#endif /* CHECK_INTEGER_USING_FORCE_NUMBER */
		d = subs->numbr;
		/* cint_array.c do not supports indexes larger than 32 bits */
		if (d <= INT32_MAX && d >= INT32_MIN && d == (int32_t) d) {
			/*
			 * The numeric value is an integer, but we must
			 * protect against strings that cannot be generated
			 * from sprintf("%ld", <subscript>). This can happen
			 * with strnum or string values. We could skip this
			 * check for pure NUMBER values, but unfortunately the
			 * code does not currently distinguish between NUMBER
			 * and strnum values.
			 */
			if (   (subs->flags & STRCUR) == 0
			    || standard_integer_string(subs->stptr, subs->stlen)) {
				subs->flags |= NUMINT;
				return & success_node;
			}
		}
		return NULL;
#ifndef CHECK_INTEGER_USING_FORCE_NUMBER
	}

	/* a[3]=1; print "3" in a    -- true
	 * a[3]=1; print "+3" in a   -- false
	 * a[3]=1; print "03" in a   -- false
	 * a[-3]=1; print "-3" in a  -- true
	 */

	/* must be a STRING */
	const char *cp = subs->stptr;
	size_t const len = subs->stlen;
	const char *const cpend = cp + len;
	awk_long_t l;

	if (len == 0 || ((l = char_digit_value((unsigned char) *cp)) == -1 && *cp != '-'))
		return NULL;

	if (len > 1 &&
		((*cp == '0')		/* "00", "011" .. */
			|| (*cp == '-' && *(cp + 1) == '0')	/* "-0", "-011" .. */
		)
	)
		return NULL;

	if (len == 1 && l != -1) {	/* single digit */
		subs->numbr = (AWKNUM) l;
		if ((subs->flags & USER_INPUT) != 0) {
			/* leave USER_INPUT set */
			subs->flags &= ~STRING;
			subs->flags |= NUMBER;
		}
		subs->flags |= (NUMCUR|NUMINT);
		return & success_node;
	}

	/* Try to parse a signed integer.  */
	if (l == -1 && (len == 1 || (l = char_digit_value((unsigned char) *++cp)) == -1))
		return NULL;

	while (++cp != cpend) {
		int i = char_digit_value((unsigned char) *cp);
		if (i == -1)
			return NULL;
		if (l <= (AWKLONGMAX - 9)/10) {
			l = l*10 + i;
			continue;
		}
		if (l > AWKLONGMAX/10)
			return NULL;	/* Out of range */
		l *= 10;
		if (i <= AWKLONGMAX - l) {
			l += i;
			continue;
		}
		/* Compile-time check that, for example, -128 + 1 == -127 */
		(void) sizeof(int[1-2*!(AWKLONGMIN + 1 == -AWKLONGMAX)]);
		if (--i == AWKLONGMAX - l &&
		    *subs->stptr == '-' &&
		    cp + 1 == cpend)
		{
			l = AWKLONGMIN;
			break;
		}
		return NULL;	/* Out of range */
	}

	if (l > 0 && *subs->stptr == '-')
		l = -l;

	subs->numbr = (AWKNUM) l;
	if ((subs->flags & USER_INPUT) != 0) {
		/* leave USER_INPUT set */
		subs->flags &= ~STRING;
		subs->flags |= NUMBER;
	}
	subs->flags |= NUMCUR;

	/* cint_array.c do not supports indexes larger than 32 bits */
	if (l <= INT32_MAX && l >= INT32_MIN) {
		subs->flags |= NUMINT;
		return & success_node;
	}

	return NULL;
#endif /* CHECK_INTEGER_USING_FORCE_NUMBER */
}


/*
 * int_lookup --- Find SYMBOL[SUBS] in the assoc array.  Install it with value ""
 * if it isn't there. Returns a pointer ala get_lhs to where its value is stored.
 */

static NODE **
int_lookup(NODE *symbol, NODE *subs)
{
	uint32_t hash1;
	awk_long_t k;
	size_t size;
	NODE **lhs;
	NODE *xn;

	/*
	 * N.B: symbol->table_size is the total # of non-integers (symbol->xarray)
	 *	and integer elements. Also, symbol->xarray must have at least one
	 *	item in it, and cannot exist if there are no integer elements.
	 * 	In that case, symbol->xarray is promoted to 'symbol' (See int_remove).
	 */


	if (! is_integer(symbol, subs)) {
		xn = symbol->xarray;
		if (xn == NULL) {
			xn = symbol->xarray = make_array();
			xn->vname = symbol->vname;	/* shallow copy */
			xn->flags |= XARRAY;
		} else if ((lhs = xn->aexists(xn, subs)) != NULL)
			return lhs;
		symbol->table_size++;
		return assoc_lookup(xn, subs);
	}

	k = (awk_long_t) subs->numbr;
	if (symbol->buckets == NULL)
		grow_int_table(symbol);

	hash1 = int_hash(k, symbol->array_size);
	if ((lhs = int_find(symbol, k, hash1)) != NULL)
		return lhs;

	/* It's not there, install it */

	symbol->table_size++;

	/* first see if we would need to grow the array, before installing */
	size = symbol->table_size;
	if ((xn = symbol->xarray) != NULL)
		size -= xn->table_size;

	if ((symbol->flags & ARRAYMAXED) == 0
		    && (size / symbol->array_size) > INT_CHAIN_MAX) {
		grow_int_table(symbol);
		/* have to recompute hash value for new size */
		hash1 = int_hash(k, symbol->array_size);
	}

	return int_insert(symbol, k, hash1);
}


/*
 * int_exists --- test whether the array element symbol[subs] exists or not,
 *	return pointer to value if it does.
 */

static NODE **
int_exists(NODE *symbol, NODE *subs)
{
	awk_long_t k;
	uint32_t hash1;

	if (! is_integer(symbol, subs)) {
		NODE *xn = symbol->xarray;
		if (xn == NULL)
			return NULL;
		return xn->aexists(xn, subs);
	}
	if (symbol->buckets == NULL)
		return NULL;

	k = (awk_long_t) subs->numbr;
	hash1 = int_hash(k, symbol->array_size);
	return int_find(symbol, k, hash1);
}

/* int_clear --- flush all the values in symbol[] */

static NODE **
int_clear(NODE *symbol, NODE *subs)
{
	size_t i, j;
	BUCKET *b, *next;
	NODE *r;
	(void) subs;

	if (symbol->xarray != NULL) {
		NODE *xn = symbol->xarray;
		assoc_clear(xn);
		freenode(xn);
		symbol->xarray = NULL;
	}

	for (i = 0; i < symbol->array_size; i++) {
		for (b = symbol->buckets[i]; b != NULL;	b = next) {
			next = b->ainext;
			for (j = 0; j < b->aicount; j++) {
				r = b->aivalue[j];
				if (r->type == Node_var_array) {
					assoc_clear(r);	/* recursively clear all sub-arrays */
					efree(r->vname);
					freenode(r);
				} else
					unref(r);
			}
			freebucket(b);
		}
		symbol->buckets[i] = NULL;
	}
	if (symbol->buckets != NULL)
		efree(symbol->buckets);
	symbol->ainit(symbol, NULL);	/* re-initialize symbol */
	return NULL;
}


/* int_remove --- If SUBS is already in the table, remove it. */

static NODE **
int_remove(NODE *symbol, NODE *subs)
{
	uint32_t hash1;
	BUCKET *b, *prev = NULL;
	awk_long_t k;
	size_t i;
	NODE *xn = symbol->xarray;

	if (!symbol->table_size || symbol->buckets == NULL)
		return NULL;

	if (! is_integer(symbol, subs)) {
		if (xn == NULL || xn->aremove(xn, subs) == NULL)
			return NULL;
		if (!xn->table_size) {
			freenode(xn);
			symbol->xarray = NULL;
		}
		assert(symbol->table_size > 1);
		symbol->table_size--;
		return & success_node;
	}

	k = (awk_long_t) subs->numbr;
	hash1 = int_hash(k, symbol->array_size);

	for (b = symbol->buckets[hash1]; b != NULL; prev = b, b = b->ainext) {
		for (i = 0; i < b->aicount; i++) {
			if (k != b->ainum[i])
				continue;

			/* item found */
			if (i == 0 && b->aicount == 2) {
				/* removing the 1st item; move 2nd item from position 1 to 0 */

				b->ainum[0] = b->ainum[1];
				b->aivalue[0] = b->aivalue[1];
			} /* else
				removing the only item or the 2nd item */

			goto removed;
		}
	}

	if (b == NULL)	/* item not in array */
		return NULL;

removed:
	b->aicount--;

	if (b->aicount == 0) {
		/* detach bucket */
		if (prev != NULL)
			prev->ainext = b->ainext;
		else
			symbol->buckets[hash1] = b->ainext;

		/* delete bucket */
		freebucket(b);
	} else if (b != symbol->buckets[hash1]) {
		BUCKET *head = symbol->buckets[hash1];

		assert(b->aicount == 1);
		/* move the last element from head to bucket to make it full. */
		i = --head->aicount;	/* head has one less element */
		b->ainum[1] = head->ainum[i];
		b->aivalue[1] = head->aivalue[i];
		b->aicount++;	/* bucket has one more element */
		if (i == 0) {
			/* head is now empty; delete head */
			symbol->buckets[hash1] = head->ainext;
			freebucket(head);
		}
	} /* else
		do nothing */

	symbol->table_size--;
	if (xn == NULL && !symbol->table_size) {
		efree(symbol->buckets);
		symbol->ainit(symbol, NULL);	/* re-initialize array 'symbol' */
	} else if (xn != NULL && symbol->table_size == xn->table_size) {
		/* promote xn (str_array) to symbol */
		xn->flags &= ~XARRAY;
		xn->parent_array = symbol->parent_array;
		efree(symbol->buckets);
		*symbol = *xn;
		freenode(xn);
	}

	return & success_node;	/* return success */
}


/* int_copy --- duplicate input array "symbol" */

static NODE **
int_copy(NODE *symbol, NODE *newsymb)
{
	BUCKET **old_b, **new_b, **pnew;
	BUCKET *chain, *newchain;
	size_t j, i, cursize;

	assert(symbol->buckets != NULL);

	/* find the current hash size */
	cursize = symbol->array_size;

	/* allocate new table */
	ezalloc(new_b, BUCKET **, cursize * sizeof(BUCKET *), "int_copy");

	old_b = symbol->buckets;

	for (i = 0; i < cursize; i++) {
		for (chain = old_b[i], pnew = & new_b[i]; chain != NULL;
				chain = chain->ainext
		) {
			getbucket(newchain);
			newchain->aicount = chain->aicount;
			newchain->ainext = NULL;
			for (j = 0; j < chain->aicount; j++) {
				NODE *oldval;

				/*
				 * copy the corresponding key and
				 * value from the original input list
				 */
				newchain->ainum[j] = chain->ainum[j];

				oldval = chain->aivalue[j];
				if (oldval->type == Node_val)
					newchain->aivalue[j] = dupnode(oldval);
				else {
					NODE *r;
					r = make_array();
					r->vname = estrdup(oldval->vname, strlen(oldval->vname));
					r->parent_array = newsymb;
					newchain->aivalue[j] = assoc_copy(oldval, r);
				}
			}

			*pnew = newchain;
			newchain->ainext = NULL;
			pnew = & newchain->ainext;
		}
	}

	if (symbol->xarray != NULL) {
		NODE *xn, *n;
		xn = symbol->xarray;
		n = make_array();
		n->vname = newsymb->vname;	/* shallow copy */
		(void) xn->acopy(xn, n);
		newsymb->xarray = n;
	} else
		newsymb->xarray = NULL;

	newsymb->table_size = symbol->table_size;
	newsymb->buckets = new_b;
	newsymb->array_size = cursize;
	newsymb->flags = symbol->flags;

	return NULL;
}


/* int_list --- return a list of array items */

static NODE**
int_list(NODE *symbol, NODE *t)
{
	NODE **list = NULL;
	size_t num_elems, i;
	size_t list_size, k = 0;
	BUCKET *b;
	NODE *r, *subs, *xn;
	size_t j;
	unsigned elem_size = 1;
	static char buf[100];
	assoc_kind_t assoc_kind;

	if (!symbol->table_size)
		return NULL;

	assoc_kind = (assoc_kind_t) t->flags;
	num_elems = symbol->table_size;
	if ((assoc_kind & (AINDEX|AVALUE|ADELETE)) == (AINDEX|ADELETE))
		num_elems = 1;

	if ((assoc_kind & (AINDEX|AVALUE)) == (AINDEX|AVALUE))
		elem_size = 2;
	list_size = elem_size * num_elems;

	if (symbol->xarray != NULL) {
		xn = symbol->xarray;
		list = xn->alist(xn, t);
		assert(list != NULL);
		if (num_elems == 1 || num_elems == xn->table_size)
			return list;
		erealloc(list, NODE **, list_size * sizeof(NODE *), "int_list");
		k = elem_size * xn->table_size;
	} else
		emalloc(list, NODE **, list_size * sizeof(NODE *), "int_list");

	/* populate it */

	for (i = 0; i < symbol->array_size; i++) {
		for (b = symbol->buckets[i]; b != NULL;	b = b->ainext) {
			for (j = 0; j < b->aicount; j++) {
				/* index */
				awk_long_t num = b->ainum[j];
				if ((assoc_kind & AISTR) != 0) {
					sprintf(buf, "%" AWKLONGFMT "", TO_AWK_LONG(num));
					subs = make_string(buf, strlen(buf));
					subs->numbr = (AWKNUM) num;
					subs->flags |= (NUMCUR|NUMINT);
				} else {
					subs = make_number((AWKNUM) num);
					subs->flags |= (INTIND|NUMINT);
				}
				list[k++] = subs;

				/* value */
				if ((assoc_kind & AVALUE) != 0) {
					r = b->aivalue[j];
					if (r->type == Node_val) {
						if ((assoc_kind & AVNUM) != 0)
							(void) force_number(r);
						else if ((assoc_kind & AVSTR) != 0)
							r = force_string(r);
					}
					list[k++] = r;
				}

				if (k >= list_size)
					return list;
			}
		}
	}
	return list;
}


/* int_kilobytes --- calculate memory consumption of the assoc array */

AWKNUM
int_kilobytes(NODE *symbol)
{
	size_t i;
	awk_ulong_t bucket_cnt = 0u;
	BUCKET *b;
	AWKNUM kb;

	for (i = 0; i < symbol->array_size; i++) {
		for (b = symbol->buckets[i]; b != NULL; b = b->ainext)
			bucket_cnt++;
	}
	kb = (((AWKNUM) bucket_cnt) * sizeof (BUCKET) +
			((AWKNUM) symbol->array_size) * sizeof (BUCKET *)) / 1024.0;

	if (symbol->xarray != NULL)
		kb += str_kilobytes(symbol->xarray);

	return kb;
}


/* int_dump --- dump array info */

static NODE **
int_dump(NODE *symbol, NODE *ndump)
{
#define HCNT	31

	unsigned indent_level;
	BUCKET *b;
	NODE *xn = NULL;
	size_t i, str_size = 0, int_size = 0;
	unsigned k;
	size_t hash_dist[HCNT + 1];

	indent_level = ndump->alevel;

	if (symbol->xarray != NULL) {
		xn = symbol->xarray;
		str_size = xn->table_size;
	}
	int_size = symbol->table_size - str_size;

	if ((symbol->flags & XARRAY) == 0)
		fprintf(output_fp, "%s `%s'\n",
				(symbol->parent_array == NULL) ? "array" : "sub-array",
				array_vname(symbol));

	indent_level++;
	indent(indent_level);
	fprintf(output_fp, "array_func: int_array_func\n");
	if (symbol->flags != 0) {
		indent(indent_level);
		fprintf(output_fp, "flags: %s\n", flags2str(symbol->flags));
	}
	indent(indent_level);
	fprintf(output_fp, "INT_CHAIN_MAX: %" AWKULONGFMT "\n", TO_AWK_ULONG(INT_CHAIN_MAX));
	indent(indent_level);
	fprintf(output_fp, "array_size: %" ZUFMT " (int)\n", symbol->array_size);
	indent(indent_level);
	fprintf(output_fp, "table_size: %" ZUFMT " (total), %" ZUFMT " (int), %" ZUFMT " (str)\n",
			symbol->table_size, int_size, str_size);
	indent(indent_level);
	fprintf(output_fp, "Avg # of items per chain (int): %.2g\n",
			((AWKNUM) int_size) / (AWKNUM) symbol->array_size);

	indent(indent_level);
	fprintf(output_fp, "memory: %.2g kB (total)\n", int_kilobytes(symbol));

	/* hash value distribution */

	memset(hash_dist, 0, sizeof(hash_dist));
	for (i = 0; i < symbol->array_size; i++) {
		size_t bucket_cnt = 0;
		for (b = symbol->buckets[i]; b != NULL;	b = b->ainext) {
			bucket_cnt += b->aicount;
			if (bucket_cnt >= HCNT) {
				bucket_cnt = HCNT;
				break;
			}
		}
		hash_dist[bucket_cnt]++;
	}

	indent(indent_level);
	fprintf(output_fp, "Hash distribution:\n");
	indent_level++;
	for (k = 0; k <= HCNT; k++) {
		if (hash_dist[k] > 0) {
			indent(indent_level);
			if (k == HCNT)
				fprintf(output_fp, "[>=%d]:%" ZUFMT "\n",
					HCNT, hash_dist[k]);
			else
				fprintf(output_fp, "[%u]:%" ZUFMT "\n",
					k, hash_dist[k]);
		}
	}
	indent_level--;

	/* dump elements */

	if (ndump->adepth >= 0) {
		NODE *subs;
		const char *aname;

		fprintf(output_fp, "\n");

		aname = make_aname(symbol);
		subs = make_number((AWKNUM) 0);
		subs->flags |= (INTIND|NUMINT);

		for (i = 0; i < symbol->array_size; i++) {
			for (b = symbol->buckets[i]; b != NULL; b = b->ainext) {
				size_t j = 0;
				for (; j < b->aicount; j++) {
					subs->numbr = (AWKNUM) b->ainum[j];
					assoc_info(subs, b->aivalue[j], ndump, aname);
				}
			}
		}
		unref(subs);
	}

	if (xn != NULL)	{
		fprintf(output_fp, "\n");
		xn->adump(xn, ndump);
	}

	return NULL;

#undef HCNT
}


/* int_hash --- calculate the hash function of the integer subs */

static uint32_t
int_hash(awk_long_t k, size_t hsize)
{
	uint32_t h = (uint32_t) k;

/*
 * Code snippet copied from:
 *	Hash functions (http://www.azillionmonkeys.com/qed/hash.html).
 *	Copyright 2004-2008 by Paul Hsieh. Licenced under LGPL 2.1.
 */

	/* This is the final mixing function used by Paul Hsieh in SuperFastHash. */

	h ^= h << 3;
	h += h >> 5;
	h ^= h << 4;
	h += h >> 17;
	h ^= h << 25;
	h += h >> 6;

	if (h >= hsize)
		h %= (uint32_t) hsize;
	return h;
}

/* int_find --- locate symbol[subs] */

static inline NODE **
int_find(NODE *symbol, awk_long_t k, uint32_t hash1)
{
	BUCKET *b;
	size_t i;

	assert(symbol->buckets != NULL);
	for (b = symbol->buckets[hash1]; b != NULL; b = b->ainext) {
		for (i = 0; i < b->aicount; i++) {
			if (b->ainum[i] == k)
				return (b->aivalue + i);
		}
	}
	return NULL;
}


/* int_insert --- install subs in the assoc array */

static NODE **
int_insert(NODE *symbol, awk_long_t k, uint32_t hash1)
{
	BUCKET *b;
	size_t i;

	b = symbol->buckets[hash1];

	/* Only the first bucket in the chain can be partially full, but is never empty. */

	if (b == NULL || (i = b->aicount) == 2) {
		getbucket(b);
		b->aicount = 0;
		b->ainext = symbol->buckets[hash1];
		symbol->buckets[hash1] = b;
		i = 0;
	}

	b->ainum[i] = k;
	b->aivalue[i] = dupnode(Nnull_string);
	b->aicount++;
	return & b->aivalue[i];
}


/* grow_int_table --- grow the hash table */

static void
grow_int_table(NODE *symbol)
{
	BUCKET **old_b, **new_b;
	BUCKET *chain, *next;
	size_t i;
	size_t oldsize, newsize, k;

	/*
	 * This is an array of primes. We grow the table by an order of
	 * magnitude each time (not just doubling) so that growing is a
	 * rare operation. We expect, on average, that it won't happen
	 * more than twice.  The final size is also chosen to be small
	 * enough so that MS-DOS mallocs can handle it. When things are
	 * very large (> 8K), we just double more or less, instead of
	 * just jumping from 8K to 64K.
	 */

	static const unsigned long sizes[] = {
		13, 127, 1021, 8191, 16381, 32749, 65497,
		131101, 262147, 524309, 1048583, 2097169,
		4194319, 8388617, 16777259, 33554467,
		67108879, 134217757, 268435459, 536870923,
		1073741827
	};

	/* find next biggest hash size */
	newsize = oldsize = symbol->array_size;

	for (i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
		if (oldsize < (size_t) sizes[i]) {
			newsize = (size_t) sizes[i];
			break;
		}
	}
	if (newsize == oldsize) {	/* table already at max (!) */
		symbol->flags |= ARRAYMAXED;
		return;
	}

	/* allocate new table */
	ezalloc(new_b, BUCKET **, newsize * sizeof(BUCKET *), "grow_int_table");

	old_b = symbol->buckets;
	symbol->buckets = new_b;
	symbol->array_size = newsize;

	/* brand new hash table */
	if (old_b == NULL)
		return;		/* DO NOT initialize symbol->table_size */

	/* old hash table there, move stuff to new, free old */
	/* note that symbol->table_size does not change if an old array. */

	for (k = 0; k < oldsize; k++) {
		for (chain = old_b[k]; chain != NULL; chain = next) {
			for (i = 0; i < chain->aicount; i++) {
				const awk_long_t num = chain->ainum[i];
				*int_insert(symbol, num, int_hash(num, newsize)) = chain->aivalue[i];
			}
			next = chain->ainext;
			freebucket(chain);
		}
	}
	efree(old_b);
}
