/*
 * readfile.c - Read an entire file into a string.
 *
 * Arnold Robbins
 * Tue Apr 23 17:43:30 IDT 2002
 * Revised per Peter Tillier
 * Mon Jun  9 17:05:11 IDT 2003
 * Revised for new dynamic function facilities
 * Mon Jun 14 14:53:07 IDT 2004
 * Revised for formal API May 2012
 * Added input parser March 2014
 *
 * Michael M. Builov
 * mbuilov@gmail.com
 * Ported to _MSC_VER 4/2020
 */

/*
 * Copyright (C) 2002, 2003, 2004, 2011, 2012, 2013, 2014, 2018
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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _BSD_SOURCE

#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <wchar.h>
#include <stdarg.h>

#ifndef _MSC_VER
#include <unistd.h>
#endif

#include <sys/types.h>
#include <sys/stat.h>

/* Include <locale.h> before "gawkapi.h" redefines setlocale().
  "gettext.h" will include <locale.h> anyway */
#ifdef HAVE_LOCALE_H
#include <locale.h>
#endif

#ifdef WINDOWS_NATIVE
#include "mscrtx/xstat.h"
#endif

#include "gawkapi.h"

#include "gettext.h"
#define _(msgid)  gettext(msgid)
#define N_(msgid) msgid

#ifndef O_BINARY
# ifdef _O_BINARY
#  define O_BINARY _O_BINARY
# else
#  define O_BINARY 0
# endif
#endif

#ifndef O_RDONLY
# ifdef _O_RDONLY
#  define O_RDONLY _O_RDONLY
# endif
#endif

#ifndef S_ISREG
# ifdef S_IFREG
#  define S_ISREG(m)	(((m) & S_IFMT) == S_IFREG)
# elif defined _S_IFREG
#  define S_ISREG(m)	(((m) & _S_IFMT) == _S_IFREG)
# endif
#endif

#ifndef INT_MAX
# define INT_MAX ((unsigned)-1/2)
#endif

GAWK_PLUGIN_GPL_COMPATIBLE
GAWK_PLUGIN("readfile extension: version 2.0");

/* read_file_to_buffer --- handle the mechanics of reading the file */

static char *
read_file_to_buffer(fd_t fd, const gawk_xstat_t *sbuf, size_t limit)
{
	char *text;
	size_t size;

	if (! S_ISREG(sbuf->st_mode)) {
		errno = EINVAL;
		update_ERRNO_int(errno);
		return NULL;
	}

	if ((size_t) sbuf->st_size > limit) {
		errno = EFBIG;
		update_ERRNO_int(errno);
		return NULL;
	}

	size = (size_t) sbuf->st_size;
	emalloc(text, char *, size + 1, "read_file_to_buffer");

	if (read(fd, text, size) != (ssize_t) size) {
		update_ERRNO_int(errno);
		free(text);
		return NULL;
	}
	text[size] = '\0';
	return text;
}

/* do_readfile --- read a file into memory */

static awk_value_t *
do_readfile(int nargs, awk_value_t *result, struct awk_ext_func *unused)
{
	awk_value_t filename;
	int ret;
	gawk_xstat_t sbuf;
	char *text;
	fd_t fd;

	(void) nargs, (void) unused;

	assert(result != NULL);
	make_null_string(result);	/* default return value */

	unset_ERRNO();

	if (get_argument(0, AWK_STRING, &filename)) {
		ret = xstat(filename.str_value.str, & sbuf);
		if (ret < 0) {
			update_ERRNO_int(errno);
			goto done;
		}

		if ((fd = open(filename.str_value.str, O_RDONLY|O_BINARY)) < 0) {
			update_ERRNO_int(errno);
			goto done;
		}

		text = read_file_to_buffer(fd, & sbuf, (size_t)-1/2);
		if (text == NULL) {
			close(fd);
			goto done;	/* ERRNO already updated */
		}

		close(fd);
		make_malloced_string(text, (size_t) sbuf.st_size, result);
		goto done;
	} else if (do_lint)
		lintwarn(_("readfile: called with wrong kind of argument"));

done:
	/* Set the return value */
	return result;
}

/* readfile_get_record --- read the whole file as one record */

static int
readfile_get_record(char **out, awk_input_buf_t *iobuf, int *errcode,
			char **rt_start, size_t *rt_len,
			const awk_fieldwidth_info_t **unused)
{
	char *text;

	(void) errcode, (void) unused;

	/*
	 * The caller sets *errcode to 0, so we should set it only if an
	 * error occurs.
	 */

	if (out == NULL || iobuf == NULL)
		return EOF;

	if (iobuf->opaque != NULL) {
		/*
		 * Already read the whole file,
		 * free up stuff and return EOF
		 */
		free(iobuf->opaque);
		iobuf->opaque = NULL;
		return EOF;
	}

	/* read file */
	text = read_file_to_buffer(iobuf->fd, awk_input_buf_get_stat(iobuf), INT_MAX);
	if (text == NULL)
		return EOF;

	/* set up the iobuf for next time */
	iobuf->opaque = text;

	/* set return values */
	*rt_start = NULL;
	*rt_len = 0;
	*out = text;

	/* return count */
	return (int) awk_input_buf_get_stat(iobuf)->st_size;
}

/* readfile_can_take_file --- return true if we want the file */

static awk_bool_t
readfile_can_take_file(const awk_input_buf_t *iobuf)
{
	awk_value_t array, index, value;

	if (iobuf == NULL)
		return awk_false;

	/*
	 * This could fail if PROCINFO isn't referenced from
	 * the awk program. It's not a "can't happen" error.
	 */
	if (! sym_lookup("PROCINFO", AWK_ARRAY, & array))
		return awk_false;

	(void) make_const_string("readfile", 8, & index);

	if (! get_array_element(array.array_cookie, & index, AWK_UNDEFINED, & value))
		return awk_false;

	return awk_true;
}

/* readfile_take_control_of --- take over the file */

static awk_bool_t
readfile_take_control_of(awk_input_buf_t *iobuf)
{
	if (iobuf == NULL)
		return awk_false;

	iobuf->get_record = readfile_get_record;
	return awk_true;
}

static awk_input_parser_t readfile_parser = {
	"readfile",
	readfile_can_take_file,
	readfile_take_control_of,
	NULL
};

/* init_readfile --- set things up */

static awk_bool_t
init_readfile(void)
{
	register_input_parser(& readfile_parser);

	return awk_true;
}

static awk_ext_func_t func_table[] = {
	{ "readfile", do_readfile, 1, 1, awk_false, NULL },
};

/* define the dl_load function using the boilerplate macro */

dl_load_func(init_readfile, func_table, readfile, "")
