
$LOAD_PATH.unshift(File.dirname(__FILE__))
$LOAD_PATH.unshift(File.dirname(__FILE__) + "/src")

# require stdlib
require 'enc/encdb'
require 'rubygems'
require 'yaml'

require 'src/gate_server.rb'
require 'src/simple_server.rb'
require 'src/time_server.rb'

Dir.chdir(File.dirname(__FILE__));


$simple_server = SimpleServer.new("src/simple_configure.yaml")
$simple_server.start()

$time_server = TimeServer.new("src/time_configure.yaml")
$time_server.start()

$gate_server = GateServer.new("src/gate_configure1.yaml")
$gate_server.start()