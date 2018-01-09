#include "skynet.h"
#include "skynet_mq.h"
#include "skynet_handle.h"
#include "spinlock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>

#define DEFAULT_QUEUE_SIZE 64 //每个ctx初始队列长度
#define MAX_GLOBAL_MQ 0x10000

// 0 means mq is not in global mq.
// 1 means mq is in global mq , or the message is dispatching.

#define MQ_IN_GLOBAL 1 //1表示mq在全局队列里，工作线程可以调度到；0表示mq不在全局队列里
#define MQ_OVERLOAD 1024 //队列长度超过阈值，意味这个ctx过载

struct message_queue { //每个ctx私有队列结构
	struct spinlock lock; //自旋锁，保证最多只有一个线程在处理
	uint32_t handle; //对应的ctx，注意是个整数，而不是指针
	int cap; //队列容量（数组长度）
	int head; //队列头
	int tail; //队列尾
	int release; //标记是否可释放（当delete ctx时会设置此标记）
	int in_global; //标记是否在全局队列中，1表示在j
	int overload; //标记是否过载
	int overload_threshold; //过载阈值，初始是MQ_OVERLOAD
	struct skynet_message *queue; //消息数据，实际上是一个数组，通过head,tail实现类似队列的功能
	struct message_queue *next; //指向下一个消息队列
};

struct global_queue { //全局队列结构
	struct message_queue *head; //指向一个ctx私有队列的头指针
	struct message_queue *tail; //指向一个ctx私有队列的尾指针
	struct spinlock lock; //自旋锁,保证同一时刻只有一个线程在处理里
};

static struct global_queue *Q = NULL; //全局队列

void 
skynet_globalmq_push(struct message_queue * queue) { //向全局队列push一个私有队列
	struct global_queue *q= Q;

	SPIN_LOCK(q) //上锁
	assert(queue->next == NULL);
	if(q->tail) {
		q->tail->next = queue;
		q->tail = queue;
	} else {
		q->head = q->tail = queue; //当队列为空时，头尾指针都指向queue
	}
	SPIN_UNLOCK(q) //解锁
}

struct message_queue * 
skynet_globalmq_pop() { //从全局队列pop一个私有队列
	struct global_queue *q = Q;

	SPIN_LOCK(q)
	struct message_queue *mq = q->head;
	if(mq) {
		q->head = mq->next;
		if(q->head == NULL) { //队列里只有一个元素，pop完后，head和tail指向都指向NULL
			assert(mq == q->tail);
			q->tail = NULL;
		}
		mq->next = NULL;
	}
	SPIN_UNLOCK(q)

	return mq;
}

struct message_queue * 
skynet_mq_create(uint32_t handle) { //创建一个私有队列，当创建一个ctx会调用，handle对应ctx
	struct message_queue *q = skynet_malloc(sizeof(*q)); //分配内存
	q->handle = handle;
	q->cap = DEFAULT_QUEUE_SIZE; //初始queue容量
	q->head = 0;
	q->tail = 0;
	SPIN_INIT(q) //初始化自旋锁
	// When the queue is create (always between service create and service init) ,
	// set in_global flag to avoid push it to global queue .
	// If the service init success, skynet_context_new will call skynet_mq_push to push it to global queue.
    // 创建队列时可以发送和接收消息，但还不能被工作线程调度，所以设置成MQ_IN_GLOBAL，保证不会push到全局队列，
    // 当ctx初始化完成再直接调用skynet_globalmq_push到全局队列
	q->in_global = MQ_IN_GLOBAL;
	q->release = 0;
	q->overload = 0;
	q->overload_threshold = MQ_OVERLOAD;
	q->queue = skynet_malloc(sizeof(struct skynet_message) * q->cap); //分配cap个skynet_message大小容量
	q->next = NULL;

	return q;
}

static void 
_release(struct message_queue *q) { //释放一个私有队列
	assert(q->next == NULL);
	SPIN_DESTROY(q)
	skynet_free(q->queue);
	skynet_free(q);
}

uint32_t 
skynet_mq_handle(struct message_queue *q) { //获取队列的ctx的handle，通过handle再找对应的ctx实例（指针）
	return q->handle;
}

