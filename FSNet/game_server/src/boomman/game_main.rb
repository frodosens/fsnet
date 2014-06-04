
require 'yaml'
require "gate_server.rb"
require "run_server.rb"
require "login_server.rb"


class GameBoomman
	
	
	def start
		
		
		begin
			configure_file = File.open(File.dirname(__FILE__) + "/game_configure.yaml");
			$game_configure = YAML.load(configure_file);
			configure_file.close();
		rescue => err
			print(err.message + "\n");
			exit
		end
		
		$login_server = LoginServer.new("configure/login_configure/configure.yaml");
		$login_server.start();
		$run_server = RunServer.new("configure/run_configure/configure.yaml");
		$run_server.start();
		$gate_server = GateServer.new("configure/gate_configure/configure.yaml");
		$gate_server.start();
		
		
		
	end
	
end
