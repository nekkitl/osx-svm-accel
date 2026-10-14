SDK := $(shell xcrun --sdk macosx --show-sdk-path)
KERNEL_HEADERS := $(SDK)/System/Library/Frameworks/Kernel.framework/Headers
BUILD := build
KEXT := $(BUILD)/AMDVMProbe.kext
OPT_IN_AUDIT := $(BUILD)/opt-in-audit
XNU_KPI_EXPORTS ?=
AMDVM_XNU_PROFILE ?= stock

KEXT_CXXFLAGS := -arch x86_64 -mkernel -fapple-kext -fno-exceptions -fno-rtti \
	-mno-red-zone -fno-builtin -DKERNEL -isystem $(KERNEL_HEADERS)
KEXT_CFLAGS := -arch x86_64 -mkernel -mno-red-zone -fno-builtin \
	-DKERNEL -isystem $(KERNEL_HEADERS)

ifeq ($(AMDVM_XNU_PROFILE),stock)
KEXT_CFLAGS += -DAMDVM_STOCK_XNU=1
KEXT_CXXFLAGS += -DAMDVM_STOCK_XNU=1
else ifeq ($(AMDVM_XNU_PROFILE),patched)
KEXT_CFLAGS += -DAMDVM_PATCHED_XNU=1
KEXT_CXXFLAGS += -DAMDVM_PATCHED_XNU=1
else
$(error AMDVM_XNU_PROFILE must be 'stock' or 'patched')
endif

ifeq ($(AMDVM_EXPERIMENTAL_HLT_RUN),1)
KEXT_CFLAGS += -DAMDVM_SVM_RUN_EXPERIMENTAL_OPT_IN=1
KEXT_CXXFLAGS += -DAMDVM_SVM_RUN_EXPERIMENTAL_OPT_IN=1
endif

# VM-scoped hardware RUN/KICK remain opt-in. This switch only compiles
# entry support; building does not sign, install, load or execute the kext.
ifeq ($(AMDVM_EXPERIMENTAL_VM_API_RUN),1)
	KEXT_CFLAGS += -DAMDVM_VM_API_RUN_EXPERIMENTAL=1
	KEXT_CXXFLAGS += -DAMDVM_VM_API_RUN_EXPERIMENTAL=1
endif

.PHONY: all clean opt-in-object-audit hlt-run-candidate hlt-run-kpi-audit
.PHONY: stock-xnu-hlt-run-candidate patched-xnu-hlt-run-candidate
.PHONY: stock-xnu-kpi-audit patched-xnu-kpi-audit

.PHONY: npt-isolation-test vmcb-state-roundtrip-test quarantine-owner-test
.PHONY: npt-capacity-test npt-scatter-capacity-test
.PHONY: guest-memory-accounting-test
.PHONY: event-injection-test
.PHONY: kick-logic-test
.PHONY: event-state-test
.PHONY: msr-helpers-test
.PHONY: physmem-unpin-test quarantine-registry-test
.PHONY: host-mtrr-test
.PHONY: npt-pat-transaction-test
.PHONY: vcpu-lifecycle-test
.PHONY: vm-lifecycle-model-test
.PHONY: vm-lifecycle-kernel-compile
.PHONY: vm-abi-draft-layout-test
.PHONY: client-mode-test client-mode-kernel-compile vm-negotiate-test
.PHONY: vm-negotiate-kernel-compile
.PHONY: vm-memory-test vm-memory-kernel-compile vm-memory-cli-test
.PHONY: vm-memory-4g-npt-test
.PHONY: vm-cpu-state-test
vm-cpu-state-test: | $(BUILD)
	xcrun --sdk macosx clang -arch x86_64 -std=c11 -g -O1 \
		-Wall -Wextra -Werror -fsanitize=address,undefined \
		../tools/amdvm_vm_cpu_state_test.c provider/amdvm_vm_cpu_state.c \
		provider/amdvm_vmcb.c provider/amdvm_xstate.c provider/amdvm_npt.c \
		-o $(BUILD)/amdvm-vm-cpu-state-test
	$(BUILD)/amdvm-vm-cpu-state-test

