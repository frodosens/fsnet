
require 'yaml'

class User
	
	attr_reader :uid
	attr_reader :user_name
	attr_reader :user_pwd
	attr_reader :user_uuid
	attr_reader :pid
	def initialize(uid, pid, usr, pwd, uuid)
		@uid = uid;
		@pid = pid;
		@user_name = usr;
		@user_pwd = pwd
		@user_uuid = uuid
	end
	
	def get_player
		
		return $game_players.find_player_by_pid(@pid);
		
	end
	
	def cache_key
		return User.generate_cache_key( @user_name, @user_pwd );
	end
	
	def cache
		key = User.generate_cache_key(@user_name, @user_pwd);
		$game_database.redis.set( key, self.to_yaml() )
	end
	
	class << self
		
		def generate_cache_key(usr, pwd)
			return "account:#{usr}:#{pwd}";		
		end
		
		def create_from_database(usr, pwd)
			
		end
		
		def create_from_redis_key(key)
			
			yaml = $game_database.redis.get(key);
			if(yaml != nil)
				return YAML.load(yaml);
			end
			return nil;
		end
		
		
		def create_from_redis(usr, pwd)
			
			key = generate_cache_key(usr, pwd);
			yaml = $game_database.redis.get(key);
			if(yaml != nil)
				return YAML.load(yaml);
			end
			return nil;
		end
		
	end
	
	
end