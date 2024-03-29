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

/*
 *	This file implements an abstract syntax tree based on
 *	content-addressible nodes.
 */
typedef enum cli_type_t {
	CLI_TYPE_INVALID = 0,
	CLI_TYPE_EXACT,
	CLI_TYPE_VARARGS,
	CLI_TYPE_OPTIONAL,
	CLI_TYPE_CONCAT,
	CLI_TYPE_ALTERNATE,
	CLI_TYPE_MACRO,
	CLI_TYPE_PLUS,
	CLI_TYPE_FORCE_EXACT   	/* fake node */
} cli_type_t;

#define FLAG_NEEDS_TTY	      (1 << 0)
#define FLAG_CASE_INSENSITIVE (1 << 1)
#define FLAGS_EXPORT		(FLAG_NEEDS_TTY)

/*
 *	Define this to get debugging about some of the operations it's
 *	doing.
 */
#define DEBUG_PRINT (0)

struct cli_syntax_t {
	cli_type_t type;

	uint32_t hash;
	void *first;
	void *next;

	int refcount;
	int length;		/* for concatenation nodes */

	int min, max;		/* for '*', '+', and [] */
};

#define FNV_MAGIC_INIT (0x811c9dc5)
#define FNV_MAGIC_PRIME (0x01000193)

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

static uint32_t fnv_hash(const void *data, size_t size)
{
	return fnv_hash_update(data, size, FNV_MAGIC_INIT);
}

static cli_syntax_t *syntax_find(cli_syntax_t *this);
#ifndef NDEBUG
void syntax_debug(const char *msg, cli_syntax_t *this);
#endif