vm-memory-cli-test: | $(BUILD)
	xcrun --sdk macosx clang -arch x86_64 -std=c11 -g -O1 \
		-Wall -Wextra -Werror -fsanitize=address,undefined \
		../tools/amdvm_vm_memory_cli_test.c provider/amdvm_vm_cpu_state.c \
		provider/amdvm_vmcb.c provider/amdvm_xstate.c provider/amdvm_npt.c -framework IOKit \
		-framework CoreFoundation -o $(BUILD)/amdvm-vm-memory-cli-test
	$(BUILD)/amdvm-vm-memory-cli-test

vm-memory-test: | $(BUILD)
	xcrun --sdk macosx clang -arch x86_64 -std=c11 -g -O1 \
		-Wall -Wextra -Werror -fsanitize=address,undefined -pthread \
		../tools/amdvm_vm_memory_test.c provider/amdvm_vm_memory.c \
		provider/amdvm_npt.c provider/amdvm_quarantine.c \
		provider/amdvm_vm_npt.c \
		-o $(BUILD)/amdvm-vm-memory-test
	$(BUILD)/amdvm-vm-memory-test

vm-memory-4g-npt-test: | $(BUILD)
	xcrun --sdk macosx clang -arch x86_64 -std=c11 -g -O1 \
		-Wall -Wextra -Werror -fsanitize=address,undefined -pthread \
		../tools/amdvm_vm_memory_4g_npt_test.c provider/amdvm_vm_memory.c \
		provider/amdvm_npt.c provider/amdvm_quarantine.c \
		provider/amdvm_vm_npt.c -o $(BUILD)/amdvm-vm-memory-4g-npt-test
	$(BUILD)/amdvm-vm-memory-4g-npt-test

vm-memory-kernel-compile: | $(BUILD)/provider
	xcrun --sdk macosx clang $(KEXT_CFLAGS) -std=c11 -Iprovider \
		-Wall -Wextra -Werror -c provider/amdvm_vm_memory.c \
		-o $(BUILD)/provider/amdvm_vm_memory.o
	xcrun --sdk macosx clang $(KEXT_CFLAGS) -std=c11 -Iprovider \
		-Wall -Wextra -Werror -c provider/amdvm_vm_memory_kernel.c \
		-o $(BUILD)/provider/amdvm_vm_memory_kernel.o
.PHONY: entry-snapshot-test
vcpu-lifecycle-test: | $(BUILD)
	xcrun --sdk macosx clang -arch x86_64 -std=c11 -g -O1 \
		-Wall -Wextra -Werror -fsanitize=address,undefined \
		../tools/amdvm_vcpu_lifecycle_test.c \
		-o $(BUILD)/amdvm-vcpu-lifecycle-test
	$(BUILD)/amdvm-vcpu-lifecycle-test

vm-lifecycle-model-test: | $(BUILD)
	xcrun --sdk macosx clang -arch x86_64 -std=c11 -g -O1 \
		-Wall -Wextra -Werror -fsanitize=address,undefined \
		../tools/amdvm_vm_lifecycle_model_test.c \
		provider/amdvm_vm_lifecycle.c \
		-o $(BUILD)/amdvm-vm-lifecycle-model-test
	$(BUILD)/amdvm-vm-lifecycle-model-test

vm-lifecycle-kernel-compile: $(BUILD)/provider
	xcrun --sdk macosx clang $(KEXT_CFLAGS) -std=c11 -Iprovider \
		-Wall -Wextra -Werror -c provider/amdvm_vm_lifecycle.c \
		-o $(BUILD)/provider/amdvm_vm_lifecycle.o

