
$LOAD_PATH.unshift(File.dirname(__FILE__))
$LOAD_PATH.unshift(File.dirname(__FILE__) + "/src")

# require stdlib
require 'enc/encdb'
require 'rubygems'
require 'yaml'
require 'src/simple_server.rb'

Dir.chdir(File.dirname(__FILE__));
$simple_server = SimpleServer.new("src/configure.yaml")
$simple_server.start()
