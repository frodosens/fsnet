require 'fsnet'



FSNET.init

$LOAD_PATH.unshift(File.dirname(__FILE__))
$LOAD_PATH.unshift(File.dirname(__FILE__) + "/rubylib");
$LOAD_PATH.unshift(File.dirname(__FILE__) + "/gamelib");

Dir.chdir(File.dirname(__FILE__));

if(ARGV.include?("-d"))
	Process.daemon(true)	
end

require 'boomman/boomman_boost.rb'


FSNET.main_loop