

$LOAD_PATH.unshift(File.dirname(__FILE__))
$LOAD_PATH.unshift(File.dirname(__FILE__) + "/rubylib");
$LOAD_PATH.unshift(File.dirname(__FILE__) + "/gamelib");

Dir.chdir(File.dirname(__FILE__));

require 'boomman/boomman_boost.rb'
