/*
 * $Id: json_object.c,v 1.17 2006/07/25 03:24:50 mclark Exp $
 *
 * Copyright (c) 2004, 2005 Metaparadigm Pte. Ltd.
 * Michael Clark <michael@metaparadigm.com>
 *
 * This library is free software; you can redistribute it and/or modify
 * it under the terms of the MIT license. See COPYING for details.
 *
 */

#include <assert.h>
#include "config.h"

/* This is an hack for strndup */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>

#include <string.h>
#include <strings.h>

#include "debug.h"
#include "printbuf.h"
#include "linkhash.h"
#include "arraylist.h"
#include "json_object.h"
#include "json_object_private.h"
#include "json_tokener.h"

/*#if !HAVE_STRNDUP*/
  char* json_c_strndup(const char* str, size_t n);
/*#endif  !HAVE_STRNDUP */

/* #define REFCOUNT_DEBUG 1 */

const char *json_number_chars = "0123456789.+-e";
const char *json_hex_chars = "0123456789abcdef";

#ifdef REFCOUNT_DEBUG
static const char* json_type_name[] = {
  "null",
  "boolean",
  "double",
  "int",
  "object",
  "array",
  "string",
};
#endif /* REFCOUNT_DEBUG */

static void json_object_generic_delete(struct json_object* this);
static struct json_object* json_object_new(enum json_type o_type);


/* ref count debugging */

#ifdef REFCOUNT_DEBUG

static struct lh_table *json_object_table;

static void json_object_init() __attribute__ ((constructor));
static void json_object_init() {
  mc_debug("json_object_init: creating object table\n");
  json_object_table = lh_kptr_table_new(128, "json_object_table", NULL);
}

static void json_object_fini() __attribute__ ((destructor));
static void json_object_fini() {
  struct lh_entry *ent;
  if(mc_get_debug() && json_object_table->count) {
    mc_debug("json_object_fini: %d referenced objects at exit\n",
	     json_object_table->count);
    lh_foreach(json_object_table, ent) {
      struct json_object* obj = (struct json_object*)ent->v;
      mc_debug("\t%s:%p\n", json_type_name[obj->o_type], obj);
    }
  }
  mc_debug("json_object_fini: freeing object table\n");
  lh_table_free(json_object_table);
}
#endif /* REFCOUNT_DEBUG */


/* string escaping */

static int json_escape_str(struct printbuf *pb, char *str)
{
  int pos = 0, start_offset = 0;
  unsigned char c;
  do {
    c = str[pos];
    switch(c) {
    case '\0':
      break;
    case '\b':
    case '\n':
    case '\r':
    case '\t':
    case '"':
    case '\\':
    case '/':
      if(pos - start_offset > 0)
	printbuf_memappend(pb, str + start_offset, pos - start_offset);
      if(c == '\b') printbuf_memappend(pb, "\\b", 2);
      else if(c == '\n') printbuf_memappend(pb, "\\n", 2);
      else if(c == '\r') printbuf_memappend(pb, "\\r", 2);
      else if(c == '\t') printbuf_memappend(pb, "\\t", 2);
      else if(c == '"') printbuf_memappend(pb, "\\\"", 2);
      else if(c == '\\') printbuf_memappend(pb, "\\\\", 2);
      else if(c == '/') printbuf_memappend(pb, "\\/", 2);
      start_offset = ++pos;
      break;
    default:
      if(c < ' ') {
	if(pos - start_offset > 0)
	  printbuf_memappend(pb, str + start_offset, pos - start_offset);
	sprintbuf(pb, "\\u00%c%c",
		  json_hex_chars[c >> 4],
		  json_hex_chars[c & 0xf]);
	start_offset = ++pos;
      } else pos++;
    }
  } while(c);
  if(pos - start_offset > 0)
    printbuf_memappend(pb, str + start_offset, pos - start_offset);
  return 0;
}


/* reference counting */

extern struct json_object* json_object_get(struct json_object *this)
{
  if(this) {
    this->_ref_count++;
  }
  return this;
}

extern void json_object_put(struct json_object *this)
{
  if(this) {
    this->_ref_count--;
    if(!this->_ref_count) this->_delete(this);
  }
}


/* generic object construction and destruction parts */

static void json_object_generic_delete(struct json_object* this)
{
#ifdef REFCOUNT_DEBUG
  mc_debug("json_object_delete_%s: %p\n",
	   json_type_name[this->o_type], this);
  lh_table_delete(json_object_table, this);
#endif /* REFCOUNT_DEBUG */
  printbuf_free(this->_pb);
  free(this);
}

