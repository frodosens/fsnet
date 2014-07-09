
$LOAD_PATH.unshift(File.dirname(__FILE__))
$LOAD_PATH.unshift(File.dirname(__FILE__) + "/src")

#_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/
# 依赖gem库
# libxml-ruby		
# =>  apt-get install libxml2
# => apt-get install libxml-dev
#
# mysql2	
# => apt-get install libmysql-ruby libmysqlclient-dev
#
# redis
#
# openssl 
# => apt-get install openssl
# => apt-get install libssl-dev
# => apt-get install libssl0.9.8
#
# 安装ruby扩展方法
# 先进入 ruby/ext/扩展子路径
# ruby extconf.rb
# make
# make install
# _/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/_/


# require stdlib
require 'enc/encdb'
require 'rubygems'
require "xml"  
require 'csv'
require 'yaml'
require 'mysql2.rb'
require 'redis.rb'


require 'db_server.rb'
require "gate_server.rb"
require "run_server.rb"
require "login_server.rb"
require "util_server.rb"

Dir[File.dirname(__FILE__) + '/src/manager/*.rb'].each {|file| require file } 
Dir[File.dirname(__FILE__) + '/src/cmds/*.rb'].each {|file| require file }
Dir[File.dirname(__FILE__) + '/src/utils/*.rb'].each {|file| require file } 
Dir[File.dirname(__FILE__) + '/src/modules/*.rb'].each {|file| require file }
Dir[File.dirname(__FILE__) + '/src/modules/templete/*.rb'].each {|file| require file }
Dir[File.dirname(__FILE__) + '/src/ext/*.rb'].each {|file| require file }
Dir[File.dirname(__FILE__) + '/src/ext/*/*.rb'].each {|file| require file }

require 'game_main.rb'

Dir.chdir(File.dirname(__FILE__));
$game = GameBoomman.new();
$game.start();
