# ipc-filter-farm — 하드 제약: C11, libc + pthread + rt, 경고 0
CC     ?= cc
CFLAGS := -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -O2
LDLIBS := -lpthread -lrt
KEY    ?= /tmp/ipcff.key
IPC    ?= fifo

COMMON := src/common/proto.c src/common/bqueue.c src/common/ppm.c src/common/filter.c src/common/timer.c src/common/resource.c
COMMON_HEADERS := $(COMMON:.c=.h)
APP_COMMON := src/common/app.c src/common/app.h $(COMMON) $(COMMON_HEADERS)
IPC_HEADERS := $(wildcard src/ipc/*.h)
IPCSRC := src/ipc/ipc.c src/ipc/ipc_meta.c src/ipc/ipc_pipe.c src/ipc/ipc_fifo.c src/ipc/ipc_sysv_mq.c src/ipc/mq_lifecycle.c src/ipc/ipc_owned.c src/ipc/ipc_sysv_shm.c src/ipc/shm_ring.c src/ipc/ipc_posix_shm.c src/ipc/pshm_owned.c
IPC_TESTS := tests/test_ipc_loopback tests/test_fifo_faults tests/test_app_protocol tests/test_client_protocol tests/test_mq tests/test_shm tests/test_posix tests/test_team tests/test_pipe_protocol
TESTS  := tests/test_proto tests/test_bqueue tests/test_ppm tests/test_filter tests/test_timer tests/test_ipc_util tests/test_app_options tests/test_basic_options \
          tests/test_images $(IPC_TESTS)

all: server client pipe_run basic_server basic_client clean_ipc basic_clean_ipc bench/aggregate bench/inspect $(TESTS)

pipe_run: src/pipe_run.c
	$(LINK.c) $< $(LOADLIBES) $(LDLIBS) -o $@

basic_server basic_client: basic_%: src/%.c $(APP_COMMON) $(IPCSRC) $(IPC_HEADERS)

basic_clean_ipc: src/clean_ipc.c src/ipc/ipc_meta.c src/ipc/ipc_owned.c src/ipc/pshm_owned.c $(IPC_HEADERS) src/common/proto.h

basic_server basic_client basic_clean_ipc:
	$(CC) $(CFLAGS) -DBASIC_BENCH=1 -DSLOT_COUNT=8 -o $@ $(filter %.c,$^) $(LDLIBS)

bench/aggregate: bench/aggregate.c
	$(CC) $(CFLAGS) -o $@ $<

bench/inspect: bench/inspect.c $(IPC_HEADERS) src/common/proto.h
	$(CC) $(CFLAGS) -o $@ $<

tests/test_ipc_util: tests/test_ipc_util.c $(IPC_HEADERS) src/common/proto.h

tests/test_basic_options: CFLAGS += -DBASIC_BENCH=1 -DSLOT_COUNT=8
tests/test_app_options tests/test_basic_options: tests/test_app_options.c src/common/app.c src/ipc/ipc.c src/ipc/ipc_fifo.c src/ipc/ipc_meta.c src/common/app.h $(IPC_HEADERS) src/common/proto.h
	$(CC) $(CFLAGS) -DIPC_BACKENDS=2 -o $@ $(filter %.c,$^) $(LDLIBS)

clean_ipc: src/clean_ipc.c src/ipc/ipc_meta.c src/ipc/ipc_owned.c src/ipc/pshm_owned.c $(IPC_HEADERS) src/common/proto.h

server client: %: src/%.c $(APP_COMMON) $(IPCSRC) $(IPC_HEADERS)

$(IPC_TESTS): %: %.c $(COMMON) $(COMMON_HEADERS) $(IPCSRC) $(IPC_HEADERS)

tests/test_posix: LDLIBS += -Wl,--wrap=sem_destroy -Wl,--wrap=sem_init -Wl,--wrap=posix_fallocate
tests/test_team: src/common/app.c src/common/app.h

server client clean_ipc tests/test_ipc_util $(IPC_TESTS):
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^) $(LDLIBS)

tests/%: tests/%.c $(COMMON) $(COMMON_HEADERS)
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^) $(LDLIBS)

test: $(TESTS)
	./tests/test_proto
	./tests/test_bqueue
	./tests/test_ppm
	./tests/test_filter
	./tests/test_timer
	./tests/test_ipc_util
	./tests/test_app_options
	./tests/test_basic_options
	timeout 20s ./tests/test_team
	./tests/test_ipc_loopback fifo
	timeout 30s ./tests/test_ipc_loopback sysv_mq
	timeout 30s ./tests/test_ipc_loopback sysv_shm
	timeout 30s ./tests/test_ipc_loopback posix_shm
	timeout 15s ./tests/test_fifo_faults
	timeout 20s ./tests/test_pipe_protocol

test-app: server client tests/test_images tests/test_app_protocol tests/test_client_protocol
	timeout 20s ./tests/test_client_protocol $(IPC)
	timeout 600s bash tests/test_app.sh $(IPC)

test-mq: tests/test_mq
	timeout 90s ./tests/test_mq

test-team: tests/test_team
	timeout 20s ./tests/test_team

test-shm: tests/test_shm tests/test_posix server client clean_ipc
	timeout 30s ./tests/test_shm sysv_shm
	timeout 30s ./tests/test_shm posix_shm
	timeout 30s ./tests/test_posix
	timeout 60s bash tests/test_clean.sh

# 계약 헤더가 실제로 컴파일되는지. 구현이 없어도 계약이 깨지면 여기서 걸린다.
check-headers:
	$(CC) $(CFLAGS) -fsyntax-only src/common/*.h src/ipc/*.h

bench: all
	bash bench/run_bench.sh $(PHASE)

test-bench: all
	timeout 120s bash tests/test_bench.sh

test-pipe: all
	timeout 20s ./tests/test_pipe_protocol
	timeout 600s bash tests/test_pipe.sh

package:
	bash bench/package.sh

package-draft:
	bash bench/package.sh --draft

# 각 버전은 해당 IPC 소스만 링크한다. 전체 빌드는 위 all에서 제공한다.
PIPE_SRC := src/ipc/ipc_pipe.c
MQ_SRC := src/ipc/ipc_sysv_mq.c src/ipc/mq_lifecycle.c src/ipc/ipc_owned.c
SHM_SRC := src/ipc/ipc_sysv_shm.c src/ipc/shm_ring.c src/ipc/ipc_owned.c
define version_targets
phase1-$(1): build/phase1/$(1)/basic_server build/phase1/$(1)/basic_client build/phase1/$(1)/pipe_run
phase2-$(1): build/phase2/$(1)/server build/phase2/$(1)/client build/phase2/$(1)/pipe_run
build/phase1/$(1)/basic_server build/phase1/$(1)/basic_client: build/phase1/$(1)/basic_%: src/%.c $(APP_COMMON) src/ipc/ipc.c src/ipc/ipc_meta.c $(2) $(IPC_HEADERS)
	@mkdir -p $$(@D)
	$$(CC) $$(CFLAGS) -DBASIC_BENCH=1 -DSLOT_COUNT=8 -DIPC_BACKENDS=$(3) -o $$@ $$(filter %.c,$$^) $$(LDLIBS)
build/phase2/$(1)/server build/phase2/$(1)/client: build/phase2/$(1)/%: src/%.c $(APP_COMMON) src/ipc/ipc.c src/ipc/ipc_meta.c $(2) $(IPC_HEADERS)
	@mkdir -p $$(@D)
	$$(CC) $$(CFLAGS) -DIPC_BACKENDS=$(3) -o $$@ $$(filter %.c,$$^) $$(LDLIBS)
build/phase1/$(1)/pipe_run build/phase2/$(1)/pipe_run: src/pipe_run.c
	@mkdir -p $$(@D)
	$$(CC) $$(CFLAGS) -o $$@ $$<
.PHONY: phase1-$(1) phase2-$(1)
endef
$(eval $(call version_targets,pipe,$(PIPE_SRC),1))
$(eval $(call version_targets,sysv_mq,$(MQ_SRC),4))
$(eval $(call version_targets,sysv_shm,$(SHM_SRC),8))

clean:
	rm -f server client pipe_run basic_server basic_client basic_clean_ipc clean_ipc bench/aggregate bench/inspect $(TESTS) src/common/*.o src/ipc/*.o src/*.o *.o
	rm -f build/phase1/*/basic_server build/phase1/*/basic_client build/phase1/*/pipe_run build/phase2/*/server build/phase2/*/client build/phase2/*/pipe_run

# 서버·클라 잠금 소실 및 소유 기록 확인 후 해당 자원만 회수한다.
clean-ipc: clean_ipc
	./clean_ipc "$(KEY)"

.PHONY: all test test-app test-mq test-team test-shm test-pipe test-bench bench package package-draft clean clean-ipc check-headers