vm-abi-draft-layout-test: | $(BUILD)
	xcrun --sdk macosx clang++ -x c++ -arch x86_64 -std=c++17 \
		-Wall -Wextra -Werror -fsyntax-only \
		../tools/amdvm_vm_abi_draft_layout_test.c
	xcrun --sdk macosx clang -arch x86_64 -std=c11 -g -O1 \
		-Wall -Wextra -Werror -fsanitize=address,undefined \
		../tools/amdvm_vm_abi_draft_layout_test.c \
		-o $(BUILD)/amdvm-vm-abi-draft-layout-test
	$(BUILD)/amdvm-vm-abi-draft-layout-test

client-mode-test: | $(BUILD)
	xcrun --sdk macosx clang -arch x86_64 -std=c11 -g -O1 \
		-Wall -Wextra -Werror -fsanitize=address,undefined -pthread \
		../tools/amdvm_client_mode_test.c provider/amdvm_client_mode.c \
		-o $(BUILD)/amdvm-client-mode-test
	$(BUILD)/amdvm-client-mode-test

client-mode-kernel-compile: $(BUILD)/provider/amdvm_client_mode.o

vm-negotiate-test: | $(BUILD)
	xcrun --sdk macosx clang -arch x86_64 -std=c11 -g -O1 \
		-Wall -Wextra -Werror -fsanitize=address,undefined -Iprovider \
		../tools/amdvm_vm_negotiate_test.c provider/amdvm_vm_negotiate.c \
		provider/amdvm_client_mode.c -o $(BUILD)/amdvm-vm-negotiate-test
	$(BUILD)/amdvm-vm-negotiate-test

vm-negotiate-kernel-compile: $(BUILD)/provider/amdvm_vm_negotiate.o

entry-snapshot-test: | $(BUILD)
	xcrun --sdk macosx clang -arch x86_64 -std=c11 -g -O1 \
		-Wall -Wextra -Werror -fsanitize=address,undefined -Iprovider \
		../tools/amdvm_entry_snapshot_test.c provider/amdvm_npt.c \
		provider/amdvm_vmcb.c provider/amdvm_xstate.c \
		-o $(BUILD)/amdvm-entry-snapshot-test
	$(BUILD)/amdvm-entry-snapshot-test

npt-isolation-test: | $(BUILD)
	xcrun --sdk macosx clang -arch x86_64 -std=c11 -g -O1 \
		-Wall -Wextra -Werror -fsanitize=address,undefined -Iprovider \
		../tools/amdvm_npt_isolation_test.c provider/amdvm_npt.c \
		-o $(BUILD)/amdvm-npt-isolation-test
	$(BUILD)/amdvm-npt-isolation-test

npt-capacity-test: | $(BUILD)
	xcrun --sdk macosx clang -arch x86_64 -std=c11 -g -O1 \
		-Wall -Wextra -Werror -fsanitize=address,undefined -Iprovider \
		tools/amdvm_npt_capacity_test.c provider/amdvm_npt.c \
		-o $(BUILD)/amdvm-npt-capacity-test
	$(BUILD)/amdvm-npt-capacity-test

npt-scatter-capacity-test: | $(BUILD)
	xcrun --sdk macosx clang -arch x86_64 -std=c11 -g -O1 \
		-Wall -Wextra -Werror -fsanitize=address,undefined -Iprovider \
		tools/amdvm_npt_scatter_capacity_test.c provider/amdvm_npt.c \
		-o $(BUILD)/amdvm-npt-scatter-capacity-test
	$(BUILD)/amdvm-npt-scatter-capacity-test

guest-memory-accounting-test: | $(BUILD)
	xcrun --sdk macosx clang -arch x86_64 -std=c11 -g -O1 \
		-Wall -Wextra -Werror -fsanitize=address,undefined \
		tools/amdvm_guest_memory_accounting_test.c \
		provider/amdvm_guest_memory.c -o $(BUILD)/amdvm-guest-memory-accounting-test
	$(BUILD)/amdvm-guest-memory-accounting-test

