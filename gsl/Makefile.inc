# GSL vendored build rules
GSL_DIR := $(CURDIR)/gsl
GSL_CFLAGS := -I$(GSL_DIR) -DHAVE_GSL_VENDORED
GSL_OBJS := \
    $(GSL_DIR)/src/rng.o \
    $(GSL_DIR)/src/randist.o \
    $(GSL_DIR)/src/cdf.o \
    $(GSL_DIR)/src/statistics.o \
    $(GSL_DIR)/src/fit.o \
    $(GSL_DIR)/src/sort.o \
    $(GSL_DIR)/src/histogram.o \
    $(GSL_DIR)/src/matrix.o \
    $(GSL_DIR)/src/vector.o \
    $(GSL_DIR)/src/blas.o \
    $(GSL_DIR)/src/permutation.o \
    $(GSL_DIR)/src/combination.o \
    $(GSL_DIR)/src/integration.o \
    $(GSL_DIR)/src/monte.o \
    $(GSL_DIR)/src/multifit.o \
    $(GSL_DIR)/src/odeiv2.o \
    $(GSL_DIR)/src/wavelet.o \
    $(GSL_DIR)/src/specfunc.o \
    $(GSL_DIR)/src/complex.o \
    $(GSL_DIR)/src/sys.o \
    $(GSL_DIR)/src/ieee_utils.o \
    $(GSL_DIR)/src/test.o \
    $(GSL_DIR)/src/err.o

BASE_CFLAGS += $(GSL_CFLAGS)
LDLIBS += -lm

$(GSL_DIR)/src/%.o: $(GSL_DIR)/src/%.c
	$(CC) $(BASE_CFLAGS) -c $< -o $@