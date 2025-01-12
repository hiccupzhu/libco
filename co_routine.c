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

#include "co_routine.h"
#include "co_routine_inner.h"
#include "co_epoll.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <poll.h>
#include <sys/time.h>
#include <errno.h>

#include <assert.h>

#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <limits.h>
#include "queue.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof(a[0]))

// 将当前的上下文保存到from中，将to的上下文切换到当前上下文
extern void coctx_swap( coctx_t *from, coctx_t* to) asm("coctx_swap");

stCoRoutine_t *GetCurrCo( stCoRoutineEnv_t *env );
struct stCoEpoll_t;

struct stCoRoutineEnv_t
{
    stCoRoutine_t *pCallStack[ 128 ];
    int iCallStackSize;
    stCoEpoll_t *pEpoll;

    //for copy stack log lastco and nextco
    stCoRoutine_t* pending_co;
    stCoRoutine_t* occupy_co;
};
//int socket(int domain, int type, int protocol);
void co_log_err( const char *fmt,... )
{
}


#if defined( __LIBCO_RDTSCP__) 
static unsigned long long counter(void)
{
    register uint32_t lo, hi;
    register unsigned long long o;
    __asm__ __volatile__ (
            "rdtscp" : "=a"(lo), "=d"(hi)::"%rcx"
            );
    o = hi;
    o <<= 32;
    return (o | lo);

}
static unsigned long long getCpuKhz()
{
    FILE *fp = fopen("/proc/cpuinfo","r");
    if(!fp) return 1;
    char buf[4096] = {0};
    fread(buf,1,sizeof(buf),fp);
    fclose(fp);

    char *lp = strstr(buf,"cpu MHz");
    if(!lp) return 1;
    lp += strlen("cpu MHz");
    while(*lp == ' ' || *lp == '\t' || *lp == ':')
    {
        ++lp;
    }

    double mhz = atof(lp);
    unsigned long long u = (unsigned long long)(mhz * 1000);
    return u;
}
#endif

static unsigned long long GetTickMS()
{
#if defined( __LIBCO_RDTSCP__) 
    static uint32_t khz = getCpuKhz();
    return counter() / khz;
#else
    struct timeval now = { 0 };
    gettimeofday( &now,NULL );
    unsigned long long u = now.tv_sec;
    u *= 1000;
    u += now.tv_usec / 1000;
    return u;
#endif
}

/////////////////for copy stack //////////////////////////
stStackMem_t* co_alloc_stackmem(unsigned int stack_size)
{
    stStackMem_t* stack_mem = (stStackMem_t*)malloc(sizeof(stStackMem_t));
    stack_mem->occupy_co= NULL;
    stack_mem->stack_size = stack_size;
    stack_mem->stack_buffer = (char*)malloc(stack_size);
    stack_mem->stack_bp = stack_mem->stack_buffer + stack_size;
    return stack_mem;
}

stShareStack_t* co_alloc_sharestack(int count, int stack_size)
{
    stShareStack_t* share_stack = (stShareStack_t*)malloc(sizeof(stShareStack_t));
    share_stack->alloc_idx = 0;
    share_stack->stack_size = stack_size;

    //alloc stack array
    share_stack->count = count;
    stStackMem_t** stack_array = (stStackMem_t**)calloc(count, sizeof(stStackMem_t*));
    for (int i = 0; i < count; i++)
    {
        stack_array[i] = co_alloc_stackmem(stack_size);
    }
    share_stack->stack_array = stack_array;
    return share_stack;
}

static stStackMem_t* co_get_stackmem(stShareStack_t* share_stack)
{
    if (!share_stack)
    {
        return NULL;
    }
    int idx = share_stack->alloc_idx % share_stack->count;
    share_stack->alloc_idx++;

    return share_stack->stack_array[idx];
}


// ----------------------------------------------------------------------------
typedef struct dq_queue_s stTimeoutItemLink_t;
struct stTimeoutItem_t;
#define _EPOLL_SIZE (1024 * 10)

