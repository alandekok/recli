/*
 * Syntax parsing and validation functions
 *
 * Copyright (c) 2011, Alan DeKok <aland at freeradius dot org>
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *  *  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *
 *  *  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <errno.h>
#include <stdarg.h>
#include <assert.h>
#include "recli.h"

static int syntax_fprintf_wrapper(void *ctx, const char *fmt, ...)
{
	int rcode;
	va_list args;

	if (!ctx) ctx = stdout;

	va_start(args, fmt);
	rcode = vfprintf(ctx, fmt, args);
	va_end(args);

	return rcode;
}

void *syntax_stdout = NULL;
void *syntax_stderr = NULL;

syntax_fprintf_t syntax_fprintf = syntax_fprintf_wrapper;


/*
 *	This file implements an abstract syntax tree based on
 *	content-addressible nodes.
 */
typedef enum cli_type_t {
	CLI_TYPE_INVALID = 0,
	CLI_TYPE_EXACT,
	CLI_TYPE_KEY,
	CLI_TYPE_OPTIONAL,
	CLI_TYPE_CONCAT,
	CLI_TYPE_ALTERNATE,
	CLI_TYPE_MACRO,
	CLI_TYPE_PLUS
} cli_type_t;

struct cli_syntax_t {
	cli_type_t type;

	uint32_t hash;
	void  *first;
	void *next;

	int refcount;
	int length;		/* for concatenation nodes */
};

#define FNV_MAGIC_INIT (0x811c9dc5)
#define FNV_MAGIC_PRIME (0x01000193)

static uint32_t fnv_hash(const void *data, size_t size)
{
	const uint8_t *p = data;
	const uint8_t *q = p + size;
	uint32_t      hash = FNV_MAGIC_INIT;

	while (p != q) {
		hash *= FNV_MAGIC_PRIME;
		hash ^= (uint32_t) (*p++);
    }

    return hash;
}

static uint32_t fnv_hash_update(const void *data, size_t size, uint32_t hash)
{
	const uint8_t *p = data;
	const uint8_t *q = p + size;

	while (p != q) {
		hash *= FNV_MAGIC_PRIME;
		hash ^= (uint32_t) (*p++);
    }
    return hash;
}

static cli_syntax_t *syntax_find(cli_syntax_t *this);
#ifndef NDEBUG
static void syntax_debug(const char *msg, cli_syntax_t *this);
#endif


/*
 *	Create a unique hash based on node contents.
 *	This is the "content-addressible" part of the DB.
 */
static uint32_t syntax_hash(cli_syntax_t *this)
{
	uint32_t hash;

	hash = fnv_hash(&this->type, sizeof(this->type));

	switch (this->type) {
	case CLI_TYPE_EXACT:
		assert(this->next == NULL);

	case CLI_TYPE_MACRO:
		hash = fnv_hash_update(this->first, strlen((char *)this->first),
				       hash);
		break;

	case CLI_TYPE_KEY:
	case CLI_TYPE_OPTIONAL:
	case CLI_TYPE_PLUS:
		assert(this->first != NULL);
		assert(this->next == NULL);
		assert(syntax_find(this->first) == this->first);
		
		hash = fnv_hash_update(this->first, sizeof(this->first),
				       hash);
		break;

	case CLI_TYPE_ALTERNATE:
	case CLI_TYPE_CONCAT:
		assert(this->first != NULL);
		assert(this->next != NULL);

		assert(syntax_find(this->first) == this->first);
		assert(syntax_find(this->next) == this->next);

		hash = fnv_hash_update(this->first, sizeof(this->first),
				       hash);
		hash = fnv_hash_update(this->next, sizeof(this->next),
				       hash);
		break;

	default:
		break;
	}

	this->hash = hash;
	return hash;
}


/*
 *	For now, there is only one global hash table containing all
 *	nodes.
 */
static int num_entries = 0;
static int table_size = 0;
static cli_syntax_t **hash_table = NULL;


/*
 *	Look up a node based on content.
 */
static cli_syntax_t *syntax_find(cli_syntax_t *this)
{
	cli_syntax_t *found;

	if (num_entries == 0) return NULL;

	if (!this) return NULL;

	if (this->hash == 0) syntax_hash(this);

	found = hash_table[this->hash & (table_size - 1)];
	if (!found) return found;

	if (found->hash == this->hash) return found;

	return NULL;
}


/*
 *	Increment the reference count, if the node exists.
 */
static cli_syntax_t *syntax_ref(cli_syntax_t *this)
{
	this = syntax_find(this);
	if (!this) return NULL;

	this->refcount++;
	return this;
}

/*
 *	Order nodes alphabetically.
 */
static int syntax_order(const cli_syntax_t *a, const cli_syntax_t *b)
{
	while (a->type != CLI_TYPE_EXACT) a = a->first;
	while (b->type != CLI_TYPE_EXACT) b = b->first;

	// FIXME: type optional && key

	/*
	 *	FIXME: if the first node of a concatenation is
	 *	identical, recurse over the second one.
	 */

	return strcmp((char *)a->first, (char *) b->first);
}

/*
 *	Free a node by decrementing its reference count.  When the
 *	count goes to zero, free the node.
 */
void syntax_free(cli_syntax_t *start)
{
	int flag = 0;
	cli_syntax_t *this, *next;

	this = start;

	if (!this) goto finish;

	assert(this->refcount > 0);

redo:
	this->refcount--;
	if (this->refcount > 0) {
		if (flag) goto finish;
		return;
	}

	switch (this->type) {
	case CLI_TYPE_ALTERNATE:
	case CLI_TYPE_CONCAT:
		assert(hash_table[this->hash & (table_size - 1)] == this);
		hash_table[this->hash & (table_size - 1)] = NULL;
		num_entries--;

		syntax_free(this->first);
		next = this->next;
#ifndef NDEBUG
		memset(this, 0, sizeof(*this));
#endif
		free(this);
		this = next;
		goto redo;

	case CLI_TYPE_KEY:
	case CLI_TYPE_OPTIONAL:
	case CLI_TYPE_PLUS:
		assert(hash_table[this->hash & (table_size - 1)] == this);
		hash_table[this->hash & (table_size - 1)] = NULL;
		num_entries--;

		next = this->first;
#ifndef NDEBUG
		memset(this, 0, sizeof(*this));
#endif
		free(this);
		this = next;
		goto redo;

	case CLI_TYPE_MACRO:
		assert(hash_table[this->hash & (table_size - 1)] == this);
		hash_table[this->hash & (table_size - 1)] = NULL;
		num_entries--;

		next = this->next;
#ifndef NDEBUG
		memset(this, 0, sizeof(*this));
#endif
		free(this);
		this = next;
		goto redo;

	case CLI_TYPE_EXACT:
		assert(hash_table[this->hash & (table_size - 1)] == this);
		hash_table[this->hash & (table_size - 1)] = NULL;
		num_entries--;

#ifndef NDEBUG
		memset(this, 0, sizeof(*this));
#endif
		free(this);
		break;

	default:
		assert(0 == 1);
		break;

	}

finish:
	if (!start && (num_entries > 4)) {
		int i;

		for (i = 0; i < table_size; i++) {
			if (!hash_table[i]) continue;

			if (hash_table[i]->type == CLI_TYPE_MACRO) {
				assert(hash_table[i]->refcount == 1);
				syntax_free(hash_table[i]);
			}
		}

		for (i = 0; i < table_size; i++) {
			if (!hash_table[i]) continue;

			if ((hash_table[i]->type == CLI_TYPE_EXACT) &&
			    (hash_table[i]->next != NULL)) {
				syntax_free(hash_table[i]);
			}
		}

		if (num_entries == 0) return;

#ifndef NDEBUG
		syntax_fprintf(syntax_stdout, "NUM ENTRIES LEFT: %d\n", num_entries);
		for (i = 0; i < table_size; i++) {
			if (!hash_table[i]) continue;

			syntax_fprintf(syntax_stdout, "LEFT %d ", hash_table[i]->refcount);
			syntax_printf(hash_table[i]);
			syntax_fprintf(syntax_stdout, "\n");
		}
#endif
	}
}


