
$LOAD_PATH.unshift(File.dirname(__FILE__))
$LOAD_PATH.unshift(File.dirname(__FILE__) + "/src")

# require stdlib
require 'enc/encdb'
require 'rubygems'
require 'src/hello_server.rb'

Dir.chdir(File.dirname(__FILE__));
$http_server = HelloServer.new("hello_server")
$http_server.start_server("0.0.0.0", 3001)
