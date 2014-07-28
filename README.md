fsnet
=====

<<<<<<< HEAD
这是一个, libevent + ruby 为基础的网游服务器. 可支持分布式节点部署
=======
一个c+ruby支持分布式部署开源的网络游戏库

#編譯為ruby擴展庫
進入fsnet
ruby extconf.rb
make & make install
然後require 'fsnet'
即可 :) 
也支持C嵌入使用. 

# 如何编译,安装

#libevent
解压libs里的libevent-2.1.4-alpha.zip
cd libevent-2.1.4-alpha
./configure 
make
sudo make install

#ruby
解压libs 里的ruby-2.1.2.zip 
cd ruby-2.1.2
./configure --enable-shared
make
sudo make install


#fsnet
cd fanet
make
sudo make install


#编译你的游戏载体
会到项目根路径
make
这样你就会的得到一个game 

#编写你的游戏服务器脚本
进入/scripts

boomman 是测试服务器的demo,可以看看