struct stCoEpoll_t
{
    int iEpollFd;

    // alloc 60s array for timeout-queue
    // every 1ms one pointer
    struct stTimeout_t *pTimeout;

    // 仅在epoll_wait的时候，作为临时变量使用
    stTimeoutItemLink_t *pstTimeoutList;

    // 仅在cond signal 和 broadcast的时候使用
    stTimeoutItemLink_t *pstActiveList;

    co_epoll_res *result; 

};
typedef void (*OnPreparePfn_t)( struct epoll_event *ev, stTimeoutItemLink_t *active );
typedef void (*OnProcessPfn_t)( stTimeoutItem_t *);
#define eMaxTimeout (40000) /*40s*/

#define ST_TIMEOUT_ITEM_COMMON                \
    dq_entry_t entry;                         \
    stTimeoutItemLink_t *pLink;               \
                                              \
    unsigned long long ullExpireTime;         \
    OnPreparePfn_t pfnPrepare;                \
    OnProcessPfn_t pfnProcess;                \
                                              \
    void *pArg; /*routine*/                   \
    int bTimeout

struct stTimeoutItem_t
{
    ST_TIMEOUT_ITEM_COMMON;
};

struct stTimeout_t
{
    // for stTimeoutItem_t
    dq_queue_t *pItems;
    int iItemSize;

    unsigned long long ullStart;
    long long llStartIdx;
};
stTimeout_t *AllocTimeout( int iSize )
{
    stTimeout_t *lp = (stTimeout_t*)calloc( 1,sizeof(stTimeout_t) );    

    lp->iItemSize = iSize;
    lp->pItems = (stTimeoutItemLink_t*)calloc( 1,sizeof(stTimeoutItemLink_t) * lp->iItemSize );

    lp->ullStart = GetTickMS();
    lp->llStartIdx = 0;

    return lp;
}
void FreeTimeout( stTimeout_t *apTimeout )
{
    free( apTimeout->pItems );
    free ( apTimeout );
}

// 添加超时事件
// return 0 success, otherwize failed.
int AddTimeout( stTimeout_t *apTimeout, stTimeoutItem_t *apItem , uint64_t allNow )
{
    if( apTimeout->ullStart == 0 )
    {
        apTimeout->ullStart = allNow;
        apTimeout->llStartIdx = 0;
    }
    if( allNow < apTimeout->ullStart )
    {
        co_log_err("CO_ERR: AddTimeout line %d allNow %llu apTimeout->ullStart %llu",
                    __LINE__,allNow,apTimeout->ullStart);

        return __LINE__;
    }
    if( apItem->ullExpireTime < allNow )
    {
        co_log_err("CO_ERR: AddTimeout line %d apItem->ullExpireTime %llu allNow %llu apTimeout->ullStart %llu",
                    __LINE__,apItem->ullExpireTime,allNow,apTimeout->ullStart);

        return __LINE__;
    }
    unsigned long long diff = apItem->ullExpireTime - apTimeout->ullStart;

    if( diff >= (unsigned long long)apTimeout->iItemSize )
    {
        diff = apTimeout->iItemSize - 1;
        co_log_err("CO_ERR: AddTimeout line %d diff %d",
                    __LINE__,diff);

        //return __LINE__;
    }

    apItem->pLink = apTimeout->pItems + ( apTimeout->llStartIdx + diff ) % apTimeout->iItemSize;
    dq_addlast(&apItem->entry, apItem->pLink);

    return 0;
}
static inline void TakeAllTimeout(stTimeout_t *apTimeout, unsigned long long allNow, stTimeoutItemLink_t *apResult)
{
    if (apTimeout->ullStart == 0) {
        apTimeout->ullStart = allNow;
        apTimeout->llStartIdx = 0;
    }

    if (allNow < apTimeout->ullStart) {
        return;
    }

    int cnt = allNow - apTimeout->ullStart + 1;
    if (cnt > apTimeout->iItemSize) {
        cnt = apTimeout->iItemSize;
    }
    if (cnt < 0) {
        return;
    }

    for (int i = 0; i < cnt; i++) {
        int idx = (apTimeout->llStartIdx + i) % apTimeout->iItemSize;
        dq_cat(apTimeout->pItems + idx, apResult);
    }

    apTimeout->ullStart = allNow;
    apTimeout->llStartIdx += cnt - 1;
}

