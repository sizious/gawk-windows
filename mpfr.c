/*
 * mpfr.c - routines for arbitrary-precision number support in gawk.
 */

/* 
 * Copyright (C) 2012 the Free Software Foundation, Inc.
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

#ifdef HAVE_MPFR

#if !defined(MPFR_VERSION_MAJOR) || MPFR_VERSION_MAJOR < 3
typedef mp_exp_t mpfr_exp_t;
#endif

extern NODE **fmt_list;          /* declared in eval.c */

mpz_t mpzval;	/* GMP integer type, used as temporary in few places */
mpz_t MNR;
mpz_t MFNR;
int do_ieee_fmt;	/* IEEE-754 floating-point emulation */
mpfr_rnd_t ROUND_MODE;

static mpfr_rnd_t get_rnd_mode(const char rmode);
static NODE *mpg_force_number(NODE *n);
static NODE *mpg_make_number(double);
static NODE *mpg_format_val(const char *format, int index, NODE *s);
static int mpg_interpret(INSTRUCTION **cp);

static mpfr_exp_t min_exp = MPFR_EMIN_DEFAULT;
static mpfr_exp_t max_exp = MPFR_EMAX_DEFAULT;

/* temporaries used in bit ops */
static NODE *_tz1;
static NODE *_tz2;
static mpz_t _mpz1;
static mpz_t _mpz2;
static mpz_ptr mpz1;
static mpz_ptr mpz2;

static NODE *get_bit_ops(const char *op);
#define free_bit_ops()	(DEREF(_tz1), DEREF(_tz2))

/* temporary MPFR floats used to hold converted GMP integer operands */
static mpfr_t _mpf_t1;
static mpfr_t _mpf_t2;

/*
 * PRECISION_MIN is the precision used to initialize _mpf_t1 and _mpf_t2.
 * 64 bits should be enough for exact conversion of most integers to floats.
 */

#define PRECISION_MIN	64

/* mf = { _mpf_t1, _mpf_t2 } */
static inline mpfr_ptr mpg_tofloat(mpfr_ptr mf, mpz_ptr mz);
/* T = {t1, t2} */
#define MP_FLOAT(T) is_mpg_integer(T) ? mpg_tofloat(_mpf_##T, (T)->mpg_i) : (T)->mpg_numbr


/* init_mpfr --- set up MPFR related variables */

void
init_mpfr(mpfr_prec_t prec, const char *rmode)
{
	mpfr_set_default_prec(prec);
	ROUND_MODE = get_rnd_mode(rmode[0]);
	mpfr_set_default_rounding_mode(ROUND_MODE);
	make_number = mpg_make_number;
	str2number = mpg_force_number;
	format_val = mpg_format_val;
	cmp_numbers = mpg_cmp;

	mpz_init(MNR);
	mpz_init(MFNR);
	do_ieee_fmt = FALSE;

	mpz_init(_mpz1);
	mpz_init(_mpz2);
	mpfr_init2(_mpf_t1, PRECISION_MIN);
	mpfr_init2(_mpf_t2, PRECISION_MIN);
	mpz_init(mpzval);

	register_exec_hook(mpg_interpret, 0);
}

/* mpg_node --- allocate a node to store MPFR float or GMP integer */

NODE *
mpg_node(unsigned int tp)
{
	NODE *r;
	getnode(r);
	r->type = Node_val;

	if (tp == MPFN) {
		/* Initialize, set precision to the default precision, and value to NaN */
		mpfr_init(r->mpg_numbr);
		r->flags = MPFN;
	} else {
		/* Initialize and set value to 0 */
		mpz_init(r->mpg_i);
		r->flags = MPZN;
	}
	
	r->valref = 1;
	r->flags |= MALLOC|NUMBER|NUMCUR;
	r->stptr = NULL;
	r->stlen = 0;
#if MBS_SUPPORT
	r->wstptr = NULL;
	r->wstlen = 0;
#endif /* defined MBS_SUPPORT */
	return r;
}

/*
 * mpg_make_number --- make a arbitrary-precision number node
 *	and initialize with a C double
 */

static NODE *
mpg_make_number(double x)
{
	NODE *r;
	double ival;

	if ((ival = double_to_int(x)) != x) {
		int tval;
		r = mpg_float();
		tval = mpfr_set_d(r->mpg_numbr, x, ROUND_MODE);
		IEEE_FMT(r->mpg_numbr, tval);
	} else {
		r = mpg_integer();
		mpz_set_d(r->mpg_i, ival);
	}
	return r;
}

/* mpg_strtoui --- assign arbitrary-precision integral value from a string */ 

int
mpg_strtoui(mpz_ptr zi, char *str, size_t len, char **end, int base)
{
	char *s = str;
	char *start;
	int ret = -1;

	/*
	 * mpz_set_str does not like leading 0x or 0X for hex (or 0 for octal)
	 * with a non-zero base argument.
	*/
	if (base == 16 && len >= 2 && *s == '0' && (s[1] == 'x' || s[1] == 'X')) {
		s += 2; len -= 2;
	} else if (base == 8 && len >= 1 && *s == '0') {
		s++; len--;
	}
	start = s;

	while (len > 0) {
		switch (*s) {
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
			break;
		case '8':
		case '9':
			if (base == 8)
				goto done;
			break;
		case 'a':
		case 'b':
		case 'c':
		case 'd':
		case 'e':
		case 'f':
		case 'A':
		case 'B':
		case 'C':
		case 'D':
		case 'E':
		case 'F':
			if (base == 16)
				break;
		default:
			goto done;
		}
		s++; len--;
	}
done:
	if (s > start) {
		char save = *s;
		*s = '\0';
		ret = mpz_set_str(zi, start, base);
		*s = save;
	}
	if (end != NULL)
		*end = s;
	return ret;
}


/* mpg_maybe_float --- test if a string may contain arbitrary-precision float */

