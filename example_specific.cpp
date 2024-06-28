/*
* Tencent is pleased to support the open source community by making Libco available.

* Copyright (C) 2014 THL A29 Limited, a Tencent company. All rights reserved.
*
* Licensed under the Apache License, Version 2.0 (the "License"); 
* you may not use this file except in compliance with the License. 
* You may obtain a copy of the License at
*
*	http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, 
* software distributed under the License is distributed on an "AS IS" BASIS, 
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. 
* See the License for the specific language governing permissions and 
* limitations under the License.
*/

#include "co_routine_specific.h"
#include "co_routine.h"
#include <unistd.h>
#include <stdio.h>
#include <vector>
#include <iostream>
using namespace std;
struct stRoutineArgs_t
{
	stCoRoutine_t* co;
	int routine_id;
};
struct stRoutineSpecificData_t
{
	int idx;
};

//CO_ROUTINE_SPECIFIC(stRoutineSpecificData_t, __routine);
static pthread_once_t _routine_once_stRoutineSpecificData_t = PTHREAD_ONCE_INIT;
static pthread_key_t _routine_key_stRoutineSpecificData_t;
static int _routine_init_stRoutineSpecificData_t = 0;
static void _routine_make_key_stRoutineSpecificData_t()
 { 
	(void)pthread_key_create(&_routine_key_stRoutineSpecificData_t, __null);
 }

template <class T>
class clsRoutineData_routine_stRoutineSpecificData_t {
public:
    inline T *operator->()
    {
        if (!_routine_init_stRoutineSpecificData_t) {
            pthread_once(&_routine_once_stRoutineSpecificData_t, _routine_make_key_stRoutineSpecificData_t);
            _routine_init_stRoutineSpecificData_t = 1;
        }
        T *p = (T *)co_getspecific(_routine_key_stRoutineSpecificData_t);
        if (!p) {
            p = (T *)calloc(1, sizeof(T));
            int ret = co_setspecific(_routine_key_stRoutineSpecificData_t, p);
            if (ret) {
                if (p) {
                    free(p);
                    p = __null;
                }
            }
        }
        return p;
    }
};
static clsRoutineData_routine_stRoutineSpecificData_t<stRoutineSpecificData_t> __routine;

void* RoutineFunc(void* args)
{
	co_enable_hook_sys();
	stRoutineArgs_t* routine_args = (stRoutineArgs_t*)args;
	__routine->idx = routine_args->routine_id;
	while (true)
	{
		printf("%s:%d routine specific data idx %d\n", __func__, __LINE__, __routine->idx);
		poll(NULL, 0, 1000);
	}
	return NULL;
}
int main()
{
	stRoutineArgs_t args[10];
	for (int i = 0; i < 10; i++)
	{
		args[i].routine_id = i;
		co_create(&args[i].co, NULL, RoutineFunc, (void*)&args[i]);
		co_resume(args[i].co);
	}
	co_eventloop(co_get_epoll_ct(), NULL, NULL);
	return 0;
}
