#include <stdio.h>
#include <assert.h>
#include <chcore/container/rbtree.h>

static inline void node_swap(RBNode *node1, RBNode *node2)
{
	rbkey_t k;
	rbvalue_t v;

	k = node1->key;
	node1->key = node2->key;
	node2->key = k;

	v = node1->value;
	node1->value = node2->value;
	node2->value = v;
}

static void left_rotate(RBTree *rbtree, RBNode *node)
{
	RBNode *parent = node->parent;
	RBNode *right = node->right;

	node->right = right->left;
	if (node->right)
		node->right->parent = node;

	right->left = node;
	node->parent = right;

	right->parent = parent;
	if (!parent)
		rbtree->root = right;
	else {
		if (parent->left == node)
			parent->left = right;
		else
			parent->right = right;
	}
}

static void right_rotate(RBTree *rbtree, RBNode *node)
{
	RBNode *parent = node->parent;
	RBNode *left = node->left;

	node->left = left->right;
	if (node->left)
		node->left->parent = node;

	left->right = node;
	node->parent = left;

	left->parent = parent;
	if (!parent)
		rbtree->root = left;
	else {
		if (parent->left == node)
			parent->left = left;
		else
			parent->right = left;
	}
}

static void rbtree_insert_red_node(RBTree *rbtree, RBNode *new_node)
{
	if (!rbtree->root) {
		rbtree->root = new_node;
		return;
	}

	RBNode *cur = rbtree->root;
	while (cur) {
		if (cur->key < new_node->key) {
			if (!cur->right) {
				new_node->parent = cur;
				cur->right = new_node;
				break;
			}
			cur = cur->right;
			continue;
		}

		if (new_node->key < cur->key) {
			if (!cur->left) {
				new_node->parent = cur;
				cur->left = new_node;
				break;
			}
			cur = cur->left;
			continue;
		}

		assert(0); /* no equal */
	}
}

static void rbtree_balance(RBTree *rbtree, RBNode *node)
{
	if (rbtree->root == node || rbtree->root == node->parent) {
		rbtree->root->color = BLACK;
		return;
	}

	if (node->parent->color == BLACK)
		return;

	RBNode *grand = node->parent->parent;
	if (!grand) {
		rbtree_balance(rbtree, node->parent);
		return;
	}
	if (grand->left && grand->left->color == RED
				&& grand->right && grand->right->color == RED) {
		grand->left->color = BLACK;
		grand->right->color = BLACK;
		grand->color = RED;
		rbtree_balance(rbtree, grand);
		return;
	}

	RBNode *parent = node->parent;
	if (parent->left == node && grand->right == node->parent) {
		right_rotate(rbtree, parent);
		rbtree_balance(rbtree, parent);
		return;
	}
	if (parent->right == node && grand->left == node->parent) {
		left_rotate(rbtree, parent);
		rbtree_balance(rbtree, parent);
		return;
	}

	parent->color = BLACK;
	grand->color = RED;
	if (parent->left == node && grand->left == node->parent) {
		right_rotate(rbtree, grand);
		return;
	}
	if (parent->right == node && grand->right == node->parent) {
		left_rotate(rbtree, grand);
		return;
	}
}

void rbtree_insert(RBTree *rbtree, RBNode *new_node)
{
	rbtree_insert_red_node(rbtree, new_node);
	rbtree_balance(rbtree, new_node);
}

RBNode *rbtree_get(RBTree *rbtree, rbkey_t key)
{
	if (!rbtree->root)
		return NULL;
	RBNode *node = rbtree->root;
	while (node) {
		if (key < node->key) {
			node = node->left;
			continue;
		}
		if (node->key < key) {
			node = node->right;
			continue;
		}
		break;
	}
	return node;
}

