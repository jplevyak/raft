# Copyright 2014 Google Inc. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Configuration
DEBUG ?= 1
CXX ?= g++
AR ?= ar
PROTOC ?= protoc
OUT_DIR = .

# Platform Detection
OS_TYPE := $(shell uname -s)
ARCH := $(shell uname -m)

# Adjust ARCH for 32-bit x86 aliases
ifneq (,$(filter i386 i486 i586 i686,$(ARCH)))
  ARCH = x86
endif

# Flags
CXXFLAGS += -std=c++17 -Wall -Wextra
CXXFLAGS += -I. -I./include -I./util -I./common -I/usr/local/include -I/usr/include
CXXFLAGS += $(shell pkg-config --cflags-only-I protobuf)
CXXFLAGS += $(shell pkg-config --cflags gtest)

LIBS += $(shell pkg-config --libs protobuf)
LIBS += $(shell pkg-config --libs gtest) -lgtest -lgtest_main

ifneq ($(OS_TYPE),Darwin)
  LIBS += -lrt -lpthread
endif

ifdef OPTIMIZE
  CXXFLAGS += -O3
endif
ifdef DEBUG
  CXXFLAGS += -ggdb
endif
ifdef PROFILE
  CXXFLAGS += -O3 -g
endif

# Source Discovery
PROTOS = raft
PROTO_SOURCES = $(addsuffix .pb.cc,$(PROTOS))
PROTO_INCLUDES = $(addsuffix .pb.h,$(PROTOS))
PROTO_PYTHON = $(addsuffix _pb2.py,$(PROTOS))
TEST_SOURCES = $(shell find . -name '*_test.cc')
TESTS = $(basename $(TEST_SOURCES))
SOURCES = $(PROTO_SOURCES) $(TEST_SOURCES)

source_to_object = $(addsuffix .o,$(basename $(1)))
source_to_depend = $(addsuffix .d,$(basename $(1)))

.PHONY: all depend clean test

default: all

all: $(TESTS)

depend: $(call source_to_depend,$(SOURCES))

# Tests
# TEST_TMPDIR defaults to a random temp dir if not set by environment
TEST_TMPDIR ?= $(shell mktemp -d)

test: $(TESTS)
	@echo "Running tests in $(TEST_TMPDIR)"
	@failed=0; \
	for t in $^; do \
		echo "***** Running $$t"; \
		rm -rf $(TEST_TMPDIR)/*; \
		if ! ./$$t --test_tmpdir=$(TEST_TMPDIR); then \
			failed=1; \
		fi; \
	done; \
	rm -rf $(TEST_TMPDIR); \
	if [ $$failed -eq 1 ]; then \
		echo "Tests FAILED"; \
		exit 1; \
	else \
		echo "All tests pass!"; \
	fi

clean:
	find . -name '*.pb.cc' -delete
	find . -name '*.pb.h' -delete
	find . -name '*_pb2.py' -delete
	find . -name '*.o' -delete
	find . -name '*.pyc' -delete
	find . -name '*.d' -delete
	rm -f $(TESTS)

# Rules
%_test: raft.pb.o %_test.cc
	$(CXX) -o $@ $*_test.cc $(CXXFLAGS) raft.pb.o $(LIBS)

example_server: raft.pb.o example_server.cc
	$(CXX) -o $@ example_server.cc $(CXXFLAGS) raft.pb.o $(LIBS)

%.pb.cc %.pb.h %_pb2.py: %.proto
	$(PROTOC) $^ --cpp_out=. --python_out=.

%.o: %.cc
	$(CXX) -c $< -o $(OUT_DIR)/$@ $(CXXFLAGS)

%.d: %.cc $(PROTO_INCLUDES)
	$(CXX) -MM $< $(CXXFLAGS) > $@

ifneq ($(MAKECMDGOALS),clean)
  -include $(call source_to_depend,$(SOURCES))
endif
