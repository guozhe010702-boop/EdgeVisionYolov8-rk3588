#!/bin/bash
# 脚本功能：自动清理Linux系统中各类下载残留文件（未完成包、临时文件等）
# 适用系统：Debian/Ubuntu、Fedora/CentOS、openSUSE 等主流Linux发行版

# 定义清理函数
clean_residuals() {
    echo "===== 开始清理下载残留文件 ====="
    
    # 1. 清理 APT 包管理器残留（Debian/Ubuntu系列）
    if [ -d "/var/cache/apt/archives/partial/" ]; then
        echo "→ 清理 APT 未完成包缓存..."
        sudo rm -rf /var/cache/apt/archives/partial/*
        sudo apt-get clean > /dev/null 2>&1  # 静默执行，仅清理缓存
    fi

    # 2. 清理 DNF/YUM 残留（Fedora/CentOS系列）
    if command -v dnf &> /dev/null; then
        echo "→ 清理 DNF 缓存文件..."
        sudo dnf clean packages > /dev/null 2>&1
    elif command -v yum &> /dev/null; then
        echo "→ 清理 YUM 缓存文件..."
        sudo yum clean packages > /dev/null 2>&1
    fi

    # 3. 清理 Zypper 残留（openSUSE）
    if command -v zypper &> /dev/null; then
        echo "→ 清理 Zypper 缓存文件..."
        sudo zypper clean > /dev/null 2>&1
    fi

    # 4. 清理 wget/curl/aria2 临时文件（当前用户目录及下载目录）
    echo "→ 清理命令行工具临时文件..."
    rm -rf ~/*.part ~/*.aria2 ~/downloads/*.part ~/downloads/*.aria2  # 个人下载目录
    rm -rf /tmp/*.part /tmp/*.aria2  # 系统临时目录

    # 5. 清理浏览器下载残留（Chrome/Firefox）
    echo "→ 清理浏览器下载残留..."
    rm -rf ~/.config/google-chrome/Default/Downloads/*.crdownload  # Chrome
    rm -rf ~/.mozilla/firefox/*.default-release/downloads/*.part  # Firefox

    echo "===== 残留文件清理完成！ ====="
}

# 执行清理并捕获错误
if [ "$(id -u)" -ne 0 ]; then
    echo "错误：请使用 sudo 权限运行脚本（如 sudo bash clean_residuals.sh）"
    exit 1
fi

clean_residuals
