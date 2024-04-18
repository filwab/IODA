#include <string.h>
#include <stdlib.h>
//#include "ssd.h"

#define AVL_NULL		(TREE_NODE *)0

#define EH_FACTOR	0
#define LH_FACTOR	1
#define RH_FACTOR	-1
#define LEFT_MINUS	0
#define RIGHT_MINUS	1

#define DIRTY 1
#define CLEAN 0


#define ORDER_LIST_WANTED


#define INSERT_PREV	0
#define INSERT_NEXT	1


/********************************************************
*buffer-management data structure
*********************************************************/
typedef struct _AVL_TREE_NODE
{
#ifdef ORDER_LIST_WANTED
	struct _AVL_TREE_NODE* prev;
	struct _AVL_TREE_NODE* next;
#endif
	struct _AVL_TREE_NODE* tree_root;
	struct _AVL_TREE_NODE* left_child;
	struct _AVL_TREE_NODE* right_child;
	int  bf;    			                     /*平衡因子；当平衡因子的绝对值大于 或等于2的时候就表示树不平衡(balance_factor)*/
}TREE_NODE;

typedef struct buffer_info
{
	unsigned long read_hit;                      /*这里的hit都表示sector的命中次数或是没命中的次数*/
	unsigned long read_miss_hit;
	unsigned long write_hit;
	unsigned long write_miss_hit;

	double read_hit_ratio;						/*读写命中率*/
	double write_hit_ratio;

	struct buffer_group* buffer_head;            /*as LRU head which is most recently used*/
	struct buffer_group* buffer_tail;            /*as LRU tail which is least recently used*/
	TREE_NODE* pTreeHeader;     				 /*for search target lsn is LRU table*/

	//unsigned int max_buffer_sector;
	//unsigned int buffer_sector_count;

	unsigned int max_buffer_page;
	unsigned int buffer_page_count;

	//unsigned int dram_page_num;
	//unsigned int remain_page_num;
	int buffer_full_flag;

	//unsigned int max_command_buff_page;
	//unsigned int command_buff_page;


#ifdef ORDER_LIST_WANTED
	TREE_NODE* pListHeader;
	TREE_NODE* pListTail;
#endif
	unsigned int	count;		                 /*AVL树里的节点总数*/
	int 			(*keyCompare)(TREE_NODE*, TREE_NODE*);
	int			(*free)(TREE_NODE*);
} tAVLTree;



typedef struct buffer_group {
	TREE_NODE node;                     //The structure of the tree node must be placed at the top of the user-defined structure
	struct buffer_group* LRU_link_next;	// next node in LRU list
	struct buffer_group* LRU_link_pre;	// previous node in LRU list

	//unsigned int group;                 //the first data logic sector number of a group stored in buffer 
	//unsigned int stored;                //indicate the sector is stored in buffer or not. 1 indicates the sector is stored and 0 indicate the sector isn't stored.EX.  00110011 indicates the first, second, fifth, sixth sector is stored in buffer.
	//unsigned int dirty_clean;           //it is flag of the data has been modified, one bit indicates one subpage. EX. 0001 indicates the first subpage is dirty
	//int flag;			                //indicates if this node is the last 20% of the LRU list	
	//unsigned int page_type;				//buff page type:0--full_page  1--partial_page

	unsigned int key;                 //the logic number of a page stored in buffer 
	//unsigned int stored;                //indicate the page is stored in buffer or not. 1 indicates the page is stored and 0 indicate the page isn't stored.
	unsigned int dirty_clean;           //it is flag of the data has been modified,0 for clean and 1 for dirty(not consistent with the pysical memory)
	//int flag;			                //indicates if this node is the last 20% of the LRU list	

}buf_node;

int avlTreeHigh(TREE_NODE*);
int avlTreeCheck(tAVLTree*, TREE_NODE*);
static void R_Rotate(TREE_NODE**);
static void L_Rotate(TREE_NODE**);
static void LeftBalance(TREE_NODE**);
static void RightBalance(TREE_NODE**);
static int avlDelBalance(tAVLTree*, TREE_NODE*, int);
void AVL_TREE_LOCK(tAVLTree*, int);
void AVL_TREE_UNLOCK(tAVLTree*);
void AVL_TREENODE_FREE(tAVLTree*, TREE_NODE*);

#ifdef ORDER_LIST_WANTED
static int orderListInsert(tAVLTree*, TREE_NODE*, TREE_NODE*, int);
static int orderListRemove(tAVLTree*, TREE_NODE*);
TREE_NODE* avlTreeFirst(tAVLTree*);
TREE_NODE* avlTreeLast(tAVLTree*);
TREE_NODE* avlTreeNext(TREE_NODE* pNode);
TREE_NODE* avlTreePrev(TREE_NODE* pNode);
#endif

static int avlTreeInsert(tAVLTree*, TREE_NODE**, TREE_NODE*, int*);
static int avlTreeRemmove(tAVLTree*, TREE_NODE*);
static TREE_NODE* avlTreeLookup(tAVLTree*, TREE_NODE*, TREE_NODE*);
tAVLTree* avlTreeCreate(int*, int*);
int avlTreeDestroy(tAVLTree*);
int avlTreeFlush(tAVLTree*);
int avlTreeAdd(tAVLTree*, TREE_NODE*);
int avlTreeDel(tAVLTree*, TREE_NODE*);
TREE_NODE* avlTreeFind(tAVLTree*, TREE_NODE*);
unsigned int avlTreeCount(tAVLTree*);

int keyCompareFunc(TREE_NODE* p, TREE_NODE* p1);
int freeFunc(TREE_NODE* pNode);
