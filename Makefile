# =============================================================================
#  Makefile — rtp-streamer
#  Builds: sender  receiver
#
#  Targets:
#    make all           — build both binaries
#    make sender        — build sender only
#    make receiver      — build receiver only
#    make clean         — remove binaries and output files
#    make gen-data      — run Python generators to create test input
#    make run-receiver  — start receiver in background / separate window hint
#
#  Platform detection:
#    On Windows with MinGW/MSYS2  → links -lws2_32
#    On Linux / macOS             → no extra libs needed
# =============================================================================

CXX      := g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Wpedantic
INCLUDES := -I.

# Detect Windows (MinGW sets MSYSTEM or OS=Windows_NT)
ifeq ($(OS),Windows_NT)
    LIBS     := -lws2_32
    EXT      := .exe
    MKDIR    := mkdir
    RM       := del /Q
    RMDIR    := rmdir /S /Q
else
    LIBS     :=
    EXT      :=
    MKDIR    := mkdir -p
    RM       := rm -f
    RMDIR    := rm -rf
endif

SENDER_BIN   := sender$(EXT)
RECEIVER_BIN := receiver$(EXT)

SENDER_SRCS   := sender.cpp
RECEIVER_SRCS := receiver.cpp

# Shared headers (both binaries depend on these)
SHARED_HEADERS := rtp_header.h common.h

# =============================================================================
# Rules
# =============================================================================

.PHONY: all sender receiver clean gen-data dirs

all: dirs $(SENDER_BIN) $(RECEIVER_BIN)

dirs:
ifeq ($(OS),Windows_NT)
	@if not exist "input\frames" $(MKDIR) "input\frames"
	@if not exist "output\frames_out" $(MKDIR) "output\frames_out"
else
	@$(MKDIR) input/frames output/frames_out
endif

$(SENDER_BIN): $(SENDER_SRCS) $(SHARED_HEADERS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $(SENDER_SRCS) $(LIBS)
	@echo "  ✓ Built $@"

$(RECEIVER_BIN): $(RECEIVER_SRCS) $(SHARED_HEADERS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $(RECEIVER_SRCS) $(LIBS)
	@echo "  ✓ Built $@"

sender: dirs $(SENDER_BIN)

receiver: dirs $(RECEIVER_BIN)

# Generate test input data (requires Python 3)
gen-data:
	python gen_test_audio.py
	python gen_test_frames.py
	@echo "  ✓ Test data generated in input/"

clean:
ifeq ($(OS),Windows_NT)
	-$(RM) $(SENDER_BIN) $(RECEIVER_BIN)
	-$(RM) output\audio_out.wav
	-$(RM) output\frames_out\*.raw
else
	$(RM) $(SENDER_BIN) $(RECEIVER_BIN)
	$(RM) output/audio_out.wav
	$(RM) output/frames_out/*.raw
endif
	@echo "  ✓ Cleaned"

# Quick help
help:
	@echo ""
	@echo "  make all          Build sender and receiver"
	@echo "  make gen-data     Generate test WAV + raw frames"
	@echo "  make clean        Remove build artifacts and output files"
	@echo ""
	@echo "  Run workflow:"
	@echo "    Terminal 1:  ./receiver [--port 5004]"
	@echo "    Terminal 2:  ./sender   [--port 5004] [--drop-pct 5]"
	@echo ""
