#
# Tencent is pleased to support the open source community by making Libco available.
# 
# Copyright (C) 2014 THL A29 Limited, a Tencent company. All rights reserved.
# 
# Licensed under the Apache License, Version 2.0 (the "License"); 
# you may not use this file except in compliance with the License. 
# You may obtain a copy of the License at
# 
#   http://www.apache.org/licenses/LICENSE-2.0
# 
# Unless required by applicable law or agreed to in writing, 
# software distributed under the License is distributed on an "AS IS" BASIS, 
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. 
# See the License for the specific language governing permissions and 
# limitations under the License.
#


COMM_MAKE = 1
COMM_ECHO = 1
version=0.5
v=debug


##### Makefile Rules ##########
MAIL_ROOT=.
SRCROOT=.

##define the compliers
CPP = $(CXX)
AR = ar -rc
RANLIB = ranlib

CPPSHARE = $(CPP) -fPIC -shared -O2 -pipe -L$(SRCROOT)/solib/ -o 
CSHARE = $(CC) -fPIC -shared -O2 -pipe -L$(SRCROOT)/solib/ -o 

ifeq ($v,release)
CFLAGS= -O2 $(INCLS) -fPIC  -DLINUX -pipe -Wno-deprecated -c
else
CFLAGS= -g $(INCLS) -fPIC -DLINUX -pipe -c -fno-inline
endif

ifneq ($v,release)
BFLAGS= -g
endif

STATICLIBPATH=$(SRCROOT)/lib
DYNAMICLIBPATH=$(SRCROOT)/solib

INCLS += -I$(SRCROOT)

## default links
ifeq ($(LINKS_DYNAMIC), 1)
LINKS += -L$(DYNAMICLIBPATH) -L$(STATICLIBPATH)
else
LINKS += -L$(STATICLIBPATH)
endif

#CPPSRCS  = $(wildcard *.cpp)
#CSRCS  = $(wildcard *.c)
#CPPOBJS  = $(patsubst %.cpp,%.o,$(CPPSRCS))
#COBJS  = $(patsubst %.c,%.o,$(CSRCS))
#
#SRCS = $(CPPSRCS) $(CSRCS)
#OBJS = $(CPPOBJS) $(COBJS)

CPPCOMPI=$(CPP) $(CFLAGS) -Wno-deprecated
CCCOMPI=$(CC) $(CFLAGS)

BUILDEXE = $(CPP) $(BFLAGS) -o $@ $^ $(LINKS) 
CLEAN = rm -f *.o 

CPPCOMPILE = $(CPPCOMPI) $< $(FLAGS) $(INCLS) $(MTOOL_INCL) -o $@
CCCOMPILE = $(CCCOMPI) $< $(FLAGS) $(INCLS) $(MTOOL_INCL) -o $@

ARSTATICLIB = $(AR) $@.tmp $^ $(AR_FLAGS); \
			  if [ $$? -ne 0 ]; then exit 1; fi; \
			  test -d $(STATICLIBPATH) || mkdir -p $(STATICLIBPATH); \
			  mv -f $@.tmp $(STATICLIBPATH)/$@;

BUILDSHARELIB = $(CPPSHARE) $@.tmp $^ $(BS_FLAGS); \
				if [ $$? -ne 0 ]; then exit 1; fi; \
				test -d $(DYNAMICLIBPATH) || mkdir -p $(DYNAMICLIBPATH); \
				mv -f $@.tmp $(DYNAMICLIBPATH)/$@;

########## options ##########
CFLAGS += -g -fno-strict-aliasing -O0 -Wall -export-dynamic \
	-Wall -pipe  -D_GNU_SOURCE -D_REENTRANT -fPIC -Wno-deprecated -m32

UNAME := $(shell uname -s)

ifeq ($(UNAME), FreeBSD)
LINKS += -g -L./lib -lcolib -lpthread -m32
else
LINKS += -g -L./lib -lcolib -lpthread -ldl -m32
endif

PROGS = colib example_poll example_echosvr example_echocli example_thread \
example_cond example_specific example_copystack example_closure example_setenv rbtree_test

all:$(PROGS) rbtree_test

rbtree_test:rbtree-tst.c rbtree.c rbtree.h
	gcc -g -o $@ $^

co_epoll.o: co_epoll.c co_epoll.h
	$(CCCOMPILE)

co_routine.o: co_routine.c co_routine.h co_routine_inner.h co_epoll.h
	$(CCCOMPILE)

co_hook_sys_call.o: co_hook_sys_call.c
	$(CCCOMPILE)

coctx_swap.o: coctx_swap.S
	$(CCCOMPILE)

coctx.o: coctx.c
	$(CCCOMPILE)

rbtree.o: rbtree.c rbtree.h
	$(CCCOMPILE)


example_echosvr.o:example_echosvr.cpp
	$(CPPCOMPILE)
example_echocli.o:example_echocli.cpp
	$(CPPCOMPILE)
example_thread.o:example_thread.cpp
	$(CPPCOMPILE)
example_poll.o:example_poll.cpp
	$(CPPCOMPILE)
example_exit.o:example_exit.cpp
	$(CPPCOMPILE)
example_cond.o:example_cond.cpp
	$(CPPCOMPILE)
example_specific.o:example_specific.cpp
	$(CPPCOMPILE)
example_copystack.o:example_copystack.cpp
	$(CPPCOMPILE)
example_setenv.o:example_setenv.cpp
	$(CPPCOMPILE)
example_closure.o:example_closure.cpp
	$(CPPCOMPILE)


COLIB_OBJS=co_epoll.o co_routine.o co_hook_sys_call.o coctx_swap.o coctx.o rbtree.o #co_comm.o
#co_swapcontext.o

colib:libcolib.a #libcolib.so

libcolib.a: $(COLIB_OBJS)
	$(ARSTATICLIB) 
libcolib.so: $(COLIB_OBJS)
	$(BUILDSHARELIB) 

example_echosvr:example_echosvr.o $(COLIB_OBJS)
	$(BUILDEXE) 
example_echocli:example_echocli.o $(COLIB_OBJS)
	$(BUILDEXE) 
example_thread:example_thread.o $(COLIB_OBJS)
	$(BUILDEXE) 
example_poll:example_poll.o $(COLIB_OBJS)
	$(BUILDEXE) 
example_exit:example_exit.o $(COLIB_OBJS)
	$(BUILDEXE) 
example_cond:example_cond.o $(COLIB_OBJS)
	$(BUILDEXE)
example_specific:example_specific.o $(COLIB_OBJS)
	$(BUILDEXE)
example_copystack:example_copystack.o $(COLIB_OBJS)
	$(BUILDEXE)
example_setenv:example_setenv.o $(COLIB_OBJS)
	$(BUILDEXE)
example_closure:example_closure.o $(COLIB_OBJS)
	$(BUILDEXE)

dist: clean libco-$(version).src.tar.gz

libco-$(version).src.tar.gz:
	@find . -type f | grep -v CVS | grep -v .svn | sed s:^./:libco-$(version)/: > MANIFEST
	@(cd ..; ln -s libco_pub libco-$(version))
	(cd ..; tar cvf - `cat libco_pub/MANIFEST` | gzip > libco_pub/libco-$(version).src.tar.gz)
	@(cd ..; rm libco-$(version))

clean:
	$(CLEAN) *.o $(PROGS)
	rm -fr MANIFEST lib solib libco-$(version).src.tar.gz libco-$(version)

