
$LOAD_PATH.unshift(File.dirname(__FILE__))
$LOAD_PATH.unshift(File.dirname(__FILE__) + "/src")

class Encoding
	
	ASCII_8BIT = "ASCII-8BIT"
	US_ASCII = 'US-ASCII'
	UTF_8 = "UTF-8"
	
end

require 'rubygems'
require 'csv'
require 'yaml'
require 'mysql2.rb'
require 'redis.rb'
require "gate_server.rb"
require "run_server.rb"
require "login_server.rb"

Dir[File.dirname(__FILE__) + '/src/cmds/*.rb'].each {|file| require file }
Dir[File.dirname(__FILE__) + '/src/manager/*.rb'].each {|file| require file }
Dir[File.dirname(__FILE__) + '/src/modules/*.rb'].each {|file| require file }

require 'game_main.rb'

Dir.chdir(File.dirname(__FILE__));

$game = GameBoomman.new();
$game.start();