static int
mpg_maybe_float(const char *str, int use_locale)
{
	int dec_point = '.';
	const char *s = str;

#if defined(HAVE_LOCALE_H)
	/*
	 * loc.decimal_point may not have been initialized yet,
	 * so double check it before using it.
	 */
	if (use_locale && loc.decimal_point != NULL && loc.decimal_point[0] != '\0')
		dec_point = loc.decimal_point[0];	/* XXX --- assumes one char */
#endif

	if (strlen(s) >= 3
		&& (   (   (s[0] == 'i' || s[0] == 'I')
			&& (s[1] == 'n' || s[1] == 'N')
			&& (s[2] == 'f' || s[2] == 'F'))
		    || (   (s[0] == 'n' || s[0] == 'N')
			&& (s[1] == 'a' || s[1] == 'A')
			&& (s[2] == 'n' || s[2] == 'N'))))
		return TRUE;

	for (; *s != '\0'; s++) {
		if (*s == dec_point || *s == 'e' || *s == 'E')
			return TRUE;
	}

	return FALSE;
}


/* mpg_zero --- initialize with arbitrary-precision integer(GMP) and set value to zero */

static inline void
mpg_zero(NODE *n)
{
	if (is_mpg_float(n)) {
		mpfr_clear(n->mpg_numbr);
		n->flags &= ~MPFN;
	}
	if (! is_mpg_integer(n)) {
		mpz_init(n->mpg_i);	/* this also sets its value to 0 */ 
		n->flags |= MPZN;
	} else
		mpz_set_si(n->mpg_i, 0);
}


/* force_mpnum --- force a value to be a GMP integer or MPFR float */

static int
force_mpnum(NODE *n, int do_nondec, int use_locale)
{
	char *cp, *cpend, *ptr, *cp1;
	char save;
	int tval, base = 10;

	if (n->stlen == 0) {
		mpg_zero(n);
		return FALSE;
	}

	cp = n->stptr;
	cpend = n->stptr + n->stlen;
	while (cp < cpend && isspace((unsigned char) *cp))
		cp++;
	if (cp == cpend) {	/* only spaces */
		mpg_zero(n);
		return FALSE;
	}

	save = *cpend;
	*cpend = '\0';

	if (*cp == '+' || *cp == '-')
		cp1 = cp + 1;
	else
		cp1 = cp;

	if (do_nondec)
		base = get_numbase(cp1, use_locale);

	if (! mpg_maybe_float(cp1, use_locale)) {
		mpg_zero(n);
		errno = 0;
		mpg_strtoui(n->mpg_i, cp1, cpend - cp1, & ptr, base);
		if (*cp == '-')
			mpz_neg(n->mpg_i, n->mpg_i);
		goto done;
	}

	if (is_mpg_integer(n)) {
		mpz_clear(n->mpg_i);
		n->flags &= ~MPZN;
	}

	if (! is_mpg_float(n)) {
		mpfr_init(n->mpg_numbr);
		n->flags |= MPFN;
	}

	errno = 0;
	tval = mpfr_strtofr(n->mpg_numbr, cp, & ptr, base, ROUND_MODE);
	IEEE_FMT(n->mpg_numbr, tval);
done:
	/* trailing space is OK for NUMBER */
	while (isspace((unsigned char) *ptr))
		ptr++;
	*cpend = save;
	if (errno == 0 && ptr == cpend)
		return TRUE;
	errno = 0;
	return FALSE; 
}

/* mpg_force_number --- force a value to be a multiple-precision number */

static NODE *
mpg_force_number(NODE *n)
{
	unsigned int newflags = 0;

	if (is_mpg_number(n) && (n->flags & NUMCUR))
		return n;

	if (n->flags & MAYBE_NUM) {
		n->flags &= ~MAYBE_NUM;
		newflags = NUMBER;
	}

	if (force_mpnum(n, (do_non_decimal_data && ! do_traditional), TRUE)) {
		n->flags |= newflags;
		n->flags |= NUMCUR;
	}
	return n;
}

/* mpg_format_val --- format a numeric value based on format */

static NODE *
mpg_format_val(const char *format, int index, NODE *s)
{
	NODE *dummy[2], *r;
	unsigned int oflags;

	/* create dummy node for a sole use of format_tree */
	dummy[1] = s;
	oflags = s->flags;

	if (is_mpg_integer(s) || mpfr_integer_p(s->mpg_numbr)) {
		/* integral value, use %d */
		r = format_tree("%d", 2, dummy, 2);
		s->stfmt = -1;
	} else {
		r = format_tree(format, fmt_list[index]->stlen, dummy, 2);
		assert(r != NULL);
		s->stfmt = (char) index;
	}
	s->flags = oflags;
	s->stlen = r->stlen;
	if ((s->flags & STRCUR) != 0)
		efree(s->stptr);
	s->stptr = r->stptr;
	freenode(r);	/* Do not unref(r)! We want to keep s->stptr == r->stpr.  */
 
	s->flags |= STRCUR;
	free_wstr(s);
	return s;
}

/* mpg_cmp --- compare two numbers */

int
mpg_cmp(const NODE *t1, const NODE *t2)
{
	/*
	 * For the purposes of sorting, NaN is considered greater than
	 * any other value, and all NaN values are considered equivalent and equal.
	 */

	if (is_mpg_float(t1)) {
		if (is_mpg_float(t2)) {
			if (mpfr_nan_p(t1->mpg_numbr))
				return ! mpfr_nan_p(t2->mpg_numbr);
			if (mpfr_nan_p(t2->mpg_numbr))
				return -1;
			return mpfr_cmp(t1->mpg_numbr, t2->mpg_numbr);
		}
		if (mpfr_nan_p(t1->mpg_numbr))
			return 1;
		return mpfr_cmp_z(t1->mpg_numbr, t2->mpg_i);
	} else if (is_mpg_float(t2)) {
		int ret;
		if (mpfr_nan_p(t2->mpg_numbr))
			return -1;
		ret = mpfr_cmp_z(t2->mpg_numbr, t1->mpg_i);
		return ret > 0 ? -1 : (ret < 0);
	} else if (is_mpg_integer(t1)) {
		return mpz_cmp(t1->mpg_i, t2->mpg_i);
	}

	/* t1 and t2 are AWKNUMs */
	return cmp_awknums(t1, t2);
}


/*
 * mpg_update_var --- update NR or FNR. 
 *	NR_node->var_value(mpz_t) = MNR(mpz_t) * LONG_MAX + NR(long) 
 */