vmcb-state-roundtrip-test: | $(BUILD)
	xcrun --sdk macosx clang -arch x86_64 -std=c11 -g -O1 \
		-Wall -Wextra -Werror -fsanitize=address,undefined -Iprovider \
		../tools/amdvm_vmcb_state_roundtrip_test.c provider/amdvm_vmcb.c \
		provider/amdvm_xstate.c provider/amdvm_npt.c \
		-o $(BUILD)/amdvm-vmcb-state-roundtrip-test
	$(BUILD)/amdvm-vmcb-state-roundtrip-test

quarantine-owner-test: | $(BUILD)
	xcrun --sdk macosx clang -arch x86_64 -std=c11 -g -O1 \
		-Wall -Wextra -Werror -fsanitize=address,undefined \
		../tools/amdvm_quarantine_owner_test.c \
		-o $(BUILD)/amdvm-quarantine-owner-test
	$(BUILD)/amdvm-quarantine-owner-test

physmem-unpin-test: | $(BUILD)
	xcrun --sdk macosx clang++ -arch x86_64 -std=c++17 -g -O1 \
		-Wall -Wextra -Werror -fsanitize=address,undefined \
		-Itests/mock -Iprovider tools/amdvm_physmem_unpin_test.cpp \
		provider/amdvm_physmem.cpp -o $(BUILD)/amdvm-physmem-unpin-test
	$(BUILD)/amdvm-physmem-unpin-test

quarantine-registry-test: | $(BUILD)
	xcrun --sdk macosx clang -arch x86_64 -std=c11 -g -O1 \
		-Wall -Wextra -Werror -fsanitize=address,undefined -Iprovider \
		tools/amdvm_quarantine_registry_test.c provider/amdvm_quarantine.c \
		-o $(BUILD)/amdvm-quarantine-registry-test
	$(BUILD)/amdvm-quarantine-registry-test

kick-logic-test: | $(BUILD)
	xcrun --sdk macosx clang -arch x86_64 -std=c11 -g -O1 \
		-Wall -Wextra -Werror -fsanitize=address,undefined \
		../tools/amdvm_kick_logic_test.c \
		-o $(BUILD)/amdvm-kick-logic-test
	$(BUILD)/amdvm-kick-logic-test

event-injection-test: | $(BUILD)
	xcrun --sdk macosx clang -arch x86_64 -std=c11 -g -O1 \
		-Wall -Wextra -Werror -fsanitize=address,undefined -Iprovider \
		../tools/amdvm_event_injection_test.c provider/amdvm_vmcb.c \
		provider/amdvm_vmrun.c provider/amdvm_xstate.c \
		provider/amdvm_npt.c \
		-o $(BUILD)/amdvm-event-injection-test
	$(BUILD)/amdvm-event-injection-test

event-state-test: | $(BUILD)
	xcrun --sdk macosx clang -arch x86_64 -std=c11 -g -O1 \
		-Wall -Wextra -Werror -fsanitize=address,undefined \
		../tools/amdsvm_event_state_test.c \
		-o $(BUILD)/amdsvm-event-state-test
	$(BUILD)/amdsvm-event-state-test

msr-helpers-test: | $(BUILD)
	xcrun --sdk macosx clang -arch x86_64 -std=c11 -g -O1 \
		-Wall -Wextra -Werror -fsanitize=address,undefined \
		../tools/amdsvm_msr_helpers_test.c \
		-o $(BUILD)/amdsvm-msr-helpers-test
	$(BUILD)/amdsvm-msr-helpers-test

host-mtrr-test: | $(BUILD)
	xcrun --sdk macosx clang -arch x86_64 -std=c11 -g -O1 \
		-Wall -Wextra -Werror -fsanitize=address,undefined \
		../tools/amdvm_host_mtrr_test.c provider/amdvm_host_mtrr.c \
		-o $(BUILD)/amdvm-host-mtrr-test
	$(BUILD)/amdvm-host-mtrr-test