static int CoRoutineFunc( stCoRoutine_t *co,void * )
{
    if( co->pfn )
    {
        co->pfn( co->arg );
    }
    co->cEnd = 1;

    stCoRoutineEnv_t *env = co->env;

    co_yield_env( env );

    return 0;
}



struct stCoRoutine_t *co_create_env( stCoRoutineEnv_t * env, const stCoRoutineAttr_t* attr,
        pfn_co_routine_t pfn,void *arg )
{

    stCoRoutineAttr_t at;
    at.stack_size = 128 * 1024;
    at.share_stack = NULL;

    if( attr )
    {
        memcpy( &at,attr,sizeof(at) );
    }
    if( at.stack_size <= 0 )
    {
        at.stack_size = 128 * 1024;
    }
    else if( at.stack_size > 1024 * 1024 * 8 )
    {
        at.stack_size = 1024 * 1024 * 8;
    }

    if( at.stack_size & 0xFFF ) 
    {
        at.stack_size &= ~0xFFF;
        at.stack_size += 0x1000;
    }

    stCoRoutine_t *lp = (stCoRoutine_t*)malloc( sizeof(stCoRoutine_t) );
    
    memset( lp,0,(long)(sizeof(stCoRoutine_t))); 


    lp->env = env;
    lp->pfn = pfn;
    lp->arg = arg;

    stStackMem_t* stack_mem = NULL;
    if( at.share_stack )
    {
        stack_mem = co_get_stackmem( at.share_stack);
        at.stack_size = at.share_stack->stack_size;
    }
    else
    {
        stack_mem = co_alloc_stackmem(at.stack_size);
    }
    lp->stack_mem = stack_mem;

    lp->ctx.ss_sp = stack_mem->stack_buffer;
    lp->ctx.ss_size = at.stack_size;

    lp->cStart = 0;
    lp->cEnd = 0;
    lp->cIsMain = 0;
    lp->cEnableSysHook = 0;
    lp->cIsShareStack = at.share_stack != NULL;

    lp->save_size = 0;
    lp->save_buffer = NULL;

    return lp;
}

int co_create(stCoRoutine_t **ppco, const stCoRoutineAttr_t *attr, pfn_co_routine_t pfn, void *arg)
{
    if( !co_get_curr_thread_env() ) 
    {
        co_init_curr_thread_env();
    }
    stCoRoutine_t *co = co_create_env( co_get_curr_thread_env(), attr, pfn,arg );
    *ppco = co;
    return 0;
}
void co_free( stCoRoutine_t *co )
{
    if (!co->cIsShareStack) 
    {    
        free(co->stack_mem->stack_buffer);
        free(co->stack_mem);
    }   
    //walkerdu fix at 2018-01-20
    //存在内存泄漏
    else 
    {
        if(co->save_buffer)
            free(co->save_buffer);

        if(co->stack_mem->occupy_co == co)
            co->stack_mem->occupy_co = NULL;
    }

    free( co );
}
void co_release( stCoRoutine_t *co )
{
    co_free( co );
}

void co_swap(stCoRoutine_t* curr, stCoRoutine_t* pending_co);

void co_resume( stCoRoutine_t *co )
{
    stCoRoutineEnv_t *env = co->env;
    stCoRoutine_t *lpCurrRoutine = env->pCallStack[ env->iCallStackSize - 1 ];
    if( !co->cStart )
    {
        coctx_make( &co->ctx,(coctx_pfn_t)CoRoutineFunc,co,0 );
        co->cStart = 1;
    }
    env->pCallStack[ env->iCallStackSize++ ] = co;
    co_swap( lpCurrRoutine, co );
}


