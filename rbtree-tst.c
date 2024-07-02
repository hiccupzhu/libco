#include "rbtree.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

struct map_s2s_node_s {
  	struct rb_node node;
  	char *key;
	char *value;
};

struct map_s2s_node_s * map_s2s_search(struct rb_root *root, char *key)
{
  	struct rb_node *node = root->rb_node;

  	while (node) {
  		struct map_s2s_node_s *data = container_of(node, struct map_s2s_node_s, node);
		int result;

		result = strcmp(key, data->key);

		if (result < 0)
  			node = node->rb_left;
		else if (result > 0)
  			node = node->rb_right;
		else
  			return data;
	}
	return NULL;
}

int map_s2s_insert(struct rb_root *root, struct map_s2s_node_s *data)
{
  	struct rb_node **new = &(root->rb_node), *parent = NULL;

  	/* Figure out where to put new node */
  	while (*new) {
  		struct map_s2s_node_s *this = container_of(*new, struct map_s2s_node_s, node);
  		int result = strcmp(data->key, this->key);

		parent = *new;
  		if (result < 0)
  			new = &((*new)->rb_left);
  		else if (result > 0)
  			new = &((*new)->rb_right);
  		else
  			return 0;
  	}

  	/* Add new node and rebalance tree. */
  	rb_link_node(&data->node, parent, new);
  	rb_insert_color(&data->node, root);

	return 1;
}

void map_s2s_free(struct map_s2s_node_s *node)
{
	if (node != NULL) {
		if (node->key != NULL) {
			free(node->key);
			node->key = NULL;
		}
		free(node);
		node = NULL;
	}
}

#define NUM_NODES 32

int main()
{
	struct rb_root mytree = RB_ROOT;
	struct map_s2s_node_s *mn[NUM_NODES];

	/* *insert */
	int i = 0;
	printf("insert node from 1 to NUM_NODES(32): \n");
	for (; i < NUM_NODES; i++) {
		mn[i] = (struct map_s2s_node_s *)malloc(sizeof(struct map_s2s_node_s));
		mn[i]->key = (char *)malloc(sizeof(char) * 4);
		sprintf(mn[i]->key, "%d", i);
		map_s2s_insert(&mytree, mn[i]);
	}
	
	/* *search */
	struct rb_node *node;
	printf("search all nodes: \n");
	for (node = rb_first(&mytree); node; node = rb_next(node))
		printf("key = %s\n", rb_entry(node, struct map_s2s_node_s, node)->key);

	/* *delete */
	printf("delete node 20: \n");
	struct map_s2s_node_s *data = map_s2s_search(&mytree, "20");
	if (data) {
		rb_erase(&data->node, &mytree);
		map_s2s_free(data);
	}

	/* *delete again*/
	printf("delete node 10: \n");
	data = map_s2s_search(&mytree, "10");
	if (data) {
		rb_erase(&data->node, &mytree);
		map_s2s_free(data);
	}

	/* *delete once again*/
	printf("delete node 15: \n");
	data = map_s2s_search(&mytree, "15");
	if (data) {
		rb_erase(&data->node, &mytree);
		map_s2s_free(data);
	}

	/* *search again*/
	printf("search again:\n");
	for (node = rb_first(&mytree); node; node = rb_next(node))
		printf("key = %s\n", rb_entry(node, struct map_s2s_node_s, node)->key);
	return 0;
}


