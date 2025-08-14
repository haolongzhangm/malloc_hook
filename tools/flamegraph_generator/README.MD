# 一步生成SVG火焰图（默认输出为flamegraph.svg）
python3 flamegraph_generator.py backtrace_heap.exit.1755078753.txt

# 指定输出文件
python3 flamegraph_generator.py backtrace_heap.exit.1755078753.txt -o my_heap_flamegraph.svg

# 只生成文本格式（用于手动处理）
python3 flamegraph_generator.py backtrace_heap.exit.1755078753.txt --text-only

# 指定文本格式输出文件
python3 flamegraph_generator.py backtrace_heap.exit.1755078753.txt --text-only --text-file heap_data.txt