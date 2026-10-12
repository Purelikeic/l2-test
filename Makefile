THREADS_BUILD 	?= 1

CMAKE_CXX_COMPILER :=
ifneq ($(origin CXX_COMPILER), undefined)
	CMAKE_CXX_COMPILER := -DCMAKE_CXX_COMPILER=$(CXX_COMPILER)
endif


init:
	git submodule update --init --recursive
	$(MAKE) -C ./dut/XSCache init

FORCE:


tltest-prepare-all:
	cmake ./main -B ./main/build -DBUILD_DPI=ON -DBUILD_V3=ON $(CMAKE_CXX_COMPILER)

tltest-prepare-dpi:
	cmake ./main -B ./main/build -DBUILD_DPI=ON -DBUILD_V3=OFF $(CMAKE_CXX_COMPILER)

tltest-prepare-v3:
	cmake ./main -B ./main/build -DBUILD_V3=ON -DBUILD_DPI=OFF $(CMAKE_CXX_COMPILER)


tltest-prepare-all-coupledL2:
	cmake ./main -B ./main/build -DBUILD_DPI=ON -DBUILD_V3=ON $(CMAKE_CXX_COMPILER) \
		-DDUT_PATH="${PWD}/dut/XSCache" -DTLTEST_MEMORY=0

tltest-prepare-dpi-coupledL2:
	cmake ./main -B ./main/build -DBUILD_DPI=ON -DBUILD_V3=OFF $(CMAKE_CXX_COMPILER) \
		-DDUT_PATH="${PWD}/dut/XSCache" -DTLTEST_MEMORY=0

tltest-prepare-v3-coupledL2:
	cmake ./main -B ./main/build -DBUILD_V3=ON -DBUILD_DPI=OFF $(CMAKE_CXX_COMPILER) \
		-DDUT_PATH="${PWD}/dut/XSCache" -DTLTEST_MEMORY=0


tltest-prepare-all-openLLC:
	cmake ./main -B ./main/build -DBUILD_DPI=ON -DBUILD_V3=ON $(CMAKE_CXX_COMPILER) \
		-DDUT_PATH="${PWD}/dut/XSCache"

tltest-prepare-dpi-openLLC:
	cmake ./main -B ./main/build -DBUILD_DPI=ON -DBUILD_V3=OFF $(CMAKE_CXX_COMPILER) \
		-DDUT_PATH="${PWD}/dut/XSCache"

tltest-prepare-v3-openLLC:
	cmake ./main -B ./main/build -DBUILD_V3=ON -DBUILD_DPI=OFF $(CMAKE_CXX_COMPILER) \
		-DDUT_PATH="${PWD}/dut/XSCache"


tltest-portgen:
	$(MAKE) -C ./main/build portgen -j$(THREADS_BUILD) -s --always-make

tltest-clean:
	$(MAKE) -C ./main/build clean -s

tltest-config-user:
	@test -d ./main/build || mkdir -p ./main/build
	@cat ./configs/user.tltest.ini
	@echo ""
	@cat ./configs/user.tltest.ini > ./main/build/tltest.ini
	@echo "" >> ./main/build/tltest.ini

tltest-config-coupledL2-test-l2l3: tltest-config-user
	@cat ./configs/coupledL2-test-l2l3.tltest.ini
	@echo ""
	@cat ./configs/coupledL2-test-l2l3.tltest.ini >> ./main/build/tltest.ini
	@echo "tltest-config-postbuild: tltest-config-coupledL2-test-l2l3" > ./main/build/Makefile.config

tltest-config-coupledL2-test-l2l3l2: tltest-config-user
	@cat ./configs/coupledL2-test-l2l3l2.tltest.ini
	@echo ""
	@cat ./configs/coupledL2-test-l2l3l2.tltest.ini >> ./main/build/tltest.ini
	@echo "tltest-config-postbuild: tltest-config-coupledL2-test-l2l3l2" > ./main/build/Makefile.config