/*
 *	Insert a new node into the hash.
 */
static int syntax_insert(cli_syntax_t *this)
{
	int i;
	uint32_t hash;
	cli_syntax_t **new_table;

	if (!num_entries) {
		hash_table = calloc(sizeof(hash_table[0]), 256);
		if (!hash_table) return 0;
		table_size = 256;
	}

	if (!this->hash) syntax_hash(this);

	assert(syntax_find(this) == NULL);

#ifndef NDEBUG
	if ((this->type == CLI_TYPE_CONCAT) ||
	    (this->type == CLI_TYPE_ALTERNATE)) {
		cli_syntax_t *first = this->first;
		assert(first->type != this->type);
	}

	if (this->type == CLI_TYPE_ALTERNATE) {
		assert(syntax_order(this->first, this->next) <= 0);
	}
#endif

retry:
	hash = this->hash;
	if (!hash_table[hash & (table_size - 1)]) {
		hash_table[hash & (table_size - 1)] = this;
		num_entries++;
		return 1;

	}	
	      
	assert(this != hash_table[hash & (table_size - 1)]);

	new_table = calloc(sizeof(new_table[0]), table_size * 2);
	if (!new_table) return 0;

	for (i = 0; i < table_size; i++) {
		if (hash_table[i]) {
			hash = hash_table[i]->hash;
			hash &= ((table_size * 2) - 1);
			new_table[hash] = hash_table[i];
			hash_table[i] = NULL;
		}
	}

	free(hash_table);
	hash_table = new_table;
	table_size *= 2;
	goto retry;
}


static cli_syntax_t *syntax_new(cli_type_t type, void *first, void *next);


/*
 *	Length of "a" which is a prefix of "b"
 */
static int syntax_prefix_length(cli_syntax_t *a, cli_syntax_t *b)
{
	if (a->type != CLI_TYPE_CONCAT) {
		assert(b->type != CLI_TYPE_CONCAT);
	}

	if (a == b) return 0;

	if (b->type != CLI_TYPE_CONCAT) {
		assert(a->type == CLI_TYPE_CONCAT);
		assert(a->length > 0);
		return a->length - 1;
	}

	assert(a->type == CLI_TYPE_CONCAT);
	assert(b->type == CLI_TYPE_CONCAT);
	assert(a->length > b->length);

	return a->length - b->length;
}


/*
 *	Longest common suffix of two lists.
 */
static cli_syntax_t *syntax_lcs(cli_syntax_t *a, cli_syntax_t *b)
{
	cli_syntax_t *c;

redo:
	assert(a != NULL);
	assert(b != NULL);

	if (a == b) {
		a->refcount++;
		return a;
	}

	if ((a->type != CLI_TYPE_CONCAT) &&
	    (b->type != CLI_TYPE_CONCAT)) {
		return NULL;
	}

	if ((a->type == CLI_TYPE_CONCAT) &&
	    (b->type != CLI_TYPE_CONCAT)) {
		c = a;
		a = b;
		b = c;
	}

	assert(b->type == CLI_TYPE_CONCAT);

	if (a->type != CLI_TYPE_CONCAT) {
		while (b->type == CLI_TYPE_CONCAT) b = b->next;
		goto redo;
	}

	assert(a->type == CLI_TYPE_CONCAT);

	while (a->length > b->length) a = a->next;
	while (b->length > a->length) b = b->next;

	assert(a->length == b->length);

	while (a->type == CLI_TYPE_CONCAT) {
		if (a == b) goto redo;
		a = a->next;
		b = b->next;
	}

	assert(a->type != CLI_TYPE_CONCAT);
	assert(b->type != CLI_TYPE_CONCAT);
	goto redo;
}


/*
 *	Longest common prefix of two lists.
 */
static int syntax_lcp(cli_syntax_t *a, cli_syntax_t *b)
{
	int lcp = 0;

	if (a == b) {
		if (a->type != CLI_TYPE_CONCAT) return 1;

		while (a->type == CLI_TYPE_CONCAT) {
			a = a->next;
			lcp++;
		}
		lcp++;

		return lcp;
	}

	if ((a->type != CLI_TYPE_CONCAT) &&
	    (b->type != CLI_TYPE_CONCAT)) {
		return 0;
	}

	if ((a->type == CLI_TYPE_CONCAT) &&
	    (b->type == CLI_TYPE_CONCAT)) {
		lcp = syntax_lcp(a->first, b->first);
		if (lcp == 0) return 0;

		return 1 + syntax_lcp(a->next, b->next);
	}

	if (a->type == CLI_TYPE_CONCAT) {
		assert(b->type != CLI_TYPE_CONCAT);
		if (a->first == b) return 1;
		return 0;
	}

	assert(a->type != CLI_TYPE_CONCAT);
	assert(b->type == CLI_TYPE_CONCAT);
	if (b->first == a) return 1;
	return 0;
}


/*
 *	FIXME: if the first node is exact or concat with exact, then
 *	walk the syntax DOWN (like syntax_check), and re-constitute it
 *	coming back up.  That code should be a lot simpler than this,
 *	and should be better at maintaining normal form.
 *
 */
static cli_syntax_t *syntax_new_alternate(cli_syntax_t *first,
					  cli_syntax_t *next)
{
	cli_syntax_t *a, *b, *c;

	if (next->type != CLI_TYPE_ALTERNATE) {
		a = first;
		first = next;
		next = a;
	}

	if (next->type != CLI_TYPE_ALTERNATE) {
		return syntax_new(CLI_TYPE_ALTERNATE, first, next);
	}

	if (first->type != CLI_TYPE_ALTERNATE) {
		/* a|a|b ==> a|b */
		if (first == next->first) {
			syntax_free(first);
			return next;
		}

		/* a|b|c ==> a|b|c */
		if (syntax_order(first, next->first) <= 0) {
			return syntax_new(CLI_TYPE_ALTERNATE, first, next);
		}

		c = next->next;
		c->refcount++;

		b = syntax_new_alternate(first, next->next);
		if (!b) {
			syntax_free(c);
			syntax_free(next);
			return NULL;
		}

		c = next->first;
		c->refcount++;

		a = syntax_new(CLI_TYPE_ALTERNATE, next->first, b);
		if (!a) {
			syntax_free(c);
			syntax_free(next);
			return NULL;
		}
		syntax_free(next);

		return a;
	}

	assert(first->type == CLI_TYPE_ALTERNATE);
	assert(next->type == CLI_TYPE_ALTERNATE);

	c = first->next;
	c->refcount++;

	b = syntax_new_alternate(first->next, next);
	if (!b) {
		syntax_free(c);
		syntax_free(first);
		return NULL;
	}

	c = first->first;
	c->refcount++;

	a = syntax_new_alternate(first->first, b);
	if (!a) {
		syntax_free(b);
		syntax_free(c);
		syntax_free(first);
		return NULL;
	}
	syntax_free(first);

	return a;
}


/*
 *	Skip "lcp" nodes of a prefix and return the suffix.
 */
static cli_syntax_t *syntax_skip(cli_syntax_t *a, int lcp)
{
redo:
	if (lcp == 0) {
		a->refcount++;
		return a;
	}

	if (a->type != CLI_TYPE_CONCAT) return NULL;

	a = a->next;
	lcp--;
	goto redo;
}


/*
 *	Add "lcp" nodes from "prefix" to the front of "tail"
 */
static cli_syntax_t *syntax_concat_prefix(cli_syntax_t *prefix, int lcp,
					  cli_syntax_t *tail)
{
	cli_syntax_t *a, *b;

	if (lcp == 0) return tail;

	if (prefix->type == CLI_TYPE_CONCAT) {
		a = prefix->first;
		b = prefix->next;
	} else {
		a = prefix;
		b = NULL;
	}

	a->refcount++;
	if (lcp == 1) {
		if (!tail) return a;

		return syntax_new(CLI_TYPE_CONCAT, a, tail);
	}

	assert(b != NULL);

	return syntax_new(CLI_TYPE_CONCAT, a,
			  syntax_concat_prefix(b, lcp - 1, tail));
}


