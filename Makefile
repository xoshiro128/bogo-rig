BACKEND ?= CPU

CC       = gcc
CXX      = g++
NVCC     = nvcc
HIPCC    = hipcc

CFLAGS   = -Wall -Wextra -std=c23 -O3 -march=native -Iinclude -D_POSIX_C_SOURCE=200809L
CXXFLAGS = -Wall -Wextra -std=c++17 -O3 -march=native -Iinclude -Ibuild -D_POSIX_C_SOURCE=200809L
CUFLAGS  = -O3 -Iinclude -arch=sm_75
HIPFLAGS = -O3 -Iinclude

LDLIBS   = -lwebsockets -lpthread

TARGET = bogowsclient
SRC    = src/dashboard.c src/main.c src/server.c src/worker.c src/cJSON.c
OBJ    = $(patsubst src/%.c,build/%.o,$(SRC))

ifeq ($(BACKEND),CUDA)
    COMPUTE_OBJ = build/compute_cuda.o
    LDLIBS     += -L/opt/cuda/lib64 -L/usr/local/cuda/lib64 -lcudart -lstdc++
    LINKER      = $(NVCC)
    LINKER_FLAGS =
else ifeq ($(BACKEND),HIP)
    COMPUTE_OBJ = build/compute_hip.o
    LDLIBS     += -lstdc++
    LINKER      = $(HIPCC)
    LINKER_FLAGS =
else ifeq ($(BACKEND),VULKAN)
    COMPUTE_OBJ = build/compute_vulkan.o
    LDLIBS     += -lvulkan -lstdc++
    LINKER      = $(CXX)
    LINKER_FLAGS = $(LDFLAGS)
    SHADER_HDR  = build/compute_spv.h
else
    COMPUTE_OBJ = build/compute_cpu.o
    LINKER      = $(CC)
    LINKER_FLAGS = $(LDFLAGS)
endif

all: $(TARGET)

$(TARGET): $(OBJ) $(COMPUTE_OBJ)
	$(LINKER) $^ $(LINKER_FLAGS) $(LDLIBS) -o $@

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -c $< -o $@

build/compute_cuda.o: src/compute/compute_cuda.cu | build
	$(NVCC) $(CUFLAGS) -c $< -o $@

build/compute_hip.o: src/compute/compute_hip.hip | build
	$(HIPCC) $(HIPFLAGS) -c $< -o $@

build/compute_vulkan.o: src/compute/compute_vulkan.cpp $(SHADER_HDR) | build
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/compute_cpu.o: src/compute/compute_cpu.c | build
	$(CC) $(CFLAGS) -c $< -o $@

build/compute.spv: src/compute/compute_vulkan.comp | build
	glslc -o $@ $<

build/compute_spv.h: build/compute.spv
	xxd -i $< > $@

build:
	mkdir -p build

clean:
	rm -rf build $(TARGET)

.PHONY: all clean