static struct json_object* json_object_new(enum json_type o_type)
{
  struct json_object *this = calloc(sizeof(struct json_object), 1);
  if(!this) return NULL;
  this->o_type = o_type;
  this->_ref_count = 1;
  this->_delete = &json_object_generic_delete;
#ifdef REFCOUNT_DEBUG
  lh_table_insert(json_object_table, this, this);
  mc_debug("json_object_new_%s: %p\n", json_type_name[this->o_type], this);
#endif /* REFCOUNT_DEBUG */
  return this;
}


/* type checking functions */

int json_object_is_type(struct json_object *this, enum json_type type)
{
	if(!this && json_type_null == type) return 1;
  return (this->o_type == type);
}

enum json_type json_object_get_type(struct json_object *this)
{
  if(!this) return json_type_null;
  return this->o_type;
}


/* json_object_to_json_string */

const char* json_object_to_json_string(struct json_object *this)
{
  if(!this) return "null";
  if(!this->_pb) {
    if(!(this->_pb = printbuf_new())) return NULL;
  } else {
    printbuf_reset(this->_pb);
  }
  if(this->_to_json_string(this, this->_pb) < 0) return NULL;
  return this->_pb->buf;
}


/* json_object_object */

static int json_object_object_to_json_string(struct json_object* this,
					     struct printbuf *pb)
{
  int i=0;
  struct json_object_iter iter;
  sprintbuf(pb, "{");

  /* CAW: scope operator to make ANSI correctness */
  /* CAW: switched to json_object_object_foreachC which uses an iterator struct */
	json_object_object_foreachC(this, iter) {
			if(i) sprintbuf(pb, ",");
			sprintbuf(pb, " \"");
			json_escape_str(pb, iter.key);
			sprintbuf(pb, "\": ");
			if(iter.val == NULL) sprintbuf(pb, "null");
			else iter.val->_to_json_string(iter.val, pb);
			i++;
	}

  return sprintbuf(pb, " }");
}

static void json_object_lh_entry_free(struct lh_entry *ent)
{
  free(ent->k);
  json_object_put((struct json_object*)ent->v);
}

static void json_object_object_delete(struct json_object* this)
{
  lh_table_free(this->o.c_object);
  json_object_generic_delete(this);
}

struct json_object* json_object_new_object()
{
  struct json_object *this = json_object_new(json_type_object);
  if(!this) return NULL;
  this->_delete = &json_object_object_delete;
  this->_to_json_string = &json_object_object_to_json_string;
  this->o.c_object = lh_kchar_table_new(JSON_OBJECT_DEF_HASH_ENTIRES,
					NULL, &json_object_lh_entry_free);
  return this;
}

struct lh_table* json_object_get_object(struct json_object *this)
{
  if(!this) return NULL;
  switch(this->o_type) {
  case json_type_object:
    return this->o.c_object;
  default:
    return NULL;
  }
}

void json_object_object_add(struct json_object* this, const char *key,
			    struct json_object *val)
{
  lh_table_delete(this->o.c_object,  (char*) key);
  lh_table_insert(this->o.c_object, strdup(key), val);
}

struct json_object* json_object_object_get(struct json_object* this, const char *key)
{
  return (struct json_object*) lh_table_lookup(this->o.c_object, (char*)key);
}

void json_object_object_del(struct json_object* this, const char *key)
{
  lh_table_delete(this->o.c_object,  (char*) key);
}


/* json_object_boolean */

static int json_object_boolean_to_json_string(struct json_object* this,
					      struct printbuf *pb)
{
  if(this->o.c_boolean) return sprintbuf(pb, "true");
  else return sprintbuf(pb, "false");
}

struct json_object* json_object_new_boolean(boolean b)
{
  struct json_object *this = json_object_new(json_type_boolean);
  if(!this) return NULL;
  this->_to_json_string = &json_object_boolean_to_json_string;
  this->o.c_boolean = b;
  return this;
}

boolean json_object_get_boolean(struct json_object *this)
{
  if(!this) return FALSE;
  switch(this->o_type) {
  case json_type_boolean:
    return this->o.c_boolean;
  case json_type_int:
    return (this->o.c_int != 0);
  case json_type_double:
    return (this->o.c_double != 0);
  case json_type_string:
    if(strlen(this->o.c_string)) return TRUE;
  default:
    return TRUE;
  }
}


/* json_object_int */

static int json_object_int_to_json_string(struct json_object* this,
					  struct printbuf *pb)
{
  return sprintbuf(pb, "%d", this->o.c_int);
}

struct json_object* json_object_new_int(int i)
{
  struct json_object *this = json_object_new(json_type_int);
  if(!this) return NULL;
  this->_to_json_string = &json_object_int_to_json_string;
  this->o.c_int = i;
  return this;
}

int json_object_get_int(struct json_object *this)
{
  int cint;

  if(!this) return 0;
  switch(this->o_type) {
  case json_type_int:
    return this->o.c_int;
  case json_type_double:
    return (int)this->o.c_double;
  case json_type_boolean:
    return this->o.c_boolean;
  case json_type_string:
    if(sscanf(this->o.c_string, "%d", &cint) == 1) return cint;
  default:
    return 0;
  }
}


