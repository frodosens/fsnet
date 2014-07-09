
class GamePlayerManager
	
	attr_reader :players
	
	def initialize()
		@players = {};
	end

	#==========================================================================================
	# => 登出
	#==========================================================================================
	def logout(pid)
		player = @players[pid];
		if(player != nil)
			player.on_logout()
			player.save();
			player.cache();
			@players[pid] = nil;
		end
		$game.info("#{pid} logout");
	end
	
	#==========================================================================================
	# => 登陆
	#==========================================================================================
	def login(pid)
		player = find_player_by_pid(pid);
		@players[pid] = player;
		player.on_login()
		$game.info("#{pid} login");
		return player;
	end
	

	#==========================================================================================
	# => 是否在线
	#==========================================================================================
	def is_online(pid)
		return find_player_by_pid(pid) != nil
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
	
	alias :[] :find_player_by_pid
	

	#==========================================================================================
	# => 从数据中生成player
	# => 先从cache中找
	# => 再从db中生成
	#==========================================================================================
	def find_player_by_db(pid)
		
		player = Player.create_from_redis(pid);
		
		if(player == nil)
			
			player = Player.create_from_database(pid);
			$game.info("create player (#{pid}) from database");
			if(player != nil)
				player.cache();
			end
		else

			$game.info("create player (#{pid}) from cache");
			
		end
		
		return player;
		
	end
	
	
	
end