// walkerdu 2018-01-14                                                                              
// 用于reset超时无法重复使用的协程                                                                  
void co_reset(stCoRoutine_t * co)
{
    if(!co->cStart || co->cIsMain)
        return;

    co->cStart = 0;
    co->cEnd = 0;

    // 如果当前协程有共享栈被切出的buff，要进行释放
    if(co->save_buffer)
    {
        free(co->save_buffer);
        co->save_buffer = NULL;
        co->save_size = 0;
    }

    // 如果共享栈被当前协程占用，要释放占用标志，否则被切换，会执行save_stack_buffer()
    if(co->stack_mem->occupy_co == co)
        co->stack_mem->occupy_co = NULL;
}

void co_yield_env( stCoRoutineEnv_t *env )
{
    
    stCoRoutine_t *last = env->pCallStack[ env->iCallStackSize - 2 ];
    stCoRoutine_t *curr = env->pCallStack[ env->iCallStackSize - 1 ];

    env->iCallStackSize--;

    co_swap( curr, last);
}

void co_yield_ct()
{

    co_yield_env( co_get_curr_thread_env() );
}
void co_yield( stCoRoutine_t *co )
{
    co_yield_env( co->env );
}

void save_stack_buffer(stCoRoutine_t* occupy_co)
{
    ///copy out
    stStackMem_t* stack_mem = occupy_co->stack_mem;
    int len = stack_mem->stack_bp - occupy_co->stack_sp;

    if (occupy_co->save_buffer)
    {
        free(occupy_co->save_buffer), occupy_co->save_buffer = NULL;
    }

    occupy_co->save_buffer = (char*)malloc(len); //malloc buf;
    occupy_co->save_size = len;

    memcpy(occupy_co->save_buffer, occupy_co->stack_sp, len);
}

void co_swap(stCoRoutine_t* curr, stCoRoutine_t* pending_co)
{
     stCoRoutineEnv_t* env = co_get_curr_thread_env();

    //get curr stack sp
    char c;
    curr->stack_sp= &c;

    if (!pending_co->cIsShareStack)
    {
        env->pending_co = NULL;
        env->occupy_co = NULL;
    }
    else 
    {
        env->pending_co = pending_co;
        //get last occupy co on the same stack mem
        stCoRoutine_t* occupy_co = pending_co->stack_mem->occupy_co;
        //set pending co to occupy thest stack mem;
        pending_co->stack_mem->occupy_co = pending_co;

        env->occupy_co = occupy_co;
        if (occupy_co && occupy_co != pending_co)
        {
            save_stack_buffer(occupy_co);
        }
    }

    //swap context
    coctx_swap(&(curr->ctx),&(pending_co->ctx) );

    //stack buffer may be overwrite, so get again;
    stCoRoutineEnv_t* curr_env = co_get_curr_thread_env();
    stCoRoutine_t* update_occupy_co =  curr_env->occupy_co;
    stCoRoutine_t* update_pending_co = curr_env->pending_co;
    
    if (update_occupy_co && update_pending_co && update_occupy_co != update_pending_co)
    {
        //resume stack buffer
        if (update_pending_co->save_buffer && update_pending_co->save_size > 0)
        {
            memcpy(update_pending_co->stack_sp, update_pending_co->save_buffer, update_pending_co->save_size);
        }
    }
}



//int poll(struct pollfd fds[], nfds_t nfds, int timeout);
// { fd,events,revents }
typedef struct stPollItem_t stPollItem_t;
typedef struct stPoll_t
{
    ST_TIMEOUT_ITEM_COMMON;

    struct pollfd *fds;
    nfds_t nfds; // typedef unsigned long int nfds_t;

    stPollItem_t *pPollItems;

    int iAllEventDetach;

    int iEpollFd;

    int iRaiseCnt;
}stPoll_t;

typedef struct stPollItem_t
{
    ST_TIMEOUT_ITEM_COMMON;

    struct pollfd *pSelf;
    stPoll_t *pPoll;

    struct epoll_event stEvent;
} stPollItem_t;
/*
 *   EPOLLPRI         POLLPRI    // There is urgent data to read.  
 *   EPOLLMSG         POLLMSG
 *
 *                   POLLREMOVE
 *                   POLLRDHUP
 *                   POLLNVAL
 *
 * */