static cli_syntax_t *syntax_concat_prefix(cli_syntax_t *prefix, int lcp,
					  cli_syntax_t *tail);

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
		hash = fnv_hash_update(this->first, strlen((char *)this->first),
				       hash);
		hash = fnv_hash_update(&this->min, sizeof(this->min),
				       hash);
		break;

	case CLI_TYPE_VARARGS:
		assert(this->next == NULL);

	case CLI_TYPE_MACRO:
		hash = fnv_hash_update(this->first, strlen((char *)this->first),
				       hash);
		break;

	case CLI_TYPE_OPTIONAL:
		assert(this->first != NULL);
		assert(this->next == NULL);
		assert(syntax_find(this->first) == this->first);
		
		hash = fnv_hash_update(this->first, sizeof(this->first),
				       hash);
		break;

	case CLI_TYPE_PLUS:
		assert(this->first != NULL);
		assert(this->next == NULL);
		assert(syntax_find(this->first) == this->first);
		
		hash = fnv_hash_update(this->first, sizeof(this->first),
				       hash);
		hash = fnv_hash_update(&this->min, sizeof(this->min),
				       hash);
		hash = fnv_hash_update(&this->max, sizeof(this->max),
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
	int order;

	if (a == b) return 0;

	if ((a->type == CLI_TYPE_EXACT) && (b->type == CLI_TYPE_EXACT)) {
		/*
		 *	Real keywords come first.
		 */
		if (a->next && !b->next) return +1;
		if (!a->next && b->next) return -1;

		return strcmp((char *)a->first, (char *) b->first);
	}

	if ((a->type == CLI_TYPE_VARARGS) && (b->type != CLI_TYPE_VARARGS)) {
		return -1;
	}

	if ((a->type != CLI_TYPE_VARARGS) && (b->type == CLI_TYPE_VARARGS)) {
		return +1;
	}

	if ((a->type != CLI_TYPE_CONCAT) && (b->type == CLI_TYPE_CONCAT)) {
		order = syntax_order(a, b->first);
		if (order != 0) return order;

		return -1;		/* a < b */
	}

	if ((a->type == CLI_TYPE_CONCAT) && (b->type != CLI_TYPE_CONCAT)) {
		order = syntax_order(a->first, b);
		if (order != 0) return order;

		return +1;		/* a > b */
	}

	if ((a->type == CLI_TYPE_CONCAT) && (b->type == CLI_TYPE_CONCAT)) {
		order = syntax_order(a->first, b->first);
		if (order != 0) return order;

		return syntax_order(a->next, b->next);
	}

	assert(a->type != CLI_TYPE_CONCAT);
	assert(b->type != CLI_TYPE_CONCAT);

	if ((a->type == CLI_TYPE_OPTIONAL) && (b->type == CLI_TYPE_OPTIONAL)) {
		return syntax_order(a->first, b->first);
	}

	if (a->type == CLI_TYPE_OPTIONAL) {
		order = syntax_order(a->first, b);
		if (order != 0) return order;

		return +1;	/* a < b */
	}

	if (b->type == CLI_TYPE_OPTIONAL) {
		order = syntax_order(a, b->first);
		if (order != 0) return order;

		return -1;	/* a > b */
	}

	if ((a->type == CLI_TYPE_ALTERNATE) && (b->type != CLI_TYPE_ALTERNATE)) {
		return +1;
	}

	if ((a->type != CLI_TYPE_ALTERNATE) && (b->type == CLI_TYPE_ALTERNATE)) {
		return -1;
	}

	if ((a->type == CLI_TYPE_ALTERNATE) && (b->type == CLI_TYPE_ALTERNATE)) {
		order = syntax_order(a->first, b->first);
		if (order != 0) return order;

		return syntax_order(a->next, b->next);
	}

	/*
	 *	We've got to pick some order, so pick a stupid one.
	 *
	 *	FIXME: If the output changes randomly, the problem is here.
	 *	find the case which isn't handled, and handle it.
	 */
	if (a < b) return -1;
	if (a > b) return +1;

	return 0;
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
	case CLI_TYPE_VARARGS:
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
		recli_fprintf(recli_stdout, "NUM ENTRIES LEFT: %d\n", num_entries);
		for (i = 0; i < table_size; i++) {
			if (!hash_table[i]) continue;

			recli_fprintf(recli_stdout, "LEFT %d ", hash_table[i]->refcount);
			syntax_printf(hash_table[i]);
			recli_fprintf(recli_stdout, "\n");
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

	/* Create new hash table if not yet allocated */
	if (!num_entries) {
		hash_table = calloc(sizeof(hash_table[0]), 256);
		if (!hash_table) return 0;
		table_size = 256;
	}

	/* Ensure entry we are inserting has a unique hash */
	if (!this->hash) syntax_hash(this);

	/* Better not be able to find it already */
	assert(syntax_find(this) == NULL);

#ifndef NDEBUG
	if ((this->type == CLI_TYPE_CONCAT) ||
	    (this->type == CLI_TYPE_ALTERNATE)) {
		cli_syntax_t *first = this->first;
		assert(first->type != this->type);
	}

	if (this->type == CLI_TYPE_ALTERNATE) {
//		assert(syntax_order(this->first, this->next) <= 0);
	}
#endif

retry:
	/*
	 *	Look for the hash in the table. If the slot is
	 *	free, insert the entry there and return.
	 */
	hash = this->hash;
	if (!hash_table[hash & (table_size - 1)]) {
		hash_table[hash & (table_size - 1)] = this;
		num_entries++;
		return 1;

	}	
	      
	assert(this != hash_table[hash & (table_size - 1)]);

	/*
	 *	Slot wasn't free. We'll double the table size and
	 *	try again. Copy all entries from old table to new
	 *	table, inserting into the newly calculated hash
	 *	slot.
	 */
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


static cli_syntax_t *syntax_alloc(cli_type_t type, void *first, void *next);


#ifdef WITH_LCS
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
#endif				/* WITH_LCS */


static cli_syntax_t *syntax_one_prefix(cli_syntax_t *a, cli_syntax_t *b)
{
	assert(a != b);

	if ((a->type != CLI_TYPE_CONCAT) &&
	    (b->type != CLI_TYPE_CONCAT)) {
		return NULL;
	}

	if ((a->type == CLI_TYPE_CONCAT) &&
	    (b->type != CLI_TYPE_CONCAT)) {
		if (a->first == b) return a->first;
		return NULL;
	}

	if ((a->type != CLI_TYPE_CONCAT) &&
	    (b->type == CLI_TYPE_CONCAT)) {
		if (b->first == a) return b->first;
		return 0;
	}

	if (a->first != b->first) return NULL;

	return a->first;
}

/*
 *	Longest common prefix of two lists.
 */
static int syntax_lcp(cli_syntax_t *a, cli_syntax_t *b)
{
	if (a == b) {
		if (a->type != CLI_TYPE_CONCAT) return 1;

		return a->length;
	}

	if (!syntax_one_prefix(a, b)) return 0;

	if ((a->type != CLI_TYPE_CONCAT) || (b->type != CLI_TYPE_CONCAT)) {
		return 1;
	}

	return 1 + syntax_lcp(a->next, b->next);
}

static int syntax_alternate_length(cli_syntax_t *a)
{
	int total = 1;

	while (a->type == CLI_TYPE_ALTERNATE) {
		total++;
		a = a->next;
	}

	return total;
}

static void syntax_alternate_split(cli_syntax_t **out, cli_syntax_t *a)
{
	while (a->type == CLI_TYPE_ALTERNATE) {
		cli_syntax_t *b = a->first;

		assert(b != NULL);
		assert(b->type != CLI_TYPE_ALTERNATE);
		*out = syntax_ref(b);
		out++;
		a = a->next;
	}

	*out = syntax_ref(a);
}


static cli_syntax_t *syntax_alternate(cli_syntax_t *a, cli_syntax_t *b);

static cli_syntax_t *syntax_split_prefix(cli_syntax_t *a, cli_syntax_t *b, int lcp)
{
	cli_syntax_t *c, *d, *e, *f;

	d = syntax_skip_prefix(a, lcp);
	e = syntax_skip_prefix(b, lcp);

	if (!d) {
		f = syntax_alloc(CLI_TYPE_OPTIONAL, e, NULL);
		if (!f) {
		error:
			syntax_free(a);
			syntax_free(b);
			return NULL;
		}

	} else if (!e) {
		f = syntax_alloc(CLI_TYPE_OPTIONAL, d, NULL);
		if (!f) goto error;

	} else {
		f = syntax_alternate(d, e);
		if (!f) goto error;
	}

	c = syntax_concat_prefix(a, lcp, f);

	syntax_free(a);
	syntax_free(b);
	return c;
}

/*
 *	Pack the entries together,
 *	so that there are no holes.
 */
static int pack_array(cli_syntax_t **nodes, int total)
{
	int i, j;

	j = 0;
	for (i = 1; i < total; i++) {
		if (!j) {
			if (!nodes[i]) {
				j = i;
			}

			continue;
		}

		if (!nodes[i]) continue;

		nodes[j] = nodes[i];
		nodes[i] = NULL;
		j++;
		assert(nodes[j] == NULL);
	}

	if (j) return j;

	return total;
}


static void recursive_prefix(cli_syntax_t **nodes, int total)
{
	int i, j, lcp, num_prefix;
	int optional;
	cli_syntax_t *a, *b, *prefix, *suffix;

	if (total <= 1) return;

	assert(nodes[0] != NULL);

	prefix = NULL;

	total = pack_array(nodes, total);

	if (total <= 1) return;

	assert(nodes[1] != NULL);

	/*
	 *	Check for a one-node prefix.  The returned prefix is
	 *	NOT referenced.
	 */
	prefix = syntax_one_prefix(nodes[0], nodes[1]);

	/*
	 *	When there's only two entries in the array, then let
	 *	the parent do the alternation (if there's no prefix),
	 *	or recurse to calculate the alternation (if there's a
	 *	prefix).
	 */
	if (total == 2) {
		/*
		 *	Let the caller deal with it.
		 */
		if (!prefix) return;

		a = syntax_alternate(nodes[0], nodes[1]);
		nodes[1] = NULL;
		nodes[0] = a;
		return;
	}

	/*
	 *	Entries 0 and 1 don't have a common prefix.  But maybe
	 *	entries 1, 2, ... have a common prefix.  Go check that.
	 */
	if (!prefix) {
		recursive_prefix(&nodes[1], total - 1);
		return;
	}

	/*
	 *	We have a prefix of at least one node.  See how many
	 *	nodes share the prefix.
	 */
	num_prefix = 2;
	for (j = 2; j < total; j++) {
		lcp = syntax_lcp(prefix, nodes[j]);
		if (lcp == 0) {
			num_prefix = j;
			break;
		}
	}

	/*
	 *	Only the first two share the prefix.  Just do
	 *	alternation, and then go process the rest of the input
	 *	array.
	 */
	if (num_prefix == 2) {
		a = syntax_alternate(nodes[0], nodes[1]);
		nodes[1] = NULL;
		nodes[0] = a;

		if (total >= 4) {
			recursive_prefix(&nodes[2], total - 2);
		}
		return;
	}

	/*
	 *	We now have "num_prefix" entries in the array, which
	 *	all have at least a one-node prefix.
	 *
	 *	We want to find the *longest* prefix, so we have to continue
	 *	this process recursively.
	 *
	 *	We also want to avoid calling syntax_alternate(), as
	 *	it might end up calling us recursively.  Instead, we
	 *	just create the alternation nodes here, manually.
	 *
	 *	We catch the special case of optional nodes manually...
	 */
	prefix->refcount++;

	for (i = 0; i < num_prefix; i++) {
		suffix = syntax_skip_prefix(nodes[i], 1);
		syntax_free(nodes[i]);
		nodes[i] = suffix;
	}

	/*
	 *	And now we check another special case.. optional
	 *	arguments.  nodes[0] MAY be NULL, in which case it's really:
	 *
	 *	prefix [ alternation stuff ... ]
	 */
	if (!nodes[0]) {
		optional = 1;
	} else {
		optional = 0;
	}

	/*
	 *	We may have: (a b c | a b d | a b e)
	 *	go check for that.  But ONLY so long as
	 *	we have a common prefix
	 */
	recursive_prefix(&nodes[optional], num_prefix);

	/*
	 *	Walk back up the array, manually doing alternation,
	 *	and skipping any NULL entries.  Once we're done,
	 */
	b = NULL;
	for (i = num_prefix - 1; i >= 0; i--) {
		if (!nodes[i]) continue;

		if (!b) {
			b = nodes[i];
			nodes[i] = NULL;
			continue;
		}

		a = syntax_alloc(CLI_TYPE_ALTERNATE, nodes[i], b);
		assert(a != NULL);
		b = a;
		nodes[i] = NULL;
	}

	if (optional) {
		a = syntax_alloc(CLI_TYPE_OPTIONAL, b, NULL);
		assert(a != NULL);
		b = a;
	}

	a = syntax_alloc(CLI_TYPE_CONCAT, prefix, b);
	assert(a != NULL);
	nodes[0] = a;

	/*
	 *	Only one trailing thing at the end, it can't have a
	 *	common prefix, or we would have found it.
	 */
	if ((total - num_prefix) == 1) return;

	/*
	 *	Look for common prefixes of the rest of the array.
	 */
	recursive_prefix(&nodes[num_prefix], total - num_prefix);
}

/*
 *	FIXME: if the first node is exact or concat with exact, then
 *	walk the syntax DOWN (like syntax_check), and re-constitute it
 *	coming back up.  That code should be a lot simpler than this,
 *	and should be better at maintaining normal form.
 *
 */
static cli_syntax_t *syntax_alternate(cli_syntax_t *a, cli_syntax_t *b)
{
	int i, j, total, total_a, total_b;
	int lcp;
	cli_syntax_t *c;
	cli_syntax_t **nodes;

	assert(a != NULL);
	assert(b != NULL);

	/*
	 *	a|a ==> a
	 */
	if (a == b) {
		syntax_free(a);
		return b;
	}
	/*
	 *	Disallow ( ... | a )
	 *	Disallow ( a | ... )
	 */
	if ((a->type == CLI_TYPE_VARARGS) ||
	    (b->type == CLI_TYPE_VARARGS)) {
		syntax_error_string = "Invalid use of ... in alternation";
		syntax_free(a);
		syntax_free(b);
		return NULL;
	}

	/*
	 *	a|b ==> a|b
	 *	b|a ==> a|b
	 */
	if ((a->type == CLI_TYPE_EXACT) &&
	    (b->type == CLI_TYPE_EXACT)) {
	create:
		if (syntax_order(a, b) > 0) {
			c = a;
			a = b;
			b = c;
		}

		c  = syntax_alloc(CLI_TYPE_ALTERNATE, a, b);
		return c;				
	}

	/*
	 *	(a b|a c) ==> a (b|c)
	 */
	lcp = syntax_lcp(a, b);
	if (lcp > 0) {
		c = syntax_split_prefix(a, b, lcp);
		return c;
	}

#ifdef WITH_LCS
	/*
	 *	(a foo|b foo) ==> (a|b) foo
	 */
	c = syntax_lcs(a, b);
	if (c) {
		int lcp;
		cli_syntax_t *d, *e, *f;

		if (a != c) {
			lcp = syntax_prefix_length(a, c);
			assert(lcp > 0);
			d = syntax_concat_prefix(a, lcp, NULL);
			if (!d) {
			error:
				syntax_free(a);
				syntax_free(b);
				syntax_free(c);
				return NULL;
			}
		} else {
			/*
			 *	a | b a = [b] a
			 */
			d = NULL;
		}

		if (b != c) {
			lcp = syntax_prefix_length(b, c);
			assert(lcp > 0);
			e = syntax_concat_prefix(b, lcp, NULL);
			if (!e) goto error;
		} else {
			assert(d != NULL);

			/*
			 *	a b | b = [a] b
			 */
 			e = NULL;
		}

		if (!d) {
			assert(e != NULL);
			f = syntax_alloc(CLI_TYPE_OPTIONAL, e, NULL);

		} else if (!e) {
			assert(d != NULL);
			f = syntax_alloc(CLI_TYPE_OPTIONAL, d, NULL);

		} else {
			f = syntax_alternate(d, e);
		}

		if (!f) goto error;

		syntax_free(a);
		syntax_free(b);

		d = syntax_alloc(CLI_TYPE_CONCAT, f, c);
		return d;
	}
#endif	/* WITH_LCS */

	/*
	 *	Anything else, just create the node.
	 */
	if ((a->type != CLI_TYPE_ALTERNATE) &&
	    (b->type != CLI_TYPE_ALTERNATE)) {
		goto create;
	}

	/*
	 *	One or both nodes are alternation.  Break them apart,
	 *	sort them, and put them back together.
	 */
	total_a = syntax_alternate_length(a);
	assert(total_a >= 1);
	total_b = syntax_alternate_length(b);
	assert(total_b >= 1);

	total = total_a + total_b;
	nodes = calloc(total * sizeof(nodes[0]), 1);
	assert(nodes != NULL);

	syntax_alternate_split(nodes, a);
	syntax_alternate_split(&nodes[total_a], b);

	/*
	 *	Bubble sort FTW.
	 */
	for (i = 0; i < (total - 1); i++) {
		if (!nodes[i]) continue;

		for (j = i + 1; j < total; j++) {
			int order;

			if (!nodes[j]) continue;

			if (nodes[i] == nodes[j]) {
				syntax_free(nodes[j]);
				nodes[j] = NULL;
				continue;
			}

			order = syntax_order(nodes[i], nodes[j]);
			if (order > 0) {
				c = nodes[i];
				nodes[i] = nodes[j];
				nodes[j] = c;
			}
		}
	}

	/*
	 *	Enforce LCP on nodes, via an O(N^2) algorithm.
	 *
	 *	FIXME: add LCS, too?  Which we care about less, to be honest.
	 *
	 *	FIXME: we don't really want a pair-wise LCP.  Instead, we want to
	 *	take the common prefix (max length 1) of the first two elements,
	 *	and then apply it to as many subsequent elemnts as possible.
	 *	This process can be applied recursively.  The result should be
	 *	prefixes for all nodes.
	 */
	recursive_prefix(&nodes[0], total);

	/*
	 *	Alternate all of them from the back up.
	 */
	c = NULL;
	for (i = total - 1; i >= 0; i--) {
		cli_syntax_t *d;

		if (!nodes[i]) continue;

		if (!c) {
			c = nodes[i];
			continue;
		}

		d = syntax_alloc(CLI_TYPE_ALTERNATE, nodes[i], c);
		if (!d) {
			int k;

			nodes[i] = NULL;

			for (k = 0; k < total; k++) {
				if (nodes[k]) syntax_free(nodes[k]);
			}
			free(nodes);
			return NULL;
		}

		c = d;
	}

	syntax_free(a);
	syntax_free(b);
	free(nodes);

	return c;
}


/*
 *	Skip "lcp" nodes of a prefix and return the suffix.
 */
cli_syntax_t *syntax_skip_prefix(cli_syntax_t *a, int lcp)
{
	/*
	 *	Don't skip anything == return ourselves.
	 */
	if (lcp == 0) {
		a->refcount++;
		return a;
	}

	/*
	 *	Nothing after this, return nothing.
	 */
	if (a->type != CLI_TYPE_CONCAT) {
		assert(lcp == 1);
		return NULL;
	}

	if (lcp >= a->length) return NULL;

	while (lcp) {
		a = a->next;
		lcp--;
	}

	a->refcount++;
	return a;
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

		return syntax_alloc(CLI_TYPE_CONCAT, a, tail);
	}

	assert(b != NULL);

	return syntax_alloc(CLI_TYPE_CONCAT, a,
			  syntax_concat_prefix(b, lcp - 1, tail));
}


#if DEBUG_PRINT
static void syntax_debug_printf(cli_type_t type, const char *msg,
			       void *first, void *next)
{
	printf("{ ");
	if ((type == CLI_TYPE_EXACT) ||
	    (type == CLI_TYPE_FORCE_EXACT) ||
	    (type == CLI_TYPE_VARARGS)) {
		printf("%s } %s\n", (char *)first, msg);
		return;
	}

	syntax_printf(first);
	printf(" } %s { ", msg);
	syntax_printf(next);
	printf(" }\n");
}

#define SYNTAX_DEBUG_PRINTF syntax_debug_printf
#else
#define SYNTAX_DEBUG_PRINTF(_a, _b, _c, _d)
#endif

/*
 *	Allocate a new node.
 */
static cli_syntax_t *syntax_alloc(cli_type_t type, void *first, void *next)
{
	cli_syntax_t find;
	cli_syntax_t *this, *a, *b, *c;
	int flags = 0;

	memset(&find, 0, sizeof(find));

	SYNTAX_DEBUG_PRINTF(type, "NEW", first, next);

	switch (type) {
	default:
		syntax_free(first);
		syntax_free(next);
		assert(0 == 1);
		return NULL;

	case CLI_TYPE_FORCE_EXACT:
		assert(next == NULL);
		type = CLI_TYPE_EXACT; /* bypass the keyword checks below */
		break;

	case CLI_TYPE_VARARGS:
		if (strcmp((char *) first, "...") != 0) return NULL;
		assert(next == NULL);
		break;

	case CLI_TYPE_EXACT:
		assert(next == NULL);

	case CLI_TYPE_MACRO:
		assert(first != NULL);

		{
			int lowercase, uppercase;
			char *p;


			p = (char *) first;
			if (!*p) {
				syntax_error_string = "Cannot create zero-length keyword";
				return NULL;
			}

			/*
			 *	Names must begin with a letter.
			 */
			if (!isalpha((int) *p)) return NULL;

			lowercase = 0;
			uppercase = 0;

			while (*p) {
				if (*p < ' ') return NULL; /* can't create binary names */

				if (islower((int) *p)) {
					lowercase = 1;
				}

				if (isupper((int) *p)) {
					uppercase = 1;
				}

				if (p[0] == '/') {
					if ((p[1] == 'i') && !p[2]) {
						flags = FLAG_CASE_INSENSITIVE;
						*p = '\0';
						break;
					}

					if ((p[1] == 't') && !p[2]) {
						flags = FLAG_NEEDS_TTY;
						*p = '\0';
						break;
					}

					syntax_error_string = "Unknown keyword modifier";
					return NULL;
				}

				p++;
			}

			if (uppercase) {
				if (lowercase) {
					syntax_error_string = "Mixed case key words are not allowed";
					return NULL;
				}

				if (type == CLI_TYPE_EXACT) {
					syntax_error_string = "Key words must be lowercase";
					return NULL;
				}

			} else {
				if (!lowercase) {
					syntax_error_string = "No letters found in the keyword";
					return NULL;
				}

				if (type == CLI_TYPE_MACRO) {
					syntax_error_string = "Macro names must be upper case";
					return NULL;
				}
			}
		}
		break;

	case CLI_TYPE_OPTIONAL:
	case CLI_TYPE_PLUS:
		assert(first != NULL);
		assert(next == NULL);

		if (((cli_syntax_t *)first)->type == CLI_TYPE_VARARGS) {
			syntax_error_string = "Invalid use of ... in []";
			syntax_free(first);
			return NULL;
		}
		break;

		/*
		 *	Only syntax_alternate() should be calling us here.
		 */
	case CLI_TYPE_ALTERNATE:
		break;

	case CLI_TYPE_CONCAT:
		if (!next) return first;

		a = first;

		/*
		 *	concat(concat(a,b),c) ==> concat(a,concat(b,c))
		 */
		if (a->type == CLI_TYPE_CONCAT) {
			SYNTAX_DEBUG_PRINTF(type, "CONCAT<", a, next);

			b = a->next;
			b->refcount++;
			c = syntax_alloc(CLI_TYPE_CONCAT, b, next);
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

		SYNTAX_DEBUG_PRINTF(type, "CONCAT", first, next);
		break;
	}

	find.type = type;
	find.first = first;
	find.next = next;
	find.min = flags;

	this = syntax_ref(&find);
	if (this) {
		if ((type == CLI_TYPE_CONCAT) ||
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

	/*
	 *	[[a]] = [a]
	 *
	 *	It should probably be an error.
	 */
	if (type == CLI_TYPE_OPTIONAL) {
		a = first;

		if (a->type == CLI_TYPE_OPTIONAL) {
			return a;
		}

		assert(a->type != CLI_TYPE_VARARGS);
	}

	switch (type) {
		size_t len;

	default:
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
		break;

	case CLI_TYPE_VARARGS:
	case CLI_TYPE_EXACT:
		len = strlen((char *) first);		
		assert(next == NULL);

		this = calloc(sizeof(*this) + len + 1, 1);
		if (!this) return NULL;

		this->first = this + 1;
		memcpy(this->first, first, len + 1);
		this->min = flags;
		break;

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
			     const cli_syntax_t *in, cli_type_t parent)
{
	size_t outlen, offset;
	cli_syntax_t *a;

	switch (in->type) {
	case CLI_TYPE_EXACT:
	case CLI_TYPE_VARARGS:
		outlen = snprintf(buffer, len, "%s", (char *) in->first);
		break;

	case CLI_TYPE_MACRO:
		outlen = snprintf(buffer, len, "%s=", (char *) in->first);
		buffer += outlen;
		len -= outlen;
		outlen += syntax_sprintf(buffer, len, in->next,
					 CLI_TYPE_MACRO);
		break;

	case CLI_TYPE_CONCAT:
		outlen = syntax_sprintf(buffer, len, in->first,
					CLI_TYPE_CONCAT);
		buffer += outlen;
		len -= outlen;
		buffer[0] = ' ';

		buffer++;
		len--;
		outlen++;

		outlen += syntax_sprintf(buffer, len, in->next,
					 CLI_TYPE_CONCAT);
		break;

	case CLI_TYPE_OPTIONAL:
		buffer[0] = '[';
		buffer++;
		len--;
		outlen = syntax_sprintf(buffer, len, in->first,
					CLI_TYPE_OPTIONAL);
		buffer += outlen;
		len -= outlen;
		buffer[0] = ']';
		buffer[1] = '\0';
		outlen += 2;
		break;

	case CLI_TYPE_PLUS:
		a = in->first;
		offset = 0;
		if (a->type == CLI_TYPE_CONCAT) {
			buffer[0] = '(';
			buffer++;
			len--;
			offset++;
		} else {
			a = NULL;
		}
		outlen = syntax_sprintf(buffer, len, in->first,
					CLI_TYPE_PLUS);
		buffer += outlen;
		len -= outlen;

		if (a) {
			buffer[0] = ')';
			buffer++;
			len--;
			offset++;
		}

		if (in->max == 0) {
			if (in->min == 0) {
				buffer[0] = '*';
			} else {
				buffer[0] = '+';
			}
		} else {
			if (in->min == in->max) {
				outlen += snprintf(buffer, len, "{%d}", in->min);
			} else {
				outlen += snprintf(buffer, len, "{%d,%d}", in->min, in->max);
			}

		}
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
					 in->first, CLI_TYPE_ALTERNATE);
		buffer[outlen] = '|';
		len--;
		outlen++;

		outlen += syntax_sprintf(buffer + outlen, len, in->next,
					 CLI_TYPE_ALTERNATE);
		if (((cli_syntax_t *)in->next)->type != CLI_TYPE_ALTERNATE) {
			buffer[outlen] = ')';
			buffer[outlen + 1] = '\0';
			outlen++;
		}
		break;

	default:
		assert(0 == 1);
		outlen = snprintf(buffer, len, "?");
		break;
	}

	return outlen;
}


/*
 *	Print syntax to STDOUT
 */
void syntax_printf(const cli_syntax_t *a)
{
	if (!a) return;

	char buffer[8192];

	syntax_sprintf(buffer, sizeof(buffer), a, CLI_TYPE_EXACT);
	recli_fprintf(recli_stdout, "%s", buffer);
}


/*
 *	Print syntax, one alternation on each line.
 */
void syntax_print_lines(const cli_syntax_t *in)
{
	char buffer[1024];

	if (!in) return;

	while (in->type == CLI_TYPE_ALTERNATE) {
		assert(((cli_syntax_t *)in->first)->type != in->type);
		syntax_sprintf(buffer, sizeof(buffer),
				      in->first, CLI_TYPE_EXACT);
		recli_fprintf(recli_stdout, "%s\r\n", buffer);
		in = in->next;
	}

	syntax_sprintf(buffer, sizeof(buffer), in, CLI_TYPE_EXACT);
	recli_fprintf(recli_stdout, "%s\r\n", buffer);
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

#if DEBUG_PRINT
	printf("PARSING %s\n", *buffer);
#endif

	p = *buffer;

	if (!*p) return 0;

	this = first = NULL;

	while (*p) {
		if (isspace((int) *p)) p++;

		if ((*p == ';') || (*p == '#')) break;

		start = p;

		if (!*p) break;

		if (*p < ' ') {
			syntax_error(start, "Cannot parse binary data");
			syntax_free(first);
			return 0;
		}

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
			this = syntax_alloc(CLI_TYPE_OPTIONAL, a, NULL);
			if (!this) {
				syntax_error(start, "Failed creating [...]");
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

				this = syntax_alternate(a, b);
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

		/*
		 *	Var args
		 */
		if (*p == '.') {
			if ((p[1] != '.') || (p[2] != '.') || (p[3])) {
				syntax_error(start, "Invalid use of variable arguments");
				syntax_free(first);
				return 0;
			}

			this = syntax_alloc(CLI_TYPE_VARARGS, "...", NULL);
			if (!this) {
				syntax_error(start, "Failed creating ...");
				syntax_free(first);
				return 0;
			}

			p += 3;
			goto next;
		}

		if ((*p > ' ') && (*p != '-') && (*p < '0') && (*p != '+') && (*p != '*')) {
			syntax_error(start, "Invalid character");
			syntax_free(first);
			return 0;
		}

		while (*p) {
			if ((*p == '(') || (*p == '[') || (*p == '|') ||
			    (*p == '{') || (*p == '}') || (*p == '=') ||
			    (*p == ')') || (*p == ']') || (*p == ' ') ||
			    (*p == '+') || (*p == '*')) {
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

			this = syntax_alloc(CLI_TYPE_MACRO, tmp, next);
			if (!this) {
				syntax_error(start, "Failed creating macro");
				syntax_free(first);
				return 0;
			}
			this = NULL;
			continue;
		}

		/*
		 *	Macros
		 */
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

		/*
		 *	Check for case insensitivity
		 */
		this = syntax_alloc(CLI_TYPE_EXACT, tmp, NULL);
		if (!this) {
			syntax_error(start, "Failed creating word");
			syntax_free(first);
			return 0;
		}

	next:
		if ((*p == '+') || (*p == '*')) {
			cli_syntax_t *a;

			if (this->type == CLI_TYPE_PLUS) {
				if (*p == '*') {
					syntax_error(start, "Unexpected '*'");
				} else {
					syntax_error(start, "Unexpected '+'");
				}
				syntax_free(this);
				syntax_free(first);
				return 0;
			}


			assert(this->type != CLI_TYPE_MACRO);
			a = syntax_alloc(CLI_TYPE_PLUS, this, NULL);
			if (!a) {
				syntax_error(start, "Failed creating +");
				syntax_free(first);
				return 0;
			}

			if (*p == '*') {
				a->min = 0;
			} else {
				a->min = 1;
			}

			this = a;
			p++;
			
		}

		if (!first) {
			first = this;
		} else {
			cli_syntax_t *a;

			assert(this != NULL);
			a = syntax_alloc(CLI_TYPE_CONCAT, first, this);
			if (!a) {
				syntax_error(start, "Failed appending word");
				syntax_free(first);
				return 0;
			}
			this = NULL;
			first = a;
		}
	}

	/*
	 *	Disallow "..." all by itself.
	 */
	if (first && (first->type == CLI_TYPE_VARARGS)) {
		syntax_error(start, "Variable arguments cannot be the only syntax");
		syntax_free(first);
		return 0;
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
int syntax_parse_add(const char *name, recli_datatype_parse_t callback)
{
	size_t len;
	cli_syntax_t *this, find;

	if (!name || !callback) return 0;

	memset(&find, 0, sizeof(find));
	find.type = CLI_TYPE_EXACT;
	find.first = (void *) name;
	find.next = NULL;
	this = syntax_find(&find);
	if (this) {
		if (this->next != callback) return 0;

		return 1;
	}

	len = strlen(name);

	/*
	 *	Do this manually so that we can set the callback,
	 *	and bypass the restrictions in syntax_alloc() that
	 *	the name can't be all uppercase.
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

#define CLI_MATCH_EXACT   (0)
#define CLI_MATCH_PREFIX  (1)


/*
 *	Returns how many prefixes match the given word.
 */
static int syntax_prefix_words(int argc, char *argv[], char const *word, int sense,
			       cli_syntax_t *this, cli_syntax_t *next)
{
	int matches, total;
	cli_syntax_t *a;

	assert(this != NULL);

	if (argc == 0) return 0;

	switch (this->type) {
	case CLI_TYPE_EXACT:
		/*
		 *	FIXME: if the possible syntax is a datatype, e.g. STRING
		 *	or INTEGER, then we really shouldn't return it here -
		 *	especially with a STRING, that can easily get added to
		 *	the user's command, which would not be a syntax error
		 *	and could cause problems if they don't realise and hit
		 *	enter too fast.
		 *	Can test for this here as this->next will be non-NULL.
		 */
		if (sense == CLI_MATCH_PREFIX) {
			if (!word) return 0;

			if ((this->min & FLAG_CASE_INSENSITIVE) != 0) {
				if (strncasecmp((char *) this->first, word, strlen(word)) != 0) return 0;
			} else {
				if (strncmp((char *) this->first, word, strlen(word)) != 0) return 0;
			}
		}

		/* FALL-THROUGH */
	case CLI_TYPE_VARARGS:
		argv[0] = this->first;
		return 1;

	case CLI_TYPE_OPTIONAL:
		/*
		 *	Note first that there's an empty option.
		 */
		argv[0] = "";
		argc--;
		argv++;
		total = 1;

		matches = syntax_prefix_words(argc, argv, word, sense, this->first, next);
		argc -= matches;
		argv += matches;
		total += matches;

		if (!next) return total;

		return total + syntax_prefix_words(argc, argv, word, sense, next, NULL);

	case CLI_TYPE_PLUS:
		/*
		 *	Count '*' and '+' as equivalent for prefix words, and always
		 *	show one option.
		 */
		matches = syntax_prefix_words(argc, argv, word, sense, this->first, next);
		argc -= matches;
		argv += matches;
		total = matches;

		if (!next) return total;

		return total + syntax_prefix_words(argc, argv, word, sense, next, NULL);

	case CLI_TYPE_CONCAT:
		a = this->next;
		if (next) {
			a->refcount++;
			next->refcount++;
			a = syntax_alloc(CLI_TYPE_CONCAT, this->next, next);
			assert(a != this);
		}
		matches = syntax_prefix_words(argc, argv, word, sense, this->first, a);
		if (next) syntax_free(a);
		return matches;

	case CLI_TYPE_ALTERNATE:
		total = 0;
		while (this->type == CLI_TYPE_ALTERNATE) {
			matches = syntax_prefix_words(argc, argv, word, sense,
						      this->first, next);
			argc -= matches;
			argv += matches;
			total += matches;
			this = this->next;
		}
		assert(this->type != CLI_TYPE_ALTERNATE);

		return total + syntax_prefix_words(argc, argv, word, sense, this, next);

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
		if (!rcode) {
			return 0;
		}
	}

#undef WALK
#define WALK(_x) if (!syntax_walk_all(_x, ctx, preorder, inorder, postorder)) return 0

	switch (this->type) {
	case CLI_TYPE_EXACT:
	case CLI_TYPE_VARARGS:
		if (inorder && !inorder(ctx, this)) {
			return 0;
		}
		break;

	case CLI_TYPE_PLUS:
		if (rcode == CLI_WALK_SKIP) return 1;

	repeat:
		WALK(this->first);

		if (inorder) {
			rcode = inorder(ctx, this);
			if (!rcode) {
				return 0;
			}
			if (rcode == CLI_WALK_REPEAT) goto repeat;
		}
		break;

	case CLI_TYPE_OPTIONAL:
		if (rcode == CLI_WALK_SKIP) return 1;

		/* FALL-THROUGH */
	case CLI_TYPE_CONCAT:
	case CLI_TYPE_ALTERNATE:
		WALK(this->first);

		if (inorder) {
			int rcode;

			rcode = inorder(ctx, this);
			if (!rcode) {
				return 0;
			}
			if (rcode == CLI_WALK_SKIP) break;
		}

		WALK(this->next);
		break;

	default:
		assert(0 == 1);
		return 0;
	}
#undef WALK

	if (postorder && !postorder(ctx, this)) {
		return 0;
	}

	return 1;
}


int syntax_walk(cli_syntax_t *this, int order, void *ctx,
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
static int syntax_print_pre(UNUSED void *ctx, cli_syntax_t *this)
{
	switch (this->type) {
	case CLI_TYPE_CONCAT:
		recli_fprintf(recli_stdout, "<");
		break;

	case CLI_TYPE_ALTERNATE:
		recli_fprintf(recli_stdout, "(");
		break;

	case CLI_TYPE_OPTIONAL:
		recli_fprintf(recli_stdout, "[");
		break;

	default:
		break;

	}

	return 1;
}

static int syntax_print_in(UNUSED void *ctx, cli_syntax_t *this)
{
	switch (this->type) {
	case CLI_TYPE_EXACT:
	case CLI_TYPE_VARARGS:
		recli_fprintf(recli_stdout, "%s", (const char *)this->first);
		break;

	case CLI_TYPE_CONCAT:
		recli_fprintf(recli_stdout, " ");
		break;

	case CLI_TYPE_ALTERNATE:
		recli_fprintf(recli_stdout, "|");
		break;

	default:
		break;

	}

	return 1;
}

static int syntax_print_post(UNUSED void *ctx, cli_syntax_t *this)
{
	switch (this->type) {
	case CLI_TYPE_CONCAT:
		recli_fprintf(recli_stdout, ">");
		break;

	case CLI_TYPE_ALTERNATE:
		recli_fprintf(recli_stdout, ")");
		break;

	case CLI_TYPE_OPTIONAL:
		recli_fprintf(recli_stdout, "]");
		break;

	case CLI_TYPE_PLUS:
		if (this->max == 0) {
			if (this->min == 0) {
				recli_fprintf(recli_stdout, "*");
			} else {
				recli_fprintf(recli_stdout, "+");
			}
		} else {
			if (this->min == this->max) {
				recli_fprintf(recli_stdout, "{%d}", this->min);
			} else {
				recli_fprintf(recli_stdout, "{%d,%d}", this->min, this->max);
			}
		}
		break;

	default:
		break;

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
	case CLI_TYPE_VARARGS:
		this->refcount++; /* always matches */
		goto do_concat;

	case CLI_TYPE_EXACT:
		if (this->next) { /* call syntax checker */
			/* FIXME: add somewhere for any errors to go */
			if (!((recli_datatype_parse_t)this->next)(word, NULL)) {
				return NULL; /* failed to match */
			}

		} else if (sense == CLI_MATCH_EXACT) {
			if ((this->min & FLAG_CASE_INSENSITIVE) != 0) {
				if (strcasecmp((char *)this->first, word) != 0) {
					return NULL;
				}

			} else if (strcmp((char *)this->first, word) != 0) {
				return NULL;
			}
		} else {
			if ((this->min & FLAG_CASE_INSENSITIVE) != 0) {
				if (strncasecmp((char *)this->first, word, strlen(word)) != 0) {
					return NULL;
				}

			} else if (strncmp((char *)this->first, word, strlen(word)) != 0) {
				return NULL;
			}
		}

		this->refcount++;
	do_concat:
		if (!next) return this;

		assert(this->refcount > 0);
		assert(next->refcount > 0);

		next->refcount++;
		return syntax_alloc(CLI_TYPE_CONCAT, this, next);

	case CLI_TYPE_OPTIONAL:
		found = syntax_match_word(word, sense, this->first, next);
		if (found) return found;

		if (!next) return NULL; /* matched, but nothing more to match */

		return syntax_match_word(word, sense, next, NULL);

	case CLI_TYPE_CONCAT:
		a = this->next;
		if (next) {
			a->refcount++;
			next->refcount++;
			a = syntax_alloc(CLI_TYPE_CONCAT, this->next, next);
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
		assert(0 == 1);
		break;
	}

	return NULL;		/* internal error */
}


/*
 *	Check argv against a syntax.
 *
 *	Returns:
 *
 *	-N		syntax error (or match failure) in argument N
 *	0		argc was zero
 *	N < argc	a partial command
 *	N = argc	a full command can be executed
 *	N > argc        we want more argc to run a full command
 *
 *	want_more is set if we matched, but needed more input.
 *	i.e. argc == 0, but we still have nodes to match.
 */
int syntax_check(cli_syntax_t *head, int argc, char *argv[],
		 const char **error, int *flags)
{
	int words, total;
	cli_syntax_t *a;
	char const *alt_error = NULL;

	*error = NULL;

	if (!head || (argc < 0)) return -1;

	a = head;

	switch (a->type) {
	case CLI_TYPE_EXACT:
		if (argc == 0) return 1; /* want one more argument */

		if (a->next) { /* call syntax checker */
			/*
			 *	Call registered data type such as IPADDR, etc.
			 */
			*error = NULL;
			if (((recli_datatype_parse_t)a->next)(argv[0], error)) {
				return 1;
			}

			if (!*error) *error = "Input does not match required syntax";
			return -1;

			/*
			 *	FIXME: have the parser take an error, too!
			 */

		} else if ((a->min & FLAG_CASE_INSENSITIVE) != 0) {
			if (strcasecmp((char *)a->first, argv[0]) == 0) {
				if (flags) *flags |= a->min & FLAGS_EXPORT;
				return 1;
			}

		} else if (strcmp((char *)a->first, argv[0]) == 0) {
			if (flags) *flags |= a->min & FLAGS_EXPORT;
			return 1;
		}

		*error = "No matching command";
		return -1;	/* didn't match */

	case CLI_TYPE_VARARGS:
		if (argc == 0) return 1; /* want one more argument */

		return argc; /* eat all of the following arguments */

	case CLI_TYPE_OPTIONAL:
		if (!argc) return 0; /* that's OK. */

		/*
		 *	If it didn't match, we return "no words for us".
		 */
		words = syntax_check(a->first, argc, argv, error, flags);
		if (words < 0) return 0;
		return words;

	case CLI_TYPE_PLUS:
		/*
		 *	Has to match at least 'min' times.
		 */
		if (a->min == 1) {
			words = syntax_check(a->first, argc, argv, error, flags);
			if (words <= 0) return words;

			if (words > argc) return words;

			argc -= words;
			argv += words;
			total = words;
		} else {
			total = 0;

			if (!argc) return 0; /* that's OK. */
		}

		while (argc > 0) {
			words = syntax_check(a->first, argc, argv, error, flags);

			/*
			 *	No match on '*' is OK.
			 */
			if (a->min == total) return 0;

			if (words < 0) return words - total;

			if (words == 0) break; /* didn't match anything */

			if (words > argc) return words;

			argc -= words;
			argv += words;
			total += words;
		}
		return total;

	case CLI_TYPE_CONCAT:
		/*
		 *	Check first entry, which might not match
		 *	anything if it's optional.
		 */
		words = syntax_check(a->first, argc, argv, error, flags);
		if (words < 0) {
			return words;
		}

		if (words > argc) return words;

		argc -= words;
		argv += words;
		total = words;

		words = syntax_check(a->next, argc, argv, error, flags);
		if (words < 0) {
			return words - total;
		}

		if (words > argc) return total + words;

		total += words;
		return total;

	case CLI_TYPE_ALTERNATE:
		total = 0;
		words = syntax_check(a->first, argc, argv, &alt_error, flags);
		if (words > 0) return words; /* found a match */

		/*
		 *	We're at the end of the input, and at the end
		 *	of the optional command.  It's allowed.
		 */
		if ((argc == 0) && (words == 0)) return 0;

		total = syntax_check(a->next, argc, argv, error, flags);

		if (total >= 0) return total;

		/*
		 *	Return the longest error.
		 */
		if (total < words) return total;

		*error = alt_error;
		return words;


	default:
		break;

	}

	*error = "Internal sanity check failed";
	return -1;
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

		this = syntax_skip_prefix(next, 1);

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
		a = syntax_alloc(CLI_TYPE_FORCE_EXACT, argv[i], NULL); /* don't do case checking */
		assert(a != NULL);

		this = syntax_alloc(CLI_TYPE_CONCAT, a, next);
		assert(this != NULL);
		next = this;
	}

	return this;
}


#ifndef NDEBUG
void syntax_debug(const char *msg, cli_syntax_t *this)
{
	recli_fprintf(recli_stdout, "%s ", msg);
	syntax_printf(this);
	recli_fprintf(recli_stdout, "\r\n");
}
#endif


int syntax_tab_complete(cli_syntax_t *head, const char *in, size_t len,
			int max_tabs, char *tabs[])
{
	int i, argc, match, exact;
	size_t out;
	cli_syntax_t *this, *next;
	char *argv[256];
	char *word;
	char *p, buffer[256];
	char mybuf[1024];

	if (!head) return 0;	/* no syntax checking */

	if (sizeof(mybuf) < len + 1) return 0;	/* don't overflow */

	memcpy(mybuf, in, len + 1);
	argc = str2argv(mybuf, len, 256, argv);
	if (argc < 0) return 0;

	this = head;
	this->refcount++;	/* so we can free it later */

	match = 0;
	exact = CLI_MATCH_EXACT;

	while (this && (match < argc)) {
		/*
		 *	Check if any ONE word matches.
		 */
		next = syntax_match_word(argv[match], exact, this, NULL);
		if (!next && ((match + 1) == argc)) {
			exact = CLI_MATCH_PREFIX;
			next = syntax_match_word(argv[match], exact, this, NULL);
		}

		if (!next) {
			syntax_free(this);
			return 0;
		}

		if (exact != CLI_MATCH_EXACT) {
			syntax_free(next);
			next = NULL;
			break;
		}

		syntax_free(this);

		this = syntax_skip_prefix(next, 1);
		assert(this != next);
		syntax_free(next);
		next = NULL;
		match++;
	}

	out = 0;
	p = buffer;
	match = argc;

	if (this && (exact == CLI_MATCH_PREFIX)) {
		match--;

		word = strdup(argv[match]);
	} else {
		word = NULL;
	}

	for (i = 0; i < match; i++) {
		out = snprintf(p, buffer + sizeof(buffer) - p, "%s ", argv[i]);
		p += out;
	}

	if (!this) return 0;

	argc = syntax_prefix_words(256, argv, word, exact, this, NULL);
	if (argc > max_tabs) argc = max_tabs;

	for (i = 0; i < argc; i++) {
		assert(argv[i] != NULL);
		strcpy(p, argv[i]);
		strcat(p, " ");
		tabs[i] = strdup(buffer);
	}

	syntax_free(this);
	free(word);

	return argc;
}


int syntax_merge(cli_syntax_t **phead, char *str)
{
	char *p;
	const char *q;
	cli_syntax_t *this, *a;

	if (!phead || !str) {
		syntax_error(str, "Invalid parameter");
		return -1;
	}

	p = strchr(str, '\r');
	if (p) *p = '\0';
	
	p = strchr(str, '\n');
	if (p) *p = '\0';
	
	p = str;
	while (isspace((int) *p)) p++;
	if (!*p) return 0;
	
	q = p;
	
#ifdef USE_UTF8
	if (!utf8_strvalid(p)) {
		syntax_error(p, "Invalid UTF-8 character");
		syntax_free(*phead);
		return -1;
	}
#endif

	if (!str2syntax(&q, &this, CLI_TYPE_EXACT)) {
		syntax_free(*phead);
		return -1;
	}

	if (!this) return 0;
	
	if (!*phead) {
#if DEBUG_PRINT
		printf("{ ");
		syntax_printf(this);
		printf(" } SET\n");
#endif
		*phead = this;
		return 0;
	}

#if DEBUG_PRINT
	printf("{ ");
	syntax_printf(*phead);
	printf(" } MERGE { ");
	syntax_printf(this);
	printf(" }\n");
#endif

	a = syntax_alternate(*phead, this);
	if (!a) {
		if (!syntax_error_string) {
			syntax_error(str, "Syntax is incompatible with previous commands");
		} else {
			syntax_error_ptr = str;
		}
		return -1;
	}

	*phead = a;
	return 0;
}

static const char *spaces = "                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                ";

/*
 *	Parse a file into a syntax, ignoring blank lines and comments.
 */
int syntax_parse_file(const char *filename, cli_syntax_t **phead)
{
	int lineno;
	FILE *fp;
	char buffer[1024];
	cli_syntax_t *head;

	if (!phead) {
		recli_fprintf(recli_stderr, "Reading syntax: internal error\n");
		return -1;
	}

	if (!*phead) {
		recli_datatypes_init();
	}

	fp = fopen(filename, "r");
	if (!fp) {
		recli_fprintf(recli_stderr, "Failed opening %s: %s\n",
			filename, strerror(errno));
		return -1;
	}

	lineno = 0;
	head = NULL;

	while (fgets(buffer, sizeof(buffer), fp) != NULL) {
		lineno++;

		if (syntax_merge(&head, buffer) < 0) {
			if ((syntax_error_ptr >= buffer) &&
			    (syntax_error_ptr < (buffer + sizeof(buffer)))) {
				recli_fprintf(recli_stderr, "%s\n", buffer);
				recli_fprintf(recli_stderr, "%.*s^\n",
					      syntax_error_ptr - buffer, spaces);
			}
			recli_fprintf(recli_stderr, "ERROR in %s line %d: %s\n",
				      filename, lineno, syntax_error_string);
			return -1;
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

	this = syntax_alloc(CLI_TYPE_FORCE_EXACT, (void *) help, NULL);
	assert(this != NULL);
	this->length = flag; /* internal flag... */

	this = syntax_alloc(CLI_TYPE_CONCAT, last, this);
	assert(this != NULL);

	if (!*phead) {
		*phead = this;
	} else {
		cli_syntax_t *a;
		
		a = syntax_alternate(*phead, this);
		assert(a != NULL);
		
		*phead = a;
	}
}


/*
 *	Parse a simplified Markdown "help" file into a syntax.
 */
int syntax_parse_help(const char *filename, cli_syntax_t **plong, cli_syntax_t **pshort)
{
	int lineno, done;
	FILE *fp;
	char buffer[1024];
	char *h, help[8192];
	cli_syntax_t *this, *last;
	cli_syntax_t *long_syntax, *short_syntax;

	if (!plong || !pshort) return -1;

	fp = fopen(filename, "r");
	if (!fp) {
		recli_fprintf(recli_stderr, "Failed opening %s: %s\n",
			filename, strerror(errno));
		return -1;
	}

	done = lineno = 0;
	last = long_syntax = short_syntax = NULL;
	h = NULL;

	while (fgets(buffer, sizeof(buffer), fp) != NULL) {
		size_t len;
		char *p;
		char *q;

		lineno++;
		p = buffer;
		
#ifdef USE_UTF8
		if (!utf8_strvalid(p)) {
			recli_fprintf(recli_stderr, "%s line %d: Invalid UTF-8 character in input \n",
				       filename, lineno);
			syntax_free(head);
			if (last) syntax_free(last);
			fclose(fp);
			return -1;
		}
#endif

		/*
		 *	Had previous command and help text.  Add it in.
		 */
		if ((*p == '#') && last) {
		do_last:
			if (!h) goto error;

			h = help;
			while (isspace((int) *h)) h++;
			if (*h) {
				add_help(&long_syntax, last, help, 1);
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
				recli_fprintf(recli_stderr, "%s line %d: Invalid syntax \"%s\"\n",
					filename, lineno, buffer);
				syntax_free(long_syntax);
				syntax_free(short_syntax);
				if (last) syntax_free(last);
				fclose(fp);
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

		if ((p == buffer) && !*p) continue; /* skip leading blank lines */

		if (last && (strncmp(buffer, "    ", 4) == 0)) {
			last->refcount++;
			add_help(&short_syntax, last, buffer + 4, 2);
			continue;
		}

		len = strlen(buffer);
		if (len > sizeof(buffer) - 3) goto too_long;

		strcat(buffer, "\r\n");
		len += 2;

		if ((h + len) >= (help + sizeof(help))) {
		too_long:
			recli_fprintf(recli_stderr, "%s line %d: Too much help text\n",
				filename, lineno);
			syntax_free(long_syntax);
			syntax_free(short_syntax);
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

	*plong = long_syntax;
	*pshort = short_syntax;
       

	return 0;
}


/*
 *	Show help for a given argv[]
 */
const char *syntax_show_help(cli_syntax_t *head, int argc, char *argv[])
{
	int i;
	cli_syntax_t *help, *a, *b;

	if (!head || (argc < 0)) return NULL;

	help = syntax_match_max(head, argc, argv);
	if (!help) return NULL;

	/*
	 *	Skip the prefix
	 */
	a = help;

	for (i = 0; i < argc; i++) {
		b = a->first;
		assert(a->type == CLI_TYPE_CONCAT);
		assert(b->type == CLI_TYPE_EXACT);

		a = a->next;
	}

	while (a->type == CLI_TYPE_ALTERNATE) {
		b = a->first;

		if ((b->type == CLI_TYPE_EXACT) && (b->length == 1)) {
			syntax_free(help);
			return (char *) b->first;
		}

		a = a->next;
	}

	b = a;

	if ((b->type == CLI_TYPE_EXACT) && (b->length == 1)) {
		syntax_free(help);
		return (char *) b->first;
	}

	syntax_free(help);

	return NULL;
}

int syntax_print_context_help(cli_syntax_t *head, int argc, char *argv[])
{
	int i;
	size_t len, bufsize;
	cli_syntax_t *help, *a, *b;
	char *p;
	char buffer[1024];

	if (!head || (argc < 0)) return -1;

	help = syntax_match_max(head, argc, argv);
	if (!help) return -1;

	p = buffer;
	*p = '\0';
	bufsize = sizeof(buffer);

	for (i = 0; i < argc; i++) {
		len = snprintf(p, bufsize, "%s ", argv[i]);
		p += len;
		bufsize -= len;
	}

	/*
	 *	Skip the prefix
	 */
	a = help;

	for (i = 0; i < argc; i++) {
		if (a->type != CLI_TYPE_CONCAT) {
			if (i != (argc - 1)) {
				syntax_free(help);
				return 0;
			}
			break;
		}

		b = a->first;

		assert(b->type == CLI_TYPE_EXACT);
		a = a->next;
	}

	while (a->type == CLI_TYPE_ALTERNATE) {
		b = a->first;

		if ((b->type == CLI_TYPE_EXACT) && (b->length == 2)) {
			if (!*buffer) {
				recli_fprintf(recli_stdout, "%s\r\n\r\n",
					      (char *) b->first);
			} else {
				recli_fprintf(recli_stdout, "%s- %s\r\n",
					      buffer, (char *) b->first);
			}
			syntax_free(help);
			return 1;
		}

		a = a->next;
	}

	b = a;

	if ((b->type == CLI_TYPE_EXACT) && (b->length == 2)) {
		recli_fprintf(recli_stdout, "%s- %s\r\n",
			      buffer, (char *) b->first);
		syntax_free(help);
		return 1;
	}

	syntax_free(help);

	return 0;
}

static int syntax_prefix_help(int argc, char *argv[], char *help_text[],
			       cli_syntax_t *this, cli_syntax_t *next)
{
	int matches, total;
	cli_syntax_t *a, *b, *c;

	assert(this != NULL);

	if (argc == 0) return 0;

	switch (this->type) {
	case CLI_TYPE_EXACT:
		if (this->length != 2) {
			argv[0] = this->first;
			help_text[0] = NULL;
		} else {
			argv[0] = NULL;
			help_text[0] = this->first;
		}
		return 1;

	case CLI_TYPE_VARARGS:
		argv[0] = this->first;
		help_text[0] = NULL;
		return 1;

	case CLI_TYPE_OPTIONAL:
		/*
		 *	Note first that there's an empty option.
		 */
		argv[0] = "";
		help_text[0] = "";
		argc--;
		argv++;
		help_text++;
		total = 1;

		matches = syntax_prefix_help(argc, argv, help_text, this->first, next);
		argc -= matches;
		argv += matches;
		help_text += matches;
		total += matches;

		if (!next) return total;

		return total + syntax_prefix_help(argc, argv, help_text, next, NULL);

	case CLI_TYPE_PLUS:
		/*
		 *	Count '*' and '+' as equivalent for prefix words, and always
		 *	show one option.
		 */
		matches = syntax_prefix_help(argc, argv, help_text, this->first, next);
		argc -= matches;
		argv += matches;
		help_text += matches;
		total = matches;

		if (!next) return total;

		return total + syntax_prefix_help(argc, argv, help_text, next, NULL);

	case CLI_TYPE_CONCAT:
		a = this->first;
		b = this->next;

		c = this->next;
		if (next) {
			c->refcount++;
			next->refcount++;
			c = syntax_alloc(CLI_TYPE_CONCAT, this->next, next);
			assert(c != this);
		}

		/*
		 *	Catch the special case of "cmd HELP_TEXT"
		 *
		 *	FIXME: this is all complicated... we should switch the
		 *	entire implementation to just fan-out tries.  The only
		 *	reason to have the complexity of the "normal form" is for
		 *	longest common suffix, which we no longer have.
		 */
		if (a->type == CLI_TYPE_EXACT) {
			if ((b->type == CLI_TYPE_EXACT) &&
			    (b->length == 2)) {
				if (next) syntax_free(c);

				argv[0] = a->first;
				help_text[0] = b->first;
				return 1;
			}

			matches = syntax_prefix_help(argc, argv, help_text, b->first, c);

			/*
			 *	Over-write anything...
			 */
			argv[0] = a->first;
			matches = 1;
		} else {
			matches = syntax_prefix_help(argc, argv, help_text, this->first, c);
		}
		if (next) syntax_free(c);
		return matches;

	case CLI_TYPE_ALTERNATE:
		total = 0;
		while (this->type == CLI_TYPE_ALTERNATE) {
			matches = syntax_prefix_help(argc, argv, help_text,
						      this->first, next);
			argc -= matches;
			argv += matches;
			help_text += matches;
			total += matches;
			this = this->next;
		}
		assert(this->type != CLI_TYPE_ALTERNATE);

		return total + syntax_prefix_help(argc, argv, help_text, this, next);

	default:
		break;
	}

	return 0;
}

int syntax_print_context_help_subcommands(cli_syntax_t *syntax, cli_syntax_t *head, int argc, char *argv[])
{
	int i, j, k, cmds_argc, help_argc;
	cli_syntax_t *help, *cmds, *a;
	size_t max_len;
	char *cmds_argv[256];
	size_t cmds_len[256];
	char *help_argv[256];
	char *help_text[256];

	if (!head || (argc < 0)) return -1;

	/*
	 *	Try to find the help.  If there is none, print the
	 *	syntax at the current point.
	 */
	help = syntax_match_max(head, argc, argv);
	cmds = syntax_match_max(syntax, argc, argv);

	if (!help && !cmds) return -1;

	cmds_argc = 0;
	if (cmds) {
		a = syntax_skip_prefix(cmds, argc);
		syntax_free(cmds);
		cmds = a;

		memset(cmds_argv, 0, sizeof(cmds_argv));
		if (cmds) cmds_argc = syntax_prefix_words(256, cmds_argv, NULL, CLI_MATCH_EXACT, cmds, NULL);
	}

	help_argc = 0;
	if (help) {
		a = syntax_skip_prefix(help, argc);
		syntax_free(help);
		help = a;

		memset(help_argv, 0, sizeof(help_argv));
		if (help) help_argc = syntax_prefix_help(256, help_argv, help_text, help, NULL);
	}

	k = 0;

#if 0
	printf("CMDS %d\r\n", cmds_argc);
	for (i = 0; i < cmds_argc; i++) {
		printf("%d - %s\r\n", i, cmds_argv[i]);
	}

	printf("HELP %d\r\n", help_argc);
	for (i = 0; i < help_argc; i++) {
		printf("%d - %s\r\n", i, help_argv[i]);
	}

	for (i = 0; i < help_argc; i++) {
		printf("%d - %s\r\n", i, help_text[i]);
	}
#endif

	max_len = 0;
	for (i = 0; i < cmds_argc; i++) {
		if (!cmds_argv[i]) {
			cmds_len[i] = 0;
			continue;
		}

		cmds_len[i] = strlen(cmds_argv[i]);
		if (max_len < cmds_len[i]) max_len = cmds_len[i];
	}
	max_len += 4;

	for (i = 0; i < cmds_argc; i++) {
		if (!cmds_argv[i]) continue;

		if (argc > 0) recli_fprintf(recli_stdout, "... ");

		for (j = k; j < help_argc; j++) {
			if (!help_argv[j]) continue; /* e.g. "show" versus "show subscriber" */

			if ((strcmp(cmds_argv[i], help_argv[j]) == 0) && help_text[j]) {
				recli_fprintf(recli_stdout, "%s%.*s%s\r\n", cmds_argv[i],
					      (int) max_len - cmds_len[i], spaces, help_text[j]);
				k++;
				break;
			}
		}

		/*
		 *	No help, just print out the command.
		 *
		 *	FIXME: print out automatic help for built-in data types.
		 */
		if (j == help_argc) {
			if (!*cmds_argv[i]) cmds_argv[i] = "<cr>";

			recli_fprintf(recli_stdout, "%s\r\n", cmds_argv[i]);
		}
	}

	if (help) syntax_free(help);
	if (cmds) syntax_free(cmds);

	return 0;
}
