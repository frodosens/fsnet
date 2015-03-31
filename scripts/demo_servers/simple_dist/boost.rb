$LOAD_PATH.unshift(File.dirname(__FILE__))
$LOAD_PATH.unshift(File.dirname(__FILE__) + "/src")



# require stdlib

# require extend channel
require 'channellib/channel_boost.rb'

# require services
require 'src/gate_server/gate_server.rb'
require 'src/database_server/database_server.rb'
require 'src/node_server/node_server.rb'
require 'src/center_server/center_server.rb'


Dir.chdir(File.dirname(__FILE__));


$center_server = CenterServer.new("src/center_server/center_configure.yaml")
$center_server.start()

$database_server = DatabaseServer.new("src/database_server/database_configure.yaml")
$database_server.start()

$gate_server1 = GateServer.new("src/gate_server/gate_configure1.yaml")
$gate_server1.start()
$gate_server2 = GateServer.new("src/gate_server/gate_configure2.yaml")
$gate_server2.start()

$node_server1 = NodeServer.new("src/node_server/node_configure1.yaml")
$node_server1.start()
$node_server2 = NodeServer.new("src/node_server/node_configure2.yaml")
$node_server2.start()