/*
 *	Create a new node, ensuring normal form.
 */
static cli_syntax_t *syntax_new(cli_type_t type, void *first, void *next)
{
	int lcp;
	cli_syntax_t find;
	cli_syntax_t *this, *a, *b, *c, *d;

	memset(&find, 0, sizeof(find));

	switch (type) {
	default:
		syntax_free(first);
		syntax_free(next);
		assert(0 == 1);
		return NULL;

	case CLI_TYPE_EXACT:
		assert(next == NULL);

	case CLI_TYPE_MACRO:
		assert(first != NULL);
		assert(strlen((char *)first) > 0);
		break;

	case CLI_TYPE_KEY:
	case CLI_TYPE_OPTIONAL:
	case CLI_TYPE_PLUS:
	optional:
		assert(first != NULL);
		assert(next == NULL);
		break;

	case CLI_TYPE_ALTERNATE:
		/*
		 *	a|a ==> a
		 */
		if (first == next) {
			syntax_free(first);
			return next;
		}

		a = first;
		b = next;

		if (!next) {
			type = CLI_TYPE_OPTIONAL;
			goto optional;
		}
		if (!first) {
			type = CLI_TYPE_OPTIONAL;
			first = b;
			next = a;
			goto optional;
		}

		/*
		 *	(a foo|b foo) ==> (a|b) foo
		 */
		c = syntax_lcs(a, b);
		if (c) {
			cli_syntax_t *e, *f;

			if (a != c) {
				lcp = syntax_prefix_length(a, c);
				assert(lcp > 0);
				d = syntax_concat_prefix(a, lcp, NULL);
				if (!d) assert(0 == 1);
			} else {
				d = NULL;
			}

			if (b != c) {
				lcp = syntax_prefix_length(b, c);
				assert(lcp > 0);
				e = syntax_concat_prefix(b, lcp, NULL);
				if (!e) assert(0 == 1);
			} else {
				e = NULL;
			}

			f = syntax_new(CLI_TYPE_ALTERNATE, d, e);
			if (!f) assert(0 == 1);

			syntax_free(first);
			syntax_free(next);

			return syntax_new(CLI_TYPE_CONCAT, f, c);
		}

		if ((a->type == CLI_TYPE_ALTERNATE) &&
		    (b->type != CLI_TYPE_ALTERNATE)) {
			first = b;
			next = a;
			a = first;
			b = next;
		}

		/*
		 *	(a b|a c) ==> a (b|c)
		 */
		lcp = syntax_lcp(first, next);
		if (lcp > 0) {
			a = syntax_skip(first, lcp);
			b = syntax_skip(next, lcp);

			d = syntax_new(CLI_TYPE_ALTERNATE, a, b);
			if (!d) {
				syntax_free(first);
				syntax_free(next);
				return NULL;
			}

			c = syntax_concat_prefix(first, lcp, d);
			syntax_free(first);
			syntax_free(next);
			if (!c) {
				syntax_free(d);
				return NULL;
			}
			return c;
		}

		assert(a != b);

		/*
		 *	b|(a|c) ==> a|(b|c)
		 */
		if ((a->type == CLI_TYPE_ALTERNATE) ||
		    ((b->type == CLI_TYPE_ALTERNATE) &&
		     (syntax_order(a, b) > 0))) {
			return syntax_new_alternate(first, next);
		}

		/*
		 *	(a b|(a c|d)) ==> (a (b|c)|d)
		 */
		if ((a->type != CLI_TYPE_ALTERNATE) &&
		    (b->type == CLI_TYPE_ALTERNATE) &&
		    ((lcp = syntax_lcp(a, b->first)) > 0)) {
			cli_syntax_t *c = next;

			a = c->first;
			a->refcount++;

			b = syntax_new(CLI_TYPE_ALTERNATE, first, a);
			if (!b) {
				syntax_free(next);
				return NULL;
			}

			a = c->next;
			a->refcount++;
			syntax_free(c);
			return syntax_new(CLI_TYPE_ALTERNATE, b, a);
		}

		/*
		 *	b|a ==> a|b
		 */
		if (syntax_order(a, b) > 0) {
			assert(first == a);
			first = next;
			next = a;
		}
		break;

	case CLI_TYPE_CONCAT:
		if (!next) return first;

		a = first;

		/*
		 *	concat(concat(a,b),c) ==> concat(a,concat(b,c))
		 */
		if (a->type == CLI_TYPE_CONCAT) {
			b = a->next;
			b->refcount++;
			c = syntax_new(CLI_TYPE_CONCAT, b, next);
			if (!c) {
				syntax_free(first);
				return NULL;
			}
			b = a->first;
			b->refcount++;
			syntax_free(first);
			first = b;
			next = c;

		}
		break;
	}

	find.type = type;
	find.first = first;
	find.next = next;

	this = syntax_ref(&find);
	if (this) {
		if ((type == CLI_TYPE_CONCAT) ||
		    (type == CLI_TYPE_KEY) ||
		    (type == CLI_TYPE_MACRO) ||
		    (type == CLI_TYPE_ALTERNATE) ||
		    (type == CLI_TYPE_OPTIONAL)) {
#ifndef NDEBUG
			a = first;
			assert(a->refcount > 1);
			
			if (next) {
				b = next;

				assert(b->refcount > 1);
			}
#endif

			syntax_free(first);
			if (next) syntax_free(next);
		}
		return this;
	}

	if (type != CLI_TYPE_EXACT) {
		this = calloc(sizeof(*this), 1);
		if (!this) return NULL;

		a = this->first = first;
		assert(a->type != type);

		if (type == CLI_TYPE_CONCAT) {
			this->length = 1;

			b = next;
			if (b->type == CLI_TYPE_CONCAT) {
				this->length += b->length;
			} else {
				this->length++;
			}
		}

	} else {
		size_t len = strlen((char *) first);
		
		assert(next == NULL);

		this = calloc(sizeof(*this) + len + 1, 1);
		if (!this) return NULL;

		this->first = this + 1;
		memcpy(this->first, first, len + 1);
	}

	this->type = type;
	this->next = next;

	if (!syntax_insert(this)) {
		free(this);
		return NULL;
	}

	assert(num_entries <= table_size);
	this->refcount++;

	return this;
}


/*
 *	Internal "print syntax to string"
 */
static size_t syntax_sprintf(char *buffer, size_t len,
			     const cli_syntax_t *this, cli_type_t parent)
{
	size_t outlen, offset;
	cli_syntax_t *a;

	switch (this->type) {
	case CLI_TYPE_EXACT:
		outlen = snprintf(buffer, len, "%s", (char *) this->first);
		break;

	case CLI_TYPE_MACRO:
		outlen = snprintf(buffer, len, "%s=", (char *) this->first);
		buffer += outlen;
		len -= outlen;
		outlen += syntax_sprintf(buffer, len, this->next,
					 CLI_TYPE_MACRO);
		break;

	case CLI_TYPE_CONCAT:
		outlen = syntax_sprintf(buffer, len, this->first,
					CLI_TYPE_CONCAT);
		buffer += outlen;
		len -= outlen;
		buffer[0] = ' ';

		buffer++;
		len--;
		outlen++;

		outlen += syntax_sprintf(buffer, len, this->next,
					 CLI_TYPE_CONCAT);
		break;

	case CLI_TYPE_OPTIONAL:
		buffer[0] = '[';
		buffer++;
		len--;
		outlen = syntax_sprintf(buffer, len, this->first,
					CLI_TYPE_OPTIONAL);
		buffer += outlen;
		len -= outlen;
		buffer[0] = ']';
		buffer[1] = '\0';
		outlen += 2;
		break;

	case CLI_TYPE_KEY:
		buffer[0] = '{';
		buffer++;
		len--;
		outlen = syntax_sprintf(buffer, len, this->first,
					CLI_TYPE_KEY);
		buffer += outlen;
		len -= outlen;
		buffer[0] = '}';
		buffer[1] = '\0';
		outlen += 2;
		break;

	case CLI_TYPE_PLUS:
		a = this->first;
		offset = 0;
		if (a->type == CLI_TYPE_CONCAT) {
			buffer[0] = '(';
			buffer++;
			len--;
			offset++;
		} else {
			a = NULL;
		}
		outlen = syntax_sprintf(buffer, len, this->first,
					CLI_TYPE_PLUS);
		buffer += outlen;
		len -= outlen;

		if (a) {
			buffer[0] = ')';
			buffer++;
			len--;
			offset++;
		}

		buffer[0] = '+';
		buffer[1] = '\0';
		outlen++;
		outlen += offset;
		break;

	case CLI_TYPE_ALTERNATE:
		outlen = 0;
		if (parent != CLI_TYPE_ALTERNATE) {
			buffer[0] = '(';
			len--;
			outlen++;
		}
		outlen += syntax_sprintf(buffer + outlen, len,
					 this->first, CLI_TYPE_ALTERNATE);
		buffer[outlen] = '|';
		len--;
		outlen++;

		outlen += syntax_sprintf(buffer + outlen, len, this->next,
					 CLI_TYPE_ALTERNATE);
		if (((cli_syntax_t *)this->next)->type != CLI_TYPE_ALTERNATE) {
			buffer[outlen] = ')';
			buffer[outlen + 1] = '\0';
			outlen++;
		}
		break;

	default:
		outlen = snprintf(buffer, len, "?");
		break;
	}

	return outlen;
}


