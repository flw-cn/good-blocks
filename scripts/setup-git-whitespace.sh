#!/usr/bin/env bash
# Git 配置脚本：设置自动处理行尾空白的配置

echo "🔧 配置Git自动处理行尾空白..."

# 配置Git在提交时自动删除行尾空白
git config core.autocrlf false
git config core.safecrlf true

# 添加 .gitattributes 文件来定义文件的处理方式
cat > .gitattributes << 'EOF'
# 自动检测文本文件并进行行尾转换
* text=auto

# 明确指定源代码文件
*.c text eol=lf
*.h text eol=lf
*.cpp text eol=lf
*.hpp text eol=lf
*.py text eol=lf
*.js text eol=lf
*.md text eol=lf
*.txt text eol=lf
*.sh text eol=lf
*.yml text eol=lf
*.yaml text eol=lf
*.json text eol=lf
Makefile text eol=lf
*.mk text eol=lf

# 二进制文件
*.exe binary
*.bin binary
*.o binary
*.a binary
*.so binary
*.dll binary
*.png binary
*.jpg binary
*.jpeg binary
*.gif binary
*.ico binary
*.pdf binary
EOF

echo "✅ Git配置完成！"
echo ""
echo "📝 已创建的配置："
echo "   - core.autocrlf = false"
echo "   - core.safecrlf = true"
echo "   - .gitattributes 文件（定义文件类型处理规则）"
echo ""
echo "💡 接下来可以运行："
echo "   ./scripts/clean-whitespace.sh  # 清理现有文件"
echo "   git add ."
echo "   git commit -m 'Add whitespace cleanup tools and configure Git'"
