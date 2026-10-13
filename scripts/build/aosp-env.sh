# source this before building: . /mnt/storage-adata/aosp-env.sh
export USE_CCACHE=1
export CCACHE_EXEC=/usr/bin/ccache
export CCACHE_DIR=/mnt/storage-adata/ccache
export PATH=$HOME/bin:$PATH
ulimit -n 65536 2>/dev/null
# 12 threads, 15GB RAM: 8 jobs, never 100% (see care-build-parallelism)
export NINJA_ARGS="-j4"
# shell/terminal are -1000 and children inherit it: make the build itself killable
echo 500 > /proc/self/oom_score_adj
