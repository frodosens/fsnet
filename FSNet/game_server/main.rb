

$LOAD_PATH.unshift(File.dirname(__FILE__))
$LOAD_PATH.unshift(File.dirname(__FILE__) + "/src")
$LOAD_PATH.unshift(File.dirname(__FILE__) + "/src/gamelib")
$LOAD_PATH.unshift(File.dirname(__FILE__) + "/src/boomman")

require 'game_main.rb'

Dir.chdir(File.dirname(__FILE__));

$game = GameBoomman.new();
$game.start();
