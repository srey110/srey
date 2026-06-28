#include "containers/slist.h"

void list_init(list_ctx *lst) {
    lst->head = NULL;
    lst->tail = NULL;
    lst->size = 0;
}
void list_push_head(list_ctx *lst, list_node *node) {
    node->prev = NULL;
    node->next = lst->head;
    if (NULL != lst->head) {
        lst->head->prev = node;
    } else {
        lst->tail = node;
    }
    lst->head = node;
    lst->size++;
}
void list_push_tail(list_ctx *lst, list_node *node) {
    node->next = NULL;
    node->prev = lst->tail;
    if (NULL != lst->tail) {
        lst->tail->next = node;
    } else {
        lst->head = node;
    }
    lst->tail = node;
    lst->size++;
}
void list_insert_before(list_ctx *lst, list_node *pos, list_node *node) {
    node->next = pos;
    node->prev = pos->prev;
    if (NULL != pos->prev) {
        pos->prev->next = node;
    } else {
        lst->head = node;
    }
    pos->prev = node;
    lst->size++;
}
void list_insert_after(list_ctx *lst, list_node *pos, list_node *node) {
    node->prev = pos;
    node->next = pos->next;
    if (NULL != pos->next) {
        pos->next->prev = node;
    } else {
        lst->tail = node;
    }
    pos->next = node;
    lst->size++;
}
void list_remove(list_ctx *lst, list_node *node) {
    if (NULL != node->prev) {
        node->prev->next = node->next;
    } else {
        lst->head = node->next;
    }
    if (NULL != node->next) {
        node->next->prev = node->prev;
    } else {
        lst->tail = node->prev;
    }
    node->next = NULL;
    node->prev = NULL;
    lst->size--;
}
list_node *list_pop_head(list_ctx *lst) {
    list_node *node = lst->head;
    if (NULL == node) {
        return NULL;
    }
    list_remove(lst, node);
    return node;
}
list_node *list_pop_tail(list_ctx *lst) {
    list_node *node = lst->tail;
    if (NULL == node) {
        return NULL;
    }
    list_remove(lst, node);
    return node;
}
void list_splice_tail(list_ctx *dst, list_ctx *src) {
    if (NULL == src->head) {
        return;
    }
    if (NULL != dst->tail) {
        dst->tail->next = src->head;
        src->head->prev = dst->tail;
    } else {
        dst->head = src->head;
    }
    dst->tail = src->tail;
    dst->size += src->size;
    src->head = NULL;
    src->tail = NULL;
    src->size = 0;
}
int32_t list_empty(const list_ctx *lst) {
    return NULL == lst->head;
}
uint32_t list_size(const list_ctx *lst) {
    return lst->size;
}
void list_iter_init(list_iter *it, const list_ctx *lst) {
    it->next = lst->head;
}
list_node *list_iter_next(list_iter *it) {
    list_node *node = it->next;
    if (NULL == node) {
        return NULL;
    }
    it->next = node->next;
    return node;
}
