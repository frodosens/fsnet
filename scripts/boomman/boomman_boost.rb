
$LOAD_PATH.unshift(File.dirname(__FILE__))
$LOAD_PATH.unshift(File.dirname(__FILE__) + "/src")

# require stdlib
require 'enc/encdb'
require 'rubygems'
require 'yaml'
require 'logger'

require 'db_server.rb'
require 'login_server.rb'
require 'run_server.rb'
require 'util_server.rb'
require 'gate_server.rb'
require 'game_main.rb'

Dir.chdir(File.dirname(__FILE__));
$game = GameBoomman.new();
$game.start();
