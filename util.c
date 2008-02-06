/*
 * libuci - Library for the Unified Configuration Interface
 * Copyright (C) 2008 Felix Fietkau <nbd@openwrt.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version 2.1
 * as published by the Free Software Foundation
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*
 * This file contains wrappers to standard functions, which
 * throw exceptions upon failure.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <stdbool.h>
#include <unistd.h>
#include <ctype.h>
#include <fcntl.h>

#define LINEBUF	32
#define LINEBUF_MAX	4096

static void *uci_malloc(struct uci_context *ctx, size_t size)
{
	void *ptr;

	ptr = malloc(size);
	if (!ptr)
		UCI_THROW(ctx, UCI_ERR_MEM);
	memset(ptr, 0, size);

	return ptr;
}

static void *uci_realloc(struct uci_context *ctx, void *ptr, size_t size)
{
	ptr = realloc(ptr, size);
	if (!ptr)
		UCI_THROW(ctx, UCI_ERR_MEM);

	return ptr;
}

static char *uci_strdup(struct uci_context *ctx, const char *str)
{
	char *ptr;

	ptr = strdup(str);
	if (!ptr)
		UCI_THROW(ctx, UCI_ERR_MEM);

	return ptr;
}

/*
 * validate strings for names and types, reject special characters
 * for names, only alphanum and _ is allowed (shell compatibility)
 * for types, we allow more characters
 */
static bool uci_validate_str(const char *str, bool name)
{
	if (!*str)
		return false;

	while (*str) {
		char c = *str;
		if (!isalnum(c) && c != '_') {
			if (name || (c < 33) || (c > 126))
				return false;
		}
		str++;
	}
	return true;
}

static inline bool uci_validate_name(const char *str)
{
	return uci_validate_str(str, true);
}

static void uci_alloc_parse_context(struct uci_context *ctx)
{
	ctx->pctx = (struct uci_parse_context *) uci_malloc(ctx, sizeof(struct uci_parse_context));
}

int uci_parse_tuple(struct uci_context *ctx, char *str, char **package, char **section, char **option, char **value)
{
	char *last = NULL;

	UCI_HANDLE_ERR(ctx);
	UCI_ASSERT(ctx, str && package && section && option);

	last = strchr(str, '=');
	if (last) {
		*last = 0;
		last++;
	}

	*package = strsep(&str, ".");
	if (!*package || !uci_validate_name(*package))
		goto error;

	*section = strsep(&str, ".");
	if (!*section)
		goto lastval;

	*option = strsep(&str, ".");
	if (!*option)
		goto lastval;

lastval:
	if (last) {
		if (!value)
			goto error;

		if (!*last)
			goto error;
		*value = last;
	}

	if (*section && !uci_validate_name(*section))
		goto error;
	if (*option && !uci_validate_name(*option))
		goto error;

	goto done;

error:
	UCI_THROW(ctx, UCI_ERR_PARSE);

done:
	return 0;
}


static void uci_parse_error(struct uci_context *ctx, char *pos, char *reason)
{
	struct uci_parse_context *pctx = ctx->pctx;

	pctx->reason = reason;
	pctx->byte = pos - pctx->buf;
	UCI_THROW(ctx, UCI_ERR_PARSE);
}


/*
 * Fetch a new line from the input stream and resize buffer if necessary
 */
static void uci_getln(struct uci_context *ctx, int offset)
{
	struct uci_parse_context *pctx = ctx->pctx;
	char *p;
	int ofs;

	if (pctx->buf == NULL) {
		pctx->buf = uci_malloc(ctx, LINEBUF);
		pctx->bufsz = LINEBUF;
	}

	ofs = offset;
	do {
		p = &pctx->buf[ofs];
		p[ofs] = 0;

		p = fgets(p, pctx->bufsz - ofs, pctx->file);
		if (!p || !*p)
			return;

		ofs += strlen(p);
		if (pctx->buf[ofs - 1] == '\n') {
			pctx->line++;
			pctx->buf[ofs - 1] = 0;
			return;
		}

		if (pctx->bufsz > LINEBUF_MAX/2)
			uci_parse_error(ctx, p, "line too long");

		pctx->bufsz *= 2;
		pctx->buf = uci_realloc(ctx, pctx->buf, pctx->bufsz);
	} while (1);
}

/* 
 * parse a character escaped by '\'
 * returns true if the escaped character is to be parsed
 * returns false if the escaped character is to be ignored
 */
static inline bool parse_backslash(struct uci_context *ctx, char **str)
{
	/* skip backslash */
	*str += 1;

	/* undecoded backslash at the end of line, fetch the next line */
	if (!**str) {
		*str += 1;
		uci_getln(ctx, *str - ctx->pctx->buf);
		return false;
	}

	/* FIXME: decode escaped char, necessary? */
	return true;
}