tltest-config-openLLC-test-l2l3: tltest-config-user
	@cat ./configs/openLLC-test-l2l3.tltest.ini
	@echo ""
	@cat ./configs/openLLC-test-l2l3.tltest.ini >> ./main/build/tltest.ini
	@echo "tltest-config-postbuild: tltest-config-openLLC-test-l2l3" > ./main/build/Makefile.config

tltest-config-openLLC-test-l2l3l2: tltest-config-user
	@cat ./configs/openLLC-test-l2l3l2.tltest.ini
	@echo ""
	@cat ./configs/openLLC-test-l2l3l2.tltest.ini >> ./main/build/tltest.ini
	@echo "tltest-config-postbuild: tltest-config-openLLC-test-l2l3l2" > ./main/build/Makefile.config

tltest-config-postbuild:

tltest-build:
	$(MAKE) -C ./main/build -j$(THREADS_BUILD) -s
	$(MAKE) -C ./main/build portgen -j$(THREADS_BUILD) -s --always-make

tltest-build-all-coupledL2: tltest-prepare-all-coupledL2 tltest-build

tltest-build-dpi-coupledL2: tltest-prepare-dpi-coupledL2 tltest-build

tltest-build-v3-coupledL2: tltest-prepare-v3-coupledL2 tltest-build

tltest-build-all-openLLC: tltest-prepare-all-openLLC tltest-build

tltest-build-dpi-openLLC: tltest-prepare-dpi-openLLC tltest-build

tltest-build-v3-openLLC: tltest-prepare-v3-openLLC tltest-build


compile:
	$(MAKE) -C ./dut/XSCache compile

coupledL2-verilog-test-top-l2l3:
	$(MAKE) -C ./dut/XSCache gen-test-top-chi SYSTEM=CHIL2 NUM_CORE=1 NUM_TL_UL=0

coupledL2-verilog-test-top-l2l3l2:
	$(MAKE) -C ./dut/XSCache gen-test-top-chi SYSTEM=CHIL2 NUM_CORE=2 NUM_TL_UL=0

coupledL2-verilog-clean:
	$(MAKE) -C ./dut/XSCache clean

openLLC-verilog-test-top-l2l3:
	$(MAKE) -C ./dut/XSCache test-top-l2l3-openllc

openLLC-verilog-test-top-l2l3l2:
	$(MAKE) -C ./dut/XSCache test-top-l2l3l2-openllc

openLLC-verilog-clean:
	$(MAKE) -C ./dut/XSCache clean