/*
 *	Print syntax to STDOUT
 */
void syntax_printf(const cli_syntax_t *this)
{
	if (!this) return;

	char buffer[8192];

	syntax_sprintf(buffer, sizeof(buffer), this, CLI_TYPE_EXACT);
	syntax_fprintf(syntax_stdout, "%s", buffer);
}


/*
 *	Print syntax, one alternation on each line.
 */
void syntax_print_lines(const cli_syntax_t *this)
{
	char buffer[1024];

	if (!this) return;

	while (this->type == CLI_TYPE_ALTERNATE) {
		assert(((cli_syntax_t *)this->first)->type != this->type);
		syntax_sprintf(buffer, sizeof(buffer),
				      this->first, CLI_TYPE_EXACT);
		syntax_fprintf(syntax_stdout, "%s\r\n", buffer);
		this = this->next;
	}

	syntax_sprintf(buffer, sizeof(buffer), this, CLI_TYPE_EXACT);
	syntax_fprintf(syntax_stdout, "%s\r\n", buffer);
}


/*
 *	Handle error messages.
 */
static const char *syntax_error_string = NULL;
static const char *syntax_error_ptr = NULL;

static void syntax_error(const char *p, const char *msg)
{
	syntax_error_ptr = p;
	syntax_error_string = msg;
}

/*
 *	Internal "parse string into syntax"
 */
static int str2syntax(const char **buffer, cli_syntax_t **out, cli_type_t type)
{
	int rcode;
	const char *p, *q, *start;
	cli_syntax_t *this, *first;
	char tmp[256];

	p = *buffer;
	assert(*p != '\0');
	this = first = NULL;

	while (*p) {
		if (isspace((int) *p)) p++;

		if ((*p == ';') || (*p == '#')) break;

		start = p;

		if (*p == '|') {
			if (type == CLI_TYPE_ALTERNATE) break;

			syntax_error(start, "Unexpected '|'");
			syntax_free(first);
			return 0;
		}

		if (*p == ')') {
			if (type == CLI_TYPE_ALTERNATE) break;

			syntax_error(start, "Unexpected ')'");
			syntax_free(first);
			return 0;
		}

		if (*p == ']') {
			if (type == CLI_TYPE_OPTIONAL) break;

			syntax_error(start, "Unexpected ']'");
			syntax_free(first);
			return 0;
		}

		if (*p == '}') {
			if (type == CLI_TYPE_KEY) break;

			syntax_error(start, "Unexpected '}'");
			syntax_free(first);
			return 0;
		}

		if (*p == '[') {
			cli_syntax_t *a;
			p++;

			rcode = str2syntax(&p, &a, CLI_TYPE_OPTIONAL);
			if (!rcode) {
				syntax_free(first);
				return 0;
			}

			if (*p != ']') {
				syntax_error(start, "No matching ']'");
				syntax_free(first);
				return 0;
			}

			p++;
			this = syntax_new(CLI_TYPE_OPTIONAL, a, NULL);
			if (!this) {
				syntax_error(start, "Failed creating [...]");
				syntax_free(first);
				return 0;
			}
			goto next;
		}

		if (*p == '{') {
			cli_syntax_t *a;
			p++;

			for (q = p; *q; q++) {
				if (*q == '}') break;
				if (*q == '{') {
					syntax_error(start, "Cannot nest '{'");
					syntax_free(first);
					return 0;
				}
			}

			rcode = str2syntax(&p, &a, CLI_TYPE_KEY);
			if (!rcode) return 0;

			if (*p != '}') {
				syntax_error(start, "No matching '}'");
				syntax_free(first);
				return 0;
			}

			p++;
			this = syntax_new(CLI_TYPE_KEY, a, NULL);
			if (!this) {
				syntax_error(start, "Failed creating {...}");
				syntax_free(first);
				return 0;
			}
			goto next;
		}

		if (*p == '(') {
			cli_syntax_t *a, *b;
			p++;

			if ((*p == '|') || (*p == ')')) {
				syntax_error(start, "Empty alternation");
				syntax_free(first);
				return 0;
			}
			
			rcode = str2syntax(&p, &a, CLI_TYPE_ALTERNATE);
			if (!rcode) {
				syntax_free(first);
				return 0;
			}

			/*
			 *	Allow (foo) to mean foo
			 */
			if (*p == ')') {
				p++;
				this = a;
				goto next;
			}

			if (*p != '|') {
				syntax_error(start, "Expected '|' in alternation");
				syntax_free(first);
				return 0;
			}

			while (*p == '|') {
				q = p;
				p++;

				rcode = str2syntax(&p, &b, CLI_TYPE_ALTERNATE);
				if (!rcode) {
					syntax_free(first);
					return 0;
				}

				this = syntax_new(CLI_TYPE_ALTERNATE,
						  a, b);
				if (!this) {
					syntax_error(q, "Failed createing (|...)");
					syntax_free(first);
					return 0;
				}
				a = this;
			}

			if (*p != ')') {
				syntax_error(start, "No matching ')'");
				syntax_free(first);
				return 0;
			}
			this = a;
			p++;
			goto next;
		}

		if (!*p) continue;

		if ((*p > ' ') && (*p != '-') && (*p < '0') && (*p != '+')) {
			syntax_error(start, "Invalid character");
			syntax_free(first);
			return 0;
		}

		while (*p) {
			if ((*p == '(') || (*p == '[') || (*p == '|') ||
			    (*p == '{') || (*p == '}') || (*p == '=') ||
			    (*p == ')') || (*p == ']') || (*p == ' ') ||
			    (*p == '+')) {
				break;
			}
			p++;
		}

		memcpy(tmp, start, p - start);
		tmp[p - start] = '\0';

		if (*p == '=') {
			cli_syntax_t *next;
			/*
			 *	FIXME: macros must be uppercase
			 */

			p++;
			rcode = str2syntax(&p, &next, CLI_TYPE_MACRO);
			if (!rcode) {
				syntax_free(first);
				return 0;
			}

			this = syntax_new(CLI_TYPE_MACRO, tmp, next);
			if (!this) {
				syntax_error(start, "Failed creating macro");
				syntax_free(first);
				return 0;
			}
			this = NULL;
			continue;
		}

		if (isupper((int) tmp[0])) {
			cli_syntax_t find;

			memset(&find, 0, sizeof(find));
			find.type = CLI_TYPE_EXACT;
			find.first = tmp;
			find.next = NULL;
			this = syntax_find(&find);
			if (this) {
				this->refcount++;
				goto next;
			}

			find.type = CLI_TYPE_MACRO;
			find.hash = 0;
			this = syntax_find(&find);
			if (this) {
				this = this->next;
				this->refcount++;
				goto next;
			}
		}

		if (!*tmp) break;

		assert(*tmp != '\0');

		this = syntax_new(CLI_TYPE_EXACT, tmp, NULL);
		if (!this) {
			syntax_error(start, "Failed creating word");
			syntax_free(first);
			return 0;
		}

	next:
		if (*p == '+') {
			cli_syntax_t *a;

			p++;

			if (this->type == CLI_TYPE_PLUS) {
				syntax_error(start, "Unexpected '+'");
				syntax_free(this);
				syntax_free(first);
				return 0;
			}


			assert(this->type != CLI_TYPE_MACRO);
			a = syntax_new(CLI_TYPE_PLUS, this, NULL);
			if (!a) {
				syntax_error(start, "Failed creating +");
				syntax_free(first);
				return 0;
			}

			this = a;
		}

		if (!first) {
			first = this;
		} else {
			cli_syntax_t *a;

			assert(this != NULL);
			a = syntax_new(CLI_TYPE_CONCAT, first, this);
			if (!a) {
				syntax_error(start, "Failed appending word");
				syntax_free(first);
				return 0;
			}
			this = NULL;
			first = a;
		}
	}

	*buffer = p;
	*out = first;

	return 1;
}

