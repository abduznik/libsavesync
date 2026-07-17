CC ?= gcc
AR ?= ar
CFLAGS ?= -Wall -Wextra -pedantic -std=gnu99
OPTFLAGS ?= -O2
DEBUGFLAGS ?= -g -O0 -DDEBUG

INCDIR = include
SRCDIR = src
TESTDIR = test
BUILDDIR = build

LIB_SRC = $(SRCDIR)/savesync.c
LIB_OBJ = $(BUILDDIR)/savesync.o
LIB_STATIC = $(BUILDDIR)/libsavesync.a
LIB_SHARED = $(BUILDDIR)/libsavesync.so

IPC_SRC = $(SRCDIR)/ipc/main.c
IPC_BIN = $(BUILDDIR)/savesync-ipc

TEST_BASIC_SRC = $(TESTDIR)/test_basic.c
TEST_BASIC_BIN = $(BUILDDIR)/test_basic
TEST_STRATEGY_SRC = $(TESTDIR)/test_strategy.c
TEST_STRATEGY_BIN = $(BUILDDIR)/test_strategy
TEST_REGR_SRC = $(TESTDIR)/test_regressions.c
TEST_REGR_BIN = $(BUILDDIR)/test_regressions
TEST_P3_REGR_SRC = $(TESTDIR)/test_phase3_regressions.c
TEST_P3_REGR_BIN = $(BUILDDIR)/test_phase3_regressions
TEST_REAL_SRC = $(TESTDIR)/test_real_manifests.c
TEST_REAL_BIN = $(BUILDDIR)/test_real_manifests
TEST_RH_SRC = $(TESTDIR)/test_rom_header_identity.c
TEST_RH_BIN = $(BUILDDIR)/test_rom_header_identity
TEST_IPC_SRC = $(TESTDIR)/test_ipc.c
TEST_IPC_BIN = $(BUILDDIR)/test_ipc

CFLAGS += -I$(INCDIR)

.PHONY: all static shared ipc test clean

all: $(BUILDDIR) static ipc test

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

static: $(LIB_STATIC)

shared: CFLAGS += -fPIC
shared: $(LIB_SHARED)

$(LIB_OBJ): $(LIB_SRC) $(INCDIR)/savesync.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(OPTFLAGS) -c $(LIB_SRC) -o $(LIB_OBJ)

$(LIB_STATIC): $(LIB_OBJ)
	$(AR) rcs $(LIB_STATIC) $(LIB_OBJ)

$(LIB_SHARED): CFLAGS += -fPIC
$(LIB_SHARED): $(LIB_SRC) $(INCDIR)/savesync.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(OPTFLAGS) -shared $(LIB_SRC) -o $(LIB_SHARED)

ipc: $(IPC_BIN)

$(IPC_BIN): $(IPC_SRC) $(LIB_SRC) $(INCDIR)/savesync.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(OPTFLAGS) $(LIB_SRC) $(IPC_SRC) -o $(IPC_BIN)

$(TEST_BASIC_BIN): $(TEST_BASIC_SRC) $(LIB_SRC) $(INCDIR)/savesync.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(DEBUGFLAGS) $(LIB_SRC) $(TEST_BASIC_SRC) -o $(TEST_BASIC_BIN)

$(TEST_STRATEGY_BIN): $(TEST_STRATEGY_SRC) $(LIB_SRC) $(INCDIR)/savesync.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(DEBUGFLAGS) $(LIB_SRC) $(TEST_STRATEGY_SRC) -o $(TEST_STRATEGY_BIN)

$(TEST_REGR_BIN): $(TEST_REGR_SRC) $(LIB_SRC) $(INCDIR)/savesync.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(DEBUGFLAGS) $(LIB_SRC) $(TEST_REGR_SRC) -o $(TEST_REGR_BIN)

$(TEST_P3_REGR_BIN): $(TEST_P3_REGR_SRC) $(LIB_SRC) $(INCDIR)/savesync.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(DEBUGFLAGS) $(LIB_SRC) $(TEST_P3_REGR_SRC) -o $(TEST_P3_REGR_BIN)

$(TEST_REAL_BIN): $(TEST_REAL_SRC) $(LIB_SRC) $(INCDIR)/savesync.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(DEBUGFLAGS) $(LIB_SRC) $(TEST_REAL_SRC) -o $(TEST_REAL_BIN)

$(TEST_RH_BIN): $(TEST_RH_SRC) $(LIB_SRC) $(INCDIR)/savesync.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(DEBUGFLAGS) $(LIB_SRC) $(TEST_RH_SRC) -o $(TEST_RH_BIN)

$(TEST_IPC_BIN): $(TEST_IPC_SRC) $(IPC_BIN) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(DEBUGFLAGS) $(TEST_IPC_SRC) -o $(TEST_IPC_BIN)

test: $(TEST_BASIC_BIN) $(TEST_STRATEGY_BIN) $(TEST_REGR_BIN) $(TEST_P3_REGR_BIN) $(TEST_REAL_BIN) $(TEST_RH_BIN) $(TEST_IPC_BIN)
	./$(TEST_BASIC_BIN)
	./$(TEST_STRATEGY_BIN)
	./$(TEST_REGR_BIN)
	./$(TEST_P3_REGR_BIN)
	./$(TEST_REAL_BIN)
	./$(TEST_RH_BIN)
	./$(TEST_IPC_BIN)

clean:
	rm -rf $(BUILDDIR)