NODE *
mpg_update_var(NODE *n)
{
	NODE *val = n->var_value;
	long nr = 0;
	mpz_ptr nq = 0;

	if (n == NR_node) {
		nr = NR;
		nq = MNR;
	} else if (n == FNR_node) {
		nr = FNR;
		nq = MFNR;
	} else
		cant_happen();

	if (mpz_sgn(nq) == 0) {
		/* Efficiency hack similar to that for AWKNUM */
		if (is_mpg_float(val) || mpz_get_si(val->mpg_i) != nr) {
			unref(n->var_value);
			val = n->var_value = mpg_integer();
			mpz_set_si(val->mpg_i, nr);
		}
	} else {
		unref(n->var_value);
		val = n->var_value = mpg_integer();
		mpz_set_si(val->mpg_i, nr);
		mpz_addmul_ui(val->mpg_i, nq, LONG_MAX);	/* val->mpg_i += nq * LONG_MAX */
	}
	return val;
}

/* mpg_set_var --- set NR or FNR */

long
mpg_set_var(NODE *n)
{
	long nr = 0;
	mpz_ptr nq = 0, r;
	NODE *val = n->var_value;

	if (n == NR_node)
		nq = MNR;
	else if (n == FNR_node)
		nq = MFNR;
	else
		cant_happen();

	if (is_mpg_integer(val))
		r = val->mpg_i;
	else {
		/* convert float to integer */ 
		mpfr_get_z(mpzval, val->mpg_numbr, MPFR_RNDZ);
		r = mpzval;
	}
	nr = mpz_fdiv_q_ui(nq, r, LONG_MAX);	/* nq (MNR or MFNR) is quotient */
	return nr;	/* remainder (NR or FNR) */
}

/* set_PREC --- update MPFR PRECISION related variables when PREC assigned to */

void
set_PREC()
{
	long prec = 0;
	NODE *val;
	static const struct ieee_fmt {
		const char *name;
		mpfr_prec_t precision;
		mpfr_exp_t emax;
		mpfr_exp_t emin;
	} ieee_fmts[] = {
{ "half",	11,	16,	-23	},	/* binary16 */
{ "single",	24,	128,	-148	},	/* binary32 */
{ "double",	53,	1024,	-1073	},	/* binary64 */
{ "quad",	113,	16384,	-16493	},	/* binary128 */
{ "oct",	237,	262144,	-262377	},	/* binary256, not in the IEEE 754-2008 standard */

		/*
 		 * For any bitwidth = 32 * k ( k >= 4),
 		 * precision = 13 + bitwidth - int(4 * log2(bitwidth))
		 * emax = 1 << bitwidth - precision - 1
		 * emin = 4 - emax - precision
 		 */
	};

	if (! do_mpfr)
		return;

	val = PREC_node->var_value;
	if (val->flags & MAYBE_NUM)
		force_number(val);

	if ((val->flags & (STRING|NUMBER)) == STRING) {
		int i, j;

		/* emulate IEEE-754 binary format */

		for (i = 0, j = sizeof(ieee_fmts)/sizeof(ieee_fmts[0]); i < j; i++) {
			if (strcasecmp(ieee_fmts[i].name, val->stptr) == 0)
				break;
		}

		if (i < j) {
			prec = ieee_fmts[i].precision;

			/*
			 * We *DO NOT* change the MPFR exponent range using
			 * mpfr_set_{emin, emax} here. See format_ieee() for details.
			 */
			max_exp = ieee_fmts[i].emax;
			min_exp = ieee_fmts[i].emin;

			do_ieee_fmt = TRUE;
		}
	}

	if (prec <= 0) {
		force_number(val);
		prec = get_number_si(val);		
		if (prec < MPFR_PREC_MIN || prec > MPFR_PREC_MAX) {
			force_string(val);
			warning(_("PREC value `%.*s' is invalid"), (int) val->stlen, val->stptr);
			prec = 0;
		} else
			do_ieee_fmt = FALSE;
	}

	if (prec > 0)
		mpfr_set_default_prec(prec);
}


/* get_rnd_mode --- convert string to MPFR rounding mode */

static mpfr_rnd_t
get_rnd_mode(const char rmode)
{
	switch (rmode) {
	case 'N':
	case 'n':
		return MPFR_RNDN;	/* round to nearest (IEEE-754 roundTiesToEven) */
	case 'Z':
	case 'z':
		return MPFR_RNDZ;	/* round toward zero (IEEE-754 roundTowardZero) */
	case 'U':
	case 'u':
		return MPFR_RNDU;	/* round toward plus infinity (IEEE-754 roundTowardPositive) */
	case 'D':
	case 'd':
		return MPFR_RNDD;	/* round toward minus infinity (IEEE-754 roundTowardNegative) */
#if defined(MPFR_VERSION_MAJOR) && MPFR_VERSION_MAJOR > 2
	case 'A':
	case 'a':
		return MPFR_RNDA;	/* round away from zero (IEEE-754 roundTiesToAway) */
#endif
	default:
		break;
	}
	return -1;
}

/*
 * set_ROUNDMODE --- update MPFR rounding mode related variables
 *	when ROUNDMODE assigned to
 */

void
set_ROUNDMODE()
{
	if (do_mpfr) {
		mpfr_rnd_t rndm = -1;
		NODE *n;
		n = force_string(ROUNDMODE_node->var_value);
		if (n->stlen == 1)
			rndm = get_rnd_mode(n->stptr[0]);
		if (rndm != -1) {
			mpfr_set_default_rounding_mode(rndm);
			ROUND_MODE = rndm;
		} else
			warning(_("RNDMODE value `%.*s' is invalid"), (int) n->stlen, n->stptr);
	}
}


/* format_ieee --- make sure a number follows IEEE-754 floating-point standard */

