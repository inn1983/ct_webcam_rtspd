CROSS_COMPILE=
ZQSENDER_ENABLE=false

CC= $(CROSS_COMPILE)gcc
CXX = $(CROSS_COMPILE)g++
STRIP = $(CROSS_COMPILE)strip
AR = $(CROSS_COMPILE)ar

CEDARX_CHIP_VERSION = A20

LIVE=./live
LIVE_INC=-I$(LIVE)/BasicUsageEnvironment/include -I$(LIVE)/groupsock/include -I$(LIVE)/UsageEnvironment/include -I$(LIVE)/liveMedia/include
LIVE_LIBS=$(LIVE)/liveMedia/libliveMedia.a \
	  $(LIVE)/groupsock/libgroupsock.a \
	  $(LIVE)/BasicUsageEnvironment/libBasicUsageEnvironment.a \
	  $(LIVE)/UsageEnvironment/libUsageEnvironment.a

SRCDIRS:=.\
		Camera \
		watermark

INCLUDES:=$(foreach dir,$(SRCDIRS),-I$(dir)) \
	-I./include \
	-I./lib \
	-I./include/include_system \
	-I./include/include_vencoder \
	-I./include/include_camera \
	-I./include/include_platform/CHIP_$(CEDARX_CHIP_VERSION)/disp \
	-I./include/include_platform/CHIP_$(CEDARX_CHIP_VERSION)

CFLAGS += -Wall -DOS_LINUX -O2 -g $(INCLUDES)
CXXFLAGS += -Wall -DOS_LINUX -O2 -g $(LIVE_INC) $(INCLUDES)
LIBS += ./lib/A20/libvencoder.so \
	$(LIVE_LIBS) -lpthread -lccgnu2 -ldl

ifeq ($(ZQSENDER_ENABLE), true)
	$(CXXFLAGS) += -DZQSENDER
	$(LIBS) += ./lib/libzqsender.a
endif

SRC := $(wildcard *.c) $(foreach dir,$(SRCDIRS),$(wildcard $(dir)/*.c))
SRCC := $(wildcard *.cpp)
OBJ := $(SRC:%.c=%.o) $(SRCC:%.cpp=%.o)

TARGET := ct_webcam_rtspd
.PHONY : clean all

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)
#	$(STRIP) $@

clean:
	@rm -f $(TARGET)
	@rm -f $(OBJ)

