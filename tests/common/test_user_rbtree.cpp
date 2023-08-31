#include <iostream>
#include <map>
#include <cassert>

#include <minunit.h>

#include "../../user/musl-1.1.24/src/chcore/rbtree.c"

#define TEST_ROUNDS (10000 * 100)

/* helpers */
int __black_height(RBNode *node)
{
	if (!node)
		return 1;

	int lheight = __black_height(node->left);
	int rheight = __black_height(node->right);
	assert(lheight == rheight);
	if (node->color == RED) {
		if (node->left && node->left->color == RED)
			assert(0);
		if (node->right && node->right->color == RED)
			assert(0);
		return lheight;
	}
	else if (node->color == BLACK)
		return lheight + 1;
	else
		assert(0);
}

bool check_is_rbtree(RBTree *rbtree)
{
	if (!rbtree->root)
		return true;
	if (rbtree->root->color != BLACK)
		return false;

	return (__black_height(rbtree->root->left) == __black_height(rbtree->root->right));
}

MU_TEST(test_rbtree)
{
    RBTree *t = new_rbtree();
    RBNode *n;
    std::map<long, long> answer;

    srand(time(NULL));

    for (int i = 0; i < TEST_ROUNDS; ++i) {
        /* check t satisfies rbtree */
        mu_check(check_is_rbtree(t));

        /* do random operations */
        int op = rand() % 5;
        if (op == 0) {
            /* random insert */
            long key = rand() % 0x10000;
            long value = rand() % 0x20000;
            auto it = answer.find(key);
            if (it == answer.end()) {
                answer[key] = value;
            } else {
                continue;
            }

            n = new_rbnode(reinterpret_cast<rbkey_t>(key), reinterpret_cast<rbvalue_t>(value));
            rbtree_insert(t, n);
        } else if (op == 1) {
            /* random get exists */
            if (answer.size() < 2)
                continue;
            auto it  = answer.begin();
            for (int k = 0; k < (rand() % (answer.size() - 1)); ++k) {
                ++it;
            }
            n = rbtree_get(t, reinterpret_cast<rbkey_t>(it->first));
            mu_check(n);
            mu_check(reinterpret_cast<long>(n->value) == it->second);
        } else if (op == 2) {
            /* random get */
            long key = rand() % 0x10000;
            auto it = answer.find(key);
            if (it != answer.end()) {
                n = rbtree_get(t, reinterpret_cast<rbkey_t>(key));
                mu_check(n);
                mu_check(reinterpret_cast<long>(n->value) == it->second);
            } else {
                n = rbtree_get(t, reinterpret_cast<rbkey_t>(key));
                mu_check(!n);
            }
        } else if (op == 3) {
            /* random delete exists */
            if (answer.size() < 2)
                continue;
            auto it  = answer.begin();
            for (int k = 0; k < (rand() % (answer.size() - 1)); ++k) {
                ++it;
            }
            n = rbtree_get(t, reinterpret_cast<rbkey_t>(it->first));
            mu_check(n);
            mu_check(reinterpret_cast<long>(n->value) == it->second);
            n = rbtree_delete(t, reinterpret_cast<rbkey_t>(it->first));
            mu_check(n != NULL);
            free_rbnode(n);
            n = rbtree_get(t, reinterpret_cast<rbkey_t>(it->first));
            mu_check(!n);
            answer.erase(it);
        } else if (op == 4) {
            /* random delete */
            long key = rand() % 0x10000;
            auto it = answer.find(key);
            if (it != answer.end()) {
                n = rbtree_get(t, reinterpret_cast<rbkey_t>(key));
                mu_check(n);
                mu_check(reinterpret_cast<long>(n->value) == it->second);
                n = rbtree_delete(t, reinterpret_cast<rbkey_t>(it->first));
                mu_check(n != NULL);
                free_rbnode(n);
                n = rbtree_get(t, reinterpret_cast<rbkey_t>(it->first));
                mu_check(!n);
                answer.erase(it);
            } else {
                n = rbtree_get(t, reinterpret_cast<rbkey_t>(key));
                mu_check(!n);
            }
        }
    }
    free_rbtree(t);
}

MU_TEST_SUITE(test_suit)
{
    MU_RUN_TEST(test_rbtree);
}

int main(int argc, char *argv[])
{
    MU_RUN_SUITE(test_suit);
    MU_REPORT();
    return minunit_status;
}