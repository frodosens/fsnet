
unless ARGV.include?("-xcode")
    require 'fsnet'
end

module FSNET
	
	class << self
		
		alias :game_init :init
    
		def init
			game_init();
			init_load_path();
		end
	
		def run

			FSNET.init

			if(check_daemon)
				set_daemon
			end
			require 'server.rb'
			
			FSNET.main_loop
			
		end
	
		
		def show_help
			print "usage:\n"
			print " -d : deamon\n"
			exit()
		end
		
		def set_daemon
			Process.daemon(true)	
		end
		
		def check_daemon
			return ARGV.include?("-d")
		end
		
		def init_load_path
			
			$LOAD_PATH.unshift(File.dirname(__FILE__))
			$LOAD_PATH.unshift(File.dirname(__FILE__) + "/rubylib");
			$LOAD_PATH.unshift(File.dirname(__FILE__) + "/gamelib");
			$LOAD_PATH.unshift(File.dirname(__FILE__) + "/mobile_server");
			Dir.chdir(File.dirname(__FILE__));
			
		end
		
	end
	
	
end


FSNET.run