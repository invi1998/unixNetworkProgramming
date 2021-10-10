# 定义项目编译的根目录，通过export 把某个变量声明为全局的[其他文件中可以使用的]，这里获取当前这个文件坐在的路径作为
export BUILD_ROOT = $(shell pwd)

# 定义头文件所在的路径变量
export INCLUDE_PATH = $(BUILD_ROOT)/_include

# 定义我们要编译的目录
BUILD_DIR = $(BUILD_ROOT)/signal/ \
			$(BUILD_ROOT)/app/

# 编译时是否生成调试信息。GNU调试器可以利用该信息
# 很多调试工具，包括valgrind工具集也都会因为这个true能够输出更多的调试信息
export DEBUG = true