/*
 *	Parse a string to a syntax.
 */
int syntax_parse(const char *name, cli_syntax_t **out)
{
	const char *p = name;

	return str2syntax(&p, out, CLI_TYPE_EXACT);
}

/*
 *	Add callbacks for a data type.
 */
int syntax_parse_add(const char *name, cli_syntax_parse_t callback)
{
	size_t len;
	cli_syntax_t *this, find;

	if (!name || !callback) return 0;

	memset(&find, 0, sizeof(find));
	find.type = CLI_TYPE_EXACT;
	find.first = (void *) name;
	find.next = NULL;
	this = syntax_find(&find);
	if (this) return 1;

	len = strlen(name);

	/*
	 *	Do this manually so that we can set the callback.
	 *	_new() expects "next" to be NULL.
	 */
	this = calloc(sizeof(*this) + len + 1, 1);
	if (!this) return 0;

	this->type = CLI_TYPE_EXACT;
	this->first = this + 1;
	memcpy(this->first, name, len + 1);

	if (!syntax_insert(this)) {
		free(this);
		return 0;
	}

	this->next = callback;	/* hack it in after the fact */

	assert(syntax_find(this) == this);
	assert(this->refcount == 0);
	this->refcount++;

	return 1;
}


/*
 *	Returns a NEW ref which matches the word
 */
static int syntax_prefix_words(int argc, char *argv[],
			       cli_syntax_t *this, cli_syntax_t *next)
{
	int matches, total;
	cli_syntax_t *a;

	assert(this != NULL);

	if (argc == 0) return 0;

	switch (this->type) {
	case CLI_TYPE_EXACT:
		argv[0] = this->first;
		return 1;

	case CLI_TYPE_KEY:
	case CLI_TYPE_OPTIONAL:
		matches = syntax_prefix_words(argc, argv, this->first, next);
		argc -= matches;
		argv += matches;

		if (!next) return matches;

		return matches + syntax_prefix_words(argc, argv, next, NULL);

	case CLI_TYPE_CONCAT:
		a = this->next;
		if (next) {
			a->refcount++;
			next->refcount++;
			a = syntax_new(CLI_TYPE_CONCAT, this->next, next);
			assert(a != this);
		}
		matches = syntax_prefix_words(argc, argv, this->first, a);
		if (next) syntax_free(a);
		return matches;

	case CLI_TYPE_ALTERNATE:
		total = 0;
		while (this->type == CLI_TYPE_ALTERNATE) {
			matches = syntax_prefix_words(argc, argv,
						      this->first, next);
			argc -= matches;
			argv += matches;
			total += matches;
			this = this->next;
		}
		assert(this->type != CLI_TYPE_ALTERNATE);

		return total + syntax_prefix_words(argc, argv, this, next);

	default:
		break;
	}

	return 0;
}


/*
 *	Walk over a syntax tree.
 */
#define CLI_WALK_PREORDER	(0)
#define CLI_WALK_INORDER	(1)
#define CLI_WALK_POSTORDER	(2)

#define CLI_WALK_STOP		(0)
#define CLI_WALK_CONTINUE	(1)
#define CLI_WALK_SKIP		(2)
#define CLI_WALK_REPEAT		(3)

typedef int (syntax_walk_t)(void *, cli_syntax_t *);

static int syntax_walk_all(cli_syntax_t *this, void *ctx,
			   syntax_walk_t *preorder,
			   syntax_walk_t *inorder,
			   syntax_walk_t *postorder)
{
	int rcode = CLI_WALK_CONTINUE;
	assert(this != NULL);

	if (preorder) {
		rcode = preorder(ctx, this);
		if (!rcode) return 0;
	}

#undef WALK
#define WALK(_x) if (!syntax_walk_all(_x, ctx, preorder, inorder, postorder)) return 0

	switch (this->type) {
	case CLI_TYPE_EXACT:
		if (inorder && !inorder(ctx, this)) return 0;
		break;

	case CLI_TYPE_PLUS:
		if (rcode == CLI_WALK_SKIP) return 1;

	repeat:
		WALK(this->first);

		if (inorder) {
			rcode = inorder(ctx, this);
			if (!rcode) return 0;
			if (rcode == CLI_WALK_REPEAT) goto repeat;
		}
		break;

	case CLI_TYPE_OPTIONAL:
		if (rcode == CLI_WALK_SKIP) return 1;

	case CLI_TYPE_KEY:
		WALK(this->first);

		if (inorder && !inorder(ctx, this)) return 0;
		break;

	case CLI_TYPE_CONCAT:
	case CLI_TYPE_ALTERNATE:
		WALK(this->first);

		if (inorder) {
			int rcode;

			rcode = inorder(ctx, this);
			if (!rcode) return 0;
			if (rcode == CLI_WALK_SKIP) break;
		}

		WALK(this->next);
		break;

	default:
		assert(0 == 1);
		return 0;
	}
#undef WALK

	if (postorder && !postorder(ctx, this)) return 0;

	return 1;
}

static int syntax_walk(cli_syntax_t *this, int order, void *ctx,
		       syntax_walk_t *callback)
{
	if (order == CLI_WALK_PREORDER) {
		return syntax_walk_all(this, ctx, callback, NULL, NULL);
	}

	if (order == CLI_WALK_INORDER) {
		return syntax_walk_all(this, ctx, NULL, callback, NULL);
	}

	if (order == CLI_WALK_PREORDER) {
		return syntax_walk_all(this, ctx, NULL, NULL, callback);
	}

	return 0;
}

/*
 *	Print raw syntax tree.
 */
static int syntax_print_pre(void *ctx, cli_syntax_t *this)
{
	ctx = ctx;		/* -Wunused */

	switch (this->type) {
	case CLI_TYPE_CONCAT:
		syntax_fprintf(syntax_stdout, "<");
		break;

	case CLI_TYPE_ALTERNATE:
		syntax_fprintf(syntax_stdout, "(");
		break;

	case CLI_TYPE_OPTIONAL:
		syntax_fprintf(syntax_stdout, "[");
		break;

	case CLI_TYPE_KEY:
		syntax_fprintf(syntax_stdout, "{");
		break;

	default:
		break;

	}

	return 1;
}

static int syntax_print_in(void *ctx, cli_syntax_t *this)
{
	ctx = ctx;		/* -Wunused */

	switch (this->type) {
	case CLI_TYPE_EXACT:
		syntax_fprintf(syntax_stdout, "%s", (const char *)this->first);
		break;

	case CLI_TYPE_CONCAT:
		syntax_fprintf(syntax_stdout, " ");
		break;

	case CLI_TYPE_ALTERNATE:
		syntax_fprintf(syntax_stdout, "|");
		break;

	default:
		break;

	}

	return 1;
}