npt-pat-transaction-test: | $(BUILD)
	xcrun --sdk macosx clang -arch x86_64 -std=c11 -g -O1 \
		-Wall -Wextra -Werror -fsanitize=address,undefined -Iprovider \
		../tools/amdvm_npt_pat_transaction_test.c provider/amdvm_npt.c \
		-o $(BUILD)/amdvm-npt-pat-transaction-test
	$(BUILD)/amdvm-npt-pat-transaction-test

all: $(KEXT)/Contents/MacOS/AMDVMProbe $(BUILD)/amdvm-probe

.PHONY: provider-primitives
provider-primitives: $(BUILD)/provider/amdvm_npt.o $(BUILD)/provider/amdvm_vmcb.o \
	$(BUILD)/provider/amdvm_vmrun.o $(BUILD)/provider/amdvm_vmrun_x86_64.o \
	$(BUILD)/provider/amdvm_xstate.o \
	$(BUILD)/provider/amdvm_physmem.o $(BUILD)/provider/amdvm_svm_owner.o \
	$(BUILD)/provider/amdvm_hlt_context.o $(BUILD)/provider/amdvm_guest_memory.o \
	$(BUILD)/provider/amdvm_host_mtrr.o $(BUILD)/provider/amdvm_client_mode.o \
	$(BUILD)/provider/amdvm_quarantine.o

# Compile the otherwise preprocessor-gated entry path as standalone objects.
# This target never links a kext, stages files, or runs SVM instructions.
opt-in-object-audit: $(OPT_IN_AUDIT)/amdvm_vmrun.o \
		$(OPT_IN_AUDIT)/amdvm_hlt_context.o \
		$(OPT_IN_AUDIT)/amdvm_svm_owner.o

# Build a separate run-enabled candidate bundle; this target never stages,
# signs, loads, or executes the hardware transition.
hlt-run-candidate:
	$(MAKE) BUILD=build/hlt-run AMDVM_EXPERIMENTAL_HLT_RUN=1 all

hlt-run-kpi-audit: hlt-run-candidate
	python3 audit_kpi_imports.py build/hlt-run/AMDVMProbe.kext/Contents/MacOS/AMDVMProbe $(XNU_KPI_EXPORTS)

# Profile-specific candidates only build and audit; they never stage/load or
# invoke VMRUN.
stock-xnu-hlt-run-candidate:
	$(MAKE) BUILD=build/stock-xnu-hlt-run AMDVM_XNU_PROFILE=stock AMDVM_EXPERIMENTAL_HLT_RUN=1 all

patched-xnu-hlt-run-candidate:
	$(MAKE) BUILD=build/patched-xnu-hlt-run AMDVM_XNU_PROFILE=patched AMDVM_EXPERIMENTAL_HLT_RUN=1 all

stock-xnu-kpi-audit: stock-xnu-hlt-run-candidate
	python3 audit_kpi_imports.py build/stock-xnu-hlt-run/AMDVMProbe.kext/Contents/MacOS/AMDVMProbe $(XNU_KPI_EXPORTS)

patched-xnu-kpi-audit: patched-xnu-hlt-run-candidate
	python3 audit_kpi_imports.py build/patched-xnu-hlt-run/AMDVMProbe.kext/Contents/MacOS/AMDVMProbe $(XNU_KPI_EXPORTS)

$(OPT_IN_AUDIT):
	mkdir -p $@

$(OPT_IN_AUDIT)/amdvm_vmrun.o: provider/amdvm_vmrun.c provider/amdvm_vmrun.h provider/amdvm_vmcb.h provider/amdvm_physmem.h provider/amdvm_vmrun_asm.h | $(OPT_IN_AUDIT)
	xcrun --sdk macosx clang $(KEXT_CFLAGS) -std=c11 -Iprovider -DAMDVM_SVM_RUN_EXPERIMENTAL_OPT_IN=1 -c $< -o $@