int
skynet_mq_length(struct message_queue *q) { //队列长度
	int head, tail,cap;

	SPIN_LOCK(q)
	head = q->head;
	tail = q->tail;
	cap = q->cap;
	SPIN_UNLOCK(q)
	
	if (head <= tail) { //头部小于尾部
		return tail - head;
	}
	return tail + cap - head; //头部大于尾部
}

int
skynet_mq_overload(struct message_queue *q) { //是否过载
	if (q->overload) {
		int overload = q->overload;
		q->overload = 0;
		return overload;
	} 
	return 0;
}

int
skynet_mq_pop(struct message_queue *q, struct skynet_message *message) { //从私有队列里pop一个消息
	int ret = 1;
	SPIN_LOCK(q)

	if (q->head != q->tail) {
		*message = q->queue[q->head++]; //取出头部位置的message，头部前移一位
		ret = 0;
		int head = q->head;
		int tail = q->tail;
		int cap = q->cap;

		if (head >= cap) { //大于容量，重置为0
			q->head = head = 0;
		}
		int length = tail - head;
		if (length < 0) { //头部大于尾部
			length += cap;
		}
		while (length > q->overload_threshold) { //超过阈值，扩大阈值范围
			q->overload = length;
			q->overload_threshold *= 2;
		}
	} else {
		// reset overload_threshold when queue is empty
		q->overload_threshold = MQ_OVERLOAD;
	}

	if (ret) {
		q->in_global = 0; //如队列为空，标记不在全局队列中，等向队列里push新消息时，再push到全局队列里
	}
	
	SPIN_UNLOCK(q)

	return ret;
}

static void
expand_queue(struct message_queue *q) { //扩充队列容量，扩大到原来的2倍
	struct skynet_message *new_queue = skynet_malloc(sizeof(struct skynet_message) * q->cap * 2);
	int i;
	for (i=0;i<q->cap;i++) { //转移数据
		new_queue[i] = q->queue[(q->head + i) % q->cap];
	}
	q->head = 0;
	q->tail = q->cap;
	q->cap *= 2;
	
	skynet_free(q->queue); //释放之前的消息数据
	q->queue = new_queue;
}

void 
skynet_mq_push(struct message_queue *q, struct skynet_message *message) { //向消息队列里push消息
	assert(message);
	SPIN_LOCK(q)

	q->queue[q->tail] = *message; //存到尾部，然后尾部+1，如果超过容量，则重置为0
	if (++ q->tail >= q->cap) {
		q->tail = 0;
	}

	if (q->head == q->tail) { //如尾部==头部，说明队列已满，需扩充容量
		expand_queue(q);
	}

	if (q->in_global == 0) { //如不在全局队列里，则push到全局队列
		q->in_global = MQ_IN_GLOBAL;
		skynet_globalmq_push(q);
	}
	
	SPIN_UNLOCK(q)
}

void 
skynet_mq_init() { //全局队列初始化
	struct global_queue *q = skynet_malloc(sizeof(*q));
	memset(q,0,sizeof(*q));
	SPIN_INIT(q);
	Q=q;
}

void 
skynet_mq_mark_release(struct message_queue *q) { //当私有队列对应的ctx delete时，需标记私有队列的状态
	SPIN_LOCK(q)
	assert(q->release == 0);
	q->release = 1;
	if (q->in_global != MQ_IN_GLOBAL) { //push到全局队列，保证可以被调度到，然后才能被真正释放
		skynet_globalmq_push(q);
	}
	SPIN_UNLOCK(q)
}

static void
_drop_queue(struct message_queue *q, message_drop drop_func, void *ud) { //准备释放队列
	struct skynet_message msg;
	while(!skynet_mq_pop(q, &msg)) { //先向队列里各个消息的源地址发送特定消息，再释放内存
		drop_func(&msg, ud);
	}
	_release(q);
}

void 
skynet_mq_release(struct message_queue *q, message_drop drop_func, void *ud) { //尝试释放私有队列
	SPIN_LOCK(q)
	//只有队列已经被标记可释放(release==1)时，说明ctx真正delete了，才能释放掉队列，
    //否则继续push到全局队列，等待下一次调度
	if (q->release) {
		SPIN_UNLOCK(q)
		_drop_queue(q, drop_func, ud);
	} else {
		skynet_globalmq_push(q);
		SPIN_UNLOCK(q)
	}
}