static int syntax_print_post(void *ctx, cli_syntax_t *this)
{
	ctx = ctx;		/* -Wunused */

	switch (this->type) {
	case CLI_TYPE_CONCAT:
		syntax_fprintf(syntax_stdout, ">");
		break;

	case CLI_TYPE_ALTERNATE:
		syntax_fprintf(syntax_stdout, ")");
		break;

	case CLI_TYPE_OPTIONAL:
		syntax_fprintf(syntax_stdout, "]");
		break;

	case CLI_TYPE_KEY:
		syntax_fprintf(syntax_stdout, "}");
		break;

	case CLI_TYPE_PLUS:
		syntax_fprintf(syntax_stdout, "+");
		break;

	default:
		break;

	}

	return 1;
}

#define CLI_MATCH_EXACT   (0)
#define CLI_MATCH_PREFIX  (1)


/*
 *	Match a word against a syntax tree.
 */
static int syntax_match_exact(const char *word, cli_syntax_t *this, int sense)
{
	if (this->next) {
		return ((cli_syntax_parse_t)this->next)(word);
	}

	if (strcmp((char *)this->first, word) != 0) {
		if (sense == CLI_MATCH_EXACT) return 0;
		if (strncmp((char *)this->first,
			    word, strlen(word)) != 0) return 0;
	}

	return 1;
}


/*
 *	Walk over a tree, matching argv[] to a tree.
 */
typedef struct cli_match_word_t {
	int start_argc;
	int argc;
	char **argv;
	int match;
	int repeat;
	int want_more;
	int fail_argc;
	cli_syntax_t *fail;
	cli_syntax_t *key;
} cli_match_word_t;

typedef struct cli_match_found_t {
	const char *word;
	const char *type;
	cli_syntax_t *key;
} cli_match_found_t;

typedef struct cli_match_t {
	int argc, ptr;
	cli_match_word_t *word;
	cli_match_word_t stack[32];
	cli_match_found_t found[64];
} cli_match_t;

#if 0
#define TRACE_MATCH syntax_debug(__FUNCTION__, this)
#else
#define TRACE_MATCH
#endif

static int syntax_match_pre(void *ctx, cli_syntax_t *this)
{
	cli_match_t *m = ctx;

	TRACE_MATCH;

	assert(m->ptr >= 0);
	assert(m->word->argc >= 0);

	switch (this->type) {
	case CLI_TYPE_OPTIONAL:
		if (m->word->argc == 0) {
			m->word->match = 1;
			return CLI_WALK_SKIP;
		}

	case CLI_TYPE_ALTERNATE:
		m->stack[m->ptr + 1] = m->stack[m->ptr];
		m->stack[m->ptr + 1].start_argc = m->stack[m->ptr].argc;
		assert(m->stack[m->ptr].argc >= 0);
		goto fix;

	case CLI_TYPE_CONCAT:
	case CLI_TYPE_KEY:
		m->stack[m->ptr + 1] = m->stack[m->ptr];
		assert( m->stack[m->ptr].argc >= 0);
	fix:
		m->ptr++;
		m->word = m->stack + m->ptr;
		assert(m->word->argc >= 0);
		m->word->match = 0;
		if (this->type == CLI_TYPE_KEY) {
			m->word->key = this;
		}
		break;

	case CLI_TYPE_PLUS:
	case CLI_TYPE_EXACT:
		break;

	default:
		return 0;
	}

	return 1;
}

static int syntax_match_in(void *ctx, cli_syntax_t *this)
{
	int offset;
	cli_match_t *m = ctx;

	TRACE_MATCH;

	assert(m->word->argc >= 0);

	switch (this->type) {
	case CLI_TYPE_PLUS:
		if (m->word->want_more) {
			m->word->repeat = 0;
			break;
		}

		if (m->word->argc == 0) {
			m->word->repeat = 0;
			m->word->want_more = 0;
			break;
		}

		if (m->word->match) { /* matches, try again */
			m->word->start_argc = m->word->argc;
			m->word->repeat = 1;
			m->word->want_more = 0;
			return CLI_WALK_REPEAT;
		}
		
		if (m->word->repeat) { /* at least one was found, stop */
			m->word->repeat = 0;
			m->word->match = 1;
			m->word->want_more = 0;
			break; 
		}
		break;

	case CLI_TYPE_KEY:
		assert(m->word->key == this);

	case CLI_TYPE_CONCAT:
		assert(m->ptr >= 0);
		if (!m->word->match) return CLI_WALK_SKIP;
		break;

	case CLI_TYPE_ALTERNATE:
		if ((m->word->match) ||
		    (m->word->argc != m->word->start_argc)) {
			return CLI_WALK_SKIP;
		}
		m->word->match = 2;
		break;

	case CLI_TYPE_OPTIONAL:
		if (m->word->match) break;
		m->word->match = 2;
		break;

	case CLI_TYPE_EXACT:
		if (m->word->argc == 0) {
			m->word->match = 1;
			m->word->want_more = 1;
			m->word->fail = this;
			break; /* FIXME: skip? */
		}

		m->word->match = syntax_match_exact(m->word->argv[0], this,
						    CLI_MATCH_EXACT);

		offset = m->argc - m->word->argc;
		m->found[offset].word = m->word->argv[0];
		if (!this->next) {
			m->found[offset].type = NULL;
		} else {
			m->found[offset].type = this->first;
		}
		m->found[offset].key = m->word->key;

		if (m->word->match) {
			assert(m->word->argc > 0);
			m->word->argc--;
			m->word->argv++;
		} else if (!m->word->fail) {
			m->word->fail_argc = m->word->argc;
			m->word->fail = this;
		}
		m->word->want_more = 0;
		break;

	default:
		return 0;
	}

	return 1;
}


static int syntax_match_post(void *ctx, cli_syntax_t *this)
{
	cli_match_t *m = ctx;

	TRACE_MATCH;

	assert(m->ptr >= 0);
	assert(m->word->argc >= 0);

	switch (this->type) {
	case CLI_TYPE_KEY:
		m->word->key = NULL;

	case CLI_TYPE_PLUS:
	case CLI_TYPE_CONCAT:
	case CLI_TYPE_ALTERNATE:
	case CLI_TYPE_OPTIONAL:

		if (m->word->match > 0) {
#undef COPY
#define COPY(_x) m->stack[m->ptr - 1]._x = m->word->_x
			COPY(argc);
			COPY(argv);
			COPY(want_more);
			COPY(fail_argc);
			COPY(fail);
			/* NOT the key field */

			if (this->next && (m->word->match == 2) &&
			    (((cli_syntax_t *)this->next)->type != this->type)) {
				m->word->match = 0;
			}

			COPY(match);

		} else if (m->word->start_argc > m->word->fail_argc) {
			COPY(fail_argc);
			COPY(fail);
		}

		m->ptr--;
		m->word = m->stack + m->ptr;
		assert(m->word->argc >= 0);
		break;

	case CLI_TYPE_EXACT:
		break;

	default:
		return 0;
	}

	return 1;
}

/*
 *	Returns a NEW ref which matches the word
 */
static cli_syntax_t *syntax_match_word(const char *word, int sense,
				       cli_syntax_t *this, cli_syntax_t *next)
{
	cli_syntax_t *a, *found;

	assert(this != NULL);
	assert(word != NULL);

	switch (this->type) {
	case CLI_TYPE_EXACT:
		if (this->next) {
			if (!((cli_syntax_parse_t)this->next)(word)) {
				return NULL;
			}
		} else

		if (strcmp((char *)this->first, word) != 0) {
			if (sense == CLI_MATCH_EXACT) return NULL;
			if (strncmp((char *)this->first,
				    word, strlen(word)) != 0) return NULL;
		}

		this->refcount++;
	do_concat:
		if (!next) return this;

		assert(this->refcount > 0);
		assert(next->refcount > 0);

		next->refcount++;
		return syntax_new(CLI_TYPE_CONCAT, this, next);

	case CLI_TYPE_KEY:
		this = syntax_match_word(word, sense, this->first, NULL);
		if (!this) return NULL;
		goto do_concat;

	case CLI_TYPE_OPTIONAL:
		found = syntax_match_word(word, sense, this->first, next);
		if (found) return found;

		if (!next) return NULL;

		return syntax_match_word(word, sense, next, NULL);

	case CLI_TYPE_CONCAT:
		a = this->next;
		if (next) {
			a->refcount++;
			next->refcount++;
			a = syntax_new(CLI_TYPE_CONCAT, this->next, next);
			assert(a != this);
		}
		found = syntax_match_word(word, sense, this->first, a);
		if (next) syntax_free(a);
		return found;

	case CLI_TYPE_ALTERNATE:
		while (this->type == CLI_TYPE_ALTERNATE) {
			found = syntax_match_word(word, sense,
						  this->first, next);
			if (found) return found;
			this = this->next;
		}
		assert(this->type != CLI_TYPE_ALTERNATE);

		return syntax_match_word(word, sense, this, next);

	default:
		break;
	}

	return NULL;
}
				 
