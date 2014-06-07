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
		
		$game_database = GameDatabase.new();
		$game_users    = GameUserManager.new();
		$game_players  = GamePlayerManager.new();
		
		# p $game_users.regist_user("a1", "b1", "c");
		user = $game_users.find_user_by_account("admin","admin");
		player = user.get_player();
		# player.gain_gold(100);
		# player.name = "中文";
		# player.cache();
		# player.save();
		p player
		$login_server = LoginServer.new("configure/login_configure/configure.yaml");
		$login_server.start();
		$run_server = RunServer.new("configure/run_configure/configure.yaml");
		$run_server.start();
		$gate_server = GateServer.new("configure/gate_configure/configure.yaml");
		$gate_server.start();
		
		
	end
	
end