/* The 2nd phase of delete node */
static void rbtree_delete_balance(RBTree *rbtree, RBNode *node)
{
	if (rbtree->root == node || node->color == RED)
		return;

	bool node_is_left = (node->parent->left == node);
	RBNode *brother = node_is_left ? node->parent->right : node->parent->left;
	if (brother->color == RED) {
		/* node is BLACK && brother is RED */
		if (node_is_left)
			left_rotate(rbtree, node->parent);
		else
			right_rotate(rbtree, node->parent);
		node->parent->color = RED;
		brother->color = BLACK;
		rbtree_delete_balance(rbtree, node);
		return;
	}

	bool brother_is_leaf = (!brother->left && !brother->right);
	bool brother_has_two_black_children =
		brother->left && brother->right
		&& brother->left->color == BLACK
		&& brother->right->color == BLACK;
	if (brother_is_leaf || brother_has_two_black_children) {
		brother->color = RED;
		if (node->parent->color == RED) {
			node->parent->color = BLACK;
			return;
		}
		rbtree_delete_balance(rbtree, node->parent);
		return;
	}

	/* node is BLACK && brother's symmetry node is RED */
	bool outer_symmetry_is_red =
		(node_is_left && brother->right && brother->right->color == RED);
	bool inner_symmetry_is_red =
		(!node_is_left && brother->left && brother->left->color == RED);
	if (outer_symmetry_is_red || inner_symmetry_is_red) {
		if (node->parent->color == RED)
			brother->color = RED;
		else
			brother->color = BLACK;
		node->parent->color = BLACK;

		if (outer_symmetry_is_red) {
			brother->right->color = BLACK;
			left_rotate(rbtree, node->parent);
		} else {
			brother->left->color = BLACK;
			right_rotate(rbtree, node->parent);
		}

		return;
	}

	/* node is BLACK && brother's same-side node is RED */
	bool same_right_of_brother_is_red =
		(!node_is_left && brother->right && brother->right->color == RED);
	bool same_left_of_brother_is_red =
		(node_is_left && brother->left && brother->left->color == RED);
	if (same_right_of_brother_is_red || same_left_of_brother_is_red) {
		brother->color = RED;
		if (same_right_of_brother_is_red) {
			brother->right->color = BLACK;
			left_rotate(rbtree, brother);
		} else {
			brother->left->color = BLACK;
			right_rotate(rbtree, brother);
		}
		rbtree_delete_balance(rbtree, node);
		return;
	}
}

static RBNode *__get_pre_node(RBTree *rbtree, RBNode *node)
{
	if (!node->left)
		return NULL;
	RBNode *ret = node->left;
	while (ret->right)
		ret = ret->right;
	return ret;
}

static RBNode *__get_post_node(RBTree *rbtree, RBNode *node)
{
	if (!node->right)
		return NULL;
	RBNode *ret = node->right;
	while (ret->left)
		ret = ret->left;
	return ret;
}

/* The 1st phase of delete */
static RBNode *swap_delete_node_to_leaf(RBTree *rbtree, RBNode *node)
{
	RBNode *to_swap = __get_post_node(rbtree, node);
	if (to_swap) {
		node_swap(node, to_swap);
		return swap_delete_node_to_leaf(rbtree, to_swap);
	}
	to_swap = __get_pre_node(rbtree, node);
	if (to_swap) {
		node_swap(node, to_swap);
		return swap_delete_node_to_leaf(rbtree, to_swap);
	}
	return node; /* node is leaf */
}

/* The 3rd phase of delete */
static void remove_leaf_node(RBTree *rbtree, RBNode *node)
{
	if (rbtree->root == node) {
		rbtree->root = NULL;
		return;
	}

	if (node->parent->left == node) {
		node->parent->left = NULL;
		return;
	}

	if (node->parent->right == node) {
		node->parent->right = NULL;
		return;
	}
}

RBNode *rbtree_delete(RBTree *rbtree, rbkey_t key)
{
	RBNode *to_del = rbtree_get(rbtree, key);
	if (!to_del)
		return NULL;

	to_del = swap_delete_node_to_leaf(rbtree, to_del);
	rbtree_delete_balance(rbtree, to_del);
	remove_leaf_node(rbtree, to_del);

	return to_del;
}

RBTree* new_rbtree()
{
	RBTree *ret = (RBTree *)malloc(sizeof(*ret));
	ret->root = NULL;
	return ret;
}

void free_rbtree(RBTree *rbtree)
{
	free(rbtree);
}

RBNode* new_rbnode(rbkey_t key, rbvalue_t value)
{
	RBNode *ret = (RBNode *)malloc(sizeof(*ret));
	assert(ret);
	ret->key = key;
	ret->value = value;
	ret->left = NULL;
	ret->right = NULL;
	ret->parent = NULL;
	ret->color = RED;
	return ret;
}

void free_rbnode(RBNode *rbnode)
{
	free(rbnode);
}