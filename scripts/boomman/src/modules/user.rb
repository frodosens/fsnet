require 'modules/basemode.rb'

require 'yaml'

class User < BaseModule
	
	attr_reader :uid
	attr_reader :user_name
	attr_reader :user_pwd
	attr_reader :uuid
	attr_reader :pid
	def initialize(uid, pid, usr, pwd, uuid)
		@uid = uid;
		@pid = pid;
		@user_name = usr;
		@user_pwd = pwd
		@uuid = uuid
	end
	
	def get_player
		
		return $game_players.find_player_by_pid(@pid);
		
	end
	
	def cache_key
		return User.generate_cache_key( @user_name, @user_pwd );
	end
	
	def cache
		key = User.generate_cache_key(@user_name, @user_pwd);
		$game_database.set( key, self.to_yaml() )
	end
	
	class << self
		
		def generate_cache_key(usr, pwd)
			return "account:#{usr}:#{pwd}";		
		end
		
		def create_from_database(usr, pwd)
			
			user = nil
			
			sql = "select * from tb_user where user_name='#{usr}' and user_pwd='#{pwd}' limit 0,1"
			result = $game_database.query(sql);
			if(result and result.size > 0)
				user = new(0, 0, "", "", "");
				result.each do |row|
					user.init_from_hash(row)
				end
			end
			
			return user
		end
		
		def create_from_redis_key(key)
			
			yaml = $game_database.get(key);
			if(yaml != nil)
				return YAML.load(yaml);
			end
			return nil;
		end
		
		
		def create_from_redis(usr, pwd)
			
			key = generate_cache_key(usr, pwd);
			yaml = $game_database.get(key);
			if(yaml != nil)
				return YAML.load(yaml);
			end
			return nil;
		end
		
	end
	
	
end