static uint32_t PollEvent2Epoll( short events )
{
    uint32_t e = 0;    
    if( events & POLLIN )     e |= EPOLLIN;
    if( events & POLLOUT )  e |= EPOLLOUT;
    if( events & POLLHUP )     e |= EPOLLHUP;
    if( events & POLLERR )    e |= EPOLLERR;
    if( events & POLLRDNORM ) e |= EPOLLRDNORM;
    if( events & POLLWRNORM ) e |= EPOLLWRNORM;
    return e;
}
static short EpollEvent2Poll( uint32_t events )
{
    short e = 0;    
    if( events & EPOLLIN )     e |= POLLIN;
    if( events & EPOLLOUT ) e |= POLLOUT;
    if( events & EPOLLHUP ) e |= POLLHUP;
    if( events & EPOLLERR ) e |= POLLERR;
    if( events & EPOLLRDNORM ) e |= POLLRDNORM;
    if( events & EPOLLWRNORM ) e |= POLLWRNORM;
    return e;
}

static __thread stCoRoutineEnv_t* gCoEnvPerThread = NULL;

void co_init_curr_thread_env()
{
    gCoEnvPerThread = (stCoRoutineEnv_t*)calloc( 1, sizeof(stCoRoutineEnv_t) );
    stCoRoutineEnv_t *env = gCoEnvPerThread;

    env->iCallStackSize = 0;
    struct stCoRoutine_t *self = co_create_env( env, NULL, NULL,NULL );
    self->cIsMain = 1;

    env->pending_co = NULL;
    env->occupy_co = NULL;

    coctx_init( &self->ctx );

    env->pCallStack[ env->iCallStackSize++ ] = self;

    stCoEpoll_t *ev = AllocEpoll();
    SetEpoll( env,ev );
}
stCoRoutineEnv_t *co_get_curr_thread_env()
{
    return gCoEnvPerThread;
}

void OnPollProcessEvent( stTimeoutItem_t * ap )
{
    stCoRoutine_t *co = (stCoRoutine_t*)ap->pArg;
    //CO_LOG_INFO("OnPollProcessEvent,co %p ", co);
    co_resume( co );
}

void OnPollPreparePfn( struct epoll_event *e, stTimeoutItemLink_t *active )
{
    stPollItem_t *lp = (stPollItem_t *)e->data.ptr;
    lp->pSelf->revents = EpollEvent2Poll( e->events );

    stPoll_t *pPoll = lp->pPoll;
    pPoll->iRaiseCnt++;

    if( !pPoll->iAllEventDetach )
    {
        pPoll->iAllEventDetach = 1;

        dq_rem(&pPoll->entry, pPoll->pLink);
        pPoll->pLink = active;
        dq_addlast(&pPoll->entry, pPoll->pLink);
    }
}