/*
 *	Return TRUE if the syntax accepts zero input.
 */
static int syntax_null_ok(const cli_syntax_t *this)
{
redo:
	assert(this != NULL);

	switch (this->type) {
	case CLI_TYPE_KEY:
		return 0;

	case CLI_TYPE_OPTIONAL:
		return 1;

	case CLI_TYPE_CONCAT:
		if (!syntax_null_ok(this->first)) return 0;
		this = this->next;
		goto redo;

	case CLI_TYPE_ALTERNATE:
		while (this->type == CLI_TYPE_ALTERNATE) {
			if (syntax_null_ok(this->first)) return 1;
			this = this->next;
		}
		goto redo;
		
	default:
		break;
	}

	return 0;
}


/*
 *	Check argv against a syntax.
 */
int syntax_check(cli_syntax_t *head, int argc, char *argv[],
		 const char **fail)
{
	int rcode;
	cli_match_t match;

	if (!head || !argc) return 1;	/* no syntax checking */

	if (argc < 0) return -1;

	memset(&match, 0, sizeof(match));
	match.ptr = 0;
	match.argc = argc;
	match.word = &match.stack[0];
	match.word->start_argc = argc;
	match.word->argc = argc;
	match.word->argv = argv;
	match.word->match = 0;

	rcode = syntax_walk_all(head, &match, syntax_match_pre,
				syntax_match_in, syntax_match_post);
			     
	if (!rcode) return -1;	/* failure walking the tree */

	if (match.stack[0].want_more) return 0;  /* matched some, not all */

	if (match.stack[0].argc != 0) {
		rcode = argc - match.stack[0].fail_argc;

		if (match.stack[0].fail) {
			*fail = argv[rcode];
		}

		return -1;
	}

	return 1;
}


cli_syntax_t *syntax_match_max(cli_syntax_t *head, int argc, char *argv[])
{
	int i, match;
	cli_syntax_t *this, *next;
	cli_syntax_t *a;

	if (!head) return NULL;	/* no syntax checking */

	this = head;
	this->refcount++;
	match = 0;

	if (argc == 0) return this;

	while (this && (match < argc)) {
		next = syntax_match_word(argv[match], CLI_MATCH_EXACT,
					 this, NULL);
		if (!next) break;

		syntax_free(this);

		this = syntax_skip(next, 1);

		assert(this != next);
		syntax_free(next);
		match++;
	}

	if (match == 0) {
		syntax_free(this);
		return NULL;
	}

	next = this;
	for (i = match - 1; i >= 0; i--) {
		a = syntax_new(CLI_TYPE_EXACT, argv[i], NULL);
		assert(a != NULL);

		this = syntax_new(CLI_TYPE_CONCAT, a, next);
		assert(this != NULL);
		next = this;
	}

	return this;
}


#ifndef NDEBUG
static void syntax_debug(const char *msg, cli_syntax_t *this)
{
	syntax_fprintf(syntax_stdout, "%s ", msg);
	syntax_printf(this);
	syntax_fprintf(syntax_stdout, "\r\n");
}
#endif


int syntax_tab_complete(cli_syntax_t *head, const char *in, size_t len,
			int max_tabs, char *tabs[])
{
	int i, argc, match, exact;
	size_t out;
	cli_syntax_t *this, *next;
	char *argv[256];
	char *p, buffer[256];
	char mybuf[1024];

	if (!head) return 0;	/* no syntax checking */

	this = head;
	this->refcount++;

	memcpy(mybuf, in, len + 1);
	argc = str2argv(mybuf, len, 256, argv);
	if (argc == 0) return 0;

	match = 0;
	exact = CLI_MATCH_EXACT;

	while (this && (match < argc)) {
		next = syntax_match_word(argv[match], exact, this, NULL);
		if (!next) {
			/* no match, last word, stop */
			if ((match + 1) != argc) {
			none:
				syntax_free(this);
				return 0;
			}

			if (exact != CLI_MATCH_EXACT) goto none;

			exact = CLI_MATCH_PREFIX;
			continue;
		}

		syntax_free(this);

		if (exact != CLI_MATCH_EXACT) {
			this = next;
			next = NULL;
			assert((match + 1) == argc);
			break;
		}

		this = syntax_skip(next, 1);
		assert(this != next);
		syntax_free(next);
		next = NULL;
		match++;
	}

	out = 0;
	p = buffer;
	match = argc;
	if (exact != CLI_MATCH_EXACT) match--;

	for (i = 0; i < match; i++) {
		out = snprintf(p, buffer + sizeof(buffer) - p, "%s ", argv[i]);
		p += out;
	}

	if (exact == CLI_MATCH_PREFIX) {
		if (this->type == CLI_TYPE_CONCAT) {
			next = this->first;
		} else {
			next = this;
		}

		assert(next->type == CLI_TYPE_EXACT);

		snprintf(p, buffer + sizeof(buffer) - p, "%s ",
			 (char *) next->first);
		tabs[0] = strdup(buffer);
		syntax_free(this);
		return 1;
	}

	if (!this) return 0;

	argc = syntax_prefix_words(256, argv, this, NULL);
	if (argc > max_tabs) argc = max_tabs;

	for (i = 0; i < argc; i++) {
		assert(argv[i] != NULL);
		strcpy(p, argv[i]);
		tabs[i] = strdup(buffer);
	}

	syntax_free(this);

	return argc;
}


/*
 *	Parse a file into a syntax, ignoring blank lines and comments.
 */
int syntax_parse_file(const char *filename, cli_syntax_t **phead)
{
	int lineno;
	FILE *fp;
	char buffer[1024];
	cli_syntax_t *head, *this;

	if (!phead) return -1;

	if (!*phead) {
		int i;

		for (i = 0; recli_datatypes[i].name != NULL; i++) {
			if (!syntax_parse_add(recli_datatypes[i].name,
					      recli_datatypes[i].parse)) {
				return -1;
			}
		}
	}

	fp = fopen(filename, "r");
	if (!fp) {
		syntax_fprintf(syntax_stderr, "Failed opening %s: %s\n",
			filename, strerror(errno));
		return -1;
	}

	lineno = 0;
	head = NULL;

	while (fgets(buffer, sizeof(buffer), fp) != NULL) {
		char *p;
		const char *q;
		lineno++;

		p = strchr(buffer, '\r');
		if (p) *p = '\0';

		p = strchr(buffer, '\n');
		if (p) *p = '\0';

		p = buffer;
		while (isspace((int) *p)) p++;
		if (!*p) continue;

		q = p;

		if (!str2syntax(&q, &this, CLI_TYPE_EXACT)) {
			syntax_fprintf(syntax_stderr, "%s line %d: Invalid syntax at \"%s\": %s\n",
				filename, lineno,
				syntax_error_ptr, syntax_error_string);
			syntax_free(head);
			return -1;
		}

		if (!this) continue; /* empty line */

		if (!head) {
			head = this;
		} else {
			cli_syntax_t *a;

			a = syntax_new(CLI_TYPE_ALTERNATE, head, this);
			if (!a) return -1;
			this = NULL;
			head = a;
		}
	}

	fclose(fp);

	if (0) {
		syntax_walk_all(head, NULL, syntax_print_pre, syntax_print_in,
				syntax_print_post);
	}

	*phead = head;

	return 0;
}


