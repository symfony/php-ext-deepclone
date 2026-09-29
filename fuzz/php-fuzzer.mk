DEEPCLONE_FUZZER_OBJ := $(DEEPCLONE_PROJECT)/fuzz/deepclone_from_array_fuzzer.lo
DEEPCLONE_FUZZER_BIN := $(DEEPCLONE_PROJECT)/fuzz/deepclone_from_array_fuzzer

.PHONY: deepclone-fuzzer
deepclone-fuzzer: $(DEEPCLONE_FUZZER_BIN)

$(DEEPCLONE_FUZZER_OBJ): $(DEEPCLONE_PROJECT)/fuzz/deepclone_from_array_fuzzer.cc
	$(LIBTOOL) --tag=CXX --mode=compile $(CXX) -Isapi/fuzzer/ -I$(srcdir)/sapi/fuzzer/ $(COMMON_FLAGS) $(CXXFLAGS_CLEAN) $(EXTRA_CFLAGS) -fno-sanitize=leak -DDEEPCLONE_FUZZ_EXTENSION='"$(DEEPCLONE_PROJECT)/modules/deepclone.so"' -c $< -o $@

$(DEEPCLONE_FUZZER_BIN): $(PHP_GLOBAL_OBJS) $(PHP_BINARY_OBJS) sapi/fuzzer/fuzzer-sapi.lo $(DEEPCLONE_FUZZER_OBJ)
	$(FUZZER_BUILD) sapi/fuzzer/fuzzer-sapi.lo $(DEEPCLONE_FUZZER_OBJ) -o $@