void co_eventloop(stCoEpoll_t *ctx, pfn_co_eventloop_t pfn, void *arg)
{
    if (!ctx->result) {
        ctx->result = co_epoll_res_alloc(_EPOLL_SIZE);
    }
    co_epoll_res *result = ctx->result;

    for(;;)
    {
        int ret = co_epoll_wait(ctx->iEpollFd, result, _EPOLL_SIZE, 1);

        stTimeoutItemLink_t *active = (ctx->pstActiveList);
        stTimeoutItemLink_t *timeout = (ctx->pstTimeoutList);

        memset( timeout, 0, sizeof(stTimeoutItemLink_t) );

        for (int i = 0; i < ret; i++)
        {
            stTimeoutItem_t *item = (stTimeoutItem_t*)result->events[i].data.ptr;
            if( item->pfnPrepare )
            {
                item->pfnPrepare( &result->events[i], active );
            }
            else
            {
                item->pLink = active;
                dq_addlast(&item->entry, active);
            }
        }

        unsigned long long now = GetTickMS();
        TakeAllTimeout(ctx->pTimeout, now, timeout);

        stTimeoutItem_t *lp = (stTimeoutItem_t *)timeout->head;
        while( lp ) {
            lp->bTimeout = 1;
            lp = (stTimeoutItem_t *)lp->entry.flink;
        }

        dq_entry_t *e = NULL;
        int old_tmout_cnt = dq_count(timeout);
        if (0 && old_tmout_cnt) {
            stTimeoutItem_t * ti = (stTimeoutItem_t *)timeout->head;
            CO_LOG_INFO("    ++timeout-cnt:%d active-cnt:%d expireTime-now=%llu-%llu=%lld arg:%p\n",
                dq_count(timeout), dq_count(active),
                ti->ullExpireTime, now, (int64_t)(ti->ullExpireTime - now),
                ti->pArg);
        }
        dq_cat(timeout, active);
        if (dq_count(active) > 1) {
            CO_LOG_INFO("    --timeout-cnt:%d active-cnt:%d\n", old_tmout_cnt, dq_count(active));
            dq_for_every(active, e) {
                lp = (stTimeoutItem_t *)e;
                CO_LOG_INFO("    ----active arg:%p\n", lp->pArg);
            }
        }

        for (; lp = (stTimeoutItem_t *)dq_remfirst(active);)
        {
            if (lp->bTimeout && now < lp->ullExpireTime) 
            {
                CO_LOG_INFO("CO_ERR: find timeout timer, but not expire. now %lld expire %lld\n",
                    now, lp->ullExpireTime);
                int ret = AddTimeout(ctx->pTimeout, lp, now);
                if (!ret)
                {
                    CO_LOG_INFO("CO_ERR: AddTimeout failed. ");
                    lp->bTimeout = 0;
                    lp = (stTimeoutItem_t *)active->head;
                    continue;
                }
            }
            if( lp->pfnProcess )
            {
                lp->pfnProcess( lp );
            }
        }
        if( pfn )
        {
            if( -1 == pfn( arg ) )
            {
                break;
            }
        }

    }
}
void OnCoroutineEvent( stTimeoutItem_t * ap )
{
    stCoRoutine_t *co = (stCoRoutine_t*)ap->pArg;
    co_resume( co );
}


stCoEpoll_t *AllocEpoll()
{
    stCoEpoll_t *ctx = (stCoEpoll_t*)calloc( 1,sizeof(stCoEpoll_t) );

    ctx->iEpollFd = co_epoll_create( _EPOLL_SIZE );
    // alloc 60s array for timeout-queue
    // every 1ms one pointer
    ctx->pTimeout = AllocTimeout( 60 * 1000 );
    
    ctx->pstActiveList = (stTimeoutItemLink_t*)calloc( 1,sizeof(stTimeoutItemLink_t) );
    ctx->pstTimeoutList = (stTimeoutItemLink_t*)calloc( 1,sizeof(stTimeoutItemLink_t) );


    return ctx;
}

void FreeEpoll( stCoEpoll_t *ctx )
{
    if( ctx )
    {
        free( ctx->pstActiveList );
        free( ctx->pstTimeoutList );
        FreeTimeout( ctx->pTimeout );
        co_epoll_res_free( ctx->result );
    }
    free( ctx );
}

stCoRoutine_t *GetCurrCo( stCoRoutineEnv_t *env )
{
    return env->pCallStack[ env->iCallStackSize - 1 ];
}
stCoRoutine_t *GetCurrThreadCo( )
{
    stCoRoutineEnv_t *env = co_get_curr_thread_env();
    if( !env ) return 0;
    return GetCurrCo(env);
}



