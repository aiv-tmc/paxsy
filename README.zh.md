# Paxsy

**paxsy** 项目在 MIT 许可证下免费提供。您可以通过[此链接](https://github.com/aiv-tmc/Paxsi/blob/main/LICENSE)阅读许可证。

<!--版本状态-->
## 版本状态
最新: [Beta version 4 BlackBerry - v0.4.1_2a](https://github.com/aiv-tmc/Paxsy/tree/beta-4.1_2A-Rowan)

当前: [Beta version 4 BlackBerry - v0.4.1_2a](https://github.com/aiv-tmc/Paxsy/tree/beta-4.1_2A-Rowan)

稳定版: [Beta version 4 BlackBerry - v0.4.1_2a](https://github.com/aiv-tmc/Paxsy/tree/beta-4.1-2A-Rowan)

<!--安装-->
## 安装 (Linux)
您必须已安装[项目依赖项](https://github.com/aiv-tmc/Paxsy#dependencies)。

1.  克隆仓库：
    `$ git clone https://github.com/aiv-tmc/Paxsy.git`

2.  进入代码目录：
    `$ cd ~/Download/Paxsy/src/`

3.  开始编译程序：
    `$ sudo make all`

<!--语法高亮-->
## 语法高亮 (vim)
要在 **vim** 中添加语法高亮：

1.  将 **paxsy.vim** 文件移动到：`~/.vim/syntax/`

2.  将 **vim_highlight/ftdetect/** 目录中的内容移动到：`~/.vim/ftdetect/`

3.  确保文件 `~/.vimrc` 中**已安装 industry 配色方案**

附注：要激活自定义高亮，请在文件 `~/.vim/syntax/paxsy.vim` 中注释掉第 **67-80** 行（也可选注释第 **27**, **33** 和 **18-20** 行）。

<!--文档-->
## 文档
<!--文档可以通过[此链接](./docs/doc-en.md)获取。-->
目前暂无文档。

<!--依赖项-->
## 依赖项
本程序依赖于 **gcc** 解释器版本 **3.2** 或更高版本。
