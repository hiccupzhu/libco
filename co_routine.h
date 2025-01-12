/*
* Tencent is pleased to support the open source community by making Libco available.

* Copyright (C) 2014 THL A29 Limited, a Tencent company. All rights reserved.
*
* Licensed under the Apache License, Version 2.0 (the "License"); 
* you may not use this file except in compliance with the License. 
* You may obtain a copy of the License at
*
*    http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, 
* software distributed under the License is distributed on an "AS IS" BASIS, 
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. 
* See the License for the specific language governing permissions and 
* limitations under the License.
*/

#ifndef __CO_ROUTINE_H__
#define __CO_ROUTINE_H__

#include <stdint.h>
#include <sys/poll.h>
#include <pthread.h>

#define CO_LOG_INFO( fmt, args... ) \
    do { \
        fprintf( stdout, "[%s:%d]" fmt, __func__, __LINE__, ##args ); \
    } while(0)

__BEGIN_DECLS
//1.struct

typedef struct stCoRoutine_t stCoRoutine_t;
typedef struct stShareStack_t stShareStack_t;
typedef struct stCoEpoll_t stCoEpoll_t;

typedef struct stCoRoutineAttr_t
{
    int stack_size;
    stShareStack_t*  share_stack;
}__attribute__ ((packed)) stCoRoutineAttr_t;

struct stCoEpoll_t;
typedef int (*pfn_co_eventloop_t)(void *);
typedef void *(*pfn_co_routine_t)( void * );


//2.co_routine
int     co_create( stCoRoutine_t **co,const stCoRoutineAttr_t *attr,void *(*routine)(void*),void *arg );
void    co_resume( stCoRoutine_t *co );
void    co_yield( stCoRoutine_t *co );
void    co_yield_ct(); //ct = current thread
void    co_release( stCoRoutine_t *co );
void    co_reset(stCoRoutine_t * co); 

stCoRoutine_t *co_self();

int        co_poll( stCoEpoll_t *ctx,struct pollfd fds[], nfds_t nfds, int timeout_ms );
void     co_eventloop( stCoEpoll_t *ctx,pfn_co_eventloop_t pfn,void *arg );

//3.specific

int     co_setspecific( pthread_key_t key, const void *value );
void *    co_getspecific( pthread_key_t key );

//4.event

stCoEpoll_t *     co_get_epoll_ct(); //ct = current thread

//5.hook syscall ( poll/read/write/recv/send/recvfrom/sendto )

void     co_enable_hook_sys();  
void     co_disable_hook_sys();  
int      co_is_enable_sys_hook();

//6.sync
typedef struct dq_queue_s stCoCond_t;

stCoCond_t *co_cond_alloc();
int co_cond_free( stCoCond_t * cc );

int co_cond_signal( stCoCond_t * );
int co_cond_broadcast( stCoCond_t * );
int co_cond_timedwait( stCoCond_t *,int timeout_ms );

//7.share stack
stShareStack_t* co_alloc_sharestack(int iCount, int iStackSize);

//8.init envlist for hook get/set env
void co_set_env_list( const char *name[],size_t cnt);

void co_log_err( const char *fmt,... );

__END_DECLS
#endif

