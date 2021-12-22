# This is the common makerule for reference

TARGET = screencap
OBJ = screencap.o buf_manage.o g2d_manage.o


INCLUDES += -I$(SDKTARGETSYSROOT)/usr/include
INCLUDES += -I$(SDKTARGETSYSROOT)/usr/include/libdrm

CC = $(CROSS_COMPILE)gcc
CPP = $(CROSS_COMPILE)g++
LD = $(CROSS_COMPILE)gcc

# CFLAGS
CFLAGS += -DLINUX -Wall -c

# LDFLAGS
LDFLAGS += -L$(SDKTARGETSYSROOT)/usr/lib
LDFLAGS +=  --sysroot=${SDKTARGETSYSROOT}

%.o:%.c
	$(CC) $(CFLAGS) $(INCLUDES) $(@D)/$(<F) -o $(@D)/$(@F) 

%.o:%.cpp
	$(CPP) $(CFLAGS) $(INCLUDES) $(@D)/$(<F) -o $(@D)/$(@F)
	

DEST_PATH=$(TARGET_ROOTFS)/bin

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $^ -o $@ $(LDFLAGS)  -ldrm -lpthread -lg2d	
	
.PHONY: clean

clean:
	- rm -f $(TARGET) $(OBJ)

