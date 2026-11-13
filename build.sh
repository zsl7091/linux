#!/bin/bash

# 1. 定义目标架构和交叉编译器前缀
export ARCH=arm64
export CROSS_COMPILE=aarch64-linux-gnu-

# 2. 如果您需要指定特定的交叉编译器路径，可以取消下面这行的注释并修改路径
# export PATH=/path/to/your/compiler/bin:$PATH

# 3. 执行真正的 make 命令，"$@" 会把您传给 build.sh 的所有参数原封不动地传给 make
make "$@"
