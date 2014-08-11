fsnet
=====

<<<<<<< HEAD
这是一个, libevent + ruby 为基础的网游服务器. 可支持分布式节点部署
=======
一个c+ruby支持分布式部署开源的网络游戏库


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

#jemalloc
解压jemalloc-3.6.0.tar.bz2
./configure
make
sudo make install

#fsnet
进入fsnet
执行
ruby extconf.rb
make & make install


#demo
进入scripts/ 
编辑server.rb
可以根据自己想看的demo取消注释. ：）