typedef int (*poll_pfn_t)(struct pollfd fds[], nfds_t nfds, int timeout);
int co_poll_inner( stCoEpoll_t *ctx,struct pollfd fds[], nfds_t nfds, int timeout, poll_pfn_t pollfunc)
{
    if (timeout == 0)
    {
        return pollfunc(fds, nfds, timeout);
    }
    if (timeout < 0)
    {
        timeout = INT_MAX;
    }
    int epfd = ctx->iEpollFd;
    stCoRoutine_t* self = co_self();

    //1.struct change
    stPoll_t* arg = (stPoll_t*)malloc(sizeof(stPoll_t));
    memset(arg, 0, sizeof(*arg));

    arg->iEpollFd = epfd;
    arg->fds = (struct pollfd*)calloc(nfds, sizeof(struct pollfd));
    arg->nfds = nfds;

    stPollItem_t arr[2];
    if( nfds < ARRAY_SIZE(arr) && !self->cIsShareStack)
    {
        arg->pPollItems = arr;
    }    
    else
    {
        arg->pPollItems = (stPollItem_t*)malloc( nfds * sizeof( stPollItem_t ) );
    }
    memset( arg->pPollItems, 0, nfds * sizeof(stPollItem_t) );

    arg->pfnProcess = OnPollProcessEvent;
    arg->pArg = GetCurrCo( co_get_curr_thread_env() );

    //2. add epoll
    for (nfds_t i = 0; i < nfds; i++)
    {
        arg->pPollItems[i].pSelf = arg->fds + i;
        arg->pPollItems[i].pPoll = arg;

        arg->pPollItems[i].pfnPrepare = OnPollPreparePfn;
        struct epoll_event *ev = &arg->pPollItems[i].stEvent;

        if( fds[i].fd > -1 )
        {
            ev->data.ptr = arg->pPollItems + i;
            ev->events = PollEvent2Epoll( fds[i].events );

            int ret = co_epoll_ctl( epfd,EPOLL_CTL_ADD, fds[i].fd, ev );
            if (ret < 0 && errno == EPERM && nfds == 1 && pollfunc != NULL)
            {
                CO_LOG_INFO("##### epoll add fail,fd %d events 0x%X", fds[i].fd, ev->events);

                if( arg->pPollItems != arr )
                {
                    free( arg->pPollItems );
                    arg->pPollItems = NULL;
                }
                free(arg->fds);
                free(arg);
                return pollfunc(fds, nfds, timeout);
            }
        }
        //if fail,the timeout would work
    }

    //3.add timeout

    unsigned long long now = GetTickMS();
    arg->ullExpireTime = now + timeout;
    int ret = AddTimeout(ctx->pTimeout, (stTimeoutItem_t *)arg, now);
    int iRaiseCnt = 0;
    if( ret != 0 )
    {
        CO_LOG_INFO("CO_ERR: AddTimeout ret %d now %lld timeout %d arg.ullExpireTime %lld",
                    ret, now, timeout, arg->ullExpireTime);
        errno = EINVAL;
        iRaiseCnt = -1;

    }
    else
    {
        co_yield_env( co_get_curr_thread_env() );
        iRaiseCnt = arg->iRaiseCnt;
    }

    {
        // FIXME: 源代码是要dq_rem，但是会导致主loop中的timeout链表被破坏
        // 后期需要研究是否是源码本身的问题
        // clear epoll status and memory
        // dq_rem(&arg->entry, arg->pLink);
        for(nfds_t i = 0;i < nfds;i++)
        {
            int fd = fds[i].fd;
            if( fd > -1 )
            {
                co_epoll_ctl( epfd,EPOLL_CTL_DEL,fd,&arg->pPollItems[i].stEvent );
            }
            fds[i].revents = arg->fds[i].revents;
        }


        if( arg->pPollItems != arr )
        {
            free( arg->pPollItems );
            arg->pPollItems = NULL;
        }

        free(arg->fds);
        free(arg);
    }

    return iRaiseCnt;
}

int    co_poll( stCoEpoll_t *ctx,struct pollfd fds[], nfds_t nfds, int timeout_ms )
{
    return co_poll_inner(ctx, fds, nfds, timeout_ms, NULL);
}