$(OPT_IN_AUDIT)/amdvm_hlt_context.o: provider/amdvm_hlt_context.c provider/amdvm_hlt_context.h provider/amdvm_host_mtrr.h provider/amdvm_quarantine.h provider/amdvm_npt.h provider/amdvm_physmem.h provider/amdvm_vmcb.h provider/amdvm_vmrun.h | $(OPT_IN_AUDIT)
	xcrun --sdk macosx clang $(KEXT_CFLAGS) -std=c11 -Iprovider -DAMDVM_SVM_RUN_EXPERIMENTAL_OPT_IN=1 -c $< -o $@

$(OPT_IN_AUDIT)/amdvm_svm_owner.o: provider/amdvm_svm_owner.c provider/amdvm_svm_owner.h provider/amdvm_physmem.h provider/amdvm_vmcb.h | $(OPT_IN_AUDIT)
	xcrun --sdk macosx clang $(KEXT_CFLAGS) -std=c11 -Iprovider -DAMDVM_SVM_RUN_EXPERIMENTAL_OPT_IN=1 -c $< -o $@

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/provider:
	mkdir -p $(BUILD)/provider

$(BUILD)/provider/amdvm_vm_memory.o: provider/amdvm_vm_memory.c provider/amdvm_vm_memory.h provider/amdvm_quarantine.h provider/amdvm_npt.h amdvm_probe_abi.h | $(BUILD)/provider
	xcrun --sdk macosx clang $(KEXT_CFLAGS) -std=c11 -Iprovider -c $< -o $@

$(BUILD)/provider/amdvm_vm_memory_kernel.o: provider/amdvm_vm_memory_kernel.c provider/amdvm_vm_memory.h provider/amdvm_hlt_context.h | $(BUILD)/provider
	xcrun --sdk macosx clang $(KEXT_CFLAGS) -std=c11 -Iprovider -c $< -o $@

$(BUILD)/provider/amdvm_vm_npt.o: provider/amdvm_vm_npt.c provider/amdvm_vm_npt.h provider/amdvm_vm_memory.h provider/amdvm_npt.h | $(BUILD)/provider
	xcrun --sdk macosx clang $(KEXT_CFLAGS) -std=c11 -Iprovider -Wall -Wextra -Werror -c $< -o $@

$(BUILD)/provider/amdvm_vm_cpu_state.o: provider/amdvm_vm_cpu_state.c provider/amdvm_vm_cpu_state.h provider/amdvm_vmcb.h amdvm_probe_abi.h | $(BUILD)/provider
	xcrun --sdk macosx clang $(KEXT_CFLAGS) -std=c11 -Iprovider -Wall -Wextra -Werror -c $< -o $@

$(BUILD)/provider/amdvm_physmem.o: provider/amdvm_physmem.cpp provider/amdvm_physmem.h provider/amdvm_vmcb.h | $(BUILD)/provider
	xcrun --sdk macosx clang++ $(KEXT_CXXFLAGS) -std=c++17 -Iprovider -c $< -o $@

$(BUILD)/provider/amdvm_svm_owner.o: provider/amdvm_svm_owner.c provider/amdvm_svm_owner.h provider/amdvm_physmem.h provider/amdvm_vmcb.h | $(BUILD)/provider
	xcrun --sdk macosx clang $(KEXT_CFLAGS) -std=c11 -Iprovider -c $< -o $@

$(BUILD)/provider/amdvm_hlt_context.o: provider/amdvm_hlt_context.c provider/amdvm_hlt_context.h provider/amdvm_guest_memory.h provider/amdvm_host_mtrr.h provider/amdvm_quarantine.h provider/amdvm_npt.h provider/amdvm_vm_npt.h provider/amdvm_vm_memory.h provider/amdvm_physmem.h provider/amdvm_vmcb.h provider/amdvm_vmrun.h | $(BUILD)/provider
	xcrun --sdk macosx clang $(KEXT_CFLAGS) -std=c11 -Iprovider -c $< -o $@

