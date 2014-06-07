
class GamePlayerManager
	
	attr_reader :players;
	
	def initialize()
		@players = {};
	end
	
	def logout(pid)
		@players[pid] = nil;
	end
	
	def login(pid)
		player = find_player_by_pid(pid);
		@players[pid] = player;
		return player;
	end
	
	def find_player_by_pid(pid)
		if(@players[pid] == nil)
			return find_player_by_db(pid);
		end
		return @players[pid];
	end
	
	
	def find_player_by_db(pid)
		
		player = Player.create_from_redis(pid);
		
		if(player == nil)
			
			player = Player.create_from_database(pid);
			if(player != nil)
				player.cache();
			end
			
		end
		
		return player;
		
	end
	
	
	
end
