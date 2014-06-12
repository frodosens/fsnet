class GameBoomman
	
	
	def start
		  
		  
		begin
			configure_file = File.open(File.dirname(__FILE__) + "/game_configure.yaml");
			$game_configure = YAML.load(configure_file)
			configure_file.close();
		rescue => err
			print(err.message + "\n");
			exit
		end
		
		$game_res      = GameManagerResoucre.new()
		$game_database = GameDatabase.new();
		$game_users    = GameUserManager.new();
		$game_players  = GamePlayerManager.new();
	
	
		$db_server = DBServer.new("configure/db_configure/configure.yaml");
		$db_server.start();
		$login_server = LoginServer.new("configure/login_configure/configure.yaml");
		$login_server.start();
		$run_server = RunServer.new("configure/run_configure/configure.yaml");
		$run_server.start();
		$gate_server = GateServer.new("configure/gate_configure/configure.yaml");
		$gate_server.start();
		
		
	end
	
end