/* json_object_double */

static int json_object_double_to_json_string(struct json_object* this,
					     struct printbuf *pb)
{
#ifdef AC_BETTER_PRECISION
	if( ((int) this->o.c_double) !=  this->o.c_double)
		return sprintbuf(pb, "%25.18Lg", this->o.c_double);
	else
		return sprintbuf(pb, "%d.0", (int) this->o.c_double);
#else
  return sprintbuf(pb, "%lf", this->o.c_double);

#endif

}

struct json_object* json_object_new_double(double d)
{
  struct json_object *this = json_object_new(json_type_double);
  if(!this) return NULL;
  this->_to_json_string = &json_object_double_to_json_string;
  this->o.c_double = d;
  return this;
}

double json_object_get_double(struct json_object *this)
{
  double cdouble;

  if(!this) return 0.0;
  switch(this->o_type) {
  case json_type_double:
    return this->o.c_double;
  case json_type_int:
    return this->o.c_int;
  case json_type_boolean:
    return this->o.c_boolean;
  case json_type_string:
    if(sscanf(this->o.c_string, "%lf", &cdouble) == 1) return cdouble;
  default:
    return 0.0;
  }
}


/* json_object_string */

static int json_object_string_to_json_string(struct json_object* this,
					     struct printbuf *pb)
{
  sprintbuf(pb, "\"");
  json_escape_str(pb, this->o.c_string);
  sprintbuf(pb, "\"");
  return 0;
}

static void json_object_string_delete(struct json_object* this)
{
  free(this->o.c_string);
  json_object_generic_delete(this);
}

struct json_object* json_object_new_string(const char *s)
{
  struct json_object *this = json_object_new(json_type_string);
  if(!this) return NULL;
  this->_delete = &json_object_string_delete;
  this->_to_json_string = &json_object_string_to_json_string;
  this->o.c_string = json_c_strndup(s, strlen(s));
  return this;
}

struct json_object* json_object_new_string_len(const char *s, int len)
{
  struct json_object *this = json_object_new(json_type_string);
  if(!this) return NULL;
  this->_delete = &json_object_string_delete;
  this->_to_json_string = &json_object_string_to_json_string;
  this->o.c_string = json_c_strndup(s, (size_t)len);
  return this;
}

char* json_object_get_string(struct json_object *this)
{
  if(!this) return NULL;
  switch(this->o_type) {
  case json_type_string:
    return this->o.c_string;
  default:
    return json_object_to_json_string(this);
  }
}


/* json_object_array */

static int json_object_array_to_json_string(struct json_object* this,
					    struct printbuf *pb)
{
  int i;
  sprintbuf(pb, "[");
  for(i=0; i < json_object_array_length(this); i++) {
	  struct json_object *val;
	  if(i) { sprintbuf(pb, ", "); }
	  else { sprintbuf(pb, " "); }

      val = json_object_array_get_idx(this, i);
	  if(val == NULL) { sprintbuf(pb, "null"); }
	  else { val->_to_json_string(val, pb); }
  }
  return sprintbuf(pb, " ]");
}

static void json_object_array_entry_free(void *data)
{
  json_object_put((struct json_object*)data);
}

static void json_object_array_delete(struct json_object* this)
{
	assert(json_object_is_type(this, json_type_array));
  array_list_free(this->o.c_array);
  json_object_generic_delete(this);
}

struct json_object* json_object_new_array()
{
  struct json_object *this = json_object_new(json_type_array);
  if(!this) return NULL;
  this->_delete = &json_object_array_delete;
  this->_to_json_string = &json_object_array_to_json_string;
  this->o.c_array = array_list_new(&json_object_array_entry_free);
  return this;
}

struct array_list* json_object_get_array(struct json_object *this)
{
  if(!this) return NULL;
  switch(this->o_type) {
  case json_type_array:
    return this->o.c_array;
  default:
    return NULL;
  }
}

int json_object_array_length(struct json_object *this)
{
	assert(json_object_is_type(this, json_type_array));
	return array_list_length(this->o.c_array);
}

int json_object_array_add(struct json_object *this,struct json_object *val)
{
	assert(json_object_is_type(this, json_type_array));
	return array_list_add(this->o.c_array, val);
}

int json_object_array_put_idx(struct json_object *this, int idx,
			      struct json_object *val)
{
	assert(json_object_is_type(this, json_type_array));
	return array_list_put_idx(this->o.c_array, idx, val);
}

struct json_object* json_object_array_get_idx(struct json_object *this,
					      int idx)
{
	assert(json_object_is_type(this, json_type_array));
	return (struct json_object*)array_list_get_idx(this->o.c_array, idx);
}

