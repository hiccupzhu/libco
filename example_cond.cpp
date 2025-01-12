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

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include "queue.h"
#include "co_routine.h"

struct stTask_t {
    dq_entry_s entry;
    int id;
};
struct stEnv_t
{
    stCoCond_t* cond;
    dq_queue_t task_queue;
};
void* Producer(void* args)
{
    co_enable_hook_sys();
    stEnv_t* env=  (stEnv_t*)args;
    int id = 0;
    while (true)
    {
        stTask_t* task = (stTask_t*)calloc(1, sizeof(stTask_t));
        task->id = id++;
        dq_addlast(&task->entry, &env->task_queue);
        printf("%s:%d produce task %d\n", __func__, __LINE__, task->id);
        co_cond_signal(env->cond);
        poll(NULL, 0, 1000);
    }
    return NULL;
}
void* Consumer(void* args)
{
    co_enable_hook_sys();
    stEnv_t* env = (stEnv_t*)args;
    while (true)
    {
        if (dq_empty(&env->task_queue))
        {
            co_cond_timedwait(env->cond, -1);
            continue;
        }
        stTask_t* task = (stTask_t*)dq_remfirst(&env->task_queue);
        printf("%s:%d consume task %d\n", __func__, __LINE__, task->id);
        free(task);
    }
    return NULL;
}
int main()
{
    stEnv_t* env = (stEnv_t *)calloc(1, sizeof(stEnv_t));
    dq_init(&env->task_queue);
    
    env->cond = co_cond_alloc();

    stCoRoutine_t* consumer_routine;
    co_create(&consumer_routine, NULL, Consumer, env);
    co_resume(consumer_routine);

    stCoRoutine_t* producer_routine;
    co_create(&producer_routine, NULL, Producer, env);
    co_resume(producer_routine);
    
    co_eventloop(co_get_epoll_ct(), NULL, NULL);
    return 0;
}
