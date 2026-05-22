ifeq (,$(CROSS_COMPILE))
$(error missing CROSS_COMPILE for this toolchain)
endif

TARGET=libhookdrastic

REMOTE_ADDR=192.168.7.42
REMOTE_PATH=/mnt/mmc/system/lib
REMOTE_DISK=root@$(REMOTE_ADDR)

SSH := sshpass -p root ssh $(REMOTE_DISK)
SCP := sshpass -p root scp

all: build upload
auth:
	ssh $(REMOTE_DISK) exit
build:
	$(CROSS_COMPILE)gcc $(TARGET).c xgl.c -o $(TARGET).so -shared -lSDL2_image -lGLESv2 -fPIC
upload:
	$(SCP) "$(TARGET).so" "$(REMOTE_DISK):$(REMOTE_PATH)/$(TARGET).so"
	$(SSH) "killall -s kill drastic"
clean:
	rm -rf $(TARGET).so