int
format_ieee(mpfr_ptr x, int tval)
{
	/*
	 * The MPFR doc says that it's our responsibility to make sure all numbers
	 * including those previously created are in range after we've changed the
	 * exponent range. Most MPFR operations and functions require
	 * the input arguments to have exponents within the current exponent range.
	 * Any argument outside the range results in a MPFR assertion failure
	 * like this:
	 *
	 *   $ gawk -M 'BEGIN { x=1.0e-10000; print x+0; PREC="double"; print x+0}'
	 *   1e-10000
	 *   init2.c:52: MPFR assertion failed ....
	 *
	 * A "naive" approach would be to keep track of the ternary state and
	 * the rounding mode for each number, and make sure it is in the current
	 * exponent range (using mpfr_check_range) before using it in an
	 * operation or function. Instead, we adopt the following strategy.
	 *
	 * When gawk starts, the exponent range is the MPFR default
	 * [MPFR_EMIN_DEFAULT, MPFR_EMAX_DEFAULT]. Any number that gawk
	 * creates must have exponent in this range (excluding infinities, NaNs and zeros).
	 * Each MPFR operation or function is performed with this default exponent
	 * range.
	 *
	 * When emulating IEEE-754 format, the exponents are *temporarily* changed,
	 * mpfr_check_range is called to make sure the number is in the new range,
	 * and mpfr_subnormalize is used to round following the rules of subnormal
	 * arithmetic. The exponent range is then *restored* to the original value
	 * [MPFR_EMIN_DEFAULT, MPFR_EMAX_DEFAULT].
	 */

	(void) mpfr_set_emin(min_exp);
	(void) mpfr_set_emax(max_exp);
	tval = mpfr_check_range(x, tval, ROUND_MODE);
	tval = mpfr_subnormalize(x, tval, ROUND_MODE);
	(void) mpfr_set_emin(MPFR_EMIN_DEFAULT);
	(void) mpfr_set_emax(MPFR_EMAX_DEFAULT);
	return tval;
}


/* do_mpfr_atan2 --- do the atan2 function */

NODE *
do_mpfr_atan2(int nargs)
{
	NODE *t1, *t2, *res;
	mpfr_ptr p1, p2;
	int tval;

	t2 = POP_SCALAR();
	t1 = POP_SCALAR();

	if (do_lint) {
		if ((t1->flags & (NUMCUR|NUMBER)) == 0)
			lintwarn(_("atan2: received non-numeric first argument"));
		if ((t2->flags & (NUMCUR|NUMBER)) == 0)
			lintwarn(_("atan2: received non-numeric second argument"));
	}
	force_number(t1);
	force_number(t2);

	p1 = MP_FLOAT(t1);
	p2 = MP_FLOAT(t2);
	res = mpg_float();
	/* See MPFR documentation for handling of special values like +inf as an argument */ 
	tval = mpfr_atan2(res->mpg_numbr, p1, p2, ROUND_MODE);
	IEEE_FMT(res->mpg_numbr, tval);

	DEREF(t1);
	DEREF(t2);
	return res;
}


