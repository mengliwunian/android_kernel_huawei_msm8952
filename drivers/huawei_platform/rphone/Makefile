ifeq ($(CONFIG_KRETPROBES), y)
KERNEL_DIR=$(PWD)
EXTRA_CFLAGS += -Ifs/
EXTRA_CFLAGS += -I$(KERNEL_DIR)/
EXTRA_CFLAGS += -fno-pic
LOCAL_MODULE_PATH:=$(TARGET_OUT)/../rphone
obj-m := rfile.o
rfile-objs := rfile/rasfile.o rasbase/rasbase.o rasbase/rasprobe.o rasbase/rasproc.o

obj-m += rmem.o
rmem-objs := rmem/rasmemory.o rasbase/rasbase.o  rasbase/rasproc.o

obj-m += rSd.o
rSd-objs := rsd/rassd.o rasbase/rasbase.o  rasbase/rasprobe.o rasbase/rasproc.o

obj-m += rEmmc.o
rEmmc-objs := remmc/rasemmc.o rasbase/rasbase.o  rasbase/rasprobe.o rasbase/rasproc.o

obj-m += rIO.o
rIO-objs := rio/rasio.o rasbase/rasbase.o  rasbase/rasprobe.o rasbase/rasproc.o

obj-m += rF2fs.o
rF2fs-objs := rf2fs/rasf2fs.o rasbase/rasbase.o  rasbase/rasprobe.o rasbase/rasproc.o
endif
