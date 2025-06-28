#!/usr/bin/env bash
# 清理项目中所有文件的行尾空白

echo "🧹 开始清理项目文件的行尾空白..."

# 要处理的文件类型
file_patterns=(
    "*.c"
    "*.h"
    "*.cpp"
    "*.hpp"
    "*.py"
    "*.js"
    "*.md"
    "*.txt"
    "*.sh"
    "*.yml"
    "*.yaml"
    "*.json"
    "Makefile"
    "*.mk"
)

# 要排除的目录
exclude_dirs=(
    ".git"
    "build"
    "node_modules"
    ".vscode"
    ".idea"
)

# 构建find命令的排除参数
exclude_args=""
for dir in "${exclude_dirs[@]}"; do
    exclude_args="$exclude_args -path ./$dir -prune -o"
done

# 构建find命令的文件类型参数
name_args=""
for pattern in "${file_patterns[@]}"; do
    if [ -z "$name_args" ]; then
        name_args="-name '$pattern'"
    else
        name_args="$name_args -o -name '$pattern'"
    fi
done

# 查找并处理文件
echo "📁 搜索需要处理的文件..."

# 使用临时文件存储文件列表
temp_file=$(mktemp)

# 查找所有匹配的文件
eval "find . $exclude_args \( $name_args \) -type f -print" > "$temp_file"

file_count=$(wc -l < "$temp_file")
echo "📄 找到 $file_count 个文件需要处理"

if [ $file_count -eq 0 ]; then
    echo "✅ 没有找到需要处理的文件"
    rm "$temp_file"
    exit 0
fi

# 统计信息
processed_count=0
modified_count=0

echo ""
echo "🔧 开始处理文件..."

while IFS= read -r file; do
    if [ -f "$file" ]; then
        # 检查文件是否有行尾空白
        if grep -q '[[:space:]]$' "$file" 2>/dev/null; then
            echo "  🔄 处理: $file"
            # 创建备份（可选）
            # cp "$file" "$file.bak"

            # 删除行尾空白
            sed -i 's/[[:space:]]*$//' "$file"

            # 确保文件以换行符结尾
            if [ -s "$file" ] && [ "$(tail -c1 "$file")" != "" ]; then
                echo "" >> "$file"
            fi

            ((modified_count++))
        fi
        ((processed_count++))

        # 显示进度
        if ((processed_count % 10 == 0)); then
            echo "  📊 进度: $processed_count/$file_count"
        fi
    fi
done < "$temp_file"

rm "$temp_file"

echo ""
echo "✅ 处理完成！"
echo "📊 统计信息："
echo "   - 总计检查: $processed_count 个文件"
echo "   - 修改文件: $modified_count 个文件"

if [ $modified_count -gt 0 ]; then
    echo ""
    echo "💡 建议运行以下命令检查修改："
    echo "   git diff"
    echo "   git add ."
    echo "   git commit -m 'Clean trailing whitespace'"
fi

echo ""
echo "🎉 行尾空白清理完成！"
