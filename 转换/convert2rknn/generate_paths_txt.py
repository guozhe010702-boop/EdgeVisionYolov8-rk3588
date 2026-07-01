import os

def images_to_abs_path_txt(
    img_folder: str,        # 图片所在文件夹路径
    output_txt: str,        # 输出txt文件路径（含文件名）
    include_subfolders: bool = True,  # 是否遍历子文件夹（默认开启）
    img_suffixes: tuple = ('.jpg', '.jpeg', '.png', '.bmp', '.gif', '.tiff')  # 支持的图片格式
):
    # 1. 校验图片文件夹是否存在
    if not os.path.exists(img_folder):
        print(f"❌ 错误：图片文件夹不存在 → {img_folder}")
        return
    
    # 2. 初始化路径集合（自动去重）
    img_abs_paths = set()
    
    # 3. 遍历文件夹，筛选图片并提取绝对路径
    for root, _, files in os.walk(img_folder):
        for file in files:
            # 筛选指定格式的图片文件（忽略大小写）
            if file.lower().endswith(img_suffixes):
                # 拼接绝对路径并添加到集合
                abs_path = os.path.abspath(os.path.join(root, file))
                img_abs_paths.add(abs_path)
    
    # 4. 校验是否找到图片
    if not img_abs_paths:
        print(f"❌ 未在文件夹 {img_folder} 中找到任何图片（支持格式：{img_suffixes}）")
        return
    
    # 5. 写入txt文件（每行1个绝对路径，按路径排序更整齐）
    with open(output_txt, 'w', encoding='utf-8') as f:
        for path in sorted(img_abs_paths):
            f.write(f"{path}\n")
    
    # 6. 输出结果提示
    print(f"✅ 成功生成txt文件！")
    print(f"📁 图片文件夹：{img_folder}")
    print(f"📄 输出txt路径：{os.path.abspath(output_txt)}")
    print(f"🖼️  包含图片数量：{len(img_abs_paths)}")


# -------------------------- 【用户需修改这3处参数】 --------------------------
if __name__ == "__main__":
    # 1. 图片所在文件夹（改成你的图片目录，绝对/相对路径均可）
    IMG_FOLDER = "/home/beast/RK3588/convert2rknn/shushe/images/train"  # 示例
    
    # 2. 输出txt文件路径（含文件名，比如存到数据集根目录）
    OUTPUT_TXT = "/home/beast/RK3588/convert2rknn/paths.txt"  

    
    # 3. 是否遍历子文件夹（True=遍历，False=只取当前文件夹）
    INCLUDE_SUBFOLDERS = True
    
    # 执行函数
    images_to_abs_path_txt(
        img_folder=IMG_FOLDER,
        output_txt=OUTPUT_TXT,
        include_subfolders=INCLUDE_SUBFOLDERS
    )