$(BUILD)/provider/amdvm_guest_memory.o: provider/amdvm_guest_memory.c provider/amdvm_guest_memory.h amdvm_probe_abi.h | $(BUILD)/provider
	xcrun --sdk macosx clang $(KEXT_CFLAGS) -std=c11 -Iprovider -c $< -o $@

$(BUILD)/provider/amdvm_quarantine.o: provider/amdvm_quarantine.c provider/amdvm_quarantine.h provider/amdvm_quarantine_owner.h provider/amdvm_physmem.h | $(BUILD)/provider
	xcrun --sdk macosx clang $(KEXT_CFLAGS) -std=c11 -Iprovider -c $< -o $@

$(BUILD)/provider/amdvm_host_mtrr.o: provider/amdvm_host_mtrr.c provider/amdvm_host_mtrr.h | $(BUILD)/provider
	xcrun --sdk macosx clang $(KEXT_CFLAGS) -std=c11 -Iprovider -c $< -o $@

$(BUILD)/provider/amdvm_client_mode.o: provider/amdvm_client_mode.c provider/amdvm_client_mode.h | $(BUILD)/provider
	xcrun --sdk macosx clang $(KEXT_CFLAGS) -std=c11 -Iprovider -c $< -o $@

$(BUILD)/provider/amdvm_vm_negotiate.o: provider/amdvm_vm_negotiate.c provider/amdvm_vm_negotiate.h provider/amdvm_client_mode.h amdvm_vm_abi_draft.h | $(BUILD)/provider
	xcrun --sdk macosx clang $(KEXT_CFLAGS) -std=c11 -Iprovider -c $< -o $@

$(BUILD)/provider/amdvm_npt.o: provider/amdvm_npt.c provider/amdvm_npt.h provider/amdvm_vmcb.h | $(BUILD)/provider
	xcrun --sdk macosx clang $(KEXT_CFLAGS) -std=c11 -Iprovider -c $< -o $@

$(BUILD)/provider/amdvm_vmcb.o: provider/amdvm_vmcb.c provider/amdvm_vmcb.h provider/amdvm_vmrun.h provider/amdvm_vmrun_asm.h | $(BUILD)/provider
	xcrun --sdk macosx clang $(KEXT_CFLAGS) -std=c11 -Iprovider -c $< -o $@

$(BUILD)/provider/amdvm_xstate.o: provider/amdvm_xstate.c provider/amdvm_xstate.h amdvm_probe_abi.h | $(BUILD)/provider
	xcrun --sdk macosx clang $(KEXT_CFLAGS) -std=c11 -Iprovider -c $< -o $@

$(BUILD)/provider/amdvm_vmrun.o: provider/amdvm_vmrun.c provider/amdvm_vmrun.h provider/amdvm_vmcb.h provider/amdvm_physmem.h provider/amdvm_vmrun_asm.h provider/amdvm_xstate.h | $(BUILD)/provider
	xcrun --sdk macosx clang $(KEXT_CFLAGS) -std=c11 -Iprovider -c $< -o $@

$(BUILD)/provider/amdvm_vmrun_x86_64.o: provider/amdvm_vmrun_x86_64.S provider/amdvm_vmrun.h provider/amdvm_vmrun_asm.h | $(BUILD)/provider
	xcrun --sdk macosx clang -arch x86_64 -mkernel -mno-red-zone -Iprovider -c $< -o $@

$(BUILD)/AMDVMProbe.o: AMDVMProbe.cpp amdvm_probe_abi.h \
		provider/amdvm_hlt_context.h provider/amdvm_npt.h \
		provider/amdvm_physmem.h provider/amdvm_svm_owner.h \
		provider/amdvm_vmcb.h provider/amdvm_vmrun.h \
		provider/amdvm_client_mode.h provider/amdvm_vm_negotiate.h \
		provider/amdvm_vm_memory.h provider/amdvm_vm_cpu_state.h amdvm_vm_abi_draft.h | $(BUILD)
	xcrun --sdk macosx clang++ $(KEXT_CXXFLAGS) -c $< -o $@

