#include <stdint.h>
#include <stdbool.h>
#include "avlTree.h"

#define my_alloc(p,s,length) {do{(p)=(s);}while((p)==NULL); memset((p),0,(length));}

//修改LRU链表，将已存在目标节点放置在LRU链表首部
bool LRU_Tofirst(tAVLTree * buffer,struct buffer_group* buffer_node);//TODO: complete difenition

//生成一个新的buffer node ,根据这个页的情况赋值各个成员，添加到队首和二叉树中
bool  create_new_bufnode(tAVLTree * buffer, uint64_t lpn);

//缓存满，替换节点时删除LRU链表尾节点在二叉树和LRU链表中的信息
bool  dram_delete_buffer_node(tAVLTree * buffer);

//检查缓存空间，更新buffer_full_flag
int  check_buffer_page(tAVLTree * buffer);

//更新缓存空间大小，并根据大小更新缓存中的数据。
int update_buffer_page(tAVLTree * buffer , uint64_t new_capacity);