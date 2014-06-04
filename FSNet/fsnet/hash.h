//
//  hash.h
//  fsnet
//
//  Created by Vincent on 14-5-22.
//  Copyright (c) 2014年 Vincent. All rights reserved.
//

#ifndef fsnet_hash_h
#define fsnet_hash_h


/*
 * list.h
 *        Generic sequential linked list node structure -- can hold any type data.
 *        cheungmine
 *      Sep. 22, 2007.  All rights reserved.
 */
#ifndef LIST_H_INCLUDED
#define LIST_H_INCLUDED



/* unistd.h
 2008-09-15 Last created by cheungmine.
 All rights reserved by cheungmine.
 */
#ifndef UNISTD_H__
#define UNISTD_H__

/* Standard C header files included */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/*============================================================================*/

typedef    long    lresult;


typedef unsigned long ulong;

#ifndef BOOL
typedef int     BOOL;
#define TRUE  1
#define FALSE 0
#endif

#ifndef RESULT
#define RESULT        lresult
#define _SUCCESS    0
#define _ERROR        -1
#endif

#ifndef IN
#define IN
#endif

#ifndef OUT
#define OUT
#endif

#ifndef INOUT
#define INOUT
#endif

#ifndef OPTIONAL
#define OPTIONAL
#endif

#define SIZE_BYTE    1
#define SIZE_ACHAR    1
#define SIZE_WCHAR    2
#define SIZE_SHORT    2
#define SIZE_INT    4
#define SIZE_LONG    4
#define SIZE_FLT    4
#define SIZE_DBL    8
#define SIZE_WORD    2
#define SIZE_DWORD    4
#define SIZE_QWORD    8
#define SIZE_LINT    8
#define SIZE_INT64    8
#define SIZE_UUID    16


/*============================================================================*/
#endif    /*UNISTD_H__*/


typedef struct _listnode_t
{
    struct _listnode_t    *next;
    union{
        void*            data;
        struct _list_t    *list;
        const char        *str;
        long            key;
    };
}listnode_t;

typedef struct _list_t
{
    size_t        size;    /* count of nodes */
    listnode_t    *head;
    listnode_t  *tail;
}list_t, *list_p;

/* A prototype of callbacked function called by list_destroy(), NULL for no use. */
typedef void(*pfcb_list_node_free)(listnode_t* node);

/* An example of free node data function implemented by callee:
 void my_list_node_free(listnode_t *node)
 {
 free(node->data);
 }
 */

/* Appends a node to a list */
extern void
list_append_node(list_t *in_list, listnode_t *in_node);

/* Removes the first node from a list and returns it */
extern listnode_t*
list_remove_head(list_t *in_list);

/* Removes all nodes but for list itself */
extern void
list_remove_all(list_t *in_list, pfcb_list_node_free pfunc /* NULL for no use or a key node */);

/* Returns a copy of a list_t from heap */
extern list_t*
list_copy(list_t in_list);

/* Concatenates two lists into first list. NOT freeing the second */
extern void
list_concat(list_t *first, list_t *second);

/* Allocates a new listnode_t from heap. NO memory allocated for input node_data */
extern listnode_t*
list_node_create(void* node_data);

/* Allocates a new listnode_t with a key node type */
extern listnode_t*
list_key_create(long node_key);

/* Allocates a empty list_t from heap */
extern list_t*
list_create();

/* Frees in_list's all nodes and destroys in_list from heap.
 * the callee is responsible for freeing node data.
 * the node freed-function(pfunc) is called by list_destroy.
 */
extern void
list_destroy(list_t *in_list, pfcb_list_node_free pfunc /* NULL for no use or a key node */);

/* Gets count of nodes in the list */
extern size_t
list_size(const list_t* in_list);

/* Gets node by index 0-based. 0 is head */
extern listnode_t*
list_node_at(const list_t* in_list, int index);


#endif  /* LIST_H_INCLUDED */

/*
 * hashmap.h
 *        Generic hash map: key(string)-value(any type).
 *        cheungmine
 *      Sep. 22, 2007.  All rights reserved.
 */
#ifndef HASHMAP_H_INCLUDED
#define HASHMAP_H_INCLUDED

#include "unistd.h"

/* You should always use 1024 */
#define     HASHMAP_SIZE    20480

/* Opaque struct pointer to _hash_map_t */
typedef struct    _hash_map_t*        hash_map;

typedef void(*pfcb_hmap_value_free)(void* value);

/* An example of free value function implemented by caller:
 void my_hmap_free_value(void* pv)
 {
 free(pv);
 }
 */


/* Create before use. eg:
 * hash_map  hm;
 * hmap_create (&hm, HASHMAP_SIZE);
 * assert (hm);     // out of memory if hm==NULL
 * void* mydata=malloc(n);
 * hmap_insert(hm, "shanghai", -1, mydata);
 ...
 * hmap_destroy(hm, my_hmap_free_value);
 */
extern void
hmap_create(hash_map *hmap, int size);

/* Destroy after use */
extern void
hmap_destroy(hash_map hmap, pfcb_hmap_value_free);

/* Insert a key-value into hash map. value is a pointer to callee-allocated memory */
extern void
hmap_insert(hash_map hmap, const char* key, int key_len/* -1 for strlen to be called */, void* value);

/* Search a hash map for value of given key string */
extern void*
hmap_search(hash_map hmap, const char  *key);


#endif  /* HASHMAP_H_INCLUDED */



#endif