void SetEpoll( stCoRoutineEnv_t *env,stCoEpoll_t *ev )
{
    env->pEpoll = ev;
}
stCoEpoll_t *co_get_epoll_ct()
{
    if( !co_get_curr_thread_env() )
    {
        co_init_curr_thread_env();
    }
    return co_get_curr_thread_env()->pEpoll;
}
struct stHookPThreadSpec_t
{
    stCoRoutine_t *co;
    void *value;
};
void *co_getspecific(pthread_key_t key)
{
    stCoRoutine_t *co = GetCurrThreadCo();
    if( !co || co->cIsMain )
    {
        return pthread_getspecific( key );
    }
    return co->aSpec[ key ].value;
}
int co_setspecific(pthread_key_t key, const void *value)
{
    stCoRoutine_t *co = GetCurrThreadCo();
    if( !co || co->cIsMain )
    {
        return pthread_setspecific( key,value );
    }
    co->aSpec[ key ].value = (void*)value;
    return 0;
}



void co_disable_hook_sys()
{
    stCoRoutine_t *co = GetCurrThreadCo();
    if( co )
    {
        co->cEnableSysHook = 0;
    }
}
void sys_hook_call_init();

int co_is_enable_sys_hook()
{
    stCoRoutine_t *co = GetCurrThreadCo();
    int ret = ( co && co->cEnableSysHook );
    if (ret)
        sys_hook_call_init();
    return ret;
}

stCoRoutine_t *co_self()
{
    return GetCurrThreadCo();
}

typedef struct stCoCondItem_t 
{
    struct dq_entry_s entry;
    stCoCond_t *pLink;
    stTimeoutItem_t timeout;
} stCoCondItem_t;

static void OnSignalProcessEvent( stTimeoutItem_t * ap )
{
    stCoRoutine_t *co = (stCoRoutine_t*)ap->pArg;
    co_resume( co );
}

stCoCondItem_t *co_cond_pop( stCoCond_t *link );
int co_cond_signal( stCoCond_t *list )
{
    stCoCondItem_t *sp = (stCoCondItem_t *)dq_remlast(list);
    if (!sp) {
        return 0;
    }

    if (sp->timeout.pLink)
        dq_rem(&sp->timeout.entry, sp->timeout.pLink);

    stCoRoutineEnv_t *env = co_get_curr_thread_env();
    sp->pLink = env->pEpoll->pstActiveList;
    dq_addlast(&sp->timeout.entry, sp->pLink);

    return 0;
}
int co_cond_broadcast( stCoCond_t *list )
{
    for (;;) {
        stCoCondItem_t *sp = (stCoCondItem_t *)dq_remlast(list);
        if (!sp)
            return 0;

        dq_rem(&sp->timeout.entry, sp->timeout.pLink);

        sp->pLink = co_get_curr_thread_env()->pEpoll->pstActiveList;
        dq_addlast(&sp->timeout.entry, sp->pLink);
    }

    return 0;
}

int co_cond_timedwait( dq_queue_t *link, int ms )
{
    stCoCondItem_t* psi = (stCoCondItem_t*)calloc(1, sizeof(stCoCondItem_t));
    psi->timeout.pArg = GetCurrThreadCo();
    psi->timeout.pfnProcess = OnSignalProcessEvent;

    if( ms > 0 )
    {
        unsigned long long now = GetTickMS();
        psi->timeout.ullExpireTime = now + ms;

        int ret = AddTimeout(co_get_curr_thread_env()->pEpoll->pTimeout, &psi->timeout, now);
        if( ret != 0 )
        {
            free(psi);
            return ret;
        }
    }

    psi->pLink = link;
    dq_addlast(&psi->entry, psi->pLink);

    co_yield_ct();

    dq_rem(&psi->entry, link);
    free(psi);

    return 0;
}
stCoCond_t *co_cond_alloc()
{
    return (stCoCond_t*)calloc( 1,sizeof(stCoCond_t) );
}
int co_cond_free( stCoCond_t * cc )
{
    free( cc );
    return 0;
}


stCoCondItem_t *co_cond_pop( stCoCond_t *link )
{
    return (stCoCondItem_t *)dq_remlast(link);
}