/*
 * move the string pointer forward until a non-whitespace character or
 * EOL is reached
 */
static void skip_whitespace(struct uci_context *ctx, char **str)
{
restart:
	while (**str && isspace(**str))
		*str += 1;

	if (**str == '\\') {
		if (!parse_backslash(ctx, str))
			goto restart;
	}
}

static inline void addc(char **dest, char **src)
{
	**dest = **src;
	*dest += 1;
	*src += 1;
}

/*
 * parse a double quoted string argument from the command line
 */
static void parse_double_quote(struct uci_context *ctx, char **str, char **target)
{
	char c;

	/* skip quote character */
	*str += 1;

	while ((c = **str)) {
		switch(c) {
		case '"':
			**target = 0;
			*str += 1;
			return;
		case '\\':
			if (!parse_backslash(ctx, str))
				continue;
			/* fall through */
		default:
			addc(target, str);
			break;
		}
	}
	uci_parse_error(ctx, *str, "unterminated \"");
}

/*
 * parse a single quoted string argument from the command line
 */
static void parse_single_quote(struct uci_context *ctx, char **str, char **target)
{
	char c;
	/* skip quote character */
	*str += 1;

	while ((c = **str)) {
		switch(c) {
		case '\'':
			**target = 0;
			*str += 1;
			return;
		default:
			addc(target, str);
		}
	}
	uci_parse_error(ctx, *str, "unterminated '");
}

/*
 * parse a string from the command line and detect the quoting style
 */
static void parse_str(struct uci_context *ctx, char **str, char **target)
{
	do {
		switch(**str) {
		case '\'':
			parse_single_quote(ctx, str, target);
			break;
		case '"':
			parse_double_quote(ctx, str, target);
			break;
		case '#':
			**str = 0;
			/* fall through */
		case 0:
			goto done;
		case '\\':
			if (!parse_backslash(ctx, str))
				continue;
			/* fall through */
		default:
			addc(target, str);
			break;
		}
	} while (**str && !isspace(**str));
done:

	/* 
	 * if the string was unquoted and we've stopped at a whitespace
	 * character, skip to the next one, because the whitespace will
	 * be overwritten by a null byte here
	 */
	if (**str)
		*str += 1;

	/* terminate the parsed string */
	**target = 0;
}

/*
 * extract the next argument from the command line
 */
static char *next_arg(struct uci_context *ctx, char **str, bool required, bool name)
{
	char *val;
	char *ptr;

	val = ptr = *str;
	skip_whitespace(ctx, str);
	parse_str(ctx, str, &ptr);
	if (!*val) {
		if (required)
			uci_parse_error(ctx, *str, "insufficient arguments");
		goto done;
	}

	if (name && !uci_validate_name(val))
		uci_parse_error(ctx, val, "invalid character in field");

done:
	return val;
}

int uci_parse_argument(struct uci_context *ctx, FILE *stream, char **str, char **result)
{
	UCI_HANDLE_ERR(ctx);
	UCI_ASSERT(ctx, str != NULL);
	UCI_ASSERT(ctx, result != NULL);

	if (ctx->pctx) {
		if (ctx->pctx->file != stream) {
			ctx->internal = true;
			uci_cleanup(ctx);
		}
	} else {
		uci_alloc_parse_context(ctx);
		ctx->pctx->file = stream;
	}
	if (!*str) {
		uci_getln(ctx, 0);
		*str = ctx->pctx->buf;
	}

	*result = next_arg(ctx, str, false, false);

	return 0;
}


/*
 * open a stream and go to the right position
 *
 * note: when opening for write and seeking to the beginning of
 * the stream, truncate the file
 */
static FILE *uci_open_stream(struct uci_context *ctx, const char *filename, int pos, bool write, bool create)
{
	struct stat statbuf;
	FILE *file = NULL;
	int fd, ret;
	int mode = (write ? O_RDWR : O_RDONLY);

	if (create)
		mode |= O_CREAT;

	if (!write && ((stat(filename, &statbuf) < 0) ||
		((statbuf.st_mode &  S_IFMT) != S_IFREG))) {
		UCI_THROW(ctx, UCI_ERR_NOTFOUND);
	}

	fd = open(filename, mode, UCI_FILEMODE);
	if (fd < 0)
		goto error;

	if (flock(fd, (write ? LOCK_EX : LOCK_SH)) < 0)
		goto error;

	ret = lseek(fd, 0, pos);

	if (ret < 0)
		goto error;

	file = fdopen(fd, (write ? "w+" : "r"));
	if (file)
		goto done;

error:
	UCI_THROW(ctx, UCI_ERR_IO);
done:
	return file;
}

static void uci_close_stream(FILE *stream)
{
	int fd;

	if (!stream)
		return;

	fd = fileno(stream);
	flock(fd, LOCK_UN);
	fclose(stream);
}