$(KEXT)/Contents/MacOS/AMDVMProbe: $(BUILD)/AMDVMProbe.o \
		$(BUILD)/provider/amdvm_npt.o \
		$(BUILD)/provider/amdvm_vmcb.o \
		$(BUILD)/provider/amdvm_xstate.o \
		$(BUILD)/provider/amdvm_vmrun.o \
		$(BUILD)/provider/amdvm_vmrun_x86_64.o \
		$(BUILD)/provider/amdvm_physmem.o \
		$(BUILD)/provider/amdvm_svm_owner.o \
		$(BUILD)/provider/amdvm_hlt_context.o \
		$(BUILD)/provider/amdvm_guest_memory.o \
		$(BUILD)/provider/amdvm_quarantine.o \
		$(BUILD)/provider/amdvm_host_mtrr.o \
		$(BUILD)/provider/amdvm_client_mode.o \
		$(BUILD)/provider/amdvm_vm_negotiate.o \
		$(BUILD)/provider/amdvm_vm_memory.o \
		$(BUILD)/provider/amdvm_vm_memory_kernel.o \
		$(BUILD)/provider/amdvm_vm_npt.o \
		$(BUILD)/provider/amdvm_vm_cpu_state.o Info.plist
	mkdir -p $(KEXT)/Contents/MacOS
	cp Info.plist $(KEXT)/Contents/Info.plist
	xcrun ld -kext -arch x86_64 -undefined dynamic_lookup -o $@ \
		$(BUILD)/AMDVMProbe.o \
		$(BUILD)/provider/amdvm_npt.o \
		$(BUILD)/provider/amdvm_vmcb.o \
		$(BUILD)/provider/amdvm_xstate.o \
		$(BUILD)/provider/amdvm_vmrun.o \
		$(BUILD)/provider/amdvm_vmrun_x86_64.o \
		$(BUILD)/provider/amdvm_physmem.o \
		$(BUILD)/provider/amdvm_svm_owner.o \
		$(BUILD)/provider/amdvm_hlt_context.o \
		$(BUILD)/provider/amdvm_guest_memory.o \
		$(BUILD)/provider/amdvm_quarantine.o \
		$(BUILD)/provider/amdvm_host_mtrr.o \
		$(BUILD)/provider/amdvm_client_mode.o \
		$(BUILD)/provider/amdvm_vm_negotiate.o \
		$(BUILD)/provider/amdvm_vm_memory.o \
		$(BUILD)/provider/amdvm_vm_memory_kernel.o \
		$(BUILD)/provider/amdvm_vm_npt.o \
		$(BUILD)/provider/amdvm_vm_cpu_state.o

$(BUILD)/amdvm-probe: amdvm_probe.c amdvm_probe_abi.h \
		provider/amdvm_vm_negotiate.h amdvm_vm_abi_draft.h | $(BUILD)
	xcrun --sdk macosx clang -arch x86_64 -I. -framework IOKit \
		-framework CoreFoundation $< -o $@

clean:
	rm -rf $(BUILD)

.PHONY: experimental-candidate offline-checks
experimental-candidate:
	$(MAKE) BUILD=build/experimental AMDVM_XNU_PROFILE=stock AMDVM_EXPERIMENTAL_HLT_RUN=1 AMDVM_EXPERIMENTAL_VM_API_RUN=1 all

offline-checks: vm-abi-draft-layout-test client-mode-test vm-negotiate-test vm-memory-test vm-memory-4g-npt-test vm-cpu-state-test vmcb-state-roundtrip-test npt-isolation-test event-state-test host-mtrr-test physmem-unpin-test
