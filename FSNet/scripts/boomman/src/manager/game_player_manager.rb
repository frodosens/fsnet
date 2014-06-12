
class GamePlayerManager
	
	attr_reader :players;
	attr_reader :logger
	
	def initialize()
		@players = {};
		@logger = Logger.new("logs/GamePlayerManager.log")
	end

	#==========================================================================================
	# => 登出
	#==========================================================================================
	def logout(pid)
		player = @players[pid];
		if(player != nil)
			player.save();
			player.cache();
			@players[pid] = nil;
		end
		@logger.info("#{pid} logout");
	end
	
	#==========================================================================================
	# => 登陆
	#==========================================================================================
	def login(pid)
		player = find_player_by_pid(pid);
		@players[pid] = player;
		@logger.info("#{pid} login");
		return player;
	end
	

	#==========================================================================================
	# => 通过PID找到player
	#==========================================================================================
	def find_player_by_pid(pid)
		if(@players[pid] == nil)
			return find_player_by_db(pid);
		end
		return @players[pid];
	end
	

	#==========================================================================================
	# => 从数据中生成player
	# => 先从cache中找
	# => 再从db中生成
	#==========================================================================================
	def find_player_by_db(pid)
		
		player = Player.create_from_redis(pid);
		
		if(player == nil)
			
			player = Player.create_from_database(pid);
			@logger.info("create player (#{pid}) from database");
			if(player != nil)
				player.cache();
			end
		else

			@logger.info("create player (#{pid}) from cache");
			
		end
		
		return player;
		
	end
	
	
	
end
