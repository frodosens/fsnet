$LOAD_PATH.unshift(File.dirname(__FILE__))
$LOAD_PATH.unshift(File.dirname(__FILE__) + "/src")

# require stdlib
require 'enc/encdb'
require 'rubygems'
require 'yaml'

require 'src/util/uuid.rb'
require 'src/util/hash.rb'
require 'src/util/array.rb'
require 'src/util/bool.rb'
require 'src/util/pack.rb'
require 'src/gate_server/gate_server.rb'
require 'src/node_server/node_server.rb'
require 'src/center_server/center_server.rb'


Dir.chdir(File.dirname(__FILE__));


$gate_server1 = GateServer.new("src/gate_server/gate_configure1.yaml")
$gate_server1.start()
$gate_server2 = GateServer.new("src/gate_server/gate_configure2.yaml")
$gate_server2.start()


$center_server = CenterServer.new("src/center_server/center_configure.yaml")
$center_server.start()

$node_server1 = NodeServer.new("src/node_server/node_configure1.yaml")
$node_server1.start()
$node_server2 = NodeServer.new("src/node_server/node_configure2.yaml")
$node_server2.start()


