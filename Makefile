# EmbMQTTNode Makefile
# 顶层 Makefile，进入 src/ 构建主程序

.PHONY: all clean test

all:
	$(MAKE) -C src

clean:
	$(MAKE) -C src clean
	$(MAKE) -C tests clean

test:
	$(MAKE) -C tests
	cd tests && ./test_sensor && ./test_storage