VERILATOR := verilator
VERILATOR_COMMON_ARGS_COUPLEDL2 := ./dut/XSCache/build/coupledl2/*.*v \
		--Mdir ./verilated \
		-O3 \
		--trace-fst \
		--top TestTop \
		--build-jobs $(THREADS_BUILD) --verilate-jobs $(THREADS_BUILD) \
		-DSIM_TOP_MODULE_NAME=TestTop \
		-Wno-fatal
VERILATOR_COMMON_ARGS_OPENLLC := ./dut/XSCache/build/openllc/*.*v \
		--Mdir ./verilated \
		-O3 \
		--trace-fst \
		--top TestTop \
		--build-jobs $(THREADS_BUILD) --verilate-jobs $(THREADS_BUILD) \
		-DSIM_TOP_MODULE_NAME=TestTop \
		-Wno-fatal

coupledL2-verilate-cc:
	rm -rf verilated
	mkdir verilated
	$(VERILATOR) $(VERILATOR_COMMON_ARGS_COUPLEDL2) --cc

coupledL2-verilate-build:
	$(VERILATOR) $(VERILATOR_COMMON_ARGS_COUPLEDL2) --cc --exe --build -o tltest_v3lt \
		libtltest_v3lt.a \
		-LDFLAGS "-lsqlite3 -ldl" -CFLAGS "-rdynamic"

coupledL2-verilate:
	rm -rf verilated
	mkdir verilated
	verilator --trace-fst --cc --build --lib-create vltdut --Mdir ./verilated ./dut/XSCache/build/coupledl2/*.*v -Wno-fatal \
		--top TestTop --build-jobs $(THREADS_BUILD) --verilate-jobs $(THREADS_BUILD) -DSIM_TOP_MODULE_NAME=TestTop

coupledL2-verilate-clean:
	rm -rf verilated

openLLC-verilate-cc:
	rm -rf verilated
	mkdir verilated
	$(VERILATOR) $(VERILATOR_COMMON_ARGS_OPENLLC) --cc

openLLC-verilate-build:
	$(VERILATOR) $(VERILATOR_COMMON_ARGS_OPENLLC) --cc --exe --build -o tltest_v3lt \
		lbtltest_v3lt.a \
		-LDFLAGS "-lsqlite3 -ldl" -CFLAGS "-rdynamic"

openLLC-verilate:
	rm -rf verilated
	mkdir verilated
	verilator --trace-fst --cc --build --lib-create vltdut --Mdir ./verilated ./dut/XSCache/build/openllc/*.*v -Wno-fatal \
		--top TestTop --build-jobs $(THREADS_BUILD) --verilate-jobs $(THREADS_BUILD) -DSIM_TOP_MODULE_NAME=TestTop

openLLC-verilate-clean:
	rm -rf verilated


coupledL2-test-l2l3: compile coupledL2-verilog-test-top-l2l3 coupledL2-verilate \
					 tltest-config-coupledL2-test-l2l3 tltest-build-all-coupledL2

coupledL2-test-l2l3-v3: compile coupledL2-verilog-test-top-l2l3 coupledL2-verilate \
					    tltest-config-coupledL2-test-l2l3 tltest-build-v3-coupledL2

coupledL2-test-l2l3-dpi: compile coupledL2-verilog-test-top-l2l3 coupledL2-verilate \
						 tltest-config-coupledL2-test-l2l3 tltest-build-dpi-coupledL2


coupledL2-test-l2l3l2: compile coupledL2-verilog-test-top-l2l3l2 coupledL2-verilate \
					   tltest-config-coupledL2-test-l2l3l2 tltest-build-all-coupledL2

coupledL2-test-l2l3l2-v3: compile coupledL2-verilog-test-top-l2l3l2 coupledL2-verilate \
						  tltest-config-coupledL2-test-l2l3l2 tltest-build-v3-coupledL2

coupledL2-test-l2l3l2-dpi: compile coupledL2-verilog-test-top-l2l3l2 coupledL2-verilate \
						   tltest-config-coupledL2-test-l2l3l2 tltest-build-dpi-coupledL2


openLLC-test-l2l3: compile openLLC-verilog-test-top-l2l3 openLLC-verilate \
				   tltest-config-openLLC-test-l2l3 tltest-build-all-openLLC

openLLC-test-l2l3-v3: compile openLLC-verilog-test-top-l2l3 openLLC-verilate \
				      tltest-config-openLLC-test-l2l3 tltest-build-v3-openLLC

openLLC-test-l2l3-dpi: compile openLLC-verilog-test-top-l2l3 openLLC-verilate \
				       tltest-config-openLLC-test-l2l3 tltest-build-dpi-openLLC


openLLC-test-l2l3l2: compile openLLC-verilog-test-top-l2l3l2 openLLC-verilate \
				     tltest-config-openLLC-test-l2l3l2 tltest-build-all-openLLC

openLLC-test-l2l3l2-v3: compile openLLC-verilog-test-top-l2l3l2 openLLC-verilate \
				        tltest-config-openLLC-test-l2l3l2 tltest-build-v3-openLLC

openLLC-test-l2l3l2-dpi: compile openLLC-verilog-test-top-l2l3l2 openLLC-verilate \
				         tltest-config-openLLC-test-l2l3l2 tltest-build-dpi-openLLC


-include ./main/build/Makefile.config

run: FORCE tltest-config-postbuild
	@rm -rf ./run
	@mkdir ./run
	@cp ./main/build/tltest_v3lt ./run/
	@cp ./main/build/tltest_portgen.so ./run/
	@cp ./main/build/tltest.ini ./run/
	@bash ./scripts/run_v3lt.sh

run_coupledL2-test-l2l3: FORCE tltest-config-coupledL2-test-l2l3
	@rm -rf ./run
	@mkdir ./run
	@cp ./main/build/tltest_v3lt ./run/
	@cp ./main/build/tltest_portgen.so ./run/
	@cp ./main/build/tltest.ini ./run/
	@bash ./scripts/run_v3lt.sh

run_coupledL2-test-l2l3l2: FORCE tltest-config-coupledL2-test-l2l3l2
	@rm -rf ./run
	@mkdir ./run
	@cp ./main/build/tltest_v3lt ./run/
	@cp ./main/build/tltest_portgen.so ./run/
	@cp ./main/build/tltest.ini ./run/
	@bash ./scripts/run_v3lt.sh

run_openLLC-test-l2l3: FORCE tltest-config-openLLC-test-l2l3
	@rm -rf ./run
	@mkdir ./run
	@cp ./main/build/tltest_v3lt ./run/
	@cp ./main/build/tltest_portgen.so ./run/
	@cp ./main/build/tltest.ini ./run/
	@bash ./scripts/run_v3lt.sh

run_openLLC-test-l2l3l2: FORCE tltest-config-openLLC-test-l2l3l2
	@rm -rf ./run
	@mkdir ./run
	@cp ./main/build/tltest_v3lt ./run/
	@cp ./main/build/tltest_portgen.so ./run/
	@cp ./main/build/tltest.ini ./run/
	@bash ./scripts/run_v3lt.sh


run-with-portgen: FORCE tltest-config-postbuild tltest-portgen
	@rm -rf ./run
	@mkdir ./run
	@cp ./main/build/tltest_v3lt ./run/
	@cp ./main/build/tltest_portgen.so ./run/
	@cp ./main/build/tltest.ini ./run/
	@bash ./scripts/run_v3lt.sh


clean: coupledL2-verilate-clean coupledL2-verilog-clean openLLC-verilate-clean openLLC-verilog-clean
	rm -rf ./main/build
	mkdir ./main/build

# Compact CHI frontend for standalone AnserL2. Generated artifacts remain
# outside main/ and the existing TileLink targets keep their original behavior.
DUT_SV ?= $(abspath ../build/L2Cache.sv)
COMPACTCHI_OBJ_DIR ?= $(abspath ../build/compactchi-native-obj)
COMPACTCHI_SEEDS ?= 42 7 12345 17 9999
COMPACTCHI_CHECK_DIR ?= $(abspath ../build/compactchi-foundation)

# Host-only checks of reused tester components; no RTL or nested DUT build.
.PHONY: compactchi-memory-test
compactchi-memory-test:
	@mkdir -p $(COMPACTCHI_CHECK_DIR)
	$(CXX) -std=c++20 -O2 $(CURDIR)/main/CompactCHI/MemoryAgentTest.cpp \
		$(CURDIR)/main/Memory/MemoryAgent.cpp $(CURDIR)/main/Base/TLGlobal.cpp \
		-o $(COMPACTCHI_CHECK_DIR)/memory-agent-test
	$(COMPACTCHI_CHECK_DIR)/memory-agent-test

.PHONY: compactchi-home-memory-test
compactchi-home-memory-test:
	@mkdir -p $(COMPACTCHI_CHECK_DIR)
	$(CXX) -std=c++20 -O2 $(CURDIR)/main/CompactCHI/HomeMemoryTest.cpp \
		$(CURDIR)/main/Memory/CHIHomeMemory.cpp $(CURDIR)/main/Memory/MemoryAgent.cpp \
		$(CURDIR)/main/Base/TLGlobal.cpp -o $(COMPACTCHI_CHECK_DIR)/home-memory-test
	$(COMPACTCHI_CHECK_DIR)/home-memory-test

.PHONY: compactchi-config-test
compactchi-config-test:
	@mkdir -p $(COMPACTCHI_CHECK_DIR)
	$(CXX) -std=c++20 -O2 $(CURDIR)/main/CompactCHI/ConfigTest.cpp \
		$(CURDIR)/main/System/TLConfig.cpp $(CURDIR)/main/Base/TLGlobal.cpp \
		-o $(COMPACTCHI_CHECK_DIR)/config-test
	$(COMPACTCHI_CHECK_DIR)/config-test $(COMPACTCHI_CHECK_DIR)

.PHONY: compactchi-board-test
compactchi-board-test:
	@mkdir -p $(COMPACTCHI_CHECK_DIR)
	$(CXX) -std=c++20 -O2 $(CURDIR)/main/CompactCHI/ArchitecturalBoardTest.cpp \
		$(CURDIR)/main/CompactCHI/ArchitecturalBoard.cpp $(CURDIR)/main/Base/TLGlobal.cpp \
		-o $(COMPACTCHI_CHECK_DIR)/board-test
	$(COMPACTCHI_CHECK_DIR)/board-test

.PHONY: compactchi-sequencer-test
compactchi-sequencer-test:
	@mkdir -p $(COMPACTCHI_CHECK_DIR)
	$(CXX) -std=c++20 -O2 $(CURDIR)/main/CompactCHI/SequencerTest.cpp \
		$(CURDIR)/main/Sequencer/TLSequencer.cpp $(CURDIR)/main/Sequencer/ProtocolSequencer.cpp \
		$(CURDIR)/main/System/TLConfig.cpp $(CURDIR)/main/Memory/MemoryAgent.cpp \
		$(CURDIR)/main/Memory/CHIHomeMemory.cpp $(CURDIR)/main/CompactCHI/ArchitecturalBoard.cpp \
		$(CURDIR)/main/CompactCHI/Requester.cpp $(CURDIR)/main/CompactCHI/CanonicalTrace.cpp \
		$(wildcard $(CURDIR)/main/Base/*.cpp $(CURDIR)/main/Events/*.cpp \
		$(CURDIR)/main/TLAgent/*.cpp $(CURDIR)/main/Fuzzer/*.cpp) \
		-o $(COMPACTCHI_CHECK_DIR)/sequencer-test
	$(COMPACTCHI_CHECK_DIR)/sequencer-test $(COMPACTCHI_CHECK_DIR)/absent-sequencer.ini
	python3 $(CURDIR)/main/CompactCHI/check-native-host-trace.py $(COMPACTCHI_CHECK_DIR)/native-host-trace.jsonl

.PHONY: compactchi-trace-test
compactchi-trace-test:
	@mkdir -p $(COMPACTCHI_CHECK_DIR)
	$(CXX) -std=c++20 -O2 $(CURDIR)/main/CompactCHI/CanonicalTraceTest.cpp \
		$(CURDIR)/main/CompactCHI/CanonicalTrace.cpp -o $(COMPACTCHI_CHECK_DIR)/trace-test
	$(COMPACTCHI_CHECK_DIR)/trace-test $(COMPACTCHI_CHECK_DIR)/canonical-test
	python3 $(CURDIR)/main/CompactCHI/check-canonical-trace.py $(COMPACTCHI_CHECK_DIR)/canonical-test

.PHONY: compactchi-requester-test
compactchi-requester-test:
	@mkdir -p $(COMPACTCHI_CHECK_DIR)
	$(CXX) -std=c++20 -O2 $(CURDIR)/main/CompactCHI/RequesterTest.cpp \
		$(CURDIR)/main/CompactCHI/Requester.cpp $(CURDIR)/main/CompactCHI/RequesterActions.cpp $(CURDIR)/main/CompactCHI/ArchitecturalBoard.cpp \
		$(CURDIR)/main/Base/TLGlobal.cpp -o $(COMPACTCHI_CHECK_DIR)/requester-test
	$(COMPACTCHI_CHECK_DIR)/requester-test

.PHONY: compactchi-eb-home-test
compactchi-eb-home-test:
	@mkdir -p $(COMPACTCHI_CHECK_DIR)
	$(CXX) -std=c++20 -O2 $(CURDIR)/main/CompactCHI/EbHomeTest.cpp \
		$(CURDIR)/main/CompactCHI/EbHome.cpp $(CURDIR)/main/Memory/CHIHomeMemory.cpp \
		$(CURDIR)/main/Memory/MemoryAgent.cpp $(CURDIR)/main/Base/TLGlobal.cpp \
		-o $(COMPACTCHI_CHECK_DIR)/eb-home-test
	$(COMPACTCHI_CHECK_DIR)/eb-home-test

.PHONY: compactchi-lower-snoop-test
compactchi-lower-snoop-test:
	@mkdir -p $(COMPACTCHI_CHECK_DIR)
	$(CXX) -std=c++20 -O2 $(CURDIR)/main/CompactCHI/LowerSnoopBoardTest.cpp \
		$(CURDIR)/main/CompactCHI/LowerSnoopBoard.cpp $(CURDIR)/main/Base/TLGlobal.cpp \
		-o $(COMPACTCHI_CHECK_DIR)/lower-snoop-test
	$(COMPACTCHI_CHECK_DIR)/lower-snoop-test

.PHONY: compactchi-victim-test
compactchi-victim-test:
	@mkdir -p $(COMPACTCHI_CHECK_DIR)
	$(CXX) -std=c++20 -O2 $(CURDIR)/main/CompactCHI/VictimProfileTest.cpp \
		-o $(COMPACTCHI_CHECK_DIR)/victim-profile-test
	$(COMPACTCHI_CHECK_DIR)/victim-profile-test

.PHONY: compactchi-node-test
compactchi-node-test:
	@mkdir -p $(COMPACTCHI_CHECK_DIR)
	$(CXX) -std=c++20 -O2 $(CURDIR)/main/CompactCHI/NodeAgentTest.cpp \
		$(CURDIR)/main/Sequencer/TLSequencer.cpp $(CURDIR)/main/Sequencer/ProtocolSequencer.cpp \
		$(CURDIR)/main/System/TLConfig.cpp $(CURDIR)/main/Memory/MemoryAgent.cpp \
		$(CURDIR)/main/CompactCHI/ArchitecturalBoard.cpp $(CURDIR)/main/CompactCHI/Requester.cpp \
		$(CURDIR)/main/CompactCHI/RequesterActions.cpp \
		$(wildcard $(CURDIR)/main/Base/*.cpp $(CURDIR)/main/Events/*.cpp \
		$(CURDIR)/main/TLAgent/*.cpp $(CURDIR)/main/Fuzzer/*.cpp) \
		-o $(COMPACTCHI_CHECK_DIR)/node-agent-test
	$(COMPACTCHI_CHECK_DIR)/node-agent-test $(COMPACTCHI_CHECK_DIR)

.PHONY: compactchi-node-fuzzer-test
compactchi-node-fuzzer-test:
	@mkdir -p $(COMPACTCHI_CHECK_DIR)
	$(CXX) -std=c++20 -O2 $(CURDIR)/main/CompactCHI/NodeFuzzerTest.cpp \
		$(CURDIR)/main/Fuzzer/NodeFuzzer.cpp $(CURDIR)/main/System/TLConfig.cpp \
		$(CURDIR)/main/Base/TLGlobal.cpp -o $(COMPACTCHI_CHECK_DIR)/node-fuzzer-test
	$(COMPACTCHI_CHECK_DIR)/node-fuzzer-test $(COMPACTCHI_CHECK_DIR)

.PHONY: compactchi-directory-test
compactchi-directory-test:
	@mkdir -p $(COMPACTCHI_CHECK_DIR)
	$(CXX) -std=c++20 -O2 $(CURDIR)/main/CompactCHI/DirectoryMonitorTest.cpp \
		-o $(COMPACTCHI_CHECK_DIR)/directory-monitor-test
	$(COMPACTCHI_CHECK_DIR)/directory-monitor-test $(COMPACTCHI_CHECK_DIR)/directory.jsonl
	python3 $(CURDIR)/main/CompactCHI/check-directory.py $(COMPACTCHI_CHECK_DIR)/directory.jsonl

.PHONY: compactchi-datastorage-test
compactchi-datastorage-test:
	@mkdir -p $(COMPACTCHI_CHECK_DIR)
	$(CXX) -std=c++20 -O2 $(CURDIR)/main/CompactCHI/DataStorageMonitorTest.cpp \
		-o $(COMPACTCHI_CHECK_DIR)/datastorage-monitor-test
	$(COMPACTCHI_CHECK_DIR)/datastorage-monitor-test $(COMPACTCHI_CHECK_DIR)/datastorage.jsonl
	python3 $(CURDIR)/main/CompactCHI/DataStorageCheckerTest.py

.PHONY: compactchi-forward-node-test
compactchi-forward-node-test:
	@mkdir -p $(COMPACTCHI_CHECK_DIR)
	$(CXX) -std=c++20 -O2 $(CURDIR)/main/CompactCHI/ForwardNodeBoardTest.cpp \
		-o $(COMPACTCHI_CHECK_DIR)/forward-node-test
	$(COMPACTCHI_CHECK_DIR)/forward-node-test

.PHONY: compactchi-cmo-forward-test
compactchi-cmo-forward-test:
	@mkdir -p $(COMPACTCHI_CHECK_DIR)/cmo-forward
	$(CXX) -std=c++20 -O2 $(CURDIR)/main/CompactCHI/CMOForwardTest.cpp \
		-o $(COMPACTCHI_CHECK_DIR)/cmo-forward-test
	$(COMPACTCHI_CHECK_DIR)/cmo-forward-test $(COMPACTCHI_CHECK_DIR)/cmo-forward/forward.jsonl
	python3 $(CURDIR)/main/CompactCHI/CMOForwardCheckerTest.py $(COMPACTCHI_CHECK_DIR)/cmo-forward

.PHONY: compactchi-transition-test
compactchi-transition-test:
	@mkdir -p $(COMPACTCHI_CHECK_DIR)/transitions
	python3 $(CURDIR)/main/CompactCHI/TransitionCheckerTest.py $(COMPACTCHI_CHECK_DIR)/transitions

.PHONY: compactchi-transport-test
compactchi-transport-test:
	@mkdir -p $(COMPACTCHI_CHECK_DIR)/transport
	$(CXX) -std=c++20 -O2 $(CURDIR)/main/CompactCHI/TransportMatrixTest.cpp \
		$(CURDIR)/main/CompactCHI/CanonicalTrace.cpp $(CURDIR)/main/CompactCHI/EbHome.cpp \
		$(CURDIR)/main/Memory/CHIHomeMemory.cpp $(CURDIR)/main/Memory/MemoryAgent.cpp \
		$(CURDIR)/main/CompactCHI/Requester.cpp $(CURDIR)/main/CompactCHI/RequesterActions.cpp \
		$(CURDIR)/main/CompactCHI/ArchitecturalBoard.cpp $(CURDIR)/main/Base/TLGlobal.cpp \
		-o $(COMPACTCHI_CHECK_DIR)/transport-matrix-test
	$(COMPACTCHI_CHECK_DIR)/transport-matrix-test $(COMPACTCHI_CHECK_DIR)/transport
	python3 $(CURDIR)/main/CompactCHI/TransportCheckerTest.py $(COMPACTCHI_CHECK_DIR)/transport

.PHONY: compactchi-residency-test
compactchi-residency-test:
	@mkdir -p $(COMPACTCHI_CHECK_DIR)/residency
	python3 $(CURDIR)/main/CompactCHI/ResidencyCheckerTest.py $(COMPACTCHI_CHECK_DIR)/residency

.PHONY: compactchi-forward-multi-test
compactchi-forward-multi-test:
	@mkdir -p $(COMPACTCHI_CHECK_DIR)/forward-multi
	$(CXX) -std=c++20 -O2 $(CURDIR)/main/CompactCHI/ForwardMultiTest.cpp \
		$(CURDIR)/main/CompactCHI/CanonicalTrace.cpp $(CURDIR)/main/CompactCHI/LowerSnoopBoard.cpp \
		$(CURDIR)/main/Base/TLGlobal.cpp -o $(COMPACTCHI_CHECK_DIR)/forward-multi-test
	$(COMPACTCHI_CHECK_DIR)/forward-multi-test $(COMPACTCHI_CHECK_DIR)/forward-multi
	python3 $(CURDIR)/main/CompactCHI/ForwardMultiCheckerTest.py $(COMPACTCHI_CHECK_DIR)/forward-multi

.PHONY: compactchi-prefetch-checker-test
compactchi-prefetch-checker-test:
	python3 $(CURDIR)/main/CompactCHI/PrefetchCheckerTest.py

.PHONY: compactchi-resource-test
compactchi-resource-test:
	@mkdir -p $(COMPACTCHI_CHECK_DIR)
	$(CXX) -std=c++20 -O2 $(CURDIR)/main/CompactCHI/ResourceMonitorTest.cpp \
		-o $(COMPACTCHI_CHECK_DIR)/resource-monitor-test
	$(COMPACTCHI_CHECK_DIR)/resource-monitor-test $(COMPACTCHI_CHECK_DIR)

.PHONY: compactchi-build compactchi-test
compactchi-build:
	@mkdir -p $(COMPACTCHI_OBJ_DIR)
	$(VERILATOR) --cc --exe --build --assert --trace -j $(THREADS_BUILD) --top-module L2Cache \
		--Mdir $(COMPACTCHI_OBJ_DIR) -O2 -Wno-fatal -CFLAGS "-std=c++20" \
		$(DUT_SV) $(CURDIR)/main/CompactCHI/AnserL2.cpp \
		$(CURDIR)/main/CompactCHI/Requester.cpp $(CURDIR)/main/CompactCHI/RequesterActions.cpp $(CURDIR)/main/CompactCHI/ArchitecturalBoard.cpp \
		$(CURDIR)/main/CompactCHI/EbHome.cpp $(CURDIR)/main/CompactCHI/EbHomeDevice.cpp $(CURDIR)/main/CompactCHI/CanonicalTrace.cpp \
		$(CURDIR)/main/CompactCHI/LowerSnoopBoard.cpp \
		$(CURDIR)/main/Memory/CHIHomeMemory.cpp $(CURDIR)/main/Memory/MemoryAgent.cpp \
		$(CURDIR)/main/Sequencer/TLSequencer.cpp $(CURDIR)/main/Sequencer/ProtocolSequencer.cpp \
		$(CURDIR)/main/System/TLConfig.cpp \
		$(wildcard $(CURDIR)/main/Base/*.cpp $(CURDIR)/main/Events/*.cpp \
		$(CURDIR)/main/TLAgent/*.cpp $(CURDIR)/main/Fuzzer/*.cpp)

compactchi-test: compactchi-build
	@for seed in $(COMPACTCHI_SEEDS); do \
		$(COMPACTCHI_OBJ_DIR)/VL2Cache $$seed || exit $$?; \
	done
