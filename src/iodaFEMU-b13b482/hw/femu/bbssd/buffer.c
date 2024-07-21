#include "buffer.h"


/*- 将LRU中存在的某个bufnode节点移动到LRU链表的头部 -*/
bool LRU_Tofirst(tAVLTree * buffer,struct buffer_group* buffer_node)
{
    if (buffer->buffer_head != buffer_node)
	{
		if (buffer->buffer_tail == buffer_node)
		{
			buffer_node->LRU_link_pre->LRU_link_next = NULL;
			buffer->buffer_tail = buffer_node->LRU_link_pre;
		}
		else
		{
			buffer_node->LRU_link_pre->LRU_link_next = buffer_node->LRU_link_next;
			buffer_node->LRU_link_next->LRU_link_pre = buffer_node->LRU_link_pre;
		}
		buffer_node->LRU_link_next = buffer->buffer_head;
		buffer->buffer_head->LRU_link_pre = buffer_node;
		buffer_node->LRU_link_pre = NULL;
		buffer->buffer_head = buffer_node;
	}
    return true;
}

bool  create_new_bufnode(tAVLTree * buffer, uint64_t lpn)
{
    struct buffer_group* new_node = NULL;
	my_alloc(new_node, (struct buffer_group*)malloc(sizeof(struct buffer_group)), sizeof(struct buffer_group));
	new_node->key = lpn;

	/*- 将节点添加到LRU链表首部 -*/
	new_node->LRU_link_pre = NULL;
	new_node->LRU_link_next = buffer->buffer_head;
	if (buffer->buffer_head != NULL) {
		buffer->buffer_head->LRU_link_pre = new_node;
	}
	else {
		buffer->buffer_tail = new_node;
	}
	buffer->buffer_head = new_node;
	/*- 添加节点到二叉树中 -*/
	avlTreeAdd(buffer, (TREE_NODE*)new_node);
	buffer->buffer_page_count++;
	check_buffer_page(buffer);
	return true;
	// ssd->user[user].dram->bufnode_create_num++;

}

/*- 将节点信息从二叉树和LRU链表中清除，同时释放缓存节点空间 -*/
bool  dram_delete_buffer_node(tAVLTree * buffer)
{
	struct buffer_group* victim_node = buffer->buffer_tail;
	avlTreeDel(buffer, (TREE_NODE *)victim_node);
	if (buffer->buffer_head->LRU_link_next == NULL) /*only one node in LRU*/
	{
		buffer->buffer_head = NULL;
		buffer->buffer_tail = NULL;
	}
	else
	{
		buffer->buffer_tail = buffer->buffer_tail->LRU_link_pre;
		buffer->buffer_tail->LRU_link_next = NULL;
	}
	victim_node->LRU_link_pre = NULL;
	victim_node->LRU_link_next = NULL;
	AVL_TREENODE_FREE(buffer, (TREE_NODE *)victim_node);
	victim_node = NULL;

	buffer->buffer_page_count--;
	check_buffer_page(buffer);
	return true;
}


int  check_buffer_page(tAVLTree * buffer)
{
	if (buffer->buffer_page_count >= buffer->max_buffer_page) {
		buffer->buffer_full_flag = 1;
	}
	else
	{
		buffer->buffer_full_flag = 0;
	}
	return buffer->buffer_full_flag;
}

/*- 根据new_capacity更新ssd的缓存空间*/
int update_buffer_page(tAVLTree * buffer , uint64_t new_capacity)
{
	buffer->max_buffer_page = (unsigned int ) new_capacity / 4096;
	while (buffer->buffer_page_count > buffer->max_buffer_page) 
	{
		dram_delete_buffer_node(buffer);
	}

	check_buffer_page(buffer);
	return buffer->buffer_full_flag;
}