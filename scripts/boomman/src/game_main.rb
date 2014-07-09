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
		  
		@logger_file = Logger.new($game_configure["log_name"])
		
		info "=== GAME SERVER START ==="
		info "=== LOAD RES ==="
		$game_res      = GameManagerResoucre.new()
		
		relose_resource();
		
		info "=== CONNECT DB ==="
		$game_database = GameDatabase.new();
		info "=== INIT USER MGR ==="
		$game_users    = GameUserManager.new();
		info "=== INIT PLAYER MGR ==="
		$game_players  = GamePlayerManager.new();
		
		$db_server = DBServer.new("configure/db_configure/configure.yaml");
		$db_server.start();
		$login_server = LoginServer.new("configure/login_configure/configure.yaml");
		$login_server.start();
		$run_server = RunServer.new("configure/run_configure/configure.yaml");
		$run_server.start();
		$gate_server = GateServer.new("configure/gate_configure/configure.yaml");
		$gate_server.start();
		
		
		$pay_server = PayServer.new("pay_server")
		$pay_server.start_server("127.0.0.1", 50566)
		
	end
	
	def relose_resource
		s1 = Time.now.tv_usec / 1000
		$game_res.reload() do |des, c, m, c_n|
			s2 = Time.now.tv_usec / 1000
			if(c_n == nil)
				info "=== LOAD #{des} RES(#{c}/#{m}) TIME(#{s2 - s1} ms) ==="
			else
				info "=== LOAD #{des} RES(#{c}/#{m})(#{c_n}) TIME(#{s2 - s1} ms) ==="
			end
			s1 = s2
		end
	end
	
	
	def warn(log)
		@logger_file.warn(log);
		print log, "\n"
	end
	
	def err(log)
		@logger_file.error(log);
		print log, "\n"
		if($@ != nil)
			for ermsg in $@
				@logger_file.error(ermsg);
				print ermsg,"\n"
			end
		end
	end
	
	def info(log)
		@logger_file.info(log)
		print log, "\n"
	end
	
	def find_node_by_name(name)
		for server in [$db_server, $login_server, $run_server, $gate_server]
			node = server.find_node_by_name(name)
			if(node != nil)
				return node
			end
		end
		return nil
	end
	
end
