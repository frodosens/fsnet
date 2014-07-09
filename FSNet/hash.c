//
//  hash.c
//  fsnet
//
//  Created by Vincent on 14-5-22.
//  Copyright (c) 2014年 Vincent. All rights reserved.
//

#include <stdio.h>


/*
 * list.c
 *        Generic linked list implementation.
 *        cheungmine
 *      Sep. 22, 2007.  All rights reserved.
 */

#include "fs_malloc.h"
#include "hash.h"

/* Appends a node to a list */
void
list_append_node(list_t *in_list, listnode_t *node)
{
    node->next = NULL;
    
    if (in_list->head)
    {
        in_list->tail->next = node;
        in_list->tail = node;
    }
    else
        in_list->head = in_list->tail = node;
    
    in_list->size++;
}

/* Removes the first node from a list and returns it */
listnode_t*
list_remove_head(list_t *in_list)
{
    listnode_t    *node = NULL;
    if (in_list->head)
    {
        node = in_list->head;
        in_list->head = in_list->head->next;
        if (in_list->head == NULL)
            in_list->tail = NULL;
        node->next = NULL;
        
        in_list->size--;
    }
    return node;
}

/* Removes all nodes but for list itself */
void
list_remove_all(list_t *in_list, pfcb_list_node_free pf)
{
    listnode_t    *node;
    while((node = list_remove_head(in_list))){
        if (pf) (*pf)(node);
        fs_free(node);
    }
    fs_assert (in_list->size==0, "");
}

/* Returns a copy of a list_t from heap */
list_t*
list_copy(list_t list)
{
    list_t    *newlist = (list_t*)fs_malloc (sizeof(list_t));
    *newlist = list;
    return newlist;
}

/* Concatenates two lists into first list */
void
list_concat(list_t *first, list_t *second)
{
    if (first->head)
    {
        if (second->head)
        {
            first->tail->next = second->head;
            first->tail = second->tail;
        }
    }
    else
        *first = *second;
    second->head = second->tail = NULL;
    
    first->size += second->size;
}

/* Allocates a new listnode_t from heap */
listnode_t*
list_node_create(void* data)
{
    listnode_t    *node = (listnode_t*)fs_malloc (sizeof(listnode_t));
    node->next = NULL;
    node->data = data;
    return node;
}

listnode_t*
list_key_create(long key)
{
    listnode_t    *node = (listnode_t*)fs_malloc (sizeof(listnode_t));
    node->next = NULL;
    node->key = key;
    return node;
}

/* Allocates a empty list_t from heap */
list_t*
list_create()
{
    list_t    *list = (list_t*)fs_malloc (sizeof(list_t));
    list->size = 0;
    list->head = list->tail = NULL;
    return list;
}

/* Frees a empty list_t from heap */
void
list_destroy(list_t *in_list, pfcb_list_node_free  pf)
{
    list_remove_all(in_list, pf);
    fs_free(in_list);
}

/* Gets count of nodes in the list */
size_t
list_size(const list_t* in_list)
{
    return in_list->size;
}

/* Gets node by index 0-based. 0 is head */
listnode_t*
list_node_at(const list_t* in_list, int index)
{
    int  i=0;
    listnode_t    *node = in_list->head;
    
    fs_assert(index >=0 && index < (int)in_list->size, "");
    
    while (i < index)
    {
        node = node->next;
        i++;
    }
    
    return node;
}



/*
 * hashmap.c
 *        Generic hashmap implementation.
 *      a map for pair of key-value. key must be a null-end string, value is any type of data.
 *        cheungmine
 *      Sep. 22, 2007.  All rights reserved.
 */


typedef struct _hash_map_t
{
    size_t            size;
    listnode_t**    key;
    listnode_t**    value;
}hash_map_t;

/* Hash a string, return a hash key */
static ulong  hash_string(const char  *s, int len)
{
    ulong h = 0;
    int   i = 0;
    assert (s);
    if (len < 0)
        len = (s? (int)strlen(s): 0);
    while(i++ < len) { h = 17*h + *s++; }
    return h;
}

static void _free_map_key(listnode_t* node)
{
    listnode_t    *old;
    while(node)
    {
        old = node;
        node = node->next;
        
        free(old->data);
        free (old);
    }
}

static void _free_map_value(listnode_t* node, pfcb_hmap_value_free pfunc)
{
    listnode_t    *old;
    while(node)
    {
        old = node;
        node = node->next;
        
        if (pfunc)
            (*pfunc)(old->data);
        free (old);
    }
}

/*=============================================================================
 Public Functions
 =============================================================================*/
/* Create before use */
void
hmap_create(hash_map *hmap, int size)
{
    (*hmap) = (hash_map_t*) malloc(sizeof(hash_map_t));
    (*hmap)->size = size;
    (*hmap)->key = (listnode_t**) calloc(size, sizeof(listnode_t*));
    (*hmap)->value = (listnode_t**) calloc(size, sizeof(listnode_t*));
}

/* Destroy after use */
extern void
hmap_destroy(hash_map hmap, pfcb_hmap_value_free pfunc)
{
    size_t i;
    for(i=0; i<hmap->size; i++){
        _free_map_key(hmap->key[i]);
        _free_map_value(hmap->value[i], pfunc);
    }
    
    free(hmap->key);
    free(hmap->value);
    free(hmap);
}


/* Insert a key-value into hash map. value is a pointer to callee-allocated memory */
void
hmap_insert(hash_map hmap, const char* key, int key_len, void* value)
{
    listnode_t    *node_key, *node_val;
    ulong        h;
    char        *s;
    assert (key);
    
    if (key_len<0) key_len = (int) strlen (key);
    s = (char*) malloc (key_len+1);
    assert(s);
    
#pragma warning(push)    /* C4996 */
#pragma warning( disable : 4996 )
    strncpy (s, key, key_len);
#pragma warning(pop)    /* C4996 */
    s[key_len] = 0;
    
    node_key = list_node_create ( (void*)s );
    node_val = list_node_create ( value );
    assert(node_key && node_val);
    
    h = hash_string (s, key_len) % hmap->size;
    
    node_key->next = hmap->key[h];
    hmap->key[h] = node_key;
    
    node_val->next = hmap->value[h];
    hmap->value[h] = node_val;
}

/* Search a hash map for value of given key string */
void*
hmap_search(hash_map hmap, const char *key)
{
    ulong        h    = hash_string (key, -1) % hmap->size;
    listnode_t  *pk = hmap->key[h];
    listnode_t  *pv = hmap->value[h];
    
    while (pk)
    {
        if (strcmp(key, pk->str) == 0)
            return pv->data;
        pk = pk->next;
        pv = pv->next;
    }
    
    return NULL;
}

