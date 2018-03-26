# Fire(Merlin) C Server

基於 C 編寫的 Server 服務器端，與 Fire Router 進行通信



## 目錄

##### asuswrt-merlin-build

交叉編譯 Dockerfile

##### shBuildForInstall

為 install 安裝文件打包

##### shDocker

docker 操作腳本

##### sourceFireServer

核心代碼

* fireServer.c 通信服務器
* fireClientSend.c 發送信息
* fireClientRead.c 讀取信息

##### sourceTestClient

測試客戶端



## TODO

- [x] Socket 通信基礎層搭建


- [ ] Socket 各平台客戶端

1. iOS
2. Android
3. Mac
4. Windows
5. Linux
6. ARM-Linux

- [x] 指令執行
- [x] 指令 stdout 回顯（Socket）
- [ ] 通信加密


1. Base64
2. Base64 + 鹽


## 服務器端初始化流程

1. asuswrt-merlin-build 下運行 download_merlin.sh 下載梅林源碼
2. shDocker 下 install.sh 初始化 Docker 的 交叉編譯 環境
3. 在「sourceFireServer」下建立「lib」放置 libevent-2.1.8-stable 庫文件

使用前注意修改 **install.sh** 裡面的文件位置



## 服務器端的編譯流程

1. shDocker: run.sh 進入 交叉編譯 容器
2. 打開 root/build 目錄，運行 shBuild-Server.sh 腳本





## 編譯 fireServer

```bash
arm-linux-g++ fireServer.cpp -o ./Binary/fireServer -I/opt/crossinstall/libevent/include/ -L/opt/crossinstall/libevent/lib/ -lrt -levent -static
```



## 編譯 libevent

##### 靜態編譯

```bash
./configure -disable-shared -enable-static --prefix=/opt/crossinstall/libevent --host=arm-linux CC=arm-linux-gcc CXX=arm-linux-g++
```

##### 動態編譯

```bash
./configure --prefix=/opt/crossinstall/libevent --host=arm-linux CC=arm-linux-gcc CXX=arm-linux-g++
make
make install
```





## 參考

http://telegra.ph/交叉编译原版shadowsocks为koolshare梅林ss插件续命-08-07



## 問題解決

##### arm-linux-gcc 權限問題

```Bash
➜  build arm-linux-gcc -v           
zsh: permission denied: arm-linux-gcc
```

```bash
cd /home/asuswrt-merlin/release/src-rt-6.x.4708/toolchains/hndtools-arm-linux-2.6.36-uclibc-4.5.3/bin/

chmod +x arm-linux-gcc
```

