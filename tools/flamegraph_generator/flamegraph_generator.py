#!/usr/bin/env python3
# unified_flamegraph_generator.py

import re
from collections import defaultdict
import argparse
import subprocess
import sys
import os

def clean_function_name(func_info):
    """
    清理和标准化函数名
    """
    # 移除地址信息，保留符号信息
    # 如果有括号和加号，例如: __dl__ZN6soinfo17call_constructorsEv+616
    # 我们保留完整的符号名
    return func_info

def parse_backtrace_file(file_path):
    """
    解析backtrace文件，提取调用栈信息
    """
    stacks = []
    current_stack = []
    stack_info = {}
    
    with open(file_path, 'r') as f:
        lines = f.readlines()
    
    i = 0
    while i < len(lines):
        line = lines[i].strip()
        
        # 跳过头部信息行
        if line.startswith("backtrace_heap.exit") or \
           line.startswith("current host used") or \
           line.startswith("++++++++++++++++"):
            i += 1
            continue
        
        # 匹配分配信息行
        if line.startswith("alloc_size:"):
            # 提取分配信息
            info_parts = line.split()
            alloc_size = float(info_parts[0].split(":")[1].replace("KB", ""))
            alloc_type = info_parts[1].split(":")[1]
            alloc_num = int(info_parts[2].split(":")[1])
            
            stack_info = {
                'size': alloc_size,
                'type': alloc_type,
                'num': alloc_num
            }
            current_stack = []
        
        # 匹配调用栈行
        elif line.startswith("#"):
            # 提取调用栈信息
            match = re.match(r"#\d+\s+[0-9a-fA-F]+\s+(.+)", line)
            if match:
                func_info = match.group(1).strip()
                # 清理函数名
                cleaned_func_name = clean_function_name(func_info)
                current_stack.append(cleaned_func_name)
        
        # 当遇到空行或文件结束时，保存当前栈
        if not line and current_stack:
            # 保存当前栈
            stacks.append({
                'info': stack_info,
                'stack': list(reversed(current_stack))  # 火焰图需要从根开始
            })
            current_stack = []
            stack_info = {}
        elif i == len(lines) - 1 and current_stack:
            # 处理文件末尾的最后一个栈
            stacks.append({
                'info': stack_info,
                'stack': list(reversed(current_stack))
            })
        
        i += 1
    
    return stacks

def generate_flamegraph_input(stacks, output_file):
    """
    生成火焰图输入文件
    """
    # 统计调用栈
    stack_counts = defaultdict(float)
    
    for stack in stacks:
        # 将调用栈转换为字符串
        stack_str = ";".join(stack['stack'])
        # 使用分配大小作为权重
        stack_counts[stack_str] += stack['info']['size'] * stack['info']['num']
    
    # 写入输出文件
    with open(output_file, 'w') as f:
        for stack, count in stack_counts.items():
            f.write(f"{stack} {count}\n")
    
    return len(stack_counts)

def check_flamegraph_tool():
    """
    检查系统中是否有flamegraph.pl工具
    """
    # 检查是否在PATH中
    flamegraph_paths = [
        "./flamegraph.pl",
        "flamegraph.pl",
        "/usr/local/bin/flamegraph.pl",
        "../FlameGraph/flamegraph.pl"
    ]
    
    for path in flamegraph_paths:
        if os.path.exists(path) and os.access(path, os.X_OK):
            return path
    
    # 尝试在常见位置查找
    common_paths = [
        "/opt/FlameGraph/flamegraph.pl",
        "/usr/share/flamegraph/flamegraph.pl",
        "/usr/local/share/flamegraph/flamegraph.pl"
    ]
    
    for path in common_paths:
        if os.path.exists(path) and os.access(path, os.X_OK):
            return path
    
    return None

def main():
    parser = argparse.ArgumentParser(
        description='Generate flamegraph from backtrace file with full symbol information',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='''
Examples:
  %(prog)s input.txt                           # Generate SVG to flamegraph.svg
  %(prog)s input.txt -o my_flamegraph.svg      # Generate SVG to custom file
  %(prog)s input.txt --text-only               # Only generate text format
        ''')
    
    parser.add_argument('input', help='Input backtrace file')
    parser.add_argument('-o', '--output', default='flamegraph.svg',
                       help='Output SVG file (default: flamegraph.svg)')
    parser.add_argument('--text-only', action='store_true',
                       help='Only generate text format, skip SVG generation')
    parser.add_argument('--text-file', default='flamegraph.txt',
                       help='Text format output file (default: flamegraph.txt)')
    
    args = parser.parse_args()
    
    # 检查输入文件
    if not os.path.exists(args.input):
        print(f"Error: Input file '{args.input}' not found", file=sys.stderr)
        sys.exit(1)
    
    print(f"Parsing backtrace file: {args.input}")
    
    # 解析backtrace文件
    try:
        stacks = parse_backtrace_file(args.input)
        print(f"Processed {len(stacks)} stacks")
    except Exception as e:
        print(f"Error parsing backtrace file: {e}", file=sys.stderr)
        sys.exit(1)
    
    # 生成火焰图输入文件
    try:
        unique_stacks = generate_flamegraph_input(stacks, args.text_file)
        print(f"Generated flamegraph input with {unique_stacks} unique stack patterns in {args.text_file}")
    except Exception as e:
        print(f"Error generating flamegraph input: {e}", file=sys.stderr)
        sys.exit(1)
    
    # 如果只需要文本格式，则退出
    if args.text_only:
        print(f"Text format saved to {args.text_file}")
        return
    
    # 查找flamegraph.pl工具
    flamegraph_cmd = check_flamegraph_tool()
    
    # 如果没找到，创建内嵌版本
    if not flamegraph_cmd:
        print("flamegraph.pl not found")
        sys.exit(1)
    
    # 生成SVG火焰图
    try:
        print(f"Generating SVG flamegraph: {args.output}")
        with open(args.output, 'w') as svg_file:
            cmd = [flamegraph_cmd, args.text_file, '--colors', 'mem', '--countname', 'KB']
            print(f"Executing command: {' '.join(cmd)}")
            result = subprocess.run(cmd,
                                  stdout=svg_file, stderr=subprocess.PIPE, 
                                  text=True)
            
            if result.returncode != 0:
                print(f"Warning: flamegraph.pl exited with code {result.returncode}")
                if result.stderr:
                    print(f"stderr: {result.stderr}")
            else:
                print(f"SVG flamegraph saved to {args.output}")
                
    except FileNotFoundError:
        print("Warning: flamegraph.pl not found and could not be created.")
        print("To generate SVG, please:")
        print("1. Install FlameGraph tools from https://github.com/brendangregg/FlameGraph")
        print("2. Or run with --text-only to only generate text format")
        print(f"Text format is available in {args.text_file}")
        
    except Exception as e:
        print(f"Error generating SVG: {e}", file=sys.stderr)
        sys.exit(1)

if __name__ == "__main__":
    main()