
$LOAD_PATH.unshift(File.dirname(__FILE__))
$LOAD_PATH.unshift(File.dirname(__FILE__) + "/src")

# require stdlib
require 'enc/encdb'
require 'rubygems'

require 'game_main.rb'

Dir.chdir(File.dirname(__FILE__));
$game = GameBoomman.new();
$game.start();
