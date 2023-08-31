
#ifdef __cplusplus
extern "C" {
#endif

#include <malloc.h>
#include <chcore/type.h>

#define color_t int
#define BLACK 1
#define RED 0

typedef long rbkey_t;
typedef void * rbvalue_t;

#ifdef __cplusplus
	#include <cassert>
	struct RBNode {
		rbkey_t key;
		rbvalue_t value;
		RBNode *left;
		RBNode *right;
		RBNode *parent;
		color_t color;
	};

	struct RBTree {
		RBNode *root;
	};
#else
	#include <assert.h>
	struct _RBNode {
		rbkey_t key;
		rbvalue_t value;
		struct _RBNode *left;
		struct _RBNode *right;
		struct _RBNode *parent;
		color_t color;
	};
	typedef struct _RBNode RBNode;
	typedef struct {
		RBNode *root;
	} RBTree;
#endif

RBTree* new_rbtree();
void free_rbtree(RBTree *rbtree);
RBNode* new_rbnode(rbkey_t key, rbvalue_t value);
void free_rbnode(RBNode *rbnode);

void rbtree_insert(RBTree *rbtree, RBNode *new_node);
RBNode *rbtree_get(RBTree *rbtree, rbkey_t key);
RBNode *rbtree_delete(RBTree *rbtree, rbkey_t key);

#ifdef __cplusplus
}
#endif