static void add_help(cli_syntax_t **phead, cli_syntax_t *last,
		     const char *help, int flag)
{
	cli_syntax_t *this;


	this = syntax_new(CLI_TYPE_EXACT, help, NULL);
	assert(this != NULL);
	this->length = flag; /* internal flag... */

	this = syntax_new(CLI_TYPE_CONCAT, last, this);
	assert(this != NULL);
	
	if (!*phead) {
		*phead = this;
	} else {
		cli_syntax_t *a;
		
		a = syntax_new(CLI_TYPE_ALTERNATE, *phead, this);
		assert(a != NULL);
		
		*phead = a;
	}
}


/*
 *	Parse a simplified Markdown "help" file into a syntax.
 */
int syntax_parse_help(const char *filename, cli_syntax_t **phead)
{
	int lineno, done;
	FILE *fp;
	char buffer[1024];
	char *h, help[8192];
	cli_syntax_t *head, *this, *last;

	if (!phead) return -1;

	fp = fopen(filename, "r");
	if (!fp) {
		syntax_fprintf(syntax_stderr, "Failed opening %s: %s\n",
			filename, strerror(errno));
		return -1;
	}

	done = lineno = 0;
	last = head = NULL;
	h = NULL;

	while (fgets(buffer, sizeof(buffer), fp) != NULL) {
		size_t len;
		char *p;
		char *q;

		lineno++;
		p = buffer;
		
		/*
		 *	Had previous command and help text.  Add it in.
		 */
		if ((*p == '#') && last) {
		do_last:
			if (!h) goto error;

			h = help;
			while (isspace((int) *h)) h++;
			if (*h) {
				add_help(&head, last, help, 1);
			} else {
				syntax_free(last);
			}
			last = NULL;
			h = NULL;
			if (done) break;
		}

		if (*p == '#') {
			assert(last == NULL);
			assert(h == NULL);

			while (*p == '#') p++;
			q = p;

			p = strchr(q, '\r');
			if (p) *p = '\0';
			
			p = strchr(q, '\n');
			if (p) *p = '\0';
			
			while (isspace((int) *q)) q++;
			if (!*q) goto error;

			p = q;
			while (*p) {
				if ((*p == '(') || (*p == '|') || (*p == ')') ||
				    (*p == '[') || (*p == ']')) goto error;
				p++;
			}

			if (!str2syntax((const char **)&q, &this, CLI_TYPE_EXACT)) {
			error:
				syntax_fprintf(syntax_stderr, "%s line %d: Invalid syntax \"%s\"\n",
					filename, lineno, buffer);
				syntax_free(head);
				if (last) syntax_free(last);
				return -1;
			}

			assert(this != NULL);

			last = this;
			h = help;
			*h = '\0';
			continue;
		}

		p = strchr(buffer, '\r');
		if (p) *p = '\0';
		p = strchr(buffer, '\n');
		if (p) *p = '\0';

		strcat(buffer, "\r\n");

		if (last && (strncmp(buffer, "    ", 4) == 0)) {
			last->refcount++;
			add_help(&head, last, buffer + 4, 2);
			continue;
		}

		len = strlen(buffer);
		if ((h + len) >= (help + sizeof(help))) {
			syntax_fprintf(syntax_stderr, "%s line %d: Too much help text\n",
				filename, lineno);
			syntax_free(head);
			if (last) syntax_free(last);
			return -1;
		}

		memcpy(h, buffer, len);
		h += len;
		*h = '\0';
	}

	if (last) {
		assert(done == 0);
		done = 1;
		goto do_last;
	}

	fclose(fp);

	*phead = head;

	return 0;
}


/*
 *	Show help for a given argv[]
 */
const char *syntax_show_help(cli_syntax_t *head, int argc, char *argv[],
			     int flag)
{
	int rcode;
	const char *help = NULL;
	cli_syntax_t *this, *tail;
	cli_match_t match;

	if (!head || (argc < 0)) return NULL;

	if (argc == 0) {
		head->refcount++;
		tail = head;
		goto show_help;
	}

	memset(&match, 0, sizeof(match));
	match.ptr = 0;
	match.argc = argc;
	match.word = &match.stack[0];
	match.word->start_argc = argc;
	match.word->argc = argc;
	match.word->argv = argv;
	match.word->match = 0;

	rcode = syntax_walk_all(head, &match, syntax_match_pre,
				syntax_match_in, syntax_match_post);
			     
	if (!rcode) return NULL;

	if (match.stack[0].argc != 0) return NULL;

	if (!match.stack[0].want_more) return NULL;

	/*
	 *	And duplicate a lot of the above work because
	 *	the function returns the wrong "fail".
	 */
	this = syntax_match_max(head, argc, argv);
	tail = syntax_skip(this, argc);
	assert(tail != NULL);
	syntax_free(this);

show_help:
	this = tail;

	while (this->type == CLI_TYPE_ALTERNATE) {
		cli_syntax_t *first = tail->first;
		if ((first->type == CLI_TYPE_EXACT) &&
		    (first->length == flag)) {
			this = first;
			break;
		}

		this = this->next;
	}

	if ((this->type == CLI_TYPE_EXACT) &&
	    (this->length == flag)) {
		help = this->first;
	}

	syntax_free(tail);
	return help;
}

static size_t syntax_sprintf_word(char *buffer, size_t len, cli_syntax_t *this)
{
	size_t outlen;
	cli_syntax_t *next;

redo:
	switch (this->type) {
	case CLI_TYPE_EXACT:
		if (this->length == 1) return 0;
		return snprintf(buffer, len, "%s", (char *) this->first);

	case CLI_TYPE_CONCAT:
		next = this->next;
		if ((next->type != CLI_TYPE_EXACT) ||
		    (next->length != 2)) return 0;
		outlen = syntax_sprintf_word(buffer, len, this->first);
		if (outlen == 0) return 0;

		buffer += outlen;
		len -= outlen;
		strcpy(buffer, ": ");
		buffer += 2;
		len -= 2;
		return outlen + syntax_sprintf_word(buffer, len, this->next);

	case CLI_TYPE_OPTIONAL:
	case CLI_TYPE_KEY:
	case CLI_TYPE_PLUS:
		this = this->first;
		goto redo;

	default:
		return 0;
	}

}

int syntax_print_context_help(cli_syntax_t *head, int argc, char *argv[])
{
	int rcode;
	size_t len;
	cli_syntax_t *this, *tail;
	cli_match_t match;
	char buffer[1024];

	if (!head || (argc < 0)) return -1;

	if (argc == 0) {
		head->refcount++;
		tail = head;
		goto show_help;
	}

	memset(&match, 0, sizeof(match));
	match.ptr = 0;
	match.argc = argc;
	match.word = &match.stack[0];
	match.word->start_argc = argc;
	match.word->argc = argc;
	match.word->argv = argv;
	match.word->match = 0;

	rcode = syntax_walk_all(head, &match, syntax_match_pre,
				syntax_match_in, syntax_match_post);
			     
	if (!rcode) return -1;

	if (match.stack[0].argc != 0) return -1;

	if (!match.stack[0].want_more) return -1;

	this = syntax_match_max(head, argc, argv);
	tail = syntax_skip(this, argc);
	assert(tail != NULL);
	syntax_free(this);

show_help:
	this = tail;

	while (this->type == CLI_TYPE_ALTERNATE) {
		len = syntax_sprintf_word(buffer, sizeof(buffer),
					  this->first);

		if (len != 0) syntax_fprintf(syntax_stdout, "\t%s", buffer);

		this = this->next;
	}

	assert(this->type != CLI_TYPE_ALTERNATE);

	syntax_free(tail);
	len = syntax_sprintf_word(buffer, sizeof(buffer), this);
	if (len == 0) return 0;
	
	syntax_fprintf(syntax_stdout, "\t%s", buffer);
	return 1;
}
