.PHONY: build

BUILD_HASH:=$(shell git rev-parse --short HEAD)
RELEASE_TIME:=$(shell TZ=GMT date +%Y%m%d)
RELEASE_BETA=beta-
RELEASE_BASE=dedicated-rgds-$(RELEASE_BETA)$(RELEASE_TIME)
RELEASE_DOT:=$(shell find -E ./releases/. -regex ".*/${RELEASE_BASE}-[0-9]+\.zip" 2>/dev/null | wc -l | sed 's/ //g')
RELEASE_NAME=$(RELEASE_BASE)-$(RELEASE_DOT)

all: setup build package done

setup:
	# make sure we're running in an input device
	tty -s 
	rm -rf ./build
	mkdir -p ./releases
	cp -R ./skeleton ./build
	cd ./build && find . -type f -name '.keep' -delete

build:
	cd source/libhookdrastic && make
	
package:
	cp source/libhookdrastic/libhookdrastic.so ./build/system/lib/
	cd ./build/system && echo "$(RELEASE_NAME)\n$(BUILD_HASH)" > version.txt
	cd ./build && find . -type f -name '.DS_Store' -delete
	cd ./build && zip -r system.zip system && rm -rf system
	cd ./build && zip -r ../releases/$(RELEASE_NAME).zip .

done:
	@echo "$(RELEASE_NAME) done!"
	say "done" 2>/dev/null || true