#define SPEC_MATH(X)						\
NODE *t1, *res;							\
mpfr_ptr p1;							\
int tval;							\
t1 = POP_SCALAR();						\
if (do_lint && (t1->flags & (NUMCUR|NUMBER)) == 0)		\
	lintwarn(_("%s: received non-numeric argument"), #X);	\
force_number(t1);						\
p1 = MP_FLOAT(t1);						\
res = mpg_float();						\
tval = mpfr_##X(res->mpg_numbr, p1, ROUND_MODE);			\
IEEE_FMT(res->mpg_numbr, tval);					\
DEREF(t1);							\
return res


/* do_mpfr_sin --- do the sin function */

NODE *
do_mpfr_sin(int nargs)
{
	SPEC_MATH(sin);
}

/* do_mpfr_cos --- do the cos function */

NODE *
do_mpfr_cos(int nargs)
{
	SPEC_MATH(cos);
}

/* do_mpfr_exp --- exponential function */

NODE *
do_mpfr_exp(int nargs)
{
	SPEC_MATH(exp);
}

/* do_mpfr_log --- the log function */

NODE *
do_mpfr_log(int nargs)
{
	SPEC_MATH(log);
}

/* do_mpfr_sqrt --- do the sqrt function */

NODE *
do_mpfr_sqrt(int nargs)
{
	SPEC_MATH(sqrt);
}

/* do_mpfr_int --- convert double to int for awk */

NODE *
do_mpfr_int(int nargs)
{
	NODE *tmp, *r;

	tmp = POP_SCALAR();
	if (do_lint && (tmp->flags & (NUMCUR|NUMBER)) == 0)
		lintwarn(_("int: received non-numeric argument"));
	force_number(tmp);

	if (is_mpg_integer(tmp)) {
		r = mpg_integer();
		mpz_set(r->mpg_i, tmp->mpg_i);
	} else {
		if (! mpfr_number_p(tmp->mpg_numbr)) {
			/* [+-]inf or NaN */
			return tmp;
		}

		r = mpg_integer();
		mpfr_get_z(r->mpg_i, tmp->mpg_numbr, MPFR_RNDZ);
	}

	DEREF(tmp);
	return r;
}

/* do_mpfr_compl --- perform a ~ operation */

NODE *
do_mpfr_compl(int nargs)
{
	NODE *tmp, *r;
	mpz_ptr zptr;

	tmp = POP_SCALAR();
	if (do_lint && (tmp->flags & (NUMCUR|NUMBER)) == 0)
		lintwarn(_("compl: received non-numeric argument"));

	force_number(tmp);
	if (is_mpg_float(tmp)) {
		mpfr_ptr p = tmp->mpg_numbr;

		if (! mpfr_number_p(p)) {
			/* [+-]inf or NaN */
			return tmp;
		}
		if (do_lint) {
			if (mpfr_sgn(p) < 0)
				lintwarn("%s",
			mpg_fmt(_("compl(%Rg): negative value will give strange results"), p)
					);
			if (! mpfr_integer_p(p))
				lintwarn("%s",
			mpg_fmt(_("comp(%Rg): fractional value will be truncated"), p)
					);
		}
		
		mpfr_get_z(mpzval, p, MPFR_RNDZ);	/* float to integer conversion */
		zptr = mpzval;
	} else {
		/* (tmp->flags & MPZN) != 0 */ 
		zptr = tmp->mpg_i;
		if (do_lint) {
			if (mpz_sgn(zptr) < 0)
				lintwarn("%s",
			mpg_fmt(_("cmpl(%Zd): negative values will give strange results"), zptr)
					);
		}
	}

	r = mpg_integer();
	mpz_com(r->mpg_i, zptr);
	DEREF(tmp);
	return r;
}


/*
 * get_bit_ops --- get the numeric operands of a binary function.
 *	Returns a copy of the operand if either is inf or nan. Otherwise
 * 	each operand is converted to an integer if necessary, and
 * 	the results are placed in the variables mpz1 and mpz2.
 */

static NODE *
get_bit_ops(const char *op)
{
	_tz2 = POP_SCALAR();
	_tz1 = POP_SCALAR();

	if (do_lint) {
		if ((_tz1->flags & (NUMCUR|NUMBER)) == 0)
			lintwarn(_("%s: received non-numeric first argument"), op);
		if ((_tz2->flags & (NUMCUR|NUMBER)) == 0)
			lintwarn(_("%s: received non-numeric second argument"), op);
	}

	force_number(_tz1);
	force_number(_tz2);

	if (is_mpg_float(_tz1)) {
		mpfr_ptr left = _tz1->mpg_numbr;
		if (! mpfr_number_p(left)) {
			/* inf or NaN */
			NODE *res;
			res = mpg_float();
			mpfr_set(res->mpg_numbr, _tz1->mpg_numbr, ROUND_MODE);
			return res;
		}

		if (do_lint) {
			if (mpfr_sgn(left) < 0)
				lintwarn("%s",
			mpg_fmt(_("%s(%Rg, ..): negative values will give strange results"),
						op, left)
					);
			if (! mpfr_integer_p(left))
				lintwarn("%s",
			mpg_fmt(_("%s(%Rg, ..): fractional values will be truncated"),
						op, left)
				);
		}
		
		mpfr_get_z(_mpz1, left, MPFR_RNDZ);	/* float to integer conversion */
		mpz1 = _mpz1;
	} else {
		/* (_tz1->flags & MPZN) != 0 */ 
		mpz1 = _tz1->mpg_i;
		if (do_lint) {
			if (mpz_sgn(mpz1) < 0)
				lintwarn("%s",
			mpg_fmt(_("%s(%Zd, ..): negative values will give strange results"),
						op, mpz1)
					);
		}
	}

	if (is_mpg_float(_tz2)) {
		mpfr_ptr right = _tz2->mpg_numbr;
		if (! mpfr_number_p(right)) {
			/* inf or NaN */
			NODE *res;
			res = mpg_float();
			mpfr_set(res->mpg_numbr, _tz2->mpg_numbr, ROUND_MODE);
			return res;
		}

		if (do_lint) {
			if (mpfr_sgn(right) < 0)
				lintwarn("%s",
			mpg_fmt(_("%s(.., %Rg): negative values will give strange results"),
						op, right)
					);
			if (! mpfr_integer_p(right))
				lintwarn("%s",
			mpg_fmt(_("%s(.., %Rg): fractional values will be truncated"),
						op, right)
				);
		}

		mpfr_get_z(_mpz2, right, MPFR_RNDZ);	/* float to integer conversion */
		mpz2 = _mpz2;
	} else {
		/* (_tz2->flags & MPZN) != 0 */ 
		mpz2 = _tz2->mpg_i;
		if (do_lint) {
			if (mpz_sgn(mpz2) < 0)
				lintwarn("%s",
			mpg_fmt(_("%s(.., %Zd): negative values will give strange results"),
						op, mpz2)
					);
		}
	}

	return NULL;
}

/* do_mpfr_lshift --- perform a << operation */

NODE *
do_mpfr_lshift(int nargs)
{
	NODE *res;
	unsigned long shift;

	if ((res = get_bit_ops("lshift")) == NULL) {

		/*
		 * mpz_get_ui: If op is too big to fit an unsigned long then just
		 * the least significant bits that do fit are returned.
		 * The sign of op is ignored, only the absolute value is used.
		 */

		shift = mpz_get_ui(mpz2);	/* GMP integer => unsigned long conversion */
		res = mpg_integer();
		mpz_mul_2exp(res->mpg_i, mpz1, shift);		/* res = mpz1 * 2^shift */
	}
	free_bit_ops();
	return res;
}

/* do_mpfr_rshift --- perform a >> operation */

NODE *
do_mpfr_rhift(int nargs)
{
	NODE *res;
	unsigned long shift;

	if ((res = get_bit_ops("rshift")) == NULL) {
		/*
		 * mpz_get_ui: If op is too big to fit an unsigned long then just
		 * the least significant bits that do fit are returned.
		 * The sign of op is ignored, only the absolute value is used.
		 */

		shift = mpz_get_ui(mpz2);	/* GMP integer => unsigned long conversion */
		res = mpg_integer();
		mpz_fdiv_q_2exp(res->mpg_i, mpz1, shift);	/* res = mpz1 / 2^shift, round towards −inf */
	}
	free_bit_ops();
	return res;
}

/* do_mpfr_and --- perform an & operation */

NODE *
do_mpfr_and(int nargs)
{
	NODE *res;

	if ((res = get_bit_ops("and")) == NULL) {
		res = mpg_integer();
		mpz_and(res->mpg_i, mpz1, mpz2);
	}
	free_bit_ops();
	return res;
}

/* do_mpfr_or --- perform an | operation */

NODE *
do_mpfr_or(int nargs)
{
	NODE *res;

	if ((res = get_bit_ops("or")) == NULL) {
		res = mpg_integer();
		mpz_ior(res->mpg_i, mpz1, mpz2);
	}
	free_bit_ops();
	return res;
}

/* do_mpfr_strtonum --- the strtonum function */

NODE *
do_mpfr_strtonum(int nargs)
{
	NODE *tmp, *r;

	tmp = POP_SCALAR();
	if ((tmp->flags & (NUMBER|NUMCUR)) == 0) {
		r = mpg_integer();	/* will be changed to MPFR float if necessary in force_mpnum() */
		r->stptr = tmp->stptr;
		r->stlen = tmp->stlen;
		force_mpnum(r, TRUE, use_lc_numeric);
		r->stptr = NULL;
		r->stlen = 0;
	} else {
		(void) force_number(tmp);
		if (is_mpg_float(tmp)) {
			int tval;
			r = mpg_float();
			tval = mpfr_set(r->mpg_numbr, tmp->mpg_numbr, ROUND_MODE);
			IEEE_FMT(r->mpg_numbr, tval);
		} else {
			r = mpg_integer();
			mpz_set(r->mpg_i, tmp->mpg_i);
		}
	}

	DEREF(tmp);
	return r;
}

/* do_mpfr_xor --- perform an ^ operation */

NODE *
do_mpfr_xor(int nargs)
{
	NODE *res;

	if ((res = get_bit_ops("xor")) == NULL) {
		res = mpg_integer();
		mpz_xor(res->mpg_i, mpz1, mpz2);
	}
	free_bit_ops();
	return res;
}


static int firstrand = TRUE;
static gmp_randstate_t state;
static mpz_t seed;	/* current seed */

/* do_mpfr_rand --- do the rand function */

NODE *
do_mpfr_rand(int nargs ATTRIBUTE_UNUSED)
{
	NODE *res;
	int tval;

	if (firstrand) {
#if 0
		/* Choose the default algorithm */
		gmp_randinit_default(state);
#endif
		/*
		 * Choose a specific (Mersenne Twister) algorithm in case the default
		 * changes in the future.
		 */

		gmp_randinit_mt(state);

		mpz_init(seed);
		mpz_set_ui(seed, 1);
		/* seed state */
		gmp_randseed(state, seed);
		firstrand = FALSE;
	}
	res = mpg_float();
	tval = mpfr_urandomb(res->mpg_numbr, state);
	IEEE_FMT(res->mpg_numbr, tval);
	return res;
}


/* do_mpfr_srand --- seed the random number generator */

NODE *
do_mpfr_srand(int nargs)
{
	NODE *res;

	if (firstrand) {
#if 0
		/* Choose the default algorithm */
		gmp_randinit_default(state);
#endif
		/*
		 * Choose a specific algorithm (Mersenne Twister) in case default
		 * changes in the future.
		 */

		gmp_randinit_mt(state);

		mpz_init(seed);
		mpz_set_ui(seed, 1);
		/* No need to seed state, will change it below */
		firstrand = FALSE;
	}

	res = mpg_integer();
	mpz_set(res->mpg_i, seed);	/* previous seed */

	if (nargs == 0)
		mpz_set_ui(seed, (unsigned long) time((time_t *) 0));
	else {
		NODE *tmp;
		tmp = POP_SCALAR();
		if (do_lint && (tmp->flags & (NUMCUR|NUMBER)) == 0)
			lintwarn(_("srand: received non-numeric argument"));
		force_number(tmp);
		if (is_mpg_float(tmp))
			mpfr_get_z(seed, tmp->mpg_numbr, MPFR_RNDZ);
		else /* MP integer */
			mpz_set(seed, tmp->mpg_i);
		DEREF(tmp);
	}

	gmp_randseed(state, seed);
	return res;
}

/*
 * mpg_tofloat --- convert an arbitrary-precision integer operand to
 *	a float without loss of precision. It is assumed that the
 *	MPFR variable has already been initialized.
 */

static inline mpfr_ptr
mpg_tofloat(mpfr_ptr mf, mpz_ptr mz)
{
	size_t prec;

	/*
	 * When implicitely converting a GMP integer operand to a MPFR float, use
	 * a precision sufficiently large to hold the converted value exactly.
	 * 	
 	 *	$ ./gawk -M 'BEGIN { print 13 % 2 }'
 	 *	1
 	 * If the user-specified precision is used to convert the integer 13 to a
	 * float, one will get:
 	 *	$ ./gawk -M 'BEGIN { PREC=2; print 13 % 2.0 }'
 	 *	0	
 	 */

	prec = mpz_sizeinbase(mz, 2);	/* most significant 1 bit position starting at 1 */
	if (prec > PRECISION_MIN) {
		prec -= (size_t) mpz_scan1(mz, 0);	/* least significant 1 bit index starting at 0 */
		if (prec > MPFR_PREC_MAX)
			prec = MPFR_PREC_MAX;
		if (prec > PRECISION_MIN) 
			mpfr_set_prec(mf, prec);
	}

	mpfr_set_z(mf, mz, ROUND_MODE);
	return mf;
}


/* mpg_add --- add arbitrary-precision numbers */ 

static NODE *
mpg_add(NODE *t1, NODE *t2)
{
	NODE *r;
	int tval;

	if (is_mpg_integer(t1) && is_mpg_integer(t2)) {
		r = mpg_integer();
		mpz_add(r->mpg_i, t1->mpg_i, t2->mpg_i);
	} else {
		r = mpg_float();
		if (is_mpg_integer(t2))
			tval = mpfr_add_z(r->mpg_numbr, t1->mpg_numbr, t2->mpg_i, ROUND_MODE);
		else if (is_mpg_integer(t1))
			tval = mpfr_add_z(r->mpg_numbr, t2->mpg_numbr, t1->mpg_i, ROUND_MODE);
		else
			tval = mpfr_add(r->mpg_numbr, t1->mpg_numbr, t2->mpg_numbr, ROUND_MODE);
		IEEE_FMT(r->mpg_numbr, tval);
	}
	return r;
}

/* mpg_sub --- subtract arbitrary-precision numbers */

static NODE *
mpg_sub(NODE *t1, NODE *t2)
{
	NODE *r;
	int tval;

	if (is_mpg_integer(t1) && is_mpg_integer(t2)) {
		r = mpg_integer();
		mpz_sub(r->mpg_i, t1->mpg_i, t2->mpg_i);
	} else {
		r = mpg_float();
		if (is_mpg_integer(t2))
			tval = mpfr_sub_z(r->mpg_numbr, t1->mpg_numbr, t2->mpg_i, ROUND_MODE);
		else if (is_mpg_integer(t1)) {
#if (!defined(MPFR_VERSION) || (MPFR_VERSION < MPFR_VERSION_NUM(3,1,0)))
			NODE *tmp = t1;
			t1 = t2;
			t2 = tmp;
			tval = mpfr_sub_z(r->mpg_numbr, t1->mpg_numbr, t2->mpg_i, ROUND_MODE);
			tval = mpfr_neg(r->mpg_numbr, r->mpg_numbr, ROUND_MODE);
			t2 = t1;
			t1 = tmp;
#else
			tval = mpfr_z_sub(r->mpg_numbr, t1->mpg_i, t2->mpg_numbr, ROUND_MODE);
#endif
		} else
			tval = mpfr_sub(r->mpg_numbr, t1->mpg_numbr, t2->mpg_numbr, ROUND_MODE);
		IEEE_FMT(r->mpg_numbr, tval);
	}
	return r;
}

/* mpg_mul --- multiply arbitrary-precision numbers */

static NODE *
mpg_mul(NODE *t1, NODE *t2)
{
	NODE *r;
	int tval;

	if (is_mpg_integer(t1) && is_mpg_integer(t2)) {
		r = mpg_integer();
		mpz_mul(r->mpg_i, t1->mpg_i, t2->mpg_i);
	} else {
		r = mpg_float();
		if (is_mpg_integer(t2))
			tval = mpfr_mul_z(r->mpg_numbr, t1->mpg_numbr, t2->mpg_i, ROUND_MODE);
		else if (is_mpg_integer(t1))
			tval = mpfr_mul_z(r->mpg_numbr, t2->mpg_numbr, t1->mpg_i, ROUND_MODE);
		else
			tval = mpfr_mul(r->mpg_numbr, t1->mpg_numbr, t2->mpg_numbr, ROUND_MODE);
		IEEE_FMT(r->mpg_numbr, tval);
	}
	return r;
}


/* mpg_pow --- exponentiation involving arbitrary-precision numbers */ 

static NODE *
mpg_pow(NODE *t1, NODE *t2)
{
	NODE *r;
	int tval;

	if (is_mpg_integer(t1) && is_mpg_integer(t2)) {
		if (mpz_sgn(t2->mpg_i) >= 0 && mpz_fits_ulong_p(t2->mpg_i)) {
			r = mpg_integer();
			mpz_pow_ui(r->mpg_i, t1->mpg_i, mpz_get_ui(t2->mpg_i));
		} else {
			mpfr_ptr p1, p2;
			p1 = MP_FLOAT(t1);
			p2 = MP_FLOAT(t2);
			r = mpg_float();
			tval = mpfr_pow(r->mpg_numbr, p1, p2, ROUND_MODE);
			IEEE_FMT(r->mpg_numbr, tval);
		}
	} else {
		r = mpg_float();
		if (is_mpg_integer(t2))
			tval = mpfr_pow_z(r->mpg_numbr, t1->mpg_numbr, t2->mpg_i, ROUND_MODE);
		else {
			mpfr_ptr p1;
			p1 = MP_FLOAT(t1);
			tval = mpfr_pow(r->mpg_numbr, p1, t2->mpg_numbr, ROUND_MODE);
		}
		IEEE_FMT(r->mpg_numbr, tval);
	}
	return r;
}

/* mpg_div --- arbitrary-precision division */

static NODE *
mpg_div(NODE *t1, NODE *t2)
{
	NODE *r;
	int tval;

	if (is_mpg_integer(t1) && is_mpg_integer(t2)
			&& (mpz_sgn(t2->mpg_i) != 0)	/* not dividing by 0 */
			&& mpz_divisible_p(t1->mpg_i, t2->mpg_i)
	) {
		r = mpg_integer();
		mpz_divexact(r->mpg_i, t1->mpg_i, t2->mpg_i);
	} else {
		mpfr_ptr p1, p2;
		p1 = MP_FLOAT(t1);
		p2 = MP_FLOAT(t2);
		r = mpg_float();
		tval = mpfr_div(r->mpg_numbr, p1, p2, ROUND_MODE);
		IEEE_FMT(r->mpg_numbr, tval);
	}
	return r;
}

/* mpg_mod --- modulus operation with arbitrary-precision numbers */

static NODE *
mpg_mod(NODE *t1, NODE *t2)
{
	NODE *r;
	int tval;

	if (is_mpg_integer(t1) && is_mpg_integer(t2)) {
		r = mpg_integer();
		mpz_mod(r->mpg_i, t1->mpg_i, t2->mpg_i);
	} else {
		mpfr_ptr p1, p2;
		p1 = MP_FLOAT(t1);
		p2 = MP_FLOAT(t2);
		r = mpg_float();
		tval = mpfr_fmod(r->mpg_numbr, p1, p2, ROUND_MODE);
		IEEE_FMT(r->mpg_numbr, tval);
	}
	return r;
}
	
/*
 * mpg_interpret --- pre-exec hook in the interpreter. Handles
 *	arithmetic operations with MPFR/GMP numbers.
 */ 

static int
mpg_interpret(INSTRUCTION **cp)
{
	INSTRUCTION *pc = *cp;	/* current instruction */
	OPCODE op;	/* current opcode */
	NODE *r = NULL;
	NODE *t1, *t2;
	NODE **lhs;
	int tval;	/* the ternary value returned by a MPFR function */

	switch ((op = pc->opcode)) {
	case Op_plus_i:
		t2 = force_number(pc->memory);
		goto plus;
	case Op_plus:
		t2 = POP_NUMBER();
plus:
		t1 = TOP_NUMBER();
		r = mpg_add(t1, t2);
		DEREF(t1);
		if (op == Op_plus)
			DEREF(t2);
		REPLACE(r);
		break;

	case Op_minus_i:
		t2 = force_number(pc->memory);
		goto minus;
	case Op_minus:
		t2 = POP_NUMBER();
minus:
		t1 = TOP_NUMBER();
		r = mpg_sub(t1, t2);
		DEREF(t1);
		if (op == Op_minus)
			DEREF(t2);
		REPLACE(r);
		break;

	case Op_times_i:
		t2 = force_number(pc->memory);
		goto times;
	case Op_times:
		t2 = POP_NUMBER();
times:
		t1 = TOP_NUMBER();
		r = mpg_mul(t1, t2);
		DEREF(t1);
		if (op == Op_times)
			DEREF(t2);
		REPLACE(r);
		break;

	case Op_exp_i:
		t2 = force_number(pc->memory);
		goto exp;
	case Op_exp:
		t2 = POP_NUMBER();
exp:
		t1 = TOP_NUMBER();
		r = mpg_pow(t1, t2);
		DEREF(t1);
		if (op == Op_exp)
			DEREF(t2);
		REPLACE(r);
		break;

	case Op_quotient_i:
		t2 = force_number(pc->memory);
		goto quotient;
	case Op_quotient:
		t2 = POP_NUMBER();
quotient:
		t1 = TOP_NUMBER();
		r = mpg_div(t1, t2);
		DEREF(t1);
		if (op == Op_quotient)
			DEREF(t2);
		REPLACE(r);
		break;		

	case Op_mod_i:
		t2 = force_number(pc->memory);
		goto mod;
	case Op_mod:
		t2 = POP_NUMBER();
mod:
		t1 = TOP_NUMBER();
		r = mpg_mod(t1, t2);
		DEREF(t1);
		if (op == Op_mod)
			DEREF(t2);
		REPLACE(r);
		break;

	case Op_preincrement:
	case Op_predecrement:
		lhs = TOP_ADDRESS();
		t1 = *lhs;
		force_number(t1);

		if (is_mpg_integer(t1)) {
			if (t1->valref == 1 && t1->flags == (MALLOC|MPZN|NUMCUR|NUMBER))
			/* Efficiency hack. Big speed-up (> 30%) in a tight loop */
				r = t1;
			else
				r = *lhs = mpg_integer();
			if (op == Op_preincrement)
				mpz_add_ui(r->mpg_i, t1->mpg_i, 1);
			else
				mpz_sub_ui(r->mpg_i, t1->mpg_i, 1);
		} else {

			/*
			 * An optimization like the one above is not going to work
			 * for a floating-point number. With it,
			 *   gawk -M 'BEGIN { PREC=53; i=2^53+0.0; PREC=113; ++i; print i}'
		 	 * will output 2^53 instead of 2^53+1.
		 	 */

			r = *lhs = mpg_float();
			tval = mpfr_add_si(r->mpg_numbr, t1->mpg_numbr,
					op == Op_preincrement ? 1 : -1,
					ROUND_MODE);
			IEEE_FMT(r->mpg_numbr, tval);
		}
		if (r != t1)
			unref(t1);
		UPREF(r);
		REPLACE(r);
		break;

	case Op_postincrement:
	case Op_postdecrement:
		lhs = TOP_ADDRESS();
		t1 = *lhs;
		force_number(t1);

		if (is_mpg_integer(t1)) {
			r = mpg_integer();
			mpz_set(r->mpg_i, t1->mpg_i);
			if (t1->valref == 1 && t1->flags == (MALLOC|MPZN|NUMCUR|NUMBER))
			/* Efficiency hack. Big speed-up (> 30%) in a tight loop */
				t2 = t1;
			else
				t2 = *lhs = mpg_integer();
			if (op == Op_postincrement)
				mpz_add_ui(t2->mpg_i, t1->mpg_i, 1);
			else
				mpz_sub_ui(t2->mpg_i, t1->mpg_i, 1);
		} else {
			r = mpg_float();
			tval = mpfr_set(r->mpg_numbr, t1->mpg_numbr, ROUND_MODE);
			IEEE_FMT(r->mpg_numbr, tval);
			t2 = *lhs = mpg_float();
			tval = mpfr_add_si(t2->mpg_numbr, t1->mpg_numbr,
					op == Op_postincrement ? 1 : -1,
					ROUND_MODE);
			IEEE_FMT(t2->mpg_numbr, tval);
		}
		if (t2 != t1)
			unref(t1);
		REPLACE(r);
		break;

	case Op_unary_minus:
		t1 = TOP_NUMBER();
		if (is_mpg_float(t1)) {
			r = mpg_float();
			tval = mpfr_neg(r->mpg_numbr, t1->mpg_numbr, ROUND_MODE);
			IEEE_FMT(r->mpg_numbr, tval);
		} else {
			r = mpg_integer();
			mpz_neg(r->mpg_i, t1->mpg_i);
		}
		DEREF(t1);
		REPLACE(r);
		break;

	case Op_assign_plus:
	case Op_assign_minus:
	case Op_assign_times:
	case Op_assign_quotient:
	case Op_assign_mod:
	case Op_assign_exp:
		lhs = POP_ADDRESS();
		t1 = *lhs;
		force_number(t1);
		t2 = TOP_NUMBER();

		switch (op) {
		case Op_assign_plus:
			r = mpg_add(t1, t2);
			break;
		case Op_assign_minus:
			r = mpg_sub(t1, t2);
			break;
		case Op_assign_times:
			r = mpg_mul(t1, t2);
			break;
		case Op_assign_quotient:
			r = mpg_div(t1, t2);
			break;
		case Op_assign_mod:
			r = mpg_mod(t1, t2);
			break;
		case Op_assign_exp:
			r = mpg_pow(t1, t2);
			break;
		default:
			cant_happen();
		}

		DEREF(t2);
		unref(*lhs);
		*lhs = r;
		UPREF(r);
		REPLACE(r);
		break;

	default:
		return TRUE;	/* unhandled */
	}

	*cp = pc->nexti;	/* next instruction to execute */
	return FALSE;
}


/* mpg_fmt --- output formatted string with special MPFR/GMP conversion specifiers */

const char *
mpg_fmt(const char *mesg, ...)
{
	static char *tmp = NULL;
	int ret;
	va_list args;

	if (tmp != NULL) {
		mpfr_free_str(tmp);
		tmp = NULL;
	}
	va_start(args, mesg);
	ret = mpfr_vasprintf(& tmp, mesg, args);
	va_end(args);
	if (ret >= 0 && tmp != NULL)
		return tmp;
	return mesg;
}

#else

void
set_PREC()
{
	/* dummy function */
}

void
set_ROUNDMODE()
{
	/* dummy function */
}